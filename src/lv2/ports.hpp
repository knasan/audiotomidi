#pragma once

#include <cstdint>

namespace lv2_ports {

/// LV2 port indices — must match lv2/audiotomidi.ttl exactly.
///
/// MOD Dwarf requires an audio output (dry passthrough) to wire the effect in the
/// pedalboard graph. MIDI leaves via the Atom port.
enum PortIndex : std::uint32_t {
    AudioIn = 0,
    AudioOut = 1,
    MidiOut = 2,
    Sensitivity = 3,
    MidiChannel = 4,
    NoteOnDebounce = 5,
    NoteOffDebounce = 6,
    PitchBendEnable = 7,
    LowestNote = 8,
    ReferenceA4Hz = 9,
    LowestNoteLimitEnable = 10,
    MidiTranspose = 11,
    PitchBiasCents = 12,
    PitchMethod = 13,

    /// Tuner / debug outputs (MOD GUI reads via monitoredOutputs).
    TunerHz = 14,
    TunerCents = 15,
    TunerMidiFromHz = 16,
    TunerMidiDetected = 17,
    TunerMidiActive = 18,
    TunerConfidence = 19,
    TunerRms = 20,

    PortCount = 21,
};

}  // namespace lv2_ports
