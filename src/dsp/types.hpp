#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dsp {

/// Reference tuning: A4 = 440 Hz (default when port is not customised).
inline constexpr float kReferenceA4Hz = 440.0F;

/// Full-guitar MIDI floor when the optional lowest-note limit is off (E2).
inline constexpr int kFullGuitarMinNote = 40;

/// Maximum MIDI events emitted per process() call (fixed buffer, RT-safe).
inline constexpr std::size_t kMaxNoteEventsPerBlock = 16;

/// Kind of MIDI-oriented event in the DSP → LV2 protocol.
enum class MidiEventKind : std::uint8_t {
    /// Channel voice Note On / Note Off.
    Note = 0,
    /// 14-bit pitch bend wheel (-8192 .. 8191, centre = 0).
    PitchBend = 1,
};

/// Communication protocol between DSP and LV2 layers — plain POD, no LV2 types.
struct NoteEvent {
    MidiEventKind kind = MidiEventKind::Note;

    /// MIDI note number (0–127) for Note events.
    int note = 0;

    /// Note velocity for Note On (0.0–127.0); ignored for Note Off.
    float velocity = 0.0F;

    /// true = Note On, false = Note Off (only when kind == Note).
    bool on = false;

    /// Pitch bend wheel value for PitchBend events (-8192 .. 8191, centre = 0).
    int pitch_bend_wheel = 0;

    /// Sample index within the current process() block (0 .. block_size - 1).
    std::uint32_t frame_offset = 0;
};

/// Live tuner readout — Hz, temperament note, and MIDI pipeline stages (debug / MOD GUI).
struct PitchMonitor {
    /// Refined fundamental after guitar folding (same as @ref PitchEstimate::frequency_hz).
    float detected_hz = 0.0F;

    /// Aubio output before folding (for comparing raw vs refined).
    float raw_hz = 0.0F;

    /// Deviation from nearest equal-tempered semitone (+ = sharp, − = flat).
    float cents = 0.0F;

    /// Aubio confidence for the current hop (0–1).
    float confidence = 0.0F;

    /// RMS of the current audio block — proves signal reaches the plugin (MOD GUI).
    float input_rms = 0.0F;

    /// Nearest MIDI note from @p detected_hz only (no bias / transpose) — external tuner view.
    int midi_from_hz = 0;

    /// Quantised candidate after bias, median, and transpose — what the analyser would send.
    int midi_detected = 0;

    /// Currently held MIDI note (0 when idle).
    int midi_active = 0;

    bool pitch_valid = false;
    bool note_active = false;
};

/// Internal pitch estimate for one analysis hop (optional debug/monitoring).
struct PitchEstimate {
    /// Refined fundamental used for MIDI / monitoring (updated after hint apply).
    float frequency_hz = 0.0F;

    /// Raw Aubio output before guitar-range folding (for single-pass refine).
    float raw_frequency_hz = 0.0F;

    float confidence = 0.0F;
    float rms = 0.0F;
    bool is_valid = false;
};

/// Aubio pitch detector selection (both are pre-allocated at prepare() for RT-safe switching).
enum class PitchMethod : std::uint8_t {
  /// Fast YIN — good for bass and low fundamentals (light CPU on MOD Dwarf).
  Yinfast = 0,
  /// Multi-comb filter — often more stable on guitar harmonics.
  Mcomb = 1,
};

/// Aubio method string for @ref new_aubio_pitch.
[[nodiscard]] inline const char* pitch_method_aubio_name(
    const PitchMethod method) noexcept {
  switch (method) {
    case PitchMethod::Mcomb:
      return "mcomb";
    case PitchMethod::Yinfast:
    default:
      return "yinfast";
  }
}

/// Snap LV2 enumerated control to @ref PitchMethod.
[[nodiscard]] inline PitchMethod snap_pitch_method(const float value) noexcept {
  const int rounded = static_cast<int>(std::lround(value));
  return rounded >= 1 ? PitchMethod::Mcomb : PitchMethod::Yinfast;
}

/// Whether Aubio exposes a meaningful @c aubio_pitch_get_confidence for this method.
///
/// mcomb has @c conf_cb == NULL in Aubio — get_confidence always returns 0.
[[nodiscard]] inline bool pitch_method_has_aubio_confidence(
    const PitchMethod method) noexcept {
  return method == PitchMethod::Yinfast;
}

/// Configuration for monophonic guitar pitch-to-note conversion.
///
/// Primary deployment target: MOD Dwarf (ARM Cortex-A35 @ 1.3 GHz, 48 kHz, 128-frame JACK).
/// MOD Desktop uses the same LV2 path — use it for functional tests; validate CPU on ARM before release.
///
/// Pitch: yinfast (~50 µs/hop) or mcomb (~heavier, better on guitar). One hop per 256 samples
/// at 48 kHz when block size ≤ 128 (MOD Dwarf).
struct AnalyserConfig {
    std::size_t hop_size = 256;
    std::size_t window_size = 2048;

    /// Aubio pitch algorithm — switchable at runtime (no allocation on audio thread).
    PitchMethod pitch_method = PitchMethod::Yinfast;

    /// Confidence threshold while a note is held.
    float sensitivity = 0.7F;

    /// Lower bar for the first Note On after an attack (guitar transient).
    float note_on_sensitivity = 0.55F;

    /// Hysteresis: note-off when confidence drops below sensitivity - delta.
    float note_off_hysteresis = 0.15F;

    int min_note = 40;
    int max_note = 88;

    /// When false, accept the full guitar range (E2 upward). When true, use @p min_note.
    bool lowest_note_limit_enable = false;

    /// A4 reference for Hz ↔ MIDI mapping (Kammerton / Stimmfrequenz).
    float reference_a4_hz = kReferenceA4Hz;

    /// Semitone offset applied after pitch quantise (calibration / transposition).
    int midi_transpose = 0;

    /// Cent bias subtracted before MIDI quantise (negative = flatter notes).
    float pitch_bias_cents = 0.0F;

    /// When true, send continuous MIDI pitch bend (vibrato / intonation). Off = semitone quantise only.
    bool pitch_bend_enable = false;

    std::uint32_t note_on_debounce_frames = 1;

    /// Audio blocks without detected pitch before MIDI Note Off (4/8/16).
    std::uint32_t note_off_delay_blocks = 8;

    float silence_rms_threshold = 0.0005F;
    float min_frequency_hz = 80.0F;
    float max_frequency_hz = 1200.0F;

    float default_velocity = 70.0F;

    float velocity_rms_min = 0.003F;
    float velocity_rms_max = 0.04F;
};

/// Confidence used by the note state machine (yinfast: Aubio; mcomb: hop RMS proxy).
[[nodiscard]] inline float effective_pitch_confidence(
    const PitchMethod method, const float aubio_confidence,
    const float raw_frequency_hz, const float hop_rms,
    const AnalyserConfig& config) noexcept {
  if (pitch_method_has_aubio_confidence(method)) {
    return aubio_confidence;
  }
  if (raw_frequency_hz <= 0.0F) {
    return 0.0F;
  }
  const float floor =
      std::max(config.velocity_rms_min, config.silence_rms_threshold);
  const float level = hop_rms / floor;
  return std::clamp(0.55F + 0.25F * std::min(level, 2.0F), 0.55F, 1.0F);
}

/// Snap LV2 control to allowed Note Off delay steps (blocks).
[[nodiscard]] inline std::uint32_t snap_note_off_delay_blocks(
    const float value) noexcept {
    const int rounded = static_cast<int>(std::lround(value));
    if (rounded <= 6) {
        return 4U;
    }
    if (rounded <= 12) {
        return 8U;
    }
    return 16U;
}

/// Effective MIDI floor after applying the optional lowest-note limit.
[[nodiscard]] inline int effective_min_note(
    const AnalyserConfig& config) noexcept {
    if (!config.lowest_note_limit_enable) {
        return kFullGuitarMinNote;
    }
    return config.min_note;
}

/// Hz for an equal-tempered MIDI note at the given A4 reference.
[[nodiscard]] inline float midi_note_to_frequency(
    const int note, const float reference_a4_hz = kReferenceA4Hz) noexcept {
    return reference_a4_hz *
           std::pow(2.0F, (static_cast<float>(note) - 69.0F) / 12.0F);
}

/// Continuous MIDI note number (float) before rounding to semitone.
///
/// Formula: n = 69 + 12 * log2(f / A4). Example: 440 Hz → 69.0 (A4 @ 440 Hz ref).
[[nodiscard]] inline float frequency_to_continuous_midi_note(
    const float frequency_hz,
    const float reference_a4_hz = kReferenceA4Hz) noexcept {
    if (frequency_hz <= 0.0F || reference_a4_hz <= 0.0F) {
        return 0.0F;
    }
    return 12.0F * std::log2(frequency_hz / reference_a4_hz) + 69.0F;
}

/// Pick MIDI note by closest equal-tempered target frequency (reduces sharp rounding).
[[nodiscard]] inline int frequency_to_midi_note_nearest_temperament(
    const float frequency_hz, const float reference_a4_hz = kReferenceA4Hz) noexcept {
    if (frequency_hz <= 0.0F) {
        return 0;
    }
    const float continuous =
        frequency_to_continuous_midi_note(frequency_hz, reference_a4_hz);
    const int low = static_cast<int>(std::floor(continuous));
    const int high = low + 1;
    const float err_low =
        std::abs(frequency_hz - midi_note_to_frequency(low, reference_a4_hz));
    const float err_high =
        std::abs(frequency_hz - midi_note_to_frequency(high, reference_a4_hz));
    return err_low <= err_high ? low : high;
}

/// Quantise Hz to MIDI with optional cent bias and hint (see quantize_midi_note).
[[nodiscard]] inline int frequency_to_midi_note_with_bias(
    const float frequency_hz, const float reference_a4_hz,
    const float pitch_bias_cents, const int hint_note = -1) noexcept {
    if (frequency_hz <= 0.0F || reference_a4_hz <= 0.0F) {
        return 0;
    }
    const float adjusted_hz =
        frequency_hz / std::pow(2.0F, pitch_bias_cents / 1200.0F);
    const int nearest =
        frequency_to_midi_note_nearest_temperament(adjusted_hz, reference_a4_hz);
    if (hint_note >= 0 && std::abs(nearest - hint_note) == 1) {
        const float continuous =
            frequency_to_continuous_midi_note(adjusted_hz, reference_a4_hz);
        const float dist_nearest =
            std::abs(continuous - static_cast<float>(nearest));
        const float dist_hint =
            std::abs(continuous - static_cast<float>(hint_note));
        if (dist_hint < dist_nearest) {
            return hint_note;
        }
    }
    return nearest;
}

/// Convert Hz to nearest MIDI note (integer 0–127).
[[nodiscard]] inline int frequency_to_midi_note(
    const float frequency_hz,
    const float reference_a4_hz = kReferenceA4Hz) noexcept {
    return static_cast<int>(
        std::lround(frequency_to_continuous_midi_note(frequency_hz, reference_a4_hz)));
}

/// Quantise Hz to MIDI; optional @p hint_note keeps the nearer semitone at boundaries.
[[nodiscard]] inline int quantize_midi_note(const float frequency_hz,
                                            const int hint_note = -1,
                                            const float reference_a4_hz =
                                                kReferenceA4Hz) noexcept {
    if (frequency_hz <= 0.0F) {
        return 0;
    }
    const float continuous =
        frequency_to_continuous_midi_note(frequency_hz, reference_a4_hz);
    const int rounded = static_cast<int>(std::lround(continuous));
    if (hint_note >= 0 && std::abs(rounded - hint_note) == 1) {
        const float dist_rounded =
            std::abs(continuous - static_cast<float>(rounded));
        const float dist_hint =
            std::abs(continuous - static_cast<float>(hint_note));
        if (dist_hint < dist_rounded) {
            return hint_note;
        }
    }
    return rounded;
}

/// Pick the octave of @p frequency_hz closest to @p hint_note (in MIDI semitones).
[[nodiscard]] inline float pick_octave_near_hint(
    float frequency_hz, const int hint_note,
    const AnalyserConfig& config) noexcept {
    if (frequency_hz <= 0.0F || hint_note < 0) {
        return frequency_hz;
    }
    float best_hz = frequency_hz;
    const float ref = config.reference_a4_hz;
    float best_error =
        std::abs(frequency_to_continuous_midi_note(frequency_hz, ref) -
                 static_cast<float>(hint_note));
    constexpr float kMultipliers[] = {0.25F, 0.5F, 1.0F, 2.0F, 4.0F};
    for (const float mult : kMultipliers) {
        const float candidate_hz = frequency_hz * mult;
        if (candidate_hz < config.min_frequency_hz * 0.9F ||
            candidate_hz > config.max_frequency_hz * 1.05F) {
            continue;
        }
        const float error =
            std::abs(frequency_to_continuous_midi_note(candidate_hz, ref) -
                     static_cast<float>(hint_note));
        if (error < best_error) {
            best_error = error;
            best_hz = candidate_hz;
        }
    }
    return best_hz;
}

/// Pitch deviation from the nearest semitone, in cents (±50 cents max per semitone).
///
/// Positive = sharp, negative = flat. Used for logging and optional Pitch Bend.
[[nodiscard]] inline float frequency_to_pitch_bend_cents(
    const float frequency_hz,
    const float reference_a4_hz = kReferenceA4Hz) noexcept {
    if (frequency_hz <= 0.0F) {
        return 0.0F;
    }
    const float continuous =
        frequency_to_continuous_midi_note(frequency_hz, reference_a4_hz);
    const float rounded = std::lround(continuous);
    return (continuous - rounded) * 100.0F;
}

/// 14-bit MIDI pitch bend wheel value (-8192 .. 8191), centre = 0.
///
/// Maps one semitone of deviation across the full bend range.
[[nodiscard]] inline int frequency_to_pitch_bend_wheel(
    const float frequency_hz,
    const float reference_a4_hz = kReferenceA4Hz) noexcept {
    if (frequency_hz <= 0.0F) {
        return 0;
    }
    const float continuous =
        frequency_to_continuous_midi_note(frequency_hz, reference_a4_hz);
    const float semitone_deviation = continuous - std::lround(continuous);
    const float wheel = semitone_deviation * 4096.0F;
    return static_cast<int>(std::clamp(wheel, -8192.0F, 8191.0F));
}

/// True when @p note lies within the configured MIDI output window.
[[nodiscard]] inline bool note_in_range(const int note,
                                        const AnalyserConfig& config) noexcept {
    return note >= effective_min_note(config) && note <= config.max_note;
}

/// Clamp note to configured range (legacy helper — prefer note_in_range for Note On).
[[nodiscard]] inline int clamp_note(const int note,
                                    const AnalyserConfig& config) noexcept {
    const int min_note = effective_min_note(config);
    if (note < min_note) {
        return min_note;
    }
    if (note > config.max_note) {
        return config.max_note;
    }
    return note;
}

/// Map input RMS to a restrained MIDI velocity (avoids harsh ePiano levels).
[[nodiscard]] inline float rms_to_velocity(const float rms,
                                           const AnalyserConfig& config) noexcept {
    if (rms <= config.velocity_rms_min) {
        return config.default_velocity;
    }
    const float span = config.velocity_rms_max - config.velocity_rms_min;
    if (span <= 0.0F) {
        return config.default_velocity;
    }
    const float normalized =
        std::clamp((rms - config.velocity_rms_min) / span, 0.0F, 1.0F);
    constexpr float kMinVelocity = 28.0F;
    constexpr float kMaxVelocity = 65.0F;
    return kMinVelocity + normalized * (kMaxVelocity - kMinVelocity);
}

/// Note On velocity: use the louder of block vs hop RMS (pick transient lives in the block).
[[nodiscard]] inline float note_on_velocity_rms(const float block_rms,
                                              const float hop_rms) noexcept {
    return std::max(block_rms, hop_rms);
}

/// Fold detected harmonics down into the configured guitar fundamental range.
[[nodiscard]] inline float fold_frequency_to_guitar_range(
    float frequency_hz, const AnalyserConfig& config) noexcept {
    if (frequency_hz <= 0.0F) {
        return frequency_hz;
    }
    constexpr int kMaxOctaveFolds = 4;
    for (int i = 0; i < kMaxOctaveFolds &&
         frequency_hz > config.max_frequency_hz * 1.05F;
         ++i) {
        frequency_hz *= 0.5F;
    }
    for (int i = 0; i < kMaxOctaveFolds &&
         frequency_hz < config.min_frequency_hz * 0.95F;
         ++i) {
        frequency_hz *= 2.0F;
    }
    return frequency_hz;
}

/// Reject Aubio false positives at sample_rate / hop_size (typically ~187 Hz @ 48 kHz).
///
/// Real guitar fundamentals near that bin (G3 open ~196 Hz, wound strings) must not
/// be zeroed — only reject when confidence is weak.
[[nodiscard]] inline bool frequency_is_analysis_artifact(
    const float frequency_hz, const double sample_rate,
    const std::size_t hop_size,
    const float confidence = 0.0F) noexcept {
    if (frequency_hz <= 0.0F || hop_size == 0) {
        return false;
    }
    const float ratio =
        static_cast<float>(sample_rate / static_cast<double>(hop_size));
    if (std::abs(frequency_hz - ratio) >= ratio * 0.04F) {
        return false;
    }
    constexpr float kGuitarArtifactLoHz = 160.0F;
    constexpr float kGuitarArtifactHiHz = 215.0F;
    constexpr float kGuitarArtifactMinConfidence = 0.48F;
    if (frequency_hz >= kGuitarArtifactLoHz &&
        frequency_hz <= kGuitarArtifactHiHz &&
        confidence >= kGuitarArtifactMinConfidence) {
        return false;
    }
    return true;
}

/// Prefer guitar fundamentals over detected subharmonics (common on the high E string).
///
/// When @p prefer_upper_octave is true (attack / note-on), yinfast readings at half the
/// true fundamental (130–280 Hz → 260–560 Hz) are shifted up one octave.
[[nodiscard]] inline float refine_guitar_fundamental(
    float frequency_hz, const AnalyserConfig& config,
    const int hint_note = -1, const bool prefer_upper_octave = false) noexcept {
    frequency_hz = fold_frequency_to_guitar_range(frequency_hz, config);

    if (hint_note >= 0) {
        const int naive_note =
            frequency_to_midi_note(frequency_hz, config.reference_a4_hz);
        if (std::abs(naive_note - hint_note) <= 1) {
            return pick_octave_near_hint(frequency_hz, hint_note, config);
        }
    }

    const float target_min_hz =
        midi_note_to_frequency(effective_min_note(config), config.reference_a4_hz) *
        0.92F;
    for (int i = 0; i < 4; ++i) {
        if (frequency_hz >= target_min_hz) {
            break;
        }
        const float doubled = frequency_hz * 2.0F;
        if (doubled > config.max_frequency_hz * 1.05F) {
            break;
        }
        frequency_hz = doubled;
    }

    if (prefer_upper_octave && hint_note < 0 && frequency_hz >= 130.0F &&
        frequency_hz < 280.0F) {
        const float doubled = frequency_hz * 2.0F;
        if (doubled <= config.max_frequency_hz * 1.05F) {
            const int note_low =
                frequency_to_midi_note(frequency_hz, config.reference_a4_hz);
            const int note_high =
                frequency_to_midi_note(doubled, config.reference_a4_hz);
            // yinfast half-reads ~156 Hz for ~312 Hz frets (D#4). Keep D3 (146 Hz) intact.
            const bool melody_octave_up =
                note_high - note_low == 12 && note_low >= 50 && note_low <= 51 &&
                doubled >= 300.0F && doubled <= 330.0F;
            if (melody_octave_up) {
                frequency_hz = doubled;
            }
        }
    }

    return frequency_hz;
}

/// Correct folded Hz when yinfast alternates between f and f/2 around ~312 Hz.
[[nodiscard]] inline float prefer_melody_course_octave(
    float frequency_hz, const AnalyserConfig& config) noexcept {
    if (frequency_hz >= 300.0F && frequency_hz <= 330.0F) {
        return frequency_hz;
    }
    if (frequency_hz >= 148.0F && frequency_hz <= 166.0F) {
        const float doubled = frequency_hz * 2.0F;
        if (doubled >= 300.0F && doubled <= 330.0F) {
            const int note_low =
                frequency_to_midi_note(frequency_hz, config.reference_a4_hz);
            const int note_high =
                frequency_to_midi_note(doubled, config.reference_a4_hz);
            if (note_high - note_low == 12 && note_low >= 50 && note_low <= 51) {
                return doubled;
            }
        }
    }
    return frequency_hz;
}

/// D#4–F4 melody course where yinfast half-reads are common (~300–355 Hz).
[[nodiscard]] inline bool frequency_in_melody_course_band(
    const float frequency_hz) noexcept {
    return frequency_hz >= 300.0F && frequency_hz <= 355.0F;
}

/// Slightly lower confidence bar for melody-course fundamentals (yinfast is weaker there).
[[nodiscard]] inline float effective_note_on_confidence(
    const AnalyserConfig& config, const float folded_hz) noexcept {
    if (frequency_in_melody_course_band(folded_hz)) {
        return std::max(0.42F, config.note_on_sensitivity - 0.08F);
    }
    return config.note_on_sensitivity;
}

/// Minimum Aubio confidence for the MOD tuner display (lower than MIDI Note On).
inline constexpr float kTunerDisplayMinConfidence = 0.18F;

/// RMS floor matching @c hasAudioSignal() in modgui/tuner.js.
inline constexpr float kTunerMinAudioRms = 0.00008F;

/// Hz written to the @c tuner_hz port (shared by LV2 outputs and MIDI sync).
[[nodiscard]] inline float tuner_output_hz(
    const PitchMonitor& monitor) noexcept {
    if (monitor.pitch_valid && monitor.detected_hz > 0.0F) {
        return monitor.detected_hz;
    }
    if (monitor.detected_hz > 0.0F) {
        return monitor.detected_hz;
    }
    if (monitor.raw_hz > 0.0F) {
        return monitor.raw_hz;
    }
    return 0.0F;
}

/// Large tuner readout states — mirrors @c updateDisplay() in modgui/tuner.js.
enum class TunerReadout : std::uint8_t {
    Blank,    ///< "--" (no Hz, no analysis activity)
    Pending,  ///< "…" (Hz not yet stable, still listening)
    Note,     ///< Named note from @c noteFromFrequency()
};

/// True while the GUI would show "…" instead of a note or "--".
[[nodiscard]] inline bool tuner_display_pending(
    const PitchMonitor& monitor) noexcept {
    return monitor.confidence >= kTunerDisplayMinConfidence ||
           monitor.input_rms >= kTunerMinAudioRms;
}

/// MIDI note shown in the large tuner readout (@c noteFromFrequency in tuner.js).
[[nodiscard]] inline int tuner_gui_midi_note(
    const PitchMonitor& monitor, const AnalyserConfig& config) noexcept {
    const float hz = tuner_output_hz(monitor);
    if (hz <= 0.0F) {
        return 0;
    }
    const int note =
        frequency_to_midi_note_nearest_temperament(hz, config.reference_a4_hz);
    if (note < 0 || note > 127) {
        return 0;
    }
    return note;
}

/// Current tuner readout (must match modgui/tuner.js @c updateDisplay()).
[[nodiscard]] inline TunerReadout tuner_gui_readout(
    const PitchMonitor& monitor, const AnalyserConfig& config) noexcept {
    if (tuner_gui_midi_note(monitor, config) > 0) {
        return TunerReadout::Note;
    }
    return tuner_display_pending(monitor) ? TunerReadout::Pending
                                          : TunerReadout::Blank;
}

/// @deprecated Use @ref tuner_output_hz — kept for existing call sites.
[[nodiscard]] inline float tuner_monitor_display_hz(
    const PitchMonitor& monitor) noexcept {
    return tuner_output_hz(monitor);
}

/// @deprecated Use @ref tuner_gui_readout.
[[nodiscard]] inline bool tuner_display_shows_blank(
    const PitchMonitor& monitor, const AnalyserConfig& config) noexcept {
    return tuner_gui_readout(monitor, config) == TunerReadout::Blank;
}

/// Best available Hz for the tuner readout (refined or raw Aubio).
[[nodiscard]] inline float tuner_display_hz(const PitchEstimate& pitch) noexcept {
    if (pitch.frequency_hz > 0.0F) {
        return pitch.frequency_hz;
    }
    return pitch.raw_frequency_hz;
}

/// Tuner GUI — show pitch like an external strobe tuner, not gated on MIDI Note On.
[[nodiscard]] inline bool pitch_valid_for_tuner_display(
    const PitchEstimate& pitch, const AnalyserConfig& config) noexcept {
    const float hz = tuner_display_hz(pitch);
    return hz >= config.min_frequency_hz && hz <= config.max_frequency_hz &&
           pitch.confidence >= kTunerDisplayMinConfidence;
}

/// MIDI note matching the MOD tuner readout (alias for @ref tuner_gui_midi_note).
[[nodiscard]] inline int tuner_shown_midi_note(
    const PitchMonitor& monitor, const AnalyserConfig& config,
    const int /*active_note*/ = -1) noexcept {
    return tuner_gui_midi_note(monitor, config);
}

/// Persistent state for @ref tuner_midi_sync_events (RT-safe POD).
struct TunerMidiSyncState {
    int active_note = -1;
    std::uint32_t blank_blocks = 0;
    TunerReadout last_readout = TunerReadout::Blank;
    int pending_note = -1;
    std::uint32_t pending_blocks = 0;
};

/// Build Note On/Off events mirroring the MOD tuner display (same rules as tuner.js).
///
/// @p hop_rms Aubio hop RMS for velocity (falls back to @p block_rms when <= 0).
/// @return Number of events written to @p out (at most @p capacity).
inline std::size_t tuner_midi_sync_events(
    const PitchMonitor& monitor, const AnalyserConfig& config,
    TunerMidiSyncState& state, const std::size_t block_size,
    const float block_rms, const float hop_rms, NoteEvent* const out,
    const std::size_t capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }

    std::size_t count = 0;
    const std::uint32_t end_frame =
        block_size > 0 ? static_cast<std::uint32_t>(block_size - 1) : 0U;
    const TunerReadout readout = tuner_gui_readout(monitor, config);
    const int note = tuner_gui_midi_note(monitor, config);
    const TunerReadout previous = state.last_readout;
    state.last_readout = readout;

    const auto push_note = [&](const int midi_note, const float velocity,
                               const bool on, const std::uint32_t frame) {
        if (count >= capacity) {
            return;
        }
        out[count++] = NoteEvent{MidiEventKind::Note, midi_note, velocity, on,
                                 0, frame};
    };

    switch (readout) {
    case TunerReadout::Blank:
        state.pending_note = -1;
        state.pending_blocks = 0;
        if (state.active_note >= 0) {
            ++state.blank_blocks;
            if (state.blank_blocks >= config.note_off_delay_blocks) {
                push_note(state.active_note, 0.0F, false, end_frame);
                state.active_note = -1;
                state.blank_blocks = 0;
            }
        }
        return count;

    case TunerReadout::Pending:
        return count;

    case TunerReadout::Note:
        break;
    }

    state.blank_blocks = 0;
    if (note <= 0) {
        return count;
    }

    if (note == state.pending_note) {
        ++state.pending_blocks;
    } else {
        state.pending_note = note;
        state.pending_blocks = 1U;
    }

    const bool should_emit = previous != TunerReadout::Note ||
                             state.active_note < 0 ||
                             state.active_note != note;
    if (!should_emit || state.pending_blocks < 1U) {
        return count;
    }

    const bool had_active = state.active_note >= 0;
    if (had_active && state.active_note != note) {
        push_note(state.active_note, 0.0F, false, end_frame);
    }

    const float velocity_rms =
        hop_rms > 0.0F ? note_on_velocity_rms(block_rms, hop_rms) : block_rms;
    const float velocity = rms_to_velocity(velocity_rms, config);
    push_note(note, velocity, true, had_active ? end_frame : 0U);
    state.active_note = note;
    return count;
}

/// True when @p a and @p b are roughly an octave apart in MIDI semitones.
[[nodiscard]] inline bool midi_notes_one_octave_apart(const int a,
                                                      const int b) noexcept {
    const int delta = std::abs(a - b);
    return delta >= 11 && delta <= 13;
}

/// Lowest allowed hold confidence — below this Note Off never triggers.
inline constexpr float kMinSensitivity = 0.55F;

[[nodiscard]] inline float clamped_sensitivity(
    const AnalyserConfig& config) noexcept {
    return std::max(config.sensitivity, kMinSensitivity);
}

/// Note-off confidence threshold derived from sensitivity and hysteresis.
[[nodiscard]] inline float note_off_threshold(
    const AnalyserConfig& config) noexcept {
    const float threshold =
        clamped_sensitivity(config) - config.note_off_hysteresis;
    return threshold > 0.0F ? threshold : 0.0F;
}

/// Clamp A4 reference from the LV2 control port (415–466 Hz).
[[nodiscard]] inline float clamp_reference_a4_hz(const float value) noexcept {
    return std::clamp(value, 415.0F, 466.0F);
}

/// Build 3-byte MIDI Pitch Bend message (status 0xE0 | channel).
inline void pitch_bend_wheel_to_midi_bytes(const int pitch_bend_wheel,
                                           const std::uint8_t midi_channel,
                                           std::uint8_t out[3]) noexcept {
    const int centered = std::clamp(pitch_bend_wheel + 8192, 0, 16383);
    out[0] = static_cast<std::uint8_t>(0xE0 | (midi_channel & 0x0F));
    out[1] = static_cast<std::uint8_t>(centered & 0x7F);
    out[2] = static_cast<std::uint8_t>((centered >> 7) & 0x7F);
}

}  // namespace dsp
