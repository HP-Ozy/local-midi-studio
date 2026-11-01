#pragma once
#include "Synth.h"
#include "MidiQueue.h"
#include "Recording.h"

// Which incoming (channel, note) is a drum pad and which kit sound it plays. The GUI writes it (learn / assign),
// the audio thread reads it: plain atomics, no lock.
struct PadRouter
{
    PadRouter() { clear(); }
    void clear()                            { for (auto& r : route) r = -1; }
    void set (int channel, int note, int sound) { route[index (channel, note)] = juce::jlimit (-1, 127, sound); }
    int soundFor (int channel, int note) const { return route[index (channel, note)].load(); }

    // Pad notes, re-mapped to their kit sound, go to `pads`; everything else to `keys`.
    void split (const juce::MidiBuffer& in, juce::MidiBuffer& keys, juce::MidiBuffer& pads) const
    {
        for (const auto meta : in)
        {
            const auto& m = meta.getMessage();
            const int sound = m.isNoteOnOrOff() ? soundFor (m.getChannel(), m.getNoteNumber()) : -1;
            if (sound < 0) { keys.addEvent (m, meta.samplePosition); continue; }
            pads.addEvent (m.isNoteOn() ? juce::MidiMessage::noteOn (m.getChannel(), sound, m.getVelocity())
                                        : juce::MidiMessage::noteOff (m.getChannel(), sound), meta.samplePosition);
        }
    }

private:
    static size_t index (int channel, int note) { return (size_t) (juce::jlimit (1, 16, channel) - 1) * 128 + (size_t) juce::jlimit (0, 127, note); }
    std::array<std::atomic<int>, 16 * 128> route;
};

// A song: up to maxTracks tracks, each its own SynthEngine (sound, instrument, effects) playing a MIDI clip timed in
// seconds from bar 1. Live MIDI goes to the selected track. The audio thread mixes all tracks, adds the metronome and
// runs a master safety clip. GUI-independent so it can be tested offline.
struct Song
{
    static constexpr int maxTracks = 8; // shortcut: fixed and built up front (no allocation when adding); raise if needed

    struct Track
    {
        SynthEngine engine;
        juce::MidiMessageSequence clip;              // GUI changes it only while the transport is stopped
        std::atomic<bool> mute { false }, solo { false };
        int idx = 0;                                 // audio thread: next clip event
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> scratch;
    };

    Song() { for (auto& t : tracks) t = std::make_unique<Track>(); }

    void prepare (double sampleRate, int maxBlock)
    {
        rate = sampleRate;
        for (auto& t : tracks)
        {
            t->engine.prepare (sampleRate);
            t->scratch.setSize (2, juce::jmax (maxBlock, 1));
            t->midi.ensureSize (MidiQueue::capacity * 16);
        }
    }

    double beatSeconds() const { return 60.0 / juce::jlimit (30.0, 300.0, bpm.load()); }
    double barSeconds() const  { return 4.0 * beatSeconds(); }

    // Audio thread. `live` = what is being played now (placed by MidiQueue), for the selected track; `padLive` = the
    // drum pads, for track padTrack whatever is selected. Timing: see songZeroWall.
    void render (juce::AudioBuffer<float>& out, int start, int n, const juce::MidiBuffer& live, double nowWall,
                 int padTrack = -1, const juce::MidiBuffer* padLive = nullptr)
    {
        const bool running = run.load();
        const bool starting = running && ! audioRunning, stopping = ! running && audioRunning;
        if (starting) { pos = startPos.load(); for (auto& t : tracks) t->idx = 0; }
        audioRunning = running;
        if (running) songZeroWall = nowWall - (double) (pos + n - 1) / rate; // MidiQueue puts an event from time a at n-1-(now-a)*rate

        bool anySolo = false;
        for (int i = 0; i < count; ++i) anySolo |= tracks[(size_t) i]->solo.load();
        out.clear (start, n);
        const int sel = selected.load(), rec = recordTrack.load();
        for (int i = 0; i < count; ++i)
        {
            auto& t = *tracks[(size_t) i];
            if (t.scratch.getNumSamples() < n) continue; // not prepared for this block size (never in practice)
            t.midi.clear();
            if (i == sel) t.midi.addEvents (live, 0, n, 0);
            if (i == padTrack && padLive != nullptr) t.midi.addEvents (*padLive, 0, n, 0);
            if (stopping) for (int ch = 1; ch <= 16; ++ch) t.midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            if (running && i != rec) t.idx = addEventsInRange (t.clip, t.idx, pos, n, rate, t.midi); // a recording track doesn't play its old clip
            t.engine.render (t.scratch, 0, n, t.midi);
            if (t.mute || (anySolo && ! t.solo)) continue; // still rendered, so envelopes stay consistent when unmuted
            for (int c = 0; c < out.getNumChannels(); ++c) out.addFrom (c, start, t.scratch, juce::jmin (c, 1), 0, n);
        }

        if (running && metronome)
        {
            const auto beat = (juce::int64) std::llround (beatSeconds() * rate);
            for (int i = 0; i < n; ++i)
            {
                const auto p = pos + i;
                if (beat > 0 && p % beat == 0) { clickLeft = (int) (0.03 * rate); clickHz = (p / beat) % 4 == 0 ? 1500.0 : 1000.0; clickPhase = 0; }
                if (clickLeft > 0)
                {
                    const float v = 0.25f * (float) std::sin (clickPhase) * (float) clickLeft / (float) (0.03 * rate);
                    clickPhase += juce::MathConstants<double>::twoPi * clickHz / rate;
                    --clickLeft;
                    for (int c = 0; c < out.getNumChannels(); ++c) out.addSample (c, start + i, v);
                }
            }
        }

        float pk = 0;
        int hits = 0;
        for (int c = 0; c < out.getNumChannels(); ++c)
            for (int i = start; i < start + n; ++i)
            {
                const float x = out.getSample (c, i), a = std::abs (x);
                pk = juce::jmax (pk, a);
                hits += a > 0.8f ? 1 : 0;
                out.setSample (c, i, SynthEngine::safetyClip (x)); // tracks are each clipped already; the sum can still overload
            }
        if (pk > peakLevel.load()) peakLevel = pk;
        if (hits > 0) limiterHits += hits;
        if (running) pos += n;
        position = (double) pos / rate;
    }

    int activeVoices() const { int v = 0; for (int i = 0; i < count; ++i) v += tracks[(size_t) i]->engine.activeVoices(); return v; }

    double length() const // end of the last event of any track, seconds
    {
        double e = 0;
        for (int i = 0; i < count; ++i) e = juce::jmax (e, tracks[(size_t) i]->clip.getEndTime());
        return e;
    }

    // Offline mixdown of the audible tracks with the same engines' settings (no metronome).
    juce::AudioBuffer<float> mixdown (double sampleRate) const
    {
        bool anySolo = false;
        for (int i = 0; i < count; ++i) anySolo |= tracks[(size_t) i]->solo.load();
        juce::AudioBuffer<float> mix (2, 0);
        for (int i = 0; i < count; ++i)
        {
            auto& t = *tracks[(size_t) i];
            if (t.mute || (anySolo && ! t.solo) || t.clip.getNumEvents() == 0) continue;
            const auto part = renderOffline (t.clip, t.engine.params, sampleRate, t.engine.instrument());
            if (part.getNumSamples() > mix.getNumSamples()) mix.setSize (2, part.getNumSamples(), true, true);
            for (int c = 0; c < 2; ++c) mix.addFrom (c, 0, part, c, 0, part.getNumSamples());
        }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < mix.getNumSamples(); ++i) mix.setSample (c, i, SynthEngine::safetyClip (mix.getSample (c, i)));
        return mix;
    }

    std::array<std::unique_ptr<Track>, maxTracks> tracks;
    std::atomic<int> count { 1 }, selected { 0 }, recordTrack { -1 };
    std::atomic<double> bpm { 120.0 };
    std::atomic<bool> metronome { true };
    std::atomic<bool> run { false };
    std::atomic<juce::int64> startPos { 0 };    // where the transport starts, in samples (negative = count-in)
    std::atomic<double> songZeroWall { 0.0 };   // wall-clock time (MidiQueue::now) at which the song's 0 s is heard
    std::atomic<double> position { 0.0 };       // seconds, for the GUI's playhead
    std::atomic<float> peakLevel { 0.0f };      // master, before its safety clip
    std::atomic<juce::int64> limiterHits { 0 };
    std::atomic<bool> audioRunning { false };   // what the audio thread last did: the GUI waits for it before touching clips
    double rate = 48000.0;
    juce::int64 pos = 0;                         // audio thread
    int clickLeft = 0;
    double clickHz = 1000.0, clickPhase = 0.0;
};
