#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

// Drum step sequencer: one bar of 16 sixteenth notes per row; row r plays kit sound `sounds[r]` (the pad's sound).
// The pattern lives in the drum track's clip (repeated over `bars`), so songs save and open it with no extra format.
namespace Steps
{
constexpr int rows = 16, steps = 16;
using Pattern = std::array<std::array<bool, steps>, rows>;

inline juce::MidiMessageSequence render (const Pattern& p, const std::array<int, rows>& sounds, double barSeconds, int bars)
{
    juce::MidiMessageSequence seq;
    const double step = barSeconds / steps;
    for (int b = 0; b < bars; ++b)
        for (int r = 0; r < rows; ++r)
            for (int s = 0; s < steps; ++s)
                if (p[(size_t) r][(size_t) s])
                {
                    const double t = b * barSeconds + s * step;
                    seq.addEvent (juce::MidiMessage::noteOn (10, sounds[(size_t) r], (juce::uint8) 100).withTimeStamp (t));
                    seq.addEvent (juce::MidiMessage::noteOff (10, sounds[(size_t) r]).withTimeStamp (t + step * 0.5));
                }
    seq.updateMatchedPairs();
    return seq;
}

// The clip's first bar, rounded to the nearest step (a recorded take shows as its quantized first bar).
inline Pattern read (const juce::MidiMessageSequence& clip, const std::array<int, rows>& sounds, double barSeconds)
{
    Pattern p {};
    for (auto* e : clip)
    {
        const auto& m = e->message;
        const int s = (int) std::lround (m.getTimeStamp() / (barSeconds / steps));
        if (! m.isNoteOn() || s >= steps) continue;
        for (int r = 0; r < rows; ++r)
            if (sounds[(size_t) r] == m.getNoteNumber()) { p[(size_t) r][(size_t) s] = true; break; }
    }
    return p;
}

inline bool same (const juce::MidiMessageSequence& a, const juce::MidiMessageSequence& b)
{
    if (a.getNumEvents() != b.getNumEvents()) return false;
    for (int i = 0; i < a.getNumEvents(); ++i)
    {
        const auto& x = a.getEventPointer (i)->message;
        const auto& y = b.getEventPointer (i)->message;
        if (x.getTimeStamp() != y.getTimeStamp() || x.getRawDataSize() != y.getRawDataSize()
            || std::memcmp (x.getRawData(), y.getRawData(), (size_t) x.getRawDataSize()) != 0) return false;
    }
    return true;
}
} // namespace Steps
