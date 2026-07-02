#include "dsp/logger.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

namespace dsp {

namespace {

void format_note_name(const int midi_note, char* buffer,
                      const std::size_t size) noexcept {
    static constexpr const char* kNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi_note < 0 || midi_note > 127) {
        std::snprintf(buffer, size, "?");
        return;
    }
    std::snprintf(buffer, size, "%s%d", kNames[midi_note % 12],
                  (midi_note / 12) - 1);
}

}  // namespace

Logger::~Logger() {
    stop();
}

bool Logger::start(const std::string& log_file_path) {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }

    log_file_path_ = log_file_path;
    dropped_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        consumer_ = std::thread([this]() { consumer_loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void Logger::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (consumer_.joinable()) {
        consumer_.join();
    }
}

bool Logger::try_enqueue(const DebugLogEntry& entry) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (queue_.try_push(entry)) {
        return true;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Logger::log_pitch(const PitchEstimate& pitch) noexcept {
    DebugLogEntry entry{};
    entry.kind = DebugLogKind::Pitch;
    entry.frequency_hz = pitch.frequency_hz;
    entry.confidence = pitch.confidence;
    entry.rms = pitch.rms;
    entry.pitch_valid = pitch.is_valid;

    if (pitch.is_valid && pitch.frequency_hz > 0.0F) {
        entry.midi_note = frequency_to_midi_note(pitch.frequency_hz);
        entry.pitch_bend_cents = frequency_to_pitch_bend_cents(pitch.frequency_hz);
        entry.pitch_bend_wheel = frequency_to_pitch_bend_wheel(pitch.frequency_hz);
    }

    (void)try_enqueue(entry);
}

void Logger::log_note(const NoteEvent& event,
                      const PitchEstimate& pitch) noexcept {
    DebugLogEntry entry{};
    entry.kind = DebugLogKind::Note;
    entry.note = event.note;
    entry.velocity = event.velocity;
    entry.note_on = event.on;
    entry.frame_offset = event.frame_offset;
    entry.pitch_bend_wheel = event.pitch_bend_wheel;

    entry.frequency_hz = pitch.frequency_hz;
    entry.confidence = pitch.confidence;
    entry.rms = pitch.rms;
    entry.pitch_valid = pitch.is_valid;

    if (event.kind == MidiEventKind::PitchBend) {
        entry.note_on = false;
        entry.note = 0;
        entry.pitch_bend_wheel = event.pitch_bend_wheel;
        if (pitch.is_valid && pitch.frequency_hz > 0.0F) {
            entry.pitch_bend_cents =
                frequency_to_pitch_bend_cents(pitch.frequency_hz);
        }
        (void)try_enqueue(entry);
        return;
    }

    if (pitch.is_valid && pitch.frequency_hz > 0.0F) {
        entry.midi_note = frequency_to_midi_note(pitch.frequency_hz);
        entry.pitch_bend_cents = frequency_to_pitch_bend_cents(pitch.frequency_hz);
        entry.pitch_bend_wheel = frequency_to_pitch_bend_wheel(pitch.frequency_hz);
    } else {
        entry.midi_note = event.note;
    }

    (void)try_enqueue(entry);
}

void Logger::write_entry(const DebugLogEntry& entry, std::ofstream& file) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());

    char note_label[16];
    char line[512];

    if (entry.kind == DebugLogKind::Pitch) {
        format_note_name(entry.midi_note, note_label, sizeof(note_label));
        std::snprintf(
            line, sizeof(line),
            "[%lld] PITCH f=%.2fHz midi=%d (%s) bend=%+.1fct wheel=%d "
            "conf=%.3f rms=%.4f valid=%d\n",
            static_cast<long long>(ms.count()), entry.frequency_hz,
            entry.midi_note, note_label, entry.pitch_bend_cents,
            entry.pitch_bend_wheel, entry.confidence, entry.rms,
            entry.pitch_valid ? 1 : 0);
    } else {
        format_note_name(entry.note, note_label, sizeof(note_label));
        if (entry.note_on) {
            std::snprintf(
                line, sizeof(line),
                "[%lld] NOTE %s note=%d (%s) vel=%.0f frame=%u f=%.2fHz "
                "bend=%+.1fct wheel=%d conf=%.3f\n",
                static_cast<long long>(ms.count()), "ON ", entry.note, note_label,
                entry.velocity, entry.frame_offset, entry.frequency_hz,
                entry.pitch_bend_cents, entry.pitch_bend_wheel, entry.confidence);
        } else if (entry.velocity == 0.0F && entry.note == 0 &&
                   entry.pitch_bend_wheel != 0) {
            std::snprintf(
                line, sizeof(line),
                "[%lld] BEND wheel=%d (%+.1fct) frame=%u f=%.2fHz conf=%.3f\n",
                static_cast<long long>(ms.count()), entry.pitch_bend_wheel,
                entry.pitch_bend_cents, entry.frame_offset, entry.frequency_hz,
                entry.confidence);
        } else {
            std::snprintf(
                line, sizeof(line),
                "[%lld] NOTE %s note=%d (%s) vel=%.0f frame=%u f=%.2fHz "
                "bend=%+.1fct wheel=%d conf=%.3f\n",
                static_cast<long long>(ms.count()), "OFF", entry.note, note_label,
                entry.velocity, entry.frame_offset, entry.frequency_hz,
                entry.pitch_bend_cents, entry.pitch_bend_wheel, entry.confidence);
        }
    }

    std::cout << line << std::flush;
    if (file.is_open()) {
        file << line;
        file.flush();
    }
}

void Logger::consumer_loop() noexcept {
    std::ofstream file;
    if (!log_file_path_.empty()) {
        file.open(log_file_path_, std::ios::out | std::ios::app);
    }

    while (running_.load(std::memory_order_acquire)) {
        DebugLogEntry entry{};
        bool got_entry = false;
        while (queue_.try_pop(entry)) {
            write_entry(entry, file);
            got_entry = true;
        }
        if (!got_entry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    DebugLogEntry entry{};
    while (queue_.try_pop(entry)) {
        write_entry(entry, file);
    }
}

}  // namespace dsp
