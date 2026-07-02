# Audio-to-MIDI Converter for MOD

Experimental LV2 plugin for the MOD platform that converts monophonic audio signals into MIDI notes in real time.

## Overview

This plugin is an experimental tool for audio-to-MIDI conversion.
It is designed to analyze audio signals (in my case, guitar) and generate corresponding MIDI data that can be
further processed within the MOD ecosystem.

## Current Status

This plugin is in an early stage of development.

Tested on: mod-desktop (Linux).

Pending: Compilation for the MOD hardware platform (Dwarf/Duo/DuoX) via mod-plugin-builder. I am still learning the ropes of the build process for the hardware.

The code is released in its current state to share my progress with the community and to invite feedback or suggestions from interested parties. If you have experience with mod-plugin-builder or audio-to-MIDI algorithms and want to contribute or share tips, please feel free to reach out!

## Technical Approach

This plugin utilizes the aubio library for real-time pitch detection. To ensure optimal detection across various input signals (e.g., clean guitar vs. synth leads), the following algorithms have been implemented and can be toggled via the plugin interface:

YIN: The primary, high-precision algorithm for monophonic pitch detection.

mcomb: An alternative, spectral-based method.

Users can switch between these modes to achieve the best detection performance for their specific source material.

