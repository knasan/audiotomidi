#pragma once

#include "dsp/spscRingBuffer.hpp"
#include "dsp/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace dsp {

/// Kind of debug record written by the audio thread.
enum class DebugLogKind : std::uint8_t {
    Pitch = 0,
    Note = 1,
};

/// POD log record transferred lock-free from audio thread to logging thread.
struct DebugLogEntry {
    DebugLogKind kind = DebugLogKind::Pitch;

    float frequency_hz = 0.0F;
    float confidence = 0.0F;
    float rms = 0.0F;
    int midi_note = 0;
    float pitch_bend_cents = 0.0F;
    int pitch_bend_wheel = 0;
    bool pitch_valid = false;

    int note = 0;
    float velocity = 0.0F;
    bool note_on = false;
    std::uint32_t frame_offset = 0;
};

inline constexpr std::size_t kDebugLogRingCapacity = 4096;

/// Asynchronous debug logger for WebUI / local development.
///
/// The audio thread calls logPitch() / logNote(), which only try_push() into a
/// lock-free SPSC ring buffer — no mutex, no I/O, no allocation. A dedicated
/// background thread drains the queue and writes to stdout and an optional file.
///
/// Lifecycle (non-real-time):
///   start()  — spawn consumer thread (activate / instantiate)
///   stop()   — flush and join (deactivate / cleanup)
class Logger {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// Start the consumer thread. Safe to call once per plugin activation.
    ///
    /// @param log_file_path Optional path; empty uses stdout only.
    /// @return false if already running or thread creation fails.
    bool start(const std::string& log_file_path = {});

    /// Stop the consumer and flush remaining entries.
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// Queue a pitch snapshot (called from audio thread each block).
    ///
    /// Drops silently when the ring buffer is full; increments dropped_count().
    void log_pitch(const PitchEstimate& pitch) noexcept;

    /// Queue a note event with associated pitch context (audio thread).
    void log_note(const NoteEvent& event, const PitchEstimate& pitch) noexcept;

    /// Number of records dropped because the ring buffer was full.
    [[nodiscard]] std::uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void consumer_loop() noexcept;
    void write_entry(const DebugLogEntry& entry, std::ofstream& file);
    bool try_enqueue(const DebugLogEntry& entry) noexcept;

    SpscRingBuffer<DebugLogEntry, kDebugLogRingCapacity> queue_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::thread consumer_;
    std::string log_file_path_;
};

}  // namespace dsp
