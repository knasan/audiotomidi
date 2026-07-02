function (event) {

    var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    var REF_A4_HZ = 440;

    function noteFromFrequency(hz) {
        if (!hz || hz <= 0) {
            return null;
        }
        var continuous = 69 + 12 * (Math.log(hz / REF_A4_HZ) / Math.LN2);
        var low = Math.floor(continuous);
        var high = low + 1;
        var errLow = Math.abs(hz - REF_A4_HZ * Math.pow(2, (low - 69) / 12));
        var errHigh = Math.abs(hz - REF_A4_HZ * Math.pow(2, (high - 69) / 12));
        var midi = errLow <= errHigh ? low : high;
        if (midi < 0 || midi > 127) {
            return null;
        }
        return {
            name: NOTE_NAMES[midi % 12],
            octave: Math.floor(midi / 12) - 1,
            midi: midi
        };
    }

    function formatNoteLabel(note) {
        if (!note) {
            return '--';
        }
        return note.name + note.octave;
    }

    function formatHz(hz) {
        if (!hz || hz <= 0) {
            return '— Hz';
        }
        if (hz >= 100) {
            return hz.toFixed(1) + ' Hz';
        }
        return hz.toFixed(2) + ' Hz';
    }

    function formatCents(hz) {
        if (!hz || hz <= 0) {
            return '±0 ct';
        }
        var continuous = 69 + 12 * (Math.log(hz / REF_A4_HZ) / Math.LN2);
        var cents = (continuous - Math.round(continuous)) * 100;
        var sign = cents >= 0 ? '+' : '';
        return sign + cents.toFixed(0) + ' ct';
    }

    function hasAudioSignal(rms) {
        return rms >= 0.00008;
    }

    function formatSignal(ds) {
        if (ds.hz && ds.hz > 0) {
            return 'tone detected';
        }
        if (hasAudioSignal(ds.rms)) {
            if (ds.confidence >= 0.12) {
                return 'analysing…';
            }
            return 'weak signal';
        }
        if (ds.confidence >= 0.05) {
            return 'weak signal';
        }
        return 'no signal';
    }

    function midiNoteLabel(midiNote) {
        if (!midiNote || midiNote <= 0 || midiNote > 127) {
            return '--';
        }
        var note = Math.round(midiNote);
        return NOTE_NAMES[note % 12] + (Math.floor(note / 12) - 1);
    }

    function updateDisplay() {
        var icon = event.icon;
        var ds = icon.data('tunerState');
        if (!ds) {
            return;
        }

        var note = ds.hz > 0 ? noteFromFrequency(ds.hz) : null;
        var pending = ds.confidence >= 0.12 || hasAudioSignal(ds.rms);
        var nameEl = icon.find('.tuner-note-name');
        var octaveEl = icon.find('.tuner-note-octave');

        if (note) {
            nameEl.text(note.name).addClass('active');
            octaveEl.text(String(note.octave));
        } else {
            nameEl.text(pending ? '…' : '--');
            octaveEl.text('');
            if (pending) {
                nameEl.addClass('active');
            } else {
                nameEl.removeClass('active');
            }
        }

        icon.find('.tuner-hz').text(formatHz(ds.hz));
        icon.find('.tuner-cents').text(formatCents(ds.hz));

        var signalEl = icon.find('.tuner-signal');
        signalEl.text(formatSignal(ds));
        signalEl.removeClass('tuner-signal-ok tuner-signal-warn');
        if (ds.hz > 0) {
            signalEl.addClass('tuner-signal-ok');
        } else if (hasAudioSignal(ds.rms) || ds.confidence >= 0.05) {
            signalEl.addClass('tuner-signal-warn');
        }

        icon.find('.tuner-read-hz').text(formatNoteLabel(note));

        var midiNote = 0;
        if (ds.midiDetected > 0) {
            midiNote = Math.round(ds.midiDetected);
        } else if (ds.midiActive > 0) {
            midiNote = Math.round(ds.midiActive);
        } else if (ds.midiFromHz > 0) {
            midiNote = Math.round(ds.midiFromHz);
        }
        icon.find('.tuner-read-midi').text(midiNoteLabel(midiNote));
    }

    function handle_event(symbol, value) {
        var icon = event.icon;
        var ds = icon.data('tunerState');
        if (!ds || !symbol) {
            return;
        }

        switch (symbol) {
        case 'tuner_hz':
            ds.hz = value;
            break;
        case 'tuner_midi_from_hz':
            ds.midiFromHz = value;
            break;
        case 'tuner_midi_detected':
            ds.midiDetected = value;
            break;
        case 'tuner_midi_active':
            ds.midiActive = value;
            break;
        case 'tuner_confidence':
            ds.confidence = value;
            break;
        case 'tuner_rms':
            ds.rms = value;
            break;
        default:
            return;
        }

        icon.data('tunerState', ds);
        updateDisplay();
    }

    if (event.type == 'start') {
        var ds = {
            hz: 0,
            confidence: 0,
            rms: 0,
            midiFromHz: 0,
            midiDetected: 0,
            midiActive: 0
        };
        event.icon.data('tunerState', ds);

        var ports = event.ports;
        for (var p in ports) {
            handle_event(ports[p].symbol, ports[p].value);
        }
    } else if (event.type == 'change') {
        handle_event(event.symbol, event.value);
    }
}
