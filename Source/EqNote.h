/*
    EqNote.h  —  musical note <-> frequency, for the EQ (P5 of
    SURGICAL_EQ_ENHANCEMENTS.md). JUCE-free, tested in test/eq_note_test.cpp.

    Two directions, two customers:
      * parseNoteToFreq — lets a move say {"type":"notch","note":"G5"} and land
        on 783.99 Hz without the model doing pitch math;
      * describeFreqAsNote — the editor's drag readout ("A4 +12c").

    A4 = 440. Accidentals: '#' or 's' for sharp, 'b' for flat. Octaves -1..9
    (MIDI's full span). Anything else is a parse failure, never a guess — a
    band landing at the wrong pitch is worse than a move that reports it
    couldn't read the note.
*/

#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>

namespace echojay
{

// "A4", "C#3", "Bb2", "f#-1", case-insensitive. Returns false (out untouched)
// when the string is not a note.
inline bool parseNoteToFreq (const char* s, float& outHz) noexcept
{
    if (s == nullptr || s[0] == '\0') return false;

    // letter
    const char letter = (char) std::tolower ((unsigned char) s[0]);
    int semitone;                    // C = 0 within an octave
    switch (letter)
    {
        case 'c': semitone = 0;  break;
        case 'd': semitone = 2;  break;
        case 'e': semitone = 4;  break;
        case 'f': semitone = 5;  break;
        case 'g': semitone = 7;  break;
        case 'a': semitone = 9;  break;
        case 'b': semitone = 11; break;
        default:  return false;
    }
    int i = 1;

    // accidental (optional). 'b' is ambiguous ("B" the letter vs flat) only in
    // position 0, which the switch above has already consumed.
    if (s[i] == '#' || s[i] == 's' || s[i] == 'S') { ++semitone; ++i; }
    else if (s[i] == 'b' || s[i] == 'B')           { --semitone; ++i; }

    // octave: -1..9, one optional sign
    bool neg = false;
    if (s[i] == '-') { neg = true; ++i; }
    if (! std::isdigit ((unsigned char) s[i])) return false;
    int oct = 0;
    for (; std::isdigit ((unsigned char) s[i]); ++i)
    {
        oct = oct * 10 + (s[i] - '0');
        if (oct > 9) return false;
    }
    if (s[i] != '\0') return false;          // trailing junk is a failure
    if (neg)
    {
        if (oct != 1) return false;          // only octave -1 exists
        oct = -1;
    }

    const int midi = (oct + 1) * 12 + semitone;      // C-1 = 0, A4 = 69
    if (midi < 0 || midi > 131) return false;        // keep within B9

    outHz = 440.0f * std::pow (2.0f, (float) (midi - 69) / 12.0f);
    return true;
}

// Nearest note to a frequency plus the deviation in cents: "A4 +12c".
// bufLen should be >= 12. Returns false (empty string) outside 8 Hz..13 kHz…
// actually outside the MIDI span, where a note name would be a lie.
inline bool describeFreqAsNote (float hz, char* buf, int bufLen) noexcept
{
    if (buf == nullptr || bufLen < 12) return false;
    buf[0] = '\0';
    if (! (hz > 0.0f)) return false;

    const float midiF = 69.0f + 12.0f * std::log2 (hz / 440.0f);
    const int   midi  = (int) std::lround (midiF);
    if (midi < 0 || midi > 131) return false;

    const int cents = (int) std::lround ((midiF - (float) midi) * 100.0f);

    static const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B" };
    const int oct = midi / 12 - 1;

    if (cents == 0)
        std::snprintf (buf, (size_t) bufLen, "%s%d", kNames[midi % 12], oct);
    else
        std::snprintf (buf, (size_t) bufLen, "%s%d %+dc", kNames[midi % 12], oct, cents);
    return true;
}

} // namespace echojay
