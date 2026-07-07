#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/analyser.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> make_sine(const float frequency_hz, const double sample_rate,
                             const std::size_t num_samples) {
    std::vector<float> buffer(num_samples);
    for (std::size_t i = 0; i < num_samples; ++i) {
        const double phase =
            2.0 * std::numbers::pi * static_cast<double>(frequency_hz) *
            static_cast<double>(i) / sample_rate;
        buffer[i] = 0.6F * static_cast<float>(std::sin(phase));
    }
    return buffer;
}

bool has_note_on(const dsp::span<const dsp::NoteEvent> events, const int note) {
    for (const dsp::NoteEvent& event : events) {
        if (event.kind == dsp::MidiEventKind::Note && event.on &&
            event.note == note) {
            return true;
        }
    }
    return false;
}

std::size_t count_note_ons(const dsp::span<const dsp::NoteEvent> events) {
    std::size_t count = 0;
    for (const dsp::NoteEvent& event : events) {
        if (event.kind == dsp::MidiEventKind::Note && event.on) {
            ++count;
        }
    }
    return count;
}

std::size_t count_note_offs(const dsp::span<const dsp::NoteEvent> events) {
    std::size_t count = 0;
    for (const dsp::NoteEvent& event : events) {
        if (event.kind == dsp::MidiEventKind::Note && !event.on) {
            ++count;
        }
    }
    return count;
}

bool has_note_off(const dsp::span<const dsp::NoteEvent> events, const int note) {
    for (const dsp::NoteEvent& event : events) {
        if (event.kind == dsp::MidiEventKind::Note && !event.on &&
            event.note == note) {
            return true;
        }
    }
    return false;
}

bool has_pitch_bend(const dsp::span<const dsp::NoteEvent> events) {
    for (const dsp::NoteEvent& event : events) {
        if (event.kind == dsp::MidiEventKind::PitchBend) {
            return true;
        }
    }
    return false;
}

dsp::AnalyserConfig fast_test_config() {
    dsp::AnalyserConfig config{};
    config.hop_size = 256;
    config.window_size = 2048;
    config.midi_transpose = 0;
    config.pitch_bias_cents = 0.0F;
    config.sensitivity = 0.7F;
    config.note_on_sensitivity = 0.55F;
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 4;
    config.pitch_method = dsp::PitchMethod::Yinfast;
    return config;
}

std::vector<float> make_continuous_sine_chunk(const float frequency_hz,
                                              const double sample_rate,
                                              const std::size_t start_sample,
                                              const std::size_t num_samples,
                                              const float amplitude = 0.6F) {
    std::vector<float> buffer(num_samples);
    for (std::size_t i = 0; i < num_samples; ++i) {
        const double phase =
            2.0 * std::numbers::pi * static_cast<double>(frequency_hz) *
            static_cast<double>(start_sample + i) / sample_rate;
        buffer[i] = amplitude * static_cast<float>(std::sin(phase));
    }
    return buffer;
}

bool drive_until_note_on(dsp::Analyser& analyser,
                         const float frequency_hz, const double sample_rate,
                         const std::size_t block_size, const int note,
                         const int max_iterations) {
    std::size_t position = 0;
    for (int i = 0; i < max_iterations; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(frequency_hz, sample_rate, position,
                                       block_size);
        position += block_size;
        if (has_note_on(analyser.process(buffer), note)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Analyser prepare rejects invalid configuration", "[analyser]") {
    dsp::Analyser analyser;

    dsp::AnalyserConfig bad_hop{};
    bad_hop.hop_size = 0;
    REQUIRE_FALSE(analyser.prepare(48000.0, 512, bad_hop));

    dsp::AnalyserConfig bad_window{};
    bad_window.window_size = 1000;
    REQUIRE_FALSE(analyser.prepare(48000.0, 512, bad_window));
}

TEST_CASE("Analyser process before prepare returns empty events", "[analyser]") {
    dsp::Analyser analyser;
    std::vector<float> silence(512, 0.0F);
    const auto events = analyser.process(silence);
    REQUIRE(events.empty());
}

TEST_CASE("Analyser reports silence for zero input", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    REQUIRE(analyser.prepare(48000.0, 2048, config));

    std::vector<float> silence(512, 0.0F);
    const auto events = analyser.process(silence);
    REQUIRE(events.empty());
    REQUIRE_FALSE(analyser.last_pitch().is_valid);
}

TEST_CASE("Analyser detects 440 Hz and emits Note On", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::size_t position = 0;
    dsp::span<const dsp::NoteEvent> events{};
    bool got_note_on = false;
    for (int i = 0; i < 80; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(440.0F, kSampleRate, position, kBlockSize);
        position += kBlockSize;
        events = analyser.process(buffer);
        got_note_on = got_note_on || has_note_on(events, 69);
    }

    REQUIRE(analyser.last_pitch().is_valid);
    REQUIRE_THAT(static_cast<double>(analyser.last_pitch().frequency_hz),
                 WithinAbs(440.0, 15.0));
    REQUIRE(got_note_on);

    const dsp::PitchMonitor monitor = analyser.pitch_monitor();
    REQUIRE(monitor.pitch_valid);
    REQUIRE(monitor.midi_from_hz == 69);
    REQUIRE(monitor.midi_detected == 69);
    REQUIRE(monitor.note_active);
    REQUIRE(monitor.midi_active == 69);
}

TEST_CASE("Analyser detects G-string F3 with full guitar lowest note", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.min_note = 40;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;
    constexpr float kF3Hz = 174.61F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, kF3Hz, kSampleRate, kBlockSize, 53, 80));
    REQUIRE(analyser.last_pitch().is_valid);
    REQUIRE_THAT(static_cast<double>(analyser.last_pitch().frequency_hz),
                 WithinAbs(static_cast<double>(kF3Hz), 20.0));
}

TEST_CASE("Analyser pitch_monitor clears on silence", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 256;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::vector<float> silence(kBlockSize, 0.0F);
    for (int i = 0; i < 8; ++i) {
        (void)analyser.process(silence);
    }

    const dsp::PitchMonitor monitor = analyser.pitch_monitor();
    REQUIRE_FALSE(monitor.pitch_valid);
    REQUIRE(monitor.detected_hz == 0.0F);
    REQUIRE(monitor.input_rms == 0.0F);
}

TEST_CASE("Analyser pitch_monitor reports input RMS on low-level sine", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kC4Hz = 261.63F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::size_t position = 0;
    for (int i = 0; i < 32; ++i) {
        const auto buffer = make_continuous_sine_chunk(
            kC4Hz, kSampleRate, position, kBlockSize, 0.15F);
        position += kBlockSize;
        (void)analyser.process(buffer);
    }

    const dsp::PitchMonitor monitor = analyser.pitch_monitor();
    REQUIRE(monitor.input_rms > 0.05F);
}

TEST_CASE("Analyser maps D#4 at 312 Hz reliably", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    config.note_on_debounce_frames = 1;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kDs4Hz = 312.0F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    int note_on_count = 0;
    std::size_t position = 0;
    for (int trial = 0; trial < 3; ++trial) {
        dsp::Analyser fresh;
        REQUIRE(fresh.prepare(kSampleRate, kBlockSize, config));
        position = 0;
        if (drive_until_note_on(fresh, kDs4Hz, kSampleRate, kBlockSize, 63, 120)) {
            ++note_on_count;
        }
    }
    REQUIRE(note_on_count >= 2);
}

TEST_CASE("Analyser maps D#4 at 312 Hz", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kDs4Hz = 312.0F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, kDs4Hz, kSampleRate, kBlockSize, 63, 120));
    REQUIRE(analyser.active_note_number() == 63);
}

TEST_CASE("Analyser maps D4 around 294 Hz", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kD4Hz = 294.0F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, kD4Hz, kSampleRate, kBlockSize, 62, 120));
    REQUIRE(analyser.is_note_active());
    REQUIRE(analyser.active_note_number() == 62);
}

TEST_CASE("Analyser holds D4 294 Hz without spurious note off", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    config.note_off_delay_blocks = 8;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kD4Hz = 294.0F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, kD4Hz, kSampleRate, kBlockSize, 62, 120));

    std::size_t position = kBlockSize * 120;
    std::size_t spurious_offs = 0;
    for (int i = 0; i < 40; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(kD4Hz, kSampleRate, position, kBlockSize);
        position += kBlockSize;
        const auto events = analyser.process(buffer);
        spurious_offs += count_note_offs(events);
    }
    REQUIRE(spurious_offs == 0);
    REQUIRE(analyser.is_note_active());
}

TEST_CASE("Analyser midi_transpose shifts output by semitone", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.midi_transpose = -1;
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, 164.81F, kSampleRate, kBlockSize, 51, 80));
}

TEST_CASE("Analyser maps C3 open C string", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;
    constexpr float kC3Hz = 130.81F;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, kC3Hz, kSampleRate, kBlockSize, 48, 100));
    REQUIRE(analyser.last_pitch().is_valid);
    REQUIRE_THAT(static_cast<double>(analyser.last_pitch().frequency_hz),
                 WithinAbs(static_cast<double>(kC3Hz), 12.0));
}

TEST_CASE("Analyser maps D standard G string D3 E3 F3", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    REQUIRE(drive_until_note_on(analyser, 146.83F, kSampleRate, kBlockSize, 50, 80));
    analyser.reset();
    REQUIRE(drive_until_note_on(analyser, 164.81F, kSampleRate, kBlockSize, 52, 80));
    analyser.reset();
    REQUIRE(drive_until_note_on(analyser, 174.61F, kSampleRate, kBlockSize, 53, 80));
}

TEST_CASE("Analyser reset clears pitch and note state", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    constexpr double kSampleRate = 48000.0;

    REQUIRE(analyser.prepare(kSampleRate, 2048, config));
    const auto tone = make_sine(440.0F, kSampleRate, 2048);
    for (int i = 0; i < 10; ++i) {
        (void)analyser.process(tone);
    }

    analyser.reset();

    std::vector<float> silence(512, 0.0F);
    const auto events = analyser.process(silence);
    REQUIRE(events.empty());
    REQUIRE_FALSE(analyser.last_pitch().is_valid);
}

TEST_CASE("Analyser emits Note Off after silence", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 4;

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 256;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    const std::vector<float> silence(kBlockSize, 0.0F);

    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));

    for (int i = 0; i < 3; ++i) {
        REQUIRE_FALSE(has_note_off(analyser.process(silence), 69));
    }
    REQUIRE(has_note_off(analyser.process(silence), 69));
}

TEST_CASE("Analyser Note Off delay is configurable in blocks", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 16;

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 256;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));

    const std::vector<float> silence(kBlockSize, 0.0F);
    for (int i = 0; i < 15; ++i) {
        REQUIRE_FALSE(has_note_off(analyser.process(silence), 69));
    }
    REQUIRE(has_note_off(analyser.process(silence), 69));
}

TEST_CASE("Analyser emits Note Off when pitch disappears before delay elapses",
          "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 4;

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 256;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));

    const std::vector<float> silence(kBlockSize, 0.0F);
    bool got_note_off = false;
    for (int i = 0; i < 8; ++i) {
        got_note_off =
            got_note_off || has_note_off(analyser.process(silence), 69);
    }
    REQUIRE(got_note_off);
}

TEST_CASE("Analyser emits Note Off when sensitivity port is zero", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.sensitivity = 0.0F;
    config.note_off_delay_blocks = 4;

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 256;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(analyser.config().sensitivity >= 0.55F);

    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));

    const std::vector<float> silence(kBlockSize, 0.0F);
    bool got_note_off = false;
    for (int i = 0; i < 8; ++i) {
        got_note_off =
            got_note_off || has_note_off(analyser.process(silence), 69);
    }
    REQUIRE(got_note_off);
}

TEST_CASE("Analyser locks MIDI note while string rings", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_on_debounce_frames = 1;

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));

    const float drifting_hz[] = {415.0F, 392.0F, 370.0F, 349.0F};
    std::size_t extra_note_ons = 0;
    std::size_t position = kBlockSize * 80;
    for (const float hz : drifting_hz) {
        for (int i = 0; i < 6; ++i) {
            const auto buffer =
                make_continuous_sine_chunk(hz, kSampleRate, position, kBlockSize);
            position += kBlockSize;
            extra_note_ons += count_note_ons(analyser.process(buffer));
        }
    }
    REQUIRE(extra_note_ons == 0);
    REQUIRE(analyser.is_note_active());
}

TEST_CASE("Analyser does not retrigger Note On on sustained tone", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    REQUIRE(analyser.prepare(48000.0, 2048, config));

    REQUIRE(drive_until_note_on(analyser, 440.0F, 48000.0, 2048, 69, 80));

    std::size_t position = 2048 * 80;
    std::size_t extra_note_ons = 0;
    std::size_t spurious_note_offs = 0;
    for (int i = 0; i < 40; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(440.0F, 48000.0, position, 2048);
        position += 2048;
        const auto events = analyser.process(buffer);
        extra_note_ons += count_note_ons(events);
        spurious_note_offs += count_note_offs(events);
    }
    REQUIRE(extra_note_ons == 0);
    REQUIRE(spurious_note_offs == 0);
}

TEST_CASE("Analyser detects high E string frets reliably", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.midi_transpose = -1;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    const std::pair<float, int> targets[] = {
        {329.63F, 63},  // E4 (G string) with transpose -1
        {440.0F, 68},   // A4
        {493.88F, 70},  // B4
    };

    for (const auto& [hz, expected_note] : targets) {
        dsp::Analyser fresh;
        REQUIRE(fresh.prepare(kSampleRate, kBlockSize, config));
        REQUIRE(drive_until_note_on(fresh, hz, kSampleRate, kBlockSize,
                                    expected_note, 120));
        REQUIRE(std::abs(fresh.active_note_number() - expected_note) <= 1);
    }
}

TEST_CASE("Analyser set_config updates sensitivity at runtime", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    REQUIRE(analyser.prepare(48000.0, 2048, config));

    config.sensitivity = 0.95F;
    analyser.set_config(config);
    REQUIRE_THAT(analyser.config().sensitivity, WithinAbs(0.95F, 1e-5F));
}

TEST_CASE("Analyser move semantics transfer ownership", "[analyser]") {
    dsp::Analyser original;
    REQUIRE(original.prepare(48000.0, 2048, fast_test_config()));

    dsp::Analyser moved = std::move(original);
    std::size_t position = 0;
    for (int i = 0; i < 80; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(440.0F, 48000.0, position, 2048);
        position += 2048;
        (void)moved.process(buffer);
    }
    REQUIRE(moved.last_pitch().is_valid);
}

TEST_CASE("Analyser emits pitch bend with note on", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.pitch_bend_enable = true;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    std::size_t position = 0;
    bool got_pitch_bend = false;
    for (int i = 0; i < 80; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(445.0F, kSampleRate, position, kBlockSize);
        position += kBlockSize;
        const auto events = analyser.process(buffer);
        got_pitch_bend = got_pitch_bend || has_pitch_bend(events);
    }

    REQUIRE(got_pitch_bend);
}

TEST_CASE("pitch_bend_wheel_to_midi_bytes encodes centre", "[types]") {
    std::uint8_t bytes[3] = {};
    dsp::pitch_bend_wheel_to_midi_bytes(0, 0, bytes);
    REQUIRE(bytes[0] == 0xE0);
    REQUIRE(bytes[1] == 0x00);
    REQUIRE(bytes[2] == 0x40);
}

TEST_CASE("Analyser config returns prepared settings", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.min_note = 45;
    REQUIRE(analyser.prepare(48000.0, 2048, config));
    REQUIRE(analyser.config().min_note == 45);
    REQUIRE(analyser.config().hop_size == 256);
}

TEST_CASE("Analyser emits stable MIDI on repeated separated attacks", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_off_delay_blocks = 4;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kFrequencyHz = 329.63F;
    constexpr int kExpectedNote = 64;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::size_t position = 0;
    std::vector<int> note_ons;
    const std::vector<float> silence(kBlockSize, 0.0F);

    for (int attack = 0; attack < 4; ++attack) {
        for (int i = 0; i < 30; ++i) {
            const auto buffer = make_continuous_sine_chunk(kFrequencyHz, kSampleRate,
                                                           position, kBlockSize);
            position += kBlockSize;
            for (const dsp::NoteEvent& event : analyser.process(buffer)) {
                if (event.kind == dsp::MidiEventKind::Note && event.on) {
                    note_ons.push_back(event.note);
                }
            }
        }
        for (int i = 0; i < 24; ++i) {
            (void)analyser.process(silence);
        }
    }

    REQUIRE(note_ons.size() >= 3);
    for (const int note : note_ons) {
        REQUIRE(std::abs(note - kExpectedNote) <= 1);
    }
}

TEST_CASE("Analyser re-attacks same note after short staccato gap", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 4;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kC4Hz = 261.63F;
    constexpr int kC4Note = 60;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::size_t position = 0;
    std::vector<int> note_ons;
    const std::vector<float> silence(kBlockSize, 0.0F);

    for (int beat = 0; beat < 4; ++beat) {
        for (int i = 0; i < 160; ++i) {
            const auto buffer = make_continuous_sine_chunk(kC4Hz, kSampleRate, position,
                                                           kBlockSize);
            position += kBlockSize;
            for (const dsp::NoteEvent& event : analyser.process(buffer)) {
                if (event.kind == dsp::MidiEventKind::Note && event.on) {
                    note_ons.push_back(event.note);
                }
            }
        }
        for (int i = 0; i < 12; ++i) {
            (void)analyser.process(silence);
        }
        bool got_attack = false;
        for (int i = 0; i < 40; ++i) {
            const auto buffer = make_continuous_sine_chunk(kC4Hz, kSampleRate, position,
                                                           kBlockSize, 0.75F);
            position += kBlockSize;
            for (const dsp::NoteEvent& event : analyser.process(buffer)) {
                if (event.kind == dsp::MidiEventKind::Note && event.on &&
                    std::abs(event.note - kC4Note) <= 1) {
                    note_ons.push_back(event.note);
                    got_attack = true;
                }
            }
            if (got_attack) {
                break;
            }
        }
        REQUIRE(got_attack);
    }

    REQUIRE(note_ons.size() >= 4);
    for (const int note : note_ons) {
        REQUIRE(std::abs(note - kC4Note) <= 1);
    }
}

TEST_CASE("Analyser switches to lower fret after missed higher fret on G string",
          "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.midi_transpose = -1;
    config.note_on_debounce_frames = 1;
    config.lowest_note_limit_enable = false;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kE4Hz = 329.63F;
    constexpr float kF4Hz = 349.23F;
    constexpr int kE4Note = 63;
    constexpr int kF4Note = 64;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    std::size_t position = 0;
    REQUIRE(drive_until_note_on(analyser, kE4Hz, kSampleRate, kBlockSize, kE4Note,
                                120));
    REQUIRE(analyser.active_note_number() == kE4Note);

    for (int i = 0; i < 25; ++i) {
        const auto buffer =
            make_continuous_sine_chunk(kF4Hz, kSampleRate, position, kBlockSize);
        position += kBlockSize;
        const auto events = analyser.process(buffer);
        for (const dsp::NoteEvent& event : events) {
            if (event.kind == dsp::MidiEventKind::Note && event.on &&
                event.note == kF4Note) {
                FAIL("F4 must not replace held E4 when fret 10 is not committed");
            }
        }
    }
    REQUIRE(analyser.active_note_number() == kE4Note);

    const std::vector<float> silence(kBlockSize, 0.0F);
    for (int i = 0; i < 24; ++i) {
        (void)analyser.process(silence);
    }
    REQUIRE_FALSE(analyser.is_note_active());
}

TEST_CASE("Analyser detects staccato semitone steps", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.midi_transpose = -1;
    config.note_on_debounce_frames = 1;
    config.note_off_delay_blocks = 4;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr float kE4Hz = 329.63F;
    constexpr float kF4Hz = 349.23F;
    constexpr int kE4Note = 63;
    constexpr int kF4Note = 64;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));

    const std::vector<float> silence(kBlockSize, 0.0F);
    const std::pair<float, int> pattern[] = {{kE4Hz, kE4Note}, {kF4Hz, kF4Note},
                                             {kE4Hz, kE4Note}};
    std::size_t position = 0;

    for (const auto& [hz, expected_note] : pattern) {
        bool matched = false;
        for (int i = 0; i < 80 && !matched; ++i) {
            const auto buffer =
                make_continuous_sine_chunk(hz, kSampleRate, position, kBlockSize);
            position += kBlockSize;
            for (const dsp::NoteEvent& event : analyser.process(buffer)) {
                if (event.kind == dsp::MidiEventKind::Note && event.on &&
                    std::abs(event.note - expected_note) <= 1) {
                    matched = true;
                    break;
                }
            }
        }
        REQUIRE(matched);
        for (int i = 0; i < 24; ++i) {
            (void)analyser.process(silence);
        }
    }
}

TEST_CASE("Analyser mcomb emits Note On after confidence proxy fix", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    config.pitch_method = dsp::PitchMethod::Mcomb;
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 2048;

    REQUIRE(analyser.prepare(kSampleRate, kBlockSize, config));
    REQUIRE(drive_until_note_on(analyser, 440.0F, kSampleRate, kBlockSize, 69, 80));
}

TEST_CASE("Analyser switches pitch method at runtime", "[analyser]") {
    dsp::Analyser analyser;
    auto config = fast_test_config();
    REQUIRE(analyser.prepare(48000.0, 2048, config));

    config.pitch_method = dsp::PitchMethod::Mcomb;
    analyser.set_config(config);
    REQUIRE(analyser.config().pitch_method == dsp::PitchMethod::Mcomb);

    config.pitch_method = dsp::PitchMethod::Yinfast;
    analyser.set_config(config);
    REQUIRE(analyser.config().pitch_method == dsp::PitchMethod::Yinfast);
    REQUIRE(drive_until_note_on(analyser, 440.0F, 48000.0, 2048, 69, 40));
}
