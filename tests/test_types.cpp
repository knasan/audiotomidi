#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/types.hpp"

#include <cmath>
#include <cstring>

using Catch::Matchers::WithinAbs;

TEST_CASE("frequency_to_continuous_midi_note maps A4 to 69", "[types]") {
    REQUIRE_THAT(dsp::frequency_to_continuous_midi_note(440.0F),
                 WithinAbs(69.0F, 1e-4F));
}

TEST_CASE("frequency_to_pitch_bend_cents is zero at exact semitone", "[types]") {
    REQUIRE_THAT(dsp::frequency_to_pitch_bend_cents(440.0F), WithinAbs(0.0F, 0.1F));
}

TEST_CASE("frequency_to_pitch_bend_cents detects sharp pitch", "[types]") {
    const float sharp_hz = 440.0F * std::pow(2.0F, 25.0F / 1200.0F);
    REQUIRE(dsp::frequency_to_pitch_bend_cents(sharp_hz) > 20.0F);
}

TEST_CASE("frequency_to_pitch_bend_wheel is centred at exact semitone", "[types]") {
    REQUIRE(dsp::frequency_to_pitch_bend_wheel(440.0F) == 0);
}

TEST_CASE("frequency_to_midi_note maps A4 to 69", "[types]") {
    REQUIRE(dsp::frequency_to_midi_note(440.0F) == 69);
}

TEST_CASE("frequency_to_midi_note returns 0 for non-positive input", "[types]") {
    REQUIRE(dsp::frequency_to_midi_note(0.0F) == 0);
    REQUIRE(dsp::frequency_to_midi_note(-10.0F) == 0);
}

TEST_CASE("note_in_range respects configured window", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 40;
    config.max_note = 88;

    REQUIRE_FALSE(dsp::note_in_range(30, config));
    REQUIRE(dsp::note_in_range(69, config));
    REQUIRE_FALSE(dsp::note_in_range(100, config));
}

TEST_CASE("clamp_note respects configured range", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 40;
    config.max_note = 88;

    REQUIRE(dsp::clamp_note(30, config) == 40);
    REQUIRE(dsp::clamp_note(69, config) == 69);
    REQUIRE(dsp::clamp_note(100, config) == 88);
}

TEST_CASE("note_off_threshold applies hysteresis", "[types]") {
    dsp::AnalyserConfig config{};
    config.sensitivity = 0.8F;
    config.note_off_hysteresis = 0.15F;

    REQUIRE_THAT(dsp::note_off_threshold(config), WithinAbs(0.65F, 1e-5F));
}

TEST_CASE("note_off_threshold never goes negative", "[types]") {
    dsp::AnalyserConfig config{};
    config.sensitivity = 0.1F;
    config.note_off_hysteresis = 0.5F;

    REQUIRE(dsp::note_off_threshold(config) >= 0.0F);
}

TEST_CASE("refine_guitar_fundamental uses hint for octave", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 64;
    config.lowest_note_limit_enable = true;

    const float fsharp4_hz = dsp::refine_guitar_fundamental(185.0F, config, 66);
    REQUIRE(dsp::frequency_to_midi_note(fsharp4_hz) == 66);
}

TEST_CASE("quantize_midi_note applies semitone hysteresis", "[types]") {
    const float sharp_e4 =
        329.63F * std::pow(2.0F, 35.0F / 1200.0F);
    REQUIRE(dsp::quantize_midi_note(sharp_e4, 64) == 64);
}

TEST_CASE("quantize_midi_note does not snap toward distant hint", "[types]") {
    const float e3_slightly_sharp =
        164.81F * std::pow(2.0F, 40.0F / 1200.0F);
    REQUIRE(dsp::quantize_midi_note(e3_slightly_sharp, 53) == 52);
}

TEST_CASE("frequency_to_midi_note_with_bias shifts by cents", "[types]") {
    REQUIRE(dsp::frequency_to_midi_note_with_bias(440.0F, 440.0F, 0.0F) == 69);
    REQUIRE(dsp::frequency_to_midi_note_with_bias(440.0F, 440.0F, 100.0F) == 68);
}

TEST_CASE("frequency_to_midi_note_nearest_temperament prefers flatter note", "[types]") {
    const float sharp_c =
        130.81F * std::pow(2.0F, 45.0F / 1200.0F);
    REQUIRE(dsp::frequency_to_midi_note_nearest_temperament(sharp_c, 440.0F) == 48);
}

TEST_CASE("refine_guitar_fundamental folds subharmonics for high E string", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 64;
    config.lowest_note_limit_enable = true;

    const float e4_hz = dsp::refine_guitar_fundamental(164.0F, config);
    REQUIRE(dsp::frequency_to_midi_note(e4_hz) == 64);

    const float fsharp4_hz = dsp::refine_guitar_fundamental(185.0F, config);
    REQUIRE(dsp::frequency_to_midi_note(fsharp4_hz) == 66);
}

TEST_CASE("frequency_in_melody_course_band targets D#4 not 290 Hz", "[types]") {
    REQUIRE_FALSE(dsp::frequency_in_melody_course_band(290.0F));
    REQUIRE(dsp::frequency_in_melody_course_band(312.0F));
}

TEST_CASE("note_on_velocity_rms uses block peak", "[types]") {
    REQUIRE(dsp::note_on_velocity_rms(0.05F, 0.02F) == 0.05F);
    REQUIRE(dsp::note_on_velocity_rms(0.02F, 0.04F) == 0.04F);
}

TEST_CASE("prefer_melody_course_octave stabilises 156 Hz to 312 Hz", "[types]") {
    dsp::AnalyserConfig config{};
    const float corrected = dsp::prefer_melody_course_octave(156.0F, config);
    REQUIRE_THAT(static_cast<double>(corrected), WithinAbs(312.0, 2.0));
    REQUIRE(dsp::frequency_in_melody_course_band(corrected));
}

TEST_CASE("refine_guitar_fundamental doubles yinfast subharmonic near 156 Hz", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 40;

    const float up = dsp::refine_guitar_fundamental(156.0F, config, -1, true);
    REQUIRE(dsp::frequency_to_midi_note(up) == 63);
    REQUIRE_THAT(static_cast<double>(up), WithinAbs(312.0, 4.0));

    const float d3 = dsp::refine_guitar_fundamental(146.83F, config, -1, true);
    REQUIRE(dsp::frequency_to_midi_note(d3) == 50);

    const float f3 = dsp::refine_guitar_fundamental(174.61F, config, -1, true);
    REQUIRE(dsp::frequency_to_midi_note(f3) == 53);
}

TEST_CASE("refine_guitar_fundamental keeps F3 on G string for full range", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 40;

    const float f3_hz = dsp::refine_guitar_fundamental(174.61F, config);
    REQUIRE(dsp::frequency_to_midi_note(f3_hz) == 53);
}

TEST_CASE("refine_guitar_fundamental keeps low E open string", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 40;

    const float e2_hz = dsp::refine_guitar_fundamental(82.0F, config);
    REQUIRE(dsp::frequency_to_midi_note(e2_hz) == 40);
}

TEST_CASE("effective_min_note uses full guitar range when limit is off", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 64;
    config.lowest_note_limit_enable = false;
    REQUIRE(dsp::effective_min_note(config) == 40);
    REQUIRE(dsp::note_in_range(53, config));
    REQUIRE_FALSE(dsp::note_in_range(39, config));
}

TEST_CASE("frequency_is_analysis_artifact rejects hop bin but keeps guitar G3", "[types]") {
    constexpr double kRate = 48000.0;
    constexpr std::size_t kHop = 256;

    REQUIRE(dsp::frequency_is_analysis_artifact(187.5F, kRate, kHop, 0.3F));
    REQUIRE_FALSE(dsp::frequency_is_analysis_artifact(196.0F, kRate, kHop, 0.6F));
    REQUIRE_FALSE(dsp::frequency_is_analysis_artifact(174.61F, kRate, kHop, 0.55F));
    REQUIRE_FALSE(dsp::frequency_is_analysis_artifact(294.0F, kRate, kHop, 0.5F));
}

TEST_CASE("effective_min_note honours enabled floor", "[types]") {
    dsp::AnalyserConfig config{};
    config.min_note = 64;
    config.lowest_note_limit_enable = true;
    REQUIRE(dsp::effective_min_note(config) == 64);
    REQUIRE_FALSE(dsp::note_in_range(53, config));
}

TEST_CASE("frequency_to_midi_note respects custom A4 reference", "[types]") {
    const float hz = dsp::midi_note_to_frequency(69, 432.0F);
    REQUIRE(dsp::frequency_to_midi_note(hz, 432.0F) == 69);
    REQUIRE_THAT(dsp::frequency_to_continuous_midi_note(hz, 440.0F),
                 WithinAbs(68.68F, 0.05F));
}

TEST_CASE("clamp_reference_a4_hz limits tuning range", "[types]") {
    REQUIRE_THAT(dsp::clamp_reference_a4_hz(440.0F), WithinAbs(440.0F, 1e-5F));
    REQUIRE_THAT(dsp::clamp_reference_a4_hz(400.0F), WithinAbs(415.0F, 1e-5F));
    REQUIRE_THAT(dsp::clamp_reference_a4_hz(500.0F), WithinAbs(466.0F, 1e-5F));
}

TEST_CASE("clamped_sensitivity enforces minimum for Note Off", "[types]") {
    dsp::AnalyserConfig config{};
    config.sensitivity = 0.0F;
    REQUIRE_THAT(dsp::clamped_sensitivity(config), WithinAbs(0.55F, 1e-5F));
}

TEST_CASE("snap_note_off_delay_blocks quantises to 4, 8, or 16", "[types]") {
    REQUIRE(dsp::snap_note_off_delay_blocks(4.0F) == 4U);
    REQUIRE(dsp::snap_note_off_delay_blocks(5.0F) == 4U);
    REQUIRE(dsp::snap_note_off_delay_blocks(8.0F) == 8U);
    REQUIRE(dsp::snap_note_off_delay_blocks(10.0F) == 8U);
    REQUIRE(dsp::snap_note_off_delay_blocks(16.0F) == 16U);
    REQUIRE(dsp::snap_note_off_delay_blocks(15.0F) == 16U);
}

TEST_CASE("snap_pitch_method maps LV2 enumeration to PitchMethod", "[types]") {
    REQUIRE(dsp::snap_pitch_method(0.0F) == dsp::PitchMethod::Yinfast);
    REQUIRE(dsp::snap_pitch_method(0.4F) == dsp::PitchMethod::Yinfast);
    REQUIRE(dsp::snap_pitch_method(1.0F) == dsp::PitchMethod::Mcomb);
    REQUIRE(dsp::snap_pitch_method(0.6F) == dsp::PitchMethod::Mcomb);
}

TEST_CASE("pitch_method_aubio_name returns Aubio method strings", "[types]") {
    REQUIRE(std::strcmp(dsp::pitch_method_aubio_name(dsp::PitchMethod::Yinfast),
                        "yinfast") == 0);
    REQUIRE(std::strcmp(dsp::pitch_method_aubio_name(dsp::PitchMethod::Mcomb),
                        "mcomb") == 0);
}

TEST_CASE("effective_pitch_confidence substitutes RMS proxy for mcomb", "[types]") {
    dsp::AnalyserConfig config{};
    REQUIRE(dsp::pitch_method_has_aubio_confidence(dsp::PitchMethod::Yinfast));
    REQUIRE_FALSE(dsp::pitch_method_has_aubio_confidence(dsp::PitchMethod::Mcomb));

    REQUIRE_THAT(dsp::effective_pitch_confidence(dsp::PitchMethod::Yinfast, 0.8F,
                                                 440.0F, 0.01F, config),
                 WithinAbs(0.8F, 1e-5F));
    REQUIRE_THAT(dsp::effective_pitch_confidence(dsp::PitchMethod::Mcomb, 0.0F,
                                                 0.0F, 0.05F, config),
                 WithinAbs(0.0F, 1e-5F));
    const float mcomb_conf = dsp::effective_pitch_confidence(
        dsp::PitchMethod::Mcomb, 0.0F, 329.0F, 0.02F, config);
    REQUIRE(mcomb_conf >= config.note_on_sensitivity);
}

TEST_CASE("pitch_valid_for_tuner_display uses lower confidence than MIDI", "[types]") {
    dsp::AnalyserConfig config{};
    dsp::PitchEstimate pitch{};
    pitch.raw_frequency_hz = 261.63F;
    pitch.frequency_hz = 261.63F;
    pitch.confidence = 0.25F;
    pitch.is_valid = false;
    REQUIRE(dsp::pitch_valid_for_tuner_display(pitch, config));
    pitch.confidence = 0.10F;
    REQUIRE_FALSE(dsp::pitch_valid_for_tuner_display(pitch, config));
}

TEST_CASE("tuner_gui_readout matches modgui states", "[types]") {
    dsp::AnalyserConfig config{};
    dsp::PitchMonitor monitor{};
    monitor.input_rms = 0.0F;
    monitor.confidence = 0.0F;
    REQUIRE(dsp::tuner_gui_readout(monitor, config) == dsp::TunerReadout::Blank);

    monitor.input_rms = 0.0001F;
    REQUIRE(dsp::tuner_gui_readout(monitor, config) == dsp::TunerReadout::Pending);

    monitor.input_rms = 0.0F;
    monitor.detected_hz = 294.0F;
    monitor.midi_from_hz = 62;
    REQUIRE(dsp::tuner_gui_readout(monitor, config) == dsp::TunerReadout::Note);
}

TEST_CASE("tuner_shown_midi_note ignores note floor like the GUI", "[types]") {
    dsp::AnalyserConfig config{};
    config.lowest_note_limit_enable = true;
    config.min_note = 64;
    dsp::PitchMonitor monitor{};
    monitor.detected_hz = 294.0F;
    monitor.midi_from_hz = 62;
    REQUIRE(dsp::tuner_shown_midi_note(monitor, config) == 62);
    REQUIRE_FALSE(dsp::note_in_range(62, config));
}

TEST_CASE("AnalyserConfig defaults to yinfast", "[types]") {
    const dsp::AnalyserConfig config{};
    REQUIRE(config.pitch_method == dsp::PitchMethod::Yinfast);
}

TEST_CASE("NoteEvent default construction", "[types]") {
    const dsp::NoteEvent event{};
    REQUIRE(event.note == 0);
    REQUIRE_THAT(event.velocity, WithinAbs(0.0F, 1e-6F));
    REQUIRE_FALSE(event.on);
    REQUIRE(event.frame_offset == 0);
}
