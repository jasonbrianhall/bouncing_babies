#pragma once

#include <vector>
#include <cstdint>

// Renders a MIDI file (given as raw bytes) to a WAV file (also as raw bytes,
// including the 44-byte header) using the OPL3 FM synth. Fully in-process,
// no external files, no network calls.
//
// Not reentrant: internally serializes on a mutex since the underlying
// synth/parser state is global (ported from a single-instance DOS player).
// Safe to call from multiple threads, but concurrent calls queue up.
bool render_midi_to_wav(const std::vector<uint8_t> &midi_bytes,
                         std::vector<uint8_t> &out_wav,
                         int volume = 500);
