#pragma once

#include <cstdint>

namespace lv2_ports {

/// LV2 port indices — must match lv2/audiotomidi.ttl exactly.
///
/// Signal interfaces for MOD / ePiano routing:
///   AudioIn  (index 0) — mono guitar audio from the pedalboard
///   MidiOut  (index 1) — Atom MIDI sequence to a synth (e.g. ePiano MIDI In)
enum PortIndex : std::uint32_t {
    AudioIn = 0,
    MidiOut = 1,
    Sensitivity = 2,
    MidiChannel = 3,
    NoteOnDebounce = 4,
    NoteOffDebounce = 5,
    PitchBendEnable = 6,
    LowestNote = 7,
    ReferenceA4Hz = 8,
    LowestNoteLimitEnable = 9,
    MidiTranspose = 10,
    PitchBiasCents = 11,
    PitchMethod = 12,

    /// Tuner / debug outputs (LV2 control outputs — MOD GUI reads via monitoredOutputs).
    TunerHz = 13,
    TunerCents = 14,
    TunerMidiFromHz = 15,
    TunerMidiDetected = 16,
    TunerMidiActive = 17,
    TunerConfidence = 18,
    TunerRms = 19,

    PortCount = 20,
};

}  // namespace lv2_ports
