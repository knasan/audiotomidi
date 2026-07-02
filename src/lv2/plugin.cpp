#include "dsp/analyser.hpp"
#include "ports.hpp"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace {

constexpr const char* kPluginUri = "http://github.com/knasan/audiotomidi";

/// MOD Dwarf JACK period is typically 128 frames (sometimes 64). Host may raise this via
/// LV2_BUF_SIZE__maxBlockLength; 256 is a safe default when the option is absent.
constexpr std::uint32_t kDefaultMaxBlockSize = 256;

/// Port-cache sentinel — must not collide with any valid control value.
constexpr float kPortCacheUnset = -1000.0F;

struct PluginInstance {
    dsp::Analyser analyser;

    float* audio_in = nullptr;
    LV2_Atom_Sequence* midi_out = nullptr;
    const float* sensitivity = nullptr;
    const float* midi_channel = nullptr;
    const float* note_on_debounce = nullptr;
    const float* note_off_debounce = nullptr;
    const float* pitch_bend_enable = nullptr;
    const float* lowest_note = nullptr;
    const float* reference_a4_hz = nullptr;
    const float* lowest_note_limit_enable = nullptr;
    const float* midi_transpose = nullptr;
    const float* pitch_bias_cents = nullptr;
    const float* pitch_method = nullptr;

    float* tuner_hz = nullptr;
    float* tuner_cents = nullptr;
    float* tuner_midi_from_hz = nullptr;
    float* tuner_midi_detected = nullptr;
    float* tuner_midi_active = nullptr;
    float* tuner_confidence = nullptr;
    float* tuner_rms = nullptr;

    double sample_rate = 48000.0;
    std::uint32_t max_block_size = kDefaultMaxBlockSize;
    std::uint8_t midi_channel_value = 0;

    struct ChannelOffPending {
        bool active = false;
        std::uint8_t channel = 0;
        int note = 0;
    };
    ChannelOffPending channel_off_pending{};

    LV2_URID_Map* urid_map = nullptr;
    LV2_Atom_Forge forge{};
    LV2_URID midi_event_urid = 0;
    bool forge_ready = false;
    bool active = false;

    float cached_sensitivity = -1.0F;
    float cached_midi_channel = -1.0F;
    float cached_note_on_debounce = -1.0F;
    float cached_note_off_debounce = -1.0F;
    float cached_pitch_bend_enable = -1.0F;
    float cached_lowest_note = -1.0F;
    float cached_reference_a4_hz = -1.0F;
    float cached_lowest_note_limit_enable = -1.0F;
    float cached_midi_transpose = kPortCacheUnset;
    float cached_pitch_bias_cents = kPortCacheUnset;
    float cached_pitch_method = kPortCacheUnset;
};

void read_max_block_option(PluginInstance* const self,
                           const LV2_Feature* const* features) {
    if (self->urid_map == nullptr) {
        return;
    }

    const LV2_URID urid_max_block =
        self->urid_map->map(self->urid_map->handle,
                           LV2_BUF_SIZE__maxBlockLength);
    const LV2_URID urid_atom_int =
        self->urid_map->map(self->urid_map->handle, LV2_ATOM__Int);

    for (std::uint32_t i = 0; features[i] != nullptr; ++i) {
        if (std::strcmp(features[i]->URI, LV2_OPTIONS__options) != 0) {
            continue;
        }
        const auto* options =
            static_cast<const LV2_Options_Option*>(features[i]->data);
        for (std::uint32_t j = 0;
             options[j].key != 0 || options[j].context != 0; ++j) {
            if (options[j].context == LV2_OPTIONS_INSTANCE &&
                options[j].key == urid_max_block &&
                options[j].type == urid_atom_int &&
                options[j].size == sizeof(std::int32_t) &&
                options[j].value != nullptr) {
                const auto* value =
                    static_cast<const std::int32_t*>(options[j].value);
                if (*value > 0) {
                    self->max_block_size = static_cast<std::uint32_t>(*value);
                }
            }
        }
    }
}

/// Apply LV2 control ports only when values change (avoids redundant work each block).
void apply_control_ports(PluginInstance* const self) {
    dsp::AnalyserConfig config = self->analyser.config();
    bool changed = false;

    if (self->sensitivity != nullptr) {
        const float value = std::clamp(*self->sensitivity, dsp::kMinSensitivity, 1.0F);
        if (value != self->cached_sensitivity) {
            config.sensitivity = value;
            config.note_on_sensitivity =
                std::max(0.45F, value - config.note_off_hysteresis);
            self->cached_sensitivity = value;
            changed = true;
        }
    }
    if (self->note_on_debounce != nullptr) {
        const float value = std::clamp(*self->note_on_debounce, 1.0F, 100.0F);
        if (value != self->cached_note_on_debounce) {
            config.note_on_debounce_frames =
                static_cast<std::uint32_t>(std::lround(value));
            self->cached_note_on_debounce = value;
            changed = true;
        }
    }
    if (self->note_off_debounce != nullptr) {
        const float value = std::clamp(*self->note_off_debounce, 4.0F, 16.0F);
        if (value != self->cached_note_off_debounce) {
            config.note_off_delay_blocks = dsp::snap_note_off_delay_blocks(value);
            self->cached_note_off_debounce = value;
            changed = true;
        }
    }
    if (self->midi_channel != nullptr) {
        const float value = std::clamp(*self->midi_channel, 0.0F, 15.0F);
        if (value != self->cached_midi_channel) {
            const std::uint8_t old_channel = self->midi_channel_value;
            const int active_note = self->analyser.active_note_number();
            if (active_note >= 0) {
                self->channel_off_pending.active = true;
                self->channel_off_pending.channel = old_channel;
                self->channel_off_pending.note = active_note;
                self->analyser.abandon_active_note();
            }
            self->midi_channel_value =
                static_cast<std::uint8_t>(std::lround(value));
            self->cached_midi_channel = value;
        }
    }
    if (self->pitch_bend_enable != nullptr) {
        const float value = std::clamp(*self->pitch_bend_enable, 0.0F, 1.0F);
        if (value != self->cached_pitch_bend_enable) {
            config.pitch_bend_enable = value >= 0.5F;
            self->cached_pitch_bend_enable = value;
            changed = true;
        }
    }
    if (self->lowest_note != nullptr) {
        const float value = std::clamp(*self->lowest_note, 40.0F, 88.0F);
        if (value != self->cached_lowest_note) {
            config.min_note = static_cast<int>(std::lround(value));
            self->cached_lowest_note = value;
            changed = true;
        }
    }
    if (self->reference_a4_hz != nullptr) {
        const float value = dsp::clamp_reference_a4_hz(*self->reference_a4_hz);
        if (value != self->cached_reference_a4_hz) {
            config.reference_a4_hz = value;
            self->cached_reference_a4_hz = value;
            changed = true;
        }
    }
    if (self->lowest_note_limit_enable != nullptr) {
        const float value = std::clamp(*self->lowest_note_limit_enable, 0.0F, 1.0F);
        if (value != self->cached_lowest_note_limit_enable) {
            config.lowest_note_limit_enable = value >= 0.5F;
            self->cached_lowest_note_limit_enable = value;
            changed = true;
        }
    }
    if (self->midi_transpose != nullptr) {
        const float value = std::clamp(*self->midi_transpose, -12.0F, 12.0F);
        if (value != self->cached_midi_transpose) {
            config.midi_transpose = static_cast<int>(std::lround(value));
            self->cached_midi_transpose = value;
            changed = true;
        }
    }
    if (self->pitch_bias_cents != nullptr) {
        const float value = std::clamp(*self->pitch_bias_cents, -100.0F, 100.0F);
        if (value != self->cached_pitch_bias_cents) {
            config.pitch_bias_cents = value;
            self->cached_pitch_bias_cents = value;
            changed = true;
        }
    }
    if (self->pitch_method != nullptr) {
        const float value = std::clamp(*self->pitch_method, 0.0F, 1.0F);
        if (value != self->cached_pitch_method) {
            config.pitch_method = dsp::snap_pitch_method(value);
            self->cached_pitch_method = value;
            changed = true;
        }
    }

    if (changed) {
        self->analyser.set_config(config);
    }
}

void init_forge(PluginInstance* const self) {
    if (self->urid_map == nullptr) {
        self->forge_ready = false;
        return;
    }
    lv2_atom_forge_init(&self->forge, self->urid_map);
    self->midi_event_urid =
        self->urid_map->map(self->urid_map->handle, LV2_MIDI__MidiEvent);
    self->forge_ready = true;
}

void forge_midi_message(LV2_Atom_Forge* const forge,
                      const LV2_URID midi_event_urid,
                      const std::uint8_t channel, const std::uint8_t status,
                      const std::uint8_t data1, const std::uint8_t data2,
                      const std::uint32_t frame_offset) {
    lv2_atom_forge_frame_time(forge, frame_offset);
    std::uint8_t midi_bytes[3] = {
        static_cast<std::uint8_t>(status | (channel & 0x0FU)), data1, data2};
    lv2_atom_forge_atom(forge, sizeof(midi_bytes), midi_event_urid);
    lv2_atom_forge_raw(forge, midi_bytes, sizeof(midi_bytes));
}

/// Initialise or overwrite the Atom MIDI output with an (possibly empty) event list.
void write_midi_events(PluginInstance* const self,
                       const std::span<const dsp::NoteEvent> events) {
    if (self->midi_out == nullptr || !self->forge_ready) {
        return;
    }

    lv2_atom_forge_set_buffer(
        &self->forge, reinterpret_cast<std::uint8_t*>(self->midi_out),
        self->midi_out->atom.size);

    LV2_Atom_Forge_Frame sequence_frame;
    lv2_atom_forge_sequence_head(&self->forge, &sequence_frame, 0);

    if (self->channel_off_pending.active) {
        forge_midi_message(&self->forge, self->midi_event_urid,
                           self->channel_off_pending.channel, 0x80,
                           static_cast<std::uint8_t>(self->channel_off_pending.note),
                           0, 0);
        self->channel_off_pending.active = false;
    }

    for (const dsp::NoteEvent& event : events) {
        lv2_atom_forge_frame_time(&self->forge, event.frame_offset);

        std::uint8_t midi_bytes[3] = {};

        if (event.kind == dsp::MidiEventKind::PitchBend) {
            dsp::pitch_bend_wheel_to_midi_bytes(
                event.pitch_bend_wheel, self->midi_channel_value, midi_bytes);
        } else {
            const std::uint8_t status = static_cast<std::uint8_t>(
                (event.on ? 0x90 : 0x80) | (self->midi_channel_value & 0x0F));
            const std::uint8_t velocity =
                event.on ? static_cast<std::uint8_t>(
                               std::clamp(event.velocity, 0.0F, 127.0F))
                         : 0;
            midi_bytes[0] = status;
            midi_bytes[1] = static_cast<std::uint8_t>(event.note);
            midi_bytes[2] = velocity;
        }

        lv2_atom_forge_atom(&self->forge, sizeof(midi_bytes),
                            self->midi_event_urid);
        lv2_atom_forge_raw(&self->forge, midi_bytes, sizeof(midi_bytes));
    }

    lv2_atom_forge_pop(&self->forge, &sequence_frame);
}

/// Write tuner control outputs for MOD GUI / host metering (RT-safe: no allocation).
void write_tuner_outputs(PluginInstance* const self) {
    const dsp::PitchMonitor monitor = self->analyser.pitch_monitor();

    const float hz = dsp::tuner_output_hz(monitor);

    const float cents = hz > 0.0F ? monitor.cents : 0.0F;
    const float midi_from_hz =
        hz > 0.0F ? static_cast<float>(monitor.midi_from_hz) : 0.0F;
    const float midi_detected =
        monitor.midi_detected > 0 ? static_cast<float>(monitor.midi_detected) : 0.0F;

    if (self->tuner_hz != nullptr) {
        *self->tuner_hz = hz;
    }
    if (self->tuner_cents != nullptr) {
        *self->tuner_cents = cents;
    }
    if (self->tuner_midi_from_hz != nullptr) {
        *self->tuner_midi_from_hz = midi_from_hz;
    }
    if (self->tuner_midi_detected != nullptr) {
        *self->tuner_midi_detected = midi_detected;
    }
    if (self->tuner_midi_active != nullptr) {
        *self->tuner_midi_active = static_cast<float>(monitor.midi_active);
    }
    if (self->tuner_confidence != nullptr) {
        *self->tuner_confidence = monitor.confidence;
    }
    if (self->tuner_rms != nullptr) {
        *self->tuner_rms = monitor.input_rms;
    }
}

LV2_Handle instantiate(const LV2_Descriptor* /*descriptor*/, const double rate,
                       const char* /*bundle_path*/,
                       const LV2_Feature* const* features) {
    auto* self = new PluginInstance{};
    self->sample_rate = rate;

    for (std::uint32_t i = 0; features[i] != nullptr; ++i) {
        if (std::strcmp(features[i]->URI, LV2_URID__map) == 0) {
            self->urid_map = static_cast<LV2_URID_Map*>(
                const_cast<void*>(features[i]->data));
        }
    }

    read_max_block_option(self, features);
    return self;
}

void connect_port(const LV2_Handle instance, const std::uint32_t port,
                  void* const data) {
    auto* self = static_cast<PluginInstance*>(instance);
    switch (port) {
        case lv2_ports::AudioIn:
            self->audio_in = static_cast<float*>(data);
            break;
        case lv2_ports::MidiOut:
            self->midi_out = static_cast<LV2_Atom_Sequence*>(data);
            break;
        case lv2_ports::Sensitivity:
            self->sensitivity = static_cast<const float*>(data);
            break;
        case lv2_ports::MidiChannel:
            self->midi_channel = static_cast<const float*>(data);
            break;
        case lv2_ports::NoteOnDebounce:
            self->note_on_debounce = static_cast<const float*>(data);
            break;
        case lv2_ports::NoteOffDebounce:
            self->note_off_debounce = static_cast<const float*>(data);
            break;
        case lv2_ports::PitchBendEnable:
            self->pitch_bend_enable = static_cast<const float*>(data);
            break;
        case lv2_ports::LowestNote:
            self->lowest_note = static_cast<const float*>(data);
            break;
        case lv2_ports::ReferenceA4Hz:
            self->reference_a4_hz = static_cast<const float*>(data);
            break;
        case lv2_ports::LowestNoteLimitEnable:
            self->lowest_note_limit_enable = static_cast<const float*>(data);
            break;
        case lv2_ports::MidiTranspose:
            self->midi_transpose = static_cast<const float*>(data);
            break;
        case lv2_ports::PitchBiasCents:
            self->pitch_bias_cents = static_cast<const float*>(data);
            break;
        case lv2_ports::PitchMethod:
            self->pitch_method = static_cast<const float*>(data);
            break;
        case lv2_ports::TunerHz:
            self->tuner_hz = static_cast<float*>(data);
            break;
        case lv2_ports::TunerCents:
            self->tuner_cents = static_cast<float*>(data);
            break;
        case lv2_ports::TunerMidiFromHz:
            self->tuner_midi_from_hz = static_cast<float*>(data);
            break;
        case lv2_ports::TunerMidiDetected:
            self->tuner_midi_detected = static_cast<float*>(data);
            break;
        case lv2_ports::TunerMidiActive:
            self->tuner_midi_active = static_cast<float*>(data);
            break;
        case lv2_ports::TunerConfidence:
            self->tuner_confidence = static_cast<float*>(data);
            break;
        case lv2_ports::TunerRms:
            self->tuner_rms = static_cast<float*>(data);
            break;
        default:
            break;
    }
}

void activate(const LV2_Handle instance) {
    auto* self = static_cast<PluginInstance*>(instance);
    self->active = self->analyser.prepare(self->sample_rate, self->max_block_size,
                                          dsp::AnalyserConfig{});

    self->cached_sensitivity = -1.0F;
    self->cached_midi_channel = -1.0F;
    self->cached_note_on_debounce = -1.0F;
    self->cached_note_off_debounce = -1.0F;
    self->cached_pitch_bend_enable = -1.0F;
    self->cached_lowest_note = -1.0F;
    self->cached_reference_a4_hz = -1.0F;
    self->cached_lowest_note_limit_enable = -1.0F;
    self->cached_midi_transpose = kPortCacheUnset;
    self->cached_pitch_bias_cents = kPortCacheUnset;
    self->cached_pitch_method = kPortCacheUnset;
    self->channel_off_pending = {};
    apply_control_ports(self);
    init_forge(self);
}

void run(const LV2_Handle instance, const std::uint32_t n_samples) {
    auto* self = static_cast<PluginInstance*>(instance);
    if (!self->active || n_samples == 0) {
        return;
    }

    if (!self->forge_ready && self->urid_map != nullptr) {
        init_forge(self);
    }

    if (self->audio_in == nullptr) {
        write_tuner_outputs(self);
        write_midi_events(self, {});
        return;
    }

    apply_control_ports(self);

    const auto input =
        std::span<const float>(self->audio_in, static_cast<std::size_t>(n_samples));
    const std::span<const dsp::NoteEvent> events = self->analyser.process(input);
    write_tuner_outputs(self);
    write_midi_events(self, events);
}

void deactivate(const LV2_Handle instance) {
    auto* self = static_cast<PluginInstance*>(instance);
    const int active_note = self->analyser.active_note_number();
    if (active_note >= 0) {
        self->channel_off_pending.active = true;
        self->channel_off_pending.channel = self->midi_channel_value;
        self->channel_off_pending.note = active_note;
    }
    self->analyser.reset();
    self->forge_ready = false;
    self->active = false;
}

void cleanup(const LV2_Handle instance) {
    auto* self = static_cast<PluginInstance*>(instance);
    delete self;
}

static const LV2_Descriptor kDescriptor = {
    kPluginUri,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    nullptr,
};

}  // namespace

extern "C" {

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(const std::uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

}  // extern "C"
