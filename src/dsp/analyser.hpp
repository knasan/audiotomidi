#pragma once

#include "dsp/types.hpp"
#include "dsp/span.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace dsp {

struct AnalyserImpl;

/// Monophonic audio-to-note converter: Aubio pitch detection + note state machine.
///
/// This is the sole DSP entry point. It accepts mono audio and returns NoteEvent
/// PODs — the "language" between DSP and LV2. No LV2 or plugin headers appear here,
/// so the full pipeline is testable with Catch2 in isolation.
///
/// Design constraints (MOD Dwarf / real-time audio):
///   - prepare() allocates Aubio (yinfast + mcomb) + one hop buffer; no per-block heap use.
///   - process() never allocates; at most one Aubio hop per call when hop is full.
///   - Target SoC: ARM Cortex-A35 — avoid full yin (O(n²)), memmove FIFOs, or logging on the audio thread.
///
/// Lifecycle:
///   1. Construct (no allocation).
///   2. prepare() on activate / sample-rate change.
///   3. process() each audio block.
///   4. reset() on transport stop.
class Analyser {
public:
    Analyser();
    ~Analyser();

    Analyser(const Analyser&) = delete;
    Analyser& operator=(const Analyser&) = delete;
    Analyser(Analyser&&) noexcept;
    Analyser& operator=(Analyser&&) noexcept;

    /// Allocate Aubio state and internal buffers for the given stream geometry.
    ///
    /// Must run outside the audio thread (e.g. LV2 activate). Returns false when
    /// Aubio init fails or window/hop parameters are invalid.
    [[nodiscard]] bool prepare(double sample_rate,
                               std::size_t max_block_size,
                               AnalyserConfig config = {});

    /// Clear pitch detector and note state without freeing memory.
    void reset();

    /// Analyse one mono block and emit zero or more note events.
    ///
    /// Runs Aubio yinfast on hop-sized chunks, applies debounce/hysteresis, and fills
    /// an internal fixed buffer. The returned span is valid until the next
    /// process() or reset().
    ///
    /// @pre prepare() succeeded and samples.size() <= max_block_size.
    /// @note Real-time safe: no heap allocation.
    [[nodiscard]] dsp::span<const NoteEvent> process(
        dsp::span<const float> samples);

    /// Last applied configuration (hop/window, thresholds, note range).
    [[nodiscard]] const AnalyserConfig& config() const noexcept;

    /// Update runtime parameters without reallocation (safe on audio thread).
    void set_config(const AnalyserConfig& config) noexcept;

    /// Most recent pitch estimate after the last process() call.
    [[nodiscard]] PitchEstimate last_pitch() const noexcept;

    /// Tuner snapshot updated at the end of each process() — safe to read from LV2 run().
    [[nodiscard]] PitchMonitor pitch_monitor() const noexcept;

    /// True while a Note On has been sent without a matching Note Off.
    [[nodiscard]] bool is_note_active() const noexcept;

    /// Active MIDI note number, or -1 when idle.
    [[nodiscard]] int active_note_number() const noexcept;

    /// Drop internal note state without emitting MIDI (host sends Note Off separately).
    void abandon_active_note() noexcept;

private:
    struct ActiveNote {
        int note = 0;
        float velocity = 0.0F;
    };

    void apply_pitch_to_note_machine(const PitchEstimate& pitch,
                                     std::size_t block_size, float block_rms);

    void force_note_off(std::size_t block_size);

    void update_note_off_delay(float block_rms, std::size_t block_size);

    void emit_note(int note, float velocity, bool on, std::uint32_t frame_offset,
                   int pitch_bend_wheel = 0);

    void emit_pitch_bend(int wheel, std::uint32_t frame_offset);

    void maybe_emit_continuous_pitch_bend(const PitchEstimate& pitch,
                                          std::uint32_t frame_offset);

    void maybe_rearticulate_on_pick(float block_rms, std::size_t block_size);

    void run_pitch_hop();

    void accumulate_samples(dsp::span<const float> samples,
                            std::size_t block_size, float block_rms,
                            std::size_t& hops_done);

    /// Rebuild @ref pitch_monitor_ from @ref last_pitch_ and note state (RT-safe).
    void update_pitch_monitor() noexcept;

    std::unique_ptr<AnalyserImpl> impl_;

    AnalyserConfig config_{};
    std::optional<ActiveNote> active_note_;
    std::uint32_t note_on_counter_ = 0;
    std::uint32_t note_off_counter_ = 0;

    /// Last pitch bend sent; suppresses redundant PitchBend events.
    int last_sent_pitch_bend_wheel_ = 0;

    bool attack_phase_ = false;

    std::array<NoteEvent, kMaxNoteEventsPerBlock> pending_{};
    std::size_t pending_count_ = 0;

    PitchEstimate last_pitch_{};
    PitchMonitor pitch_monitor_{};
};

}  // namespace dsp
