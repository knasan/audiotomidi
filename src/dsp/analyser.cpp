#include "dsp/analyser.hpp"

#if __has_include(<aubio/aubio.h>)
#include <aubio/aubio.h>
#else
#include "aubio.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace dsp {

struct AnalyserImpl {
    aubio_pitch_t* pitch_yinfast = nullptr;
    aubio_pitch_t* pitch_mcomb = nullptr;
    fvec_t* hop_input = nullptr;
    fvec_t* pitch_output = nullptr;

    double sample_rate = 0.0;
    std::size_t max_block_size = 0;
    bool prepared = false;

    /// Samples accumulated until one hop is ready (no memmove FIFO).
    std::vector<float> hop_buffer;
    std::size_t hop_fill = 0;

    bool was_silent = true;
    std::uint32_t silent_block_counter = 0;

    /// Blocks new Note On after release — avoids decay re-triggers on ringing strings.
    std::uint32_t squelch_blocks_after_off = 0;

    /// After Note On, block Note Off while the string transient settles.
    std::uint32_t sustain_guard_blocks = 0;

    /// Require a new pick attack before Note On after a release.
    bool awaiting_fresh_attack = false;

    float prev_block_rms = 0.0F;
    float peak_block_rms = 0.0F;

    int pending_candidate_note = -1;
    std::uint32_t pending_candidate_hops = 0;

    /// Blocks new pick re-articulation after Note Off + On (avoids double triggers).
    std::uint32_t rearticulate_cooldown_blocks = 0;

    /// Last quantised candidate from the most recent pitch hop.
    int last_candidate_note = -1;

    /// Continuous MIDI of pending candidate — detects semitone-boundary flicker.
    float pending_continuous_midi = 0.0F;
    bool pending_has_continuous = false;

    std::array<float, 3> recent_folded_hz{};
    std::uint32_t recent_folded_hz_count = 0;
    std::uint32_t recent_folded_hz_write = 0;

    float noise_floor_rms = 0.001F;

    ~AnalyserImpl() { release_aubio_objects(); }

    void release_aubio_objects() noexcept {
        if (pitch_yinfast != nullptr) {
            del_aubio_pitch(pitch_yinfast);
            pitch_yinfast = nullptr;
        }
        if (pitch_mcomb != nullptr) {
            del_aubio_pitch(pitch_mcomb);
            pitch_mcomb = nullptr;
        }
        if (hop_input != nullptr) {
            del_fvec(hop_input);
            hop_input = nullptr;
        }
        if (pitch_output != nullptr) {
            del_fvec(pitch_output);
            pitch_output = nullptr;
        }
    }

    /// Active Aubio detector for the configured method (both are allocated at prepare()).
    [[nodiscard]] aubio_pitch_t* active_detector(
        const PitchMethod method) const noexcept {
        switch (method) {
            case PitchMethod::Mcomb:
                return pitch_mcomb != nullptr ? pitch_mcomb : pitch_yinfast;
            case PitchMethod::Yinfast:
            default:
                return pitch_yinfast;
        }
    }
};

namespace {

/// yin: ~1.3 ms/hop (too heavy for 64-sample MOD buffers). yinfast: ~50 µs/hop.
bool pitch_in_range(const float frequency_hz,
                    const AnalyserConfig& config) noexcept {
    return frequency_hz >= config.min_frequency_hz &&
           frequency_hz <= config.max_frequency_hz;
}

/// Allocate both Aubio pitch detectors (RT-safe switching without re-allocation).
[[nodiscard]] bool create_pitch_detectors(AnalyserImpl& impl,
                                        const AnalyserConfig& config,
                                        const double sample_rate) noexcept {
    const auto window = static_cast<uint_t>(config.window_size);
    const auto hop = static_cast<uint_t>(config.hop_size);
    const auto rate = static_cast<uint_t>(sample_rate);

    impl.pitch_yinfast =
        new_aubio_pitch("yinfast", window, hop, rate);
    impl.pitch_mcomb = new_aubio_pitch("mcomb", window, hop, rate);
    return impl.pitch_yinfast != nullptr && impl.pitch_mcomb != nullptr;
}

/// RT hop budget: MOD 128-frame blocks get one Aubio call; larger blocks (tests) may do more.
[[nodiscard]] std::size_t max_pitch_hops_per_process(
    const std::size_t block_size, const std::size_t hop_size) noexcept {
    if (hop_size == 0) {
        return 0;
    }
    if (block_size <= hop_size * 2) {
        return 1U;
    }
    return std::min(block_size / hop_size, std::size_t{8});
}

/// Consecutive sub-threshold blocks before declaring silence (string decay hangover).
constexpr std::uint32_t kSilentBlocksToSleep = 4;

/// Ignore new Note On for this many blocks after Note Off (string decay tail).
constexpr std::uint32_t kSquelchBlocksAfterOff = 6;

/// Minimum hold time after Note On before Note Off is allowed (~43 ms @ 48 kHz / 128 fpp).
constexpr std::uint32_t kSustainGuardBlocks = 16;

/// Consecutive hops on the same semitone before Note On (idle attack).
constexpr std::uint32_t kNoteOnHopConsensus = 2;

/// Hops required for melodic change when not a clear pick attack.
constexpr std::uint32_t kMelodicHopConsensus = 2;

/// Hops for melodic change after pick attack on a held string.
constexpr std::uint32_t kMelodicAttackHopConsensus = 1;

/// Minimum blocks between pick re-articulations on the same held note.
constexpr std::uint32_t kRearticulateCooldownBlocks = 48;

/// Block RMS must rise by this factor vs. previous block to count as a new pick.
constexpr float kAttackRmsRiseRatio = 1.35F;

/// Stricter RMS rise for re-articulation — avoids false Note Off/On "pips" on sustain.
constexpr float kRearticulateRmsRiseRatio = 1.85F;

/// Slow peak decay while a note is held (reduces spurious pick detection).
constexpr float kSustainPeakDecay = 0.998F;

/// Semitone jump that clears folded-Hz history (failed prior fret, new note).
constexpr int kPitchHistoryClearSemitones = 3;

/// Active note changes when a new pitch is stable at least one semitone away.
constexpr int kMelodicIntervalMinSemitones = 1;

constexpr int kPitchBendSendThreshold = 64;

float compute_rms(const std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return 0.0F;
    }
    double sum_squares = 0.0;
    for (const float sample : samples) {
        sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return static_cast<float>(std::sqrt(sum_squares /
                                        static_cast<double>(samples.size())));
}

bool is_power_of_two(const std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool pitch_valid_for_hold(const PitchEstimate& pitch, const float gate_rms,
                          const AnalyserConfig& config) noexcept {
    return pitch.is_valid && pitch_in_range(pitch.frequency_hz, config) &&
           pitch.confidence >= note_off_threshold(config) &&
           gate_rms >= config.velocity_rms_min;
}

/// True while the input still carries a played note (pitch + audible level).
bool audio_sustains_active_note(const PitchEstimate& pitch,
                                const float gate_rms,
                                const AnalyserConfig& config) noexcept {
    if (gate_rms < config.velocity_rms_min) {
        return false;
    }
    return pitch_valid_for_hold(pitch, gate_rms, config);
}

/// Guitar still ringing for Note-Off purposes — matches tuner display tolerance.
///
/// Aubio confidence often dips during sustained playing while the MOD tuner still
/// shows a note. Treating that as silence caused spurious Note Off and left
/// @c awaiting_fresh_attack set until the string was fully muted.
bool pitch_audible_for_sustain(const PitchEstimate& pitch, const float gate_rms,
                               const AnalyserConfig& config) noexcept {
    if (audio_sustains_active_note(pitch, gate_rms, config)) {
        return true;
    }
    if (gate_rms < config.velocity_rms_min) {
        return false;
    }
    const float hz = tuner_display_hz(pitch);
    return hz > 0.0F && pitch_in_range(hz, config) &&
           pitch.confidence >= kTunerDisplayMinConfidence;
}

bool pitch_valid_for_note_on(const PitchEstimate& pitch, const float gate_rms,
                             const AnalyserConfig& config) noexcept {
    return pitch.is_valid && pitch_in_range(pitch.frequency_hz, config) &&
           pitch.confidence >= config.note_on_sensitivity &&
           gate_rms >= config.silence_rms_threshold;
}

float compute_hop_rms(const float* const samples,
                      const std::size_t count) noexcept {
    if (count == 0) {
        return 0.0F;
    }
    double sum_squares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sum_squares +=
            static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return static_cast<float>(std::sqrt(sum_squares / static_cast<double>(count)));
}

bool is_attack_transient(const float block_rms, const float prev_block_rms,
                         const AnalyserConfig& config) noexcept {
    if (block_rms < config.velocity_rms_min * 4.0F) {
        return false;
    }
    if (prev_block_rms <= config.silence_rms_threshold) {
        return true;
    }
    return block_rms > prev_block_rms * kAttackRmsRiseRatio;
}

/// Pick attack on a held note: current block exceeds the decayed sustain peak.
bool is_pick_attack_above_sustain_peak(const float block_rms,
                                       const float peak_block_rms,
                                       const AnalyserConfig& config) noexcept {
    if (block_rms < config.velocity_rms_min * 4.0F) {
        return false;
    }
    if (peak_block_rms <= config.silence_rms_threshold) {
        return false;
    }
    return block_rms > peak_block_rms * kRearticulateRmsRiseRatio;
}

void record_folded_hz(AnalyserImpl& impl, const float frequency_hz) noexcept {
    if (frequency_hz <= 0.0F) {
        return;
    }
    impl.recent_folded_hz[impl.recent_folded_hz_write++ % 3] = frequency_hz;
    if (impl.recent_folded_hz_count < 3U) {
        ++impl.recent_folded_hz_count;
    }
}

void clear_folded_hz_history(AnalyserImpl& impl) noexcept {
    impl.recent_folded_hz = {};
    impl.recent_folded_hz_count = 0;
    impl.recent_folded_hz_write = 0;
}

void clear_pitch_tracking_history(AnalyserImpl& impl) noexcept {
    impl.pending_candidate_note = -1;
    impl.pending_candidate_hops = 0;
    impl.last_candidate_note = -1;
    impl.pending_continuous_midi = 0.0F;
    impl.pending_has_continuous = false;
    clear_folded_hz_history(impl);
}

/// Semitone-boundary flicker (common 290–350 Hz on guitar) must not reset hop consensus.
void track_note_candidate(AnalyserImpl& impl, const int candidate,
                          const float quantize_hz,
                          const float reference_a4_hz) noexcept {
    const float continuous =
        frequency_to_continuous_midi_note(quantize_hz, reference_a4_hz);

    if (impl.pending_candidate_note >= 0 && candidate != impl.pending_candidate_note &&
        std::abs(candidate - impl.pending_candidate_note) == 1 &&
        impl.pending_has_continuous &&
        std::abs(continuous - impl.pending_continuous_midi) < 0.45F) {
        ++impl.pending_candidate_hops;
        impl.pending_continuous_midi = continuous;
        return;
    }

    if (impl.pending_candidate_note >= 0 &&
        midi_notes_one_octave_apart(candidate, impl.pending_candidate_note) &&
        candidate < impl.pending_candidate_note) {
        ++impl.pending_candidate_hops;
        return;
    }

    impl.pending_continuous_midi = continuous;
    impl.pending_has_continuous = quantize_hz > 0.0F;

    if (impl.pending_candidate_note >= 0 &&
        std::abs(candidate - impl.pending_candidate_note) >=
            kPitchHistoryClearSemitones) {
        clear_folded_hz_history(impl);
        impl.pending_candidate_note = candidate;
        impl.pending_candidate_hops = 1U;
        return;
    }
    if (impl.pending_candidate_note >= 0 &&
        std::abs(candidate - impl.pending_candidate_note) > 5) {
        impl.pending_candidate_note = candidate;
        impl.pending_candidate_hops = 1U;
        return;
    }
    if (candidate == impl.pending_candidate_note) {
        ++impl.pending_candidate_hops;
    } else {
        impl.pending_candidate_note = candidate;
        impl.pending_candidate_hops = 1U;
    }
}

bool note_candidate_is_stable(const AnalyserImpl& impl,
                              const bool attack_phase) noexcept {
    const std::uint32_t need = attack_phase ? 1U : kNoteOnHopConsensus;
    return impl.pending_candidate_hops >= need;
}

bool note_candidate_stable_for_attack(const AnalyserImpl& impl,
                                      const float quantize_hz,
                                      const bool attack_phase) noexcept {
    if (note_candidate_is_stable(impl, attack_phase)) {
        return true;
    }
    return attack_phase && frequency_in_melody_course_band(quantize_hz) &&
           impl.pending_candidate_hops >= 1U &&
           impl.pending_candidate_note >= 0;
}

bool note_candidate_stable_for_melodic(const AnalyserImpl& impl,
                                       const int candidate,
                                       const bool deliberate_pick) noexcept {
    if (impl.pending_candidate_note != candidate) {
        return false;
    }
    if (impl.pending_candidate_hops >= kMelodicHopConsensus) {
        return true;
    }
    return deliberate_pick &&
           impl.pending_candidate_hops >= kMelodicAttackHopConsensus;
}

float median_recent_folded_hz(const AnalyserImpl& impl) noexcept {
    if (impl.recent_folded_hz_count == 0U) {
        return 0.0F;
    }
    std::array<float, 3> values{};
    for (std::uint32_t i = 0; i < impl.recent_folded_hz_count; ++i) {
        const std::uint32_t index =
            (impl.recent_folded_hz_write + 3U - impl.recent_folded_hz_count + i) % 3U;
        values[i] = impl.recent_folded_hz[index];
    }
    if (impl.recent_folded_hz_count == 1U) {
        return values[0];
    }
    if (impl.recent_folded_hz_count == 2U) {
        return 0.5F * (values[0] + values[1]);
    }
    std::sort(values.begin(), values.begin() + 3);
    return values[1];
}

int pitch_hint_from_tracking(const AnalyserImpl& impl) noexcept {
    if (impl.pending_candidate_hops >= kNoteOnHopConsensus &&
        impl.pending_candidate_note >= 0) {
        return impl.pending_candidate_note;
    }
    return -1;
}

int resolve_pitch_hint(const AnalyserImpl& impl, const int active_hint,
                       const bool awaiting_stable_note_on,
                       const int midi_transpose) noexcept {
    if (active_hint >= 0) {
        return active_hint;
    }
    if (impl.pending_candidate_note >= 0 && impl.pending_candidate_hops >= 1U) {
        return impl.pending_candidate_note - midi_transpose;
    }
    if (!awaiting_stable_note_on) {
        const int tracked = pitch_hint_from_tracking(impl);
        return tracked >= 0 ? tracked - midi_transpose : -1;
    }
    return -1;
}

}  // namespace

Analyser::Analyser() : impl_(std::make_unique<AnalyserImpl>()) {}

Analyser::~Analyser() = default;

Analyser::Analyser(Analyser&&) noexcept = default;

Analyser& Analyser::operator=(Analyser&&) noexcept = default;

bool Analyser::prepare(const double sample_rate, const std::size_t max_block_size,
                       AnalyserConfig config) {
    impl_->release_aubio_objects();
    impl_->hop_buffer.clear();
    impl_->hop_fill = 0;
    impl_->was_silent = true;
    impl_->silent_block_counter = 0;
    impl_->squelch_blocks_after_off = 0;
    impl_->sustain_guard_blocks = 0;
    impl_->awaiting_fresh_attack = false;
    impl_->prev_block_rms = 0.0F;
    impl_->peak_block_rms = 0.0F;
    impl_->pending_candidate_note = -1;
    impl_->pending_candidate_hops = 0;
    impl_->rearticulate_cooldown_blocks = 0;
    impl_->last_candidate_note = -1;
    clear_folded_hz_history(*impl_);
    impl_->noise_floor_rms = 0.001F;
    impl_->prepared = false;

    if (sample_rate <= 0.0 || max_block_size == 0) {
        return false;
    }
    if (config.hop_size == 0 || config.window_size < config.hop_size) {
        return false;
    }
    if (!is_power_of_two(config.window_size)) {
        return false;
    }

    impl_->pitch_yinfast = nullptr;
    impl_->pitch_mcomb = nullptr;
    if (!create_pitch_detectors(*impl_, config, sample_rate)) {
        impl_->release_aubio_objects();
        return false;
    }

    impl_->hop_input = new_fvec(static_cast<uint_t>(config.hop_size));
    impl_->pitch_output = new_fvec(1);
    if (impl_->hop_input == nullptr || impl_->pitch_output == nullptr) {
        impl_->release_aubio_objects();
        return false;
    }

    impl_->sample_rate = sample_rate;
    impl_->max_block_size = max_block_size;
    impl_->hop_buffer.assign(config.hop_size, 0.0F);
    impl_->prepared = true;

    config_ = config;
    config_.sensitivity = clamped_sensitivity(config_);
    config_.note_on_sensitivity =
        std::max(0.45F, config_.sensitivity - config_.note_off_hysteresis);
    reset();
    return true;
}

void Analyser::reset() {
    active_note_.reset();
    note_on_counter_ = 0;
    note_off_counter_ = 0;
    pending_count_ = 0;
    last_pitch_ = {};
    last_sent_pitch_bend_wheel_ = 0;
    attack_phase_ = false;

    if (!impl_->prepared) {
        return;
    }

    impl_->hop_fill = 0;
    impl_->was_silent = true;
    impl_->silent_block_counter = 0;
    impl_->squelch_blocks_after_off = 0;
    impl_->sustain_guard_blocks = 0;
    impl_->awaiting_fresh_attack = false;
    impl_->prev_block_rms = 0.0F;
    impl_->peak_block_rms = 0.0F;
    impl_->pending_candidate_note = -1;
    impl_->pending_candidate_hops = 0;
    impl_->rearticulate_cooldown_blocks = 0;
    impl_->last_candidate_note = -1;
    clear_folded_hz_history(*impl_);
}

void Analyser::set_config(const AnalyserConfig& config) noexcept {
    const bool method_changed = config_.pitch_method != config.pitch_method;
    config_ = config;
    config_.sensitivity = clamped_sensitivity(config_);
    config_.note_on_sensitivity =
        std::max(0.45F, config_.sensitivity - config_.note_off_hysteresis);

    if (method_changed && impl_->prepared) {
        impl_->hop_fill = 0;
        clear_pitch_tracking_history(*impl_);
        last_pitch_ = {};
    }
}

const AnalyserConfig& Analyser::config() const noexcept {
    return config_;
}

PitchEstimate Analyser::last_pitch() const noexcept {
    return last_pitch_;
}

PitchMonitor Analyser::pitch_monitor() const noexcept {
    return pitch_monitor_;
}

bool Analyser::is_note_active() const noexcept {
    return active_note_.has_value();
}

int Analyser::active_note_number() const noexcept {
    return active_note_.has_value() ? active_note_->note : -1;
}

void Analyser::abandon_active_note() noexcept {
    active_note_.reset();
    note_on_counter_ = 0;
    note_off_counter_ = 0;
    attack_phase_ = false;
    last_sent_pitch_bend_wheel_ = 0;
}

void Analyser::emit_note(const int note, const float velocity, const bool on,
                         const std::uint32_t frame_offset,
                         const int pitch_bend_wheel) {
    if (pending_count_ >= pending_.size()) {
        return;
    }
    pending_[pending_count_++] = NoteEvent{
        MidiEventKind::Note, note, velocity, on, pitch_bend_wheel, frame_offset};
}

void Analyser::emit_pitch_bend(const int wheel,
                               const std::uint32_t frame_offset) {
    if (pending_count_ >= pending_.size()) {
        return;
    }
    pending_[pending_count_++] =
        NoteEvent{MidiEventKind::PitchBend, 0,   0.0F, false,
                  wheel,                  frame_offset};
    last_sent_pitch_bend_wheel_ = wheel;
}

void Analyser::maybe_emit_continuous_pitch_bend(
    const PitchEstimate& pitch, const std::uint32_t frame_offset) {
    if (!config_.pitch_bend_enable || attack_phase_ || !active_note_.has_value()) {
        return;
    }
    // Locked semitone during sustain — send centre bend only (no yinfast wobble).
    if (std::abs(last_sent_pitch_bend_wheel_) < kPitchBendSendThreshold) {
        return;
    }
    emit_pitch_bend(0, frame_offset);
}

void Analyser::maybe_rearticulate_on_pick(const float /*block_rms*/,
                                          const std::size_t /*block_size*/) {
    // Disabled: same-note Note Off/On on sustain dynamics caused audible pitch
    // wobble on MOD (especially D4/D#4). Melodic changes use deliberate_pick paths.
}

void Analyser::force_note_off(const std::size_t block_size) {
    if (!active_note_.has_value()) {
        return;
    }
    const std::uint32_t end_frame =
        block_size > 0 ? static_cast<std::uint32_t>(block_size - 1) : 0U;
    if (config_.pitch_bend_enable) {
        emit_pitch_bend(0, end_frame);
    }
    emit_note(active_note_->note, 0.0F, false, end_frame);
    active_note_.reset();
    note_off_counter_ = 0;
    note_on_counter_ = 0;
    attack_phase_ = false;
    impl_->squelch_blocks_after_off = kSquelchBlocksAfterOff;
    impl_->sustain_guard_blocks = 0;
    impl_->awaiting_fresh_attack = true;
    clear_pitch_tracking_history(*impl_);
}

void Analyser::update_note_off_delay(const float block_rms,
                                     const std::size_t block_size) {
    if (!active_note_.has_value()) {
        note_off_counter_ = 0;
        return;
    }

    if (impl_->sustain_guard_blocks > 0U) {
        if (block_rms >= config_.silence_rms_threshold) {
            --impl_->sustain_guard_blocks;
            note_off_counter_ = 0;
            return;
        }
        impl_->sustain_guard_blocks = 0U;
    }

    const float gate_rms = std::min(last_pitch_.rms, block_rms);

    if (pitch_audible_for_sustain(last_pitch_, gate_rms, config_)) {
        if (note_off_counter_ > 0U) {
            --note_off_counter_;
        }
        return;
    }

    ++note_off_counter_;
    if (note_off_counter_ >= config_.note_off_delay_blocks) {
        force_note_off(block_size);
    }
}

void Analyser::apply_pitch_to_note_machine(const PitchEstimate& pitch,
                                           const std::size_t block_size,
                                           const float block_rms) {
    const std::uint32_t end_frame =
        block_size > 0 ? static_cast<std::uint32_t>(block_size - 1) : 0U;

    const float gate_rms = std::min(pitch.rms, block_rms);

    const float source_hz =
        pitch.raw_frequency_hz > 0.0F ? pitch.raw_frequency_hz : pitch.frequency_hz;

    const bool deliberate_pick =
        attack_phase_ ||
        is_attack_transient(block_rms, impl_->prev_block_rms, config_) ||
        is_pick_attack_above_sustain_peak(block_rms, impl_->peak_block_rms, config_);

    const bool awaiting_stable_note_on =
        !active_note_.has_value() &&
        impl_->pending_candidate_hops < kNoteOnHopConsensus;

    const bool prefer_upper_octave =
        awaiting_stable_note_on || attack_phase_ || deliberate_pick;

    const float quick_folded_hz = prefer_melody_course_octave(
        refine_guitar_fundamental(source_hz, config_, -1, prefer_upper_octave),
        config_);
    const int quick_candidate = std::clamp(
        frequency_to_midi_note_with_bias(quick_folded_hz, config_.reference_a4_hz,
                                         config_.pitch_bias_cents, -1) +
            config_.midi_transpose,
        0, 127);

    const bool new_note_pick =
        active_note_.has_value() && deliberate_pick &&
        std::abs(quick_candidate - active_note_->note) >=
            kMelodicIntervalMinSemitones;

    if (new_note_pick) {
        clear_folded_hz_history(*impl_);
    }

    const int active_hint =
        active_note_.has_value() && !new_note_pick
            ? active_note_->note - config_.midi_transpose
            : -1;
    const int pitch_hint = resolve_pitch_hint(*impl_, active_hint,
                                              awaiting_stable_note_on,
                                              config_.midi_transpose);
    float folded_hz = refine_guitar_fundamental(source_hz, config_, pitch_hint,
                                                prefer_upper_octave && pitch_hint < 0);
    folded_hz = prefer_melody_course_octave(folded_hz, config_);
    last_pitch_.frequency_hz = folded_hz;

    const float held_target_hz =
        active_note_.has_value()
            ? midi_note_to_frequency(active_note_->note - config_.midi_transpose,
                                     config_.reference_a4_hz)
            : 0.0F;
    const bool significant_pitch_change =
        active_note_.has_value() && deliberate_pick && folded_hz > 0.0F &&
        held_target_hz > 0.0F &&
        std::abs(folded_hz - held_target_hz) / held_target_hz > 0.045F;

    if (significant_pitch_change) {
        clear_pitch_tracking_history(*impl_);
    }

    record_folded_hz(*impl_, folded_hz);

    const bool smooth_octave = !awaiting_stable_note_on;

    if (smooth_octave && impl_->recent_folded_hz_count >= 2U) {
        const float median_hz = median_recent_folded_hz(*impl_);
        if (median_hz > 0.0F && folded_hz > median_hz * 1.85F) {
            const int note_folded =
                frequency_to_midi_note(folded_hz, config_.reference_a4_hz);
            const int note_median =
                frequency_to_midi_note(median_hz, config_.reference_a4_hz);
            if (note_folded - note_median >= 11 && note_folded - note_median <= 13) {
                folded_hz *= 0.5F;
                last_pitch_.frequency_hz = folded_hz;
            }
        }
    }

    float quantize_hz = folded_hz;
    if (!new_note_pick && !significant_pitch_change &&
        impl_->recent_folded_hz_count >= 2U) {
        const float median_hz = median_recent_folded_hz(*impl_);
        if (median_hz > 0.0F) {
            const int note_folded = frequency_to_midi_note(folded_hz, config_.reference_a4_hz);
            const int note_median = frequency_to_midi_note(median_hz, config_.reference_a4_hz);
            const int max_median_pull = awaiting_stable_note_on ? 1 : 2;
            if (note_folded == note_median ||
                std::abs(note_folded - note_median) <= max_median_pull) {
                quantize_hz = median_hz;
            } else if (smooth_octave && note_folded - note_median >= 11) {
                quantize_hz = median_hz;
            }
        }
    }

    int quantize_hint = -1;
    if (impl_->pending_candidate_hops >= 1U && impl_->pending_candidate_note >= 0) {
        quantize_hint = impl_->pending_candidate_note - config_.midi_transpose;
    }
    int candidate = frequency_to_midi_note_with_bias(
        quantize_hz, config_.reference_a4_hz, config_.pitch_bias_cents, quantize_hint);
    candidate = std::clamp(candidate + config_.midi_transpose, 0, 127);

    if (midi_notes_one_octave_apart(candidate, quick_candidate) &&
        candidate < quick_candidate && prefer_upper_octave) {
        candidate = quick_candidate;
        quantize_hz = quick_folded_hz;
        folded_hz = quick_folded_hz;
        last_pitch_.frequency_hz = folded_hz;
    }

    impl_->last_candidate_note = candidate;
    const bool in_note_range = note_in_range(candidate, config_);

    const int bend_wheel =
        pitch.is_valid && folded_hz > 0.0F
            ? frequency_to_pitch_bend_wheel(folded_hz, config_.reference_a4_hz)
            : 0;

    const bool pitch_ok = pitch_valid_for_note_on(pitch, gate_rms, config_) &&
                          in_note_range;
    const bool attack_pitch_ok =
        attack_phase_ && !active_note_.has_value() &&
        pitch_audible_for_sustain(pitch, gate_rms, config_) && in_note_range;
    const bool recovery_pitch_ok =
        !active_note_.has_value() && impl_->awaiting_fresh_attack &&
        impl_->squelch_blocks_after_off == 0U &&
        pitch_audible_for_sustain(pitch, gate_rms, config_) && in_note_range;
    const bool on_eligible = pitch_ok || attack_pitch_ok || recovery_pitch_ok;
    const bool subharmonic_glitch =
        on_eligible && quick_candidate > 0 &&
        midi_notes_one_octave_apart(candidate, quick_candidate) &&
        candidate < quick_candidate &&
        !frequency_in_melody_course_band(folded_hz);
    const bool on_ok = on_eligible && !subharmonic_glitch;

    if (on_eligible && !subharmonic_glitch) {
        track_note_candidate(*impl_, candidate, quantize_hz, config_.reference_a4_hz);
    }

    if (!active_note_.has_value() && on_ok) {
        ++note_on_counter_;
    }

    // Pick / slide to a new fret while the string still rings.
    const bool melodic_switch =
        active_note_.has_value() && deliberate_pick && on_eligible &&
        impl_->squelch_blocks_after_off == 0U &&
        candidate != active_note_->note && note_in_range(candidate, config_) &&
        !midi_notes_one_octave_apart(candidate, active_note_->note) &&
        (new_note_pick || significant_pitch_change);

    if (melodic_switch) {
        const float velocity = rms_to_velocity(
            note_on_velocity_rms(block_rms, pitch.rms), config_);
        if (config_.pitch_bend_enable) {
            emit_pitch_bend(0, end_frame);
        }
        emit_note(active_note_->note, 0.0F, false, end_frame);
        emit_note(candidate, velocity, true, end_frame);
        active_note_ = ActiveNote{candidate, velocity};
        note_on_counter_ = 0;
        note_off_counter_ = 0;
        attack_phase_ = false;
        impl_->awaiting_fresh_attack = false;
        impl_->sustain_guard_blocks = kSustainGuardBlocks;
        impl_->peak_block_rms = block_rms;
        impl_->rearticulate_cooldown_blocks = 0;
        clear_pitch_tracking_history(*impl_);
        maybe_emit_continuous_pitch_bend(pitch, end_frame);
        return;
    }

    // Melodic interval change (consensus fallback).
    const bool deliberate_new_pitch = deliberate_pick;
    if (active_note_.has_value() && on_ok && deliberate_new_pitch &&
        std::abs(candidate - active_note_->note) >= kMelodicIntervalMinSemitones &&
        note_candidate_stable_for_melodic(*impl_, candidate, deliberate_new_pitch) &&
        note_in_range(candidate, config_) &&
        impl_->squelch_blocks_after_off == 0U &&
        !midi_notes_one_octave_apart(candidate, active_note_->note)) {
        const float velocity = rms_to_velocity(
            note_on_velocity_rms(block_rms, pitch.rms), config_);
        if (config_.pitch_bend_enable) {
            emit_pitch_bend(0, end_frame);
        }
        emit_note(active_note_->note, 0.0F, false, end_frame);
        emit_note(candidate, velocity, true, end_frame);
        active_note_ = ActiveNote{candidate, velocity};
        note_on_counter_ = 0;
        note_off_counter_ = 0;
        attack_phase_ = false;
        impl_->awaiting_fresh_attack = false;
        impl_->sustain_guard_blocks = kSustainGuardBlocks;
        impl_->peak_block_rms = block_rms;
        impl_->rearticulate_cooldown_blocks = 0;
        clear_pitch_tracking_history(*impl_);
        maybe_emit_continuous_pitch_bend(pitch, end_frame);
        return;
    }

    const std::uint32_t on_debounce =
        attack_phase_ ? 1U : config_.note_on_debounce_frames;

    // After squelch, stable pitch is enough — do not require another pick transient.
    if (impl_->awaiting_fresh_attack && impl_->squelch_blocks_after_off == 0U &&
        pitch_audible_for_sustain(pitch, gate_rms, config_) &&
        note_candidate_is_stable(*impl_, attack_phase_)) {
        impl_->awaiting_fresh_attack = false;
    }

    const bool may_trigger_note_on =
        attack_phase_ || !impl_->awaiting_fresh_attack ||
        is_attack_transient(block_rms, impl_->prev_block_rms, config_) ||
        recovery_pitch_ok;

    // Note is locked from first Note On until Note Off — no semitone stepping on decay.
    const bool quick_agrees =
        std::abs(candidate - quick_candidate) <= 2 ||
        midi_notes_one_octave_apart(candidate, quick_candidate);
    const bool stable_for_on =
        note_candidate_is_stable(*impl_, attack_phase_) ||
        note_candidate_stable_for_attack(*impl_, quantize_hz, attack_phase_);
    if (!active_note_.has_value() && on_ok && quick_agrees &&
        note_on_counter_ >= on_debounce &&
        impl_->squelch_blocks_after_off == 0U && may_trigger_note_on &&
        stable_for_on) {
        const float velocity = rms_to_velocity(
            note_on_velocity_rms(block_rms, pitch.rms), config_);
        emit_note(candidate, velocity, true, 0);
        if (config_.pitch_bend_enable &&
            std::abs(bend_wheel) >= kPitchBendSendThreshold) {
            emit_pitch_bend(bend_wheel, 0);
        }
        active_note_ = ActiveNote{candidate, velocity};
        note_on_counter_ = 0;
        note_off_counter_ = 0;
        attack_phase_ = false;
        impl_->awaiting_fresh_attack = false;
        impl_->sustain_guard_blocks = kSustainGuardBlocks;
        impl_->peak_block_rms = block_rms;
        clear_pitch_tracking_history(*impl_);
    }

    maybe_emit_continuous_pitch_bend(pitch, end_frame);
}

void Analyser::run_pitch_hop() {
    const std::size_t hop = config_.hop_size;
    const float hop_rms = compute_hop_rms(impl_->hop_buffer.data(), hop);

    std::memcpy(impl_->hop_input->data, impl_->hop_buffer.data(),
                hop * sizeof(float));
    aubio_pitch_t* const detector = impl_->active_detector(config_.pitch_method);
    if (detector == nullptr) {
        return;
    }
    aubio_pitch_do(detector, impl_->hop_input, impl_->pitch_output);

    float raw_frequency = impl_->pitch_output->data[0];
    const float aubio_confidence = aubio_pitch_get_confidence(detector);

    float folded_hz = raw_frequency;
    if (!frequency_is_analysis_artifact(raw_frequency, impl_->sample_rate, hop,
                                        aubio_confidence)) {
        const bool prefer_upper =
            !active_note_.has_value() || attack_phase_;
        folded_hz = refine_guitar_fundamental(raw_frequency, config_, -1, prefer_upper);
        folded_hz = prefer_melody_course_octave(folded_hz, config_);
    } else {
        folded_hz = 0.0F;
        raw_frequency = 0.0F;
    }

    PitchEstimate estimate{};
    estimate.raw_frequency_hz = raw_frequency;
    estimate.frequency_hz = folded_hz;
    estimate.rms = hop_rms;
    const float confidence = effective_pitch_confidence(
        config_.pitch_method, aubio_confidence, raw_frequency, hop_rms, config_);
    estimate.confidence = confidence;
    estimate.is_valid = folded_hz > 0.0F && pitch_in_range(folded_hz, config_) &&
                        confidence >= effective_note_on_confidence(
                            config_, folded_hz);

    last_pitch_ = estimate;
}

void Analyser::accumulate_samples(const std::span<const float> samples,
                                  const std::size_t block_size,
                                  const float block_rms,
                                  std::size_t& hops_done) {
    const std::size_t hop = config_.hop_size;
    const std::size_t hop_budget =
        max_pitch_hops_per_process(block_size, hop);

    for (const float sample : samples) {
        if (impl_->hop_fill < hop) {
            impl_->hop_buffer[impl_->hop_fill++] = sample;
        }
        if (impl_->hop_fill < hop) {
            continue;
        }

        if (hops_done >= hop_budget) {
            impl_->hop_fill = 0;
            continue;
        }

        run_pitch_hop();
        apply_pitch_to_note_machine(last_pitch_, block_size, block_rms);
        impl_->hop_fill = 0;
        ++hops_done;
    }
}

void Analyser::update_pitch_monitor() noexcept {
    const float input_rms = pitch_monitor_.input_rms;
    pitch_monitor_ = {};
    pitch_monitor_.input_rms = input_rms;

    pitch_monitor_.note_active = active_note_.has_value();
    pitch_monitor_.midi_active =
        active_note_.has_value() ? active_note_->note : 0;

    const PitchEstimate& pitch = last_pitch_;
    pitch_monitor_.raw_hz = pitch.raw_frequency_hz;
    pitch_monitor_.confidence = pitch.confidence;

    const float display_hz = tuner_display_hz(pitch);
    if (display_hz > 0.0F && pitch_in_range(display_hz, config_)) {
        pitch_monitor_.detected_hz = display_hz;
    }

    if (!pitch_valid_for_tuner_display(pitch, config_)) {
        if (display_hz > 0.0F) {
            pitch_monitor_.cents =
                frequency_to_pitch_bend_cents(display_hz, config_.reference_a4_hz);
            pitch_monitor_.midi_from_hz = frequency_to_midi_note_nearest_temperament(
                display_hz, config_.reference_a4_hz);
        }
        return;
    }

    pitch_monitor_.pitch_valid = true;
    pitch_monitor_.detected_hz = display_hz;
    pitch_monitor_.cents =
        frequency_to_pitch_bend_cents(display_hz, config_.reference_a4_hz);
    pitch_monitor_.midi_from_hz = frequency_to_midi_note_nearest_temperament(
        display_hz, config_.reference_a4_hz);

    if (impl_->last_candidate_note >= 0 && pitch.is_valid) {
        pitch_monitor_.midi_detected = impl_->last_candidate_note;
        return;
    }

    if (pitch.is_valid) {
        const int quick = std::clamp(
            frequency_to_midi_note_with_bias(display_hz, config_.reference_a4_hz,
                                             config_.pitch_bias_cents, -1) +
                config_.midi_transpose,
            0, 127);
        pitch_monitor_.midi_detected = quick;
    }
}

std::span<const NoteEvent> Analyser::process(
    const std::span<const float> samples) {
    pending_count_ = 0;

    const float block_rms =
        samples.empty() ? 0.0F : compute_rms(samples);
    pitch_monitor_.input_rms = block_rms;

    if (!impl_->prepared || impl_->pitch_yinfast == nullptr ||
        impl_->pitch_mcomb == nullptr || impl_->hop_input == nullptr ||
        impl_->pitch_output == nullptr) {
        last_pitch_ = {};
        update_pitch_monitor();
        return std::span<const NoteEvent>(pending_.data(), pending_count_);
    }
    if (samples.size() > impl_->max_block_size) {
        last_pitch_ = {};
        update_pitch_monitor();
        return std::span<const NoteEvent>(pending_.data(), pending_count_);
    }

    const float silence_gate = config_.silence_rms_threshold;
    const bool hard_silence = block_rms < silence_gate * 0.05F;
    const bool below_silence = block_rms < silence_gate;

    if (hard_silence) {
        ++impl_->silent_block_counter;

        if (impl_->silent_block_counter >= kSilentBlocksToSleep) {
            impl_->hop_fill = 0;
            if (!impl_->was_silent) {
                impl_->was_silent = true;
                attack_phase_ = false;
            }
            last_pitch_ = {};
        }
        last_pitch_.rms = block_rms;
        last_pitch_.is_valid = false;
        if (impl_->squelch_blocks_after_off > 0U) {
            --impl_->squelch_blocks_after_off;
        }
        update_note_off_delay(block_rms, samples.size());
        update_pitch_monitor();
        return std::span<const NoteEvent>(pending_.data(), pending_count_);
    }

    if (below_silence) {
        ++impl_->silent_block_counter;
        if (impl_->silent_block_counter >= kSilentBlocksToSleep) {
            if (!impl_->was_silent) {
                impl_->was_silent = true;
                attack_phase_ = false;
            }
        }
    } else {
        impl_->silent_block_counter = 0;

        if (impl_->was_silent) {
            impl_->was_silent = false;
            impl_->hop_fill = 0;
            attack_phase_ = true;
            clear_pitch_tracking_history(*impl_);
        }
    }

    if (impl_->rearticulate_cooldown_blocks > 0U) {
        --impl_->rearticulate_cooldown_blocks;
    }

    std::size_t hops_done = 0;
    accumulate_samples(samples, samples.size(), block_rms, hops_done);

    if (impl_->squelch_blocks_after_off > 0U) {
        --impl_->squelch_blocks_after_off;
    }

    if (impl_->hop_fill == 0 && !samples.empty() && last_pitch_.frequency_hz <= 0.0F) {
        last_pitch_.rms = block_rms;
        last_pitch_.is_valid = false;
    }

    maybe_rearticulate_on_pick(block_rms, samples.size());

    update_note_off_delay(block_rms, samples.size());

    if (active_note_.has_value()) {
        impl_->peak_block_rms =
            std::max(block_rms, impl_->peak_block_rms * kSustainPeakDecay);
    } else if (block_rms < config_.silence_rms_threshold * 4.0F) {
        impl_->noise_floor_rms =
            impl_->noise_floor_rms * 0.98F + block_rms * 0.02F;
    }
    impl_->prev_block_rms = block_rms;

    update_pitch_monitor();
    return std::span<const NoteEvent>(pending_.data(), pending_count_);
}

}  // namespace dsp
