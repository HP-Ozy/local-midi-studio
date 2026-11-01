#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "Synth.h"

// A take is a juce::MidiMessageSequence with timestamps in seconds from the start of recording.

// Adds the events whose time falls in [from, from + n) samples, at bufferOffset + (t - from).
// Returns the index of the first event not yet added. Real-time safe if `out` has capacity.
inline int addEventsInRange (const juce::MidiMessageSequence& seq, int idx, juce::int64 from, int n,
                             double sampleRate, juce::MidiBuffer& out, int bufferOffset = 0)
{
    for (; idx < seq.getNumEvents(); ++idx)
    {
        const auto& m = seq.getEventPointer (idx)->message;
        const auto t = (juce::int64) std::llround (m.getTimeStamp() * sampleRate);
        if (t >= from + n) break;
        out.addEvent (m, bufferOffset + (int) juce::jmax<juce::int64> (0, t - from));
    }
    return idx;
}

// Standard MIDI File, 960 PPQ at 120 BPM, so seconds map exactly to ticks.
inline void writeMidiFile (const juce::MidiMessageSequence& take, juce::OutputStream& out)
{
    constexpr double ticksPerSecond = 960.0 * 120.0 / 60.0;
    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0); // 120 BPM
    for (auto* e : take)
    {
        auto m = e->message;
        m.setTimeStamp (std::round (m.getTimeStamp() * ticksPerSecond));
        track.addEvent (m);
    }
    track.updateMatchedPairs();
    juce::MidiFile file;
    file.setTicksPerQuarterNote (960);
    file.addTrack (track);
    file.writeTo (out);
}

// A Standard MIDI File split into song parts, timestamps in seconds (the file's tempo map applied).
struct MidiParts
{
    struct Part { juce::String name; juce::MidiMessageSequence seq; };
    juce::MidiMessageSequence drums; // every channel-10 event of any file track (General MIDI drums)
    std::vector<Part> parts;         // one per file track with notes on other channels, in file order
    double bpm = 120.0;              // the file's first tempo
};

inline bool readMidiParts (juce::InputStream& in, MidiParts& out)
{
    juce::MidiFile file;
    if (! file.readFrom (in)) return false;
    file.convertTimestampTicksToSeconds();
    out = {};
    bool tempoSeen = false;
    for (int t = 0; t < file.getNumTracks(); ++t)
    {
        MidiParts::Part part;
        bool notes = false;
        for (auto* e : *file.getTrack (t))
        {
            const auto& m = e->message;
            if (m.isTrackNameEvent()) part.name = m.getTextFromTextMetaEvent();
            else if (m.isTempoMetaEvent() && ! tempoSeen) { tempoSeen = true; out.bpm = 60.0 / m.getTempoSecondsPerQuarterNote(); }
            else if (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isChannelPressure() || m.isAftertouch())
            {
                if (m.getChannel() == 10) out.drums.addEvent (m);
                else { part.seq.addEvent (m); notes |= m.isNoteOn(); }
            }
        }
        if (notes) { part.seq.updateMatchedPairs(); out.parts.push_back (std::move (part)); }
    }
    out.drums.updateMatchedPairs();
    return true;
}

// Renders a take offline with the same engine and settings as the live synth (adds release tail).
// instrument: the sampler set to play the take with (nullptr = the synth); its sample data is shared, not copied.
inline juce::AudioBuffer<float> renderOffline (const juce::MidiMessageSequence& take, const SynthParams& params, double sampleRate,
                                              juce::SynthesiserSound::Ptr instrument = nullptr)
{
    SynthEngine engine;
    engine.params.copyFrom (params);
    engine.setInstrument (instrument);
    engine.prepare (sampleRate);
    const int total = (int) ((take.getEndTime() + params.release.load() + 0.5) * sampleRate);
    juce::AudioBuffer<float> out (2, total);
    juce::MidiBuffer midi;
    int idx = 0;
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = juce::jmin (512, total - pos);
        midi.clear();
        idx = addEventsInRange (take, idx, pos, n, sampleRate, midi, pos); // Synthesiser wants absolute buffer positions
        engine.render (out, pos, n, midi);
    }
    return out;
}

inline bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0));
    if (writer == nullptr) return false;
    stream.release(); // the writer owns it now
    return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}
