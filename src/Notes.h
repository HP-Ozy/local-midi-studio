#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cmath>

// Piano-roll editing: a clip as a list of notes (start/end in seconds), edited, then written back.
// Everything that isn't a note (pedal, CC, pitch bend) is kept as it was.
namespace Notes
{
struct Note { double start, end; int note, velocity, channel; };

inline std::vector<Note> read (juce::MidiMessageSequence clip) // by value: matching the note-offs needs a sequence of its own
{
    clip.updateMatchedPairs();
    std::vector<Note> notes;
    for (int i = 0; i < clip.getNumEvents(); ++i)
    {
        const auto& m = clip.getEventPointer (i)->message;
        if (! m.isNoteOn()) continue;
        const int off = clip.getIndexOfMatchingKeyUp (i);
        const double end = off >= 0 ? clip.getEventTime (off) : m.getTimeStamp() + 0.25; // a note never released: give it a length
        notes.push_back ({ m.getTimeStamp(), juce::jmax (end, m.getTimeStamp() + 0.001), m.getNoteNumber(), m.getVelocity(), m.getChannel() });
    }
    return notes;
}

inline juce::MidiMessageSequence write (const std::vector<Note>& notes, const juce::MidiMessageSequence& original)
{
    juce::MidiMessageSequence seq;
    for (auto* e : original) if (! e->message.isNoteOnOrOff()) seq.addEvent (e->message);
    // offs first: at equal times they sort before the ons, so a note ending where the next starts doesn't cut it
    for (auto& n : notes) seq.addEvent (juce::MidiMessage::noteOff (n.channel, n.note).withTimeStamp (n.end));
    for (auto& n : notes) seq.addEvent (juce::MidiMessage::noteOn (n.channel, n.note, (juce::uint8) n.velocity).withTimeStamp (n.start));
    seq.updateMatchedPairs();
    return seq;
}

// Starts to the nearest grid line; lengths to whole grid steps (at least one).
inline void quantize (std::vector<Note>& notes, double grid)
{
    for (auto& n : notes)
    {
        const double len = juce::jmax (1.0, (double) std::llround ((n.end - n.start) / grid)) * grid;
        n.start = (double) std::llround (n.start / grid) * grid;
        n.end = n.start + len;
    }
}

// Loop recording: notes played over several passes of a loop `len` seconds long, folded into one pass
// (a note still sounding at the loop end is cut there).
inline void fold (std::vector<Note>& notes, double len)
{
    for (auto& n : notes)
    {
        const double k = std::floor (n.start / len);
        n.start -= k * len;
        n.end = juce::jmin (n.end - k * len, len);
    }
}

// Undo / redo of clip changes, any track. Undo hands back what a track had before; the clip it replaces goes on the
// redo side (and back again). A new change clears redo.
struct History
{
    static constexpr size_t maxSteps = 50; // shortcut: oldest steps dropped; MIDI clips are small, raise freely
    struct Step { int track; juce::MidiMessageSequence clip; };

    void changed (int track, const juce::MidiMessageSequence& before)
    {
        undos.push_back ({ track, before });
        if (undos.size() > maxSteps) undos.erase (undos.begin());
        redos.clear();
    }
    template <typename Current> bool undo (Current current, Step& out) { return swap (undos, redos, current, out); }
    template <typename Current> bool redo (Current current, Step& out) { return swap (redos, undos, current, out); }
    void clear() { undos.clear(); redos.clear(); }

    std::vector<Step> undos, redos;

private:
    template <typename Current> static bool swap (std::vector<Step>& from, std::vector<Step>& to, Current current, Step& out)
    {
        if (from.empty()) return false;
        out = std::move (from.back());
        from.pop_back();
        to.push_back ({ out.track, current (out.track) });
        return true;
    }
};
} // namespace Notes
