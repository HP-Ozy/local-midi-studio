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
        AudioClip audio;                             // recorded voice or dropped file, at `rate`
        juce::AudioBuffer<float> in;                 // audio thread: this block of `audio`
    };

    Song() { for (auto& t : tracks) t = std::make_unique<Track>(); }

    void prepare (double sampleRate, int maxBlock)
    {
        rate = sampleRate;
        masterSmoothed.reset (sampleRate, 0.02); // same ramp as each track's Volume
        masterSmoothed.setCurrentAndTargetValue (master);
        for (auto& t : tracks)
        {
            t->engine.prepare (sampleRate);
            t->scratch.setSize (2, juce::jmax (maxBlock, 1));
            t->midi.ensureSize (MidiQueue::capacity * 16);
            t->in.setSize (2, juce::jmax (maxBlock, 1));
        }
    }

    double beatSeconds() const { return 60.0 / juce::jlimit (30.0, 300.0, bpm.load()); }
    double barSeconds() const  { return 4.0 * beatSeconds(); }

    // Audio thread. `live` = what is being played now (placed by MidiQueue), for the selected track; `padLive` = the
    // drum pads, for track padTrack whatever is selected. Timing: see songZeroWall.
    void render (juce::AudioBuffer<float>& out, int start, int n, const juce::MidiBuffer& live, double nowWall,
                 int padTrack = -1, const juce::MidiBuffer* padLive = nullptr, const float* mic = nullptr)
    {
        const bool running = run.load();
        const bool starting = running && ! audioRunning, stopping = ! running && audioRunning;
        const juce::int64 L = running ? loopLen.load() : 0;
        if (starting)
        {
            pos = startPos.load();
            loopBase = 0;
            if (L > 0) loopCount = 0;
            for (auto& t : tracks) // resuming mid-song: skip what is already past, or it would all sound at once
                for (t->idx = 0; t->idx < t->clip.getNumEvents() && std::llround (t->clip.getEventTime (t->idx) * rate) < pos;) ++t->idx;
        }
        audioRunning = running;
        if (running) songZeroWall = nowWall - (double) (pos + n - 1) / rate; // MidiQueue puts an event from time a at n-1-(now-a)*rate

        bool anySolo = false;
        for (int i = 0; i < count; ++i) anySolo |= tracks[(size_t) i]->solo.load();
        out.clear (start, n);
        const int sel = selected.load(), rec = recordTrack.load();
        const int cut = L > 0 && pos + n > L ? (int) (L - pos) : n; // samples before the loop wraps (n = no wrap here)
        for (int i = 0; i < count; ++i)
        {
            auto& t = *tracks[(size_t) i];
            if (t.scratch.getNumSamples() < n) continue; // not prepared for this block size (never in practice)
            t.midi.clear();
            if (i == sel) t.midi.addEvents (live, 0, n, 0);
            if (i == padTrack && padLive != nullptr) t.midi.addEvents (*padLive, 0, n, 0);
            if (stopping) for (int ch = 1; ch <= 16; ++ch) t.midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            if (running && (i != rec || L > 0)) // a recording track doesn't play its old clip, unless looping (overdub)
            {
                t.idx = addEventsInRange (t.clip, t.idx, pos, cut, rate, t.midi);
                if (cut < n) // wrap: end the clip's notes still sounding at the loop end (not live keys), start again at 0
                {
                    for (int j = 0; j < t.idx; ++j)
                        if (const auto* e = t.clip.getEventPointer (j); e->message.isNoteOn()
                            && (e->noteOffObject == nullptr || std::llround (e->noteOffObject->message.getTimeStamp() * rate) >= L))
                            t.midi.addEvent (juce::MidiMessage::noteOff (e->message.getChannel(), e->message.getNoteNumber()), cut);
                    t.idx = addEventsInRange (t.clip, 0, 0, n - cut, rate, t.midi, cut);
                }
            }
            if (running && L > 0 && i == rec) loopOverdub (t.midi, L, cut, n, i == sel ? &live : nullptr, i == padTrack ? padLive : nullptr);
            const juce::AudioBuffer<float>* in = nullptr;
            if (running && i != voiceTrack.load() && ! t.audio.empty() && t.in.getNumSamples() >= n) // a voice take doesn't play the old voice
            {
                t.in.clear (0, n);
                t.audio.addTo (t.in, 0, n, pos, rate, L);
                in = &t.in;
            }
            t.engine.render (t.scratch, 0, n, t.midi, in);
            if (t.mute || (anySolo && ! t.solo)) continue; // still rendered, so envelopes stay consistent when unmuted
            for (int c = 0; c < out.getNumChannels(); ++c) out.addFrom (c, start, t.scratch, juce::jmin (c, 1), 0, n);
        }

        if (const int v = voiceTrack.load(); running && v >= 0 && mic != nullptr) // voice take: the mic, placed on the song's timeline
        {
            auto* w = voiceTake.getWritePointer (0);
            for (int k = 0; k < n; ++k)
                if (const auto p = pos + k; p >= 0 && p < voiceTake.getNumSamples()) w[p] = mic[k];
            voiceEnd = juce::jlimit<juce::int64> (0, voiceTake.getNumSamples(), pos + n);
        }

        if (running && metronome)
        {
            const auto beat = (juce::int64) std::llround (beatSeconds() * rate);
            for (int i = 0; i < n; ++i)
            {
                const auto p = L > 0 && pos + i >= L ? pos + i - L : pos + i;
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
        masterSmoothed.setTargetValue (master);
        for (int i = start; i < start + n; ++i)
        {
            const float g = masterSmoothed.getNextValue();
            for (int c = 0; c < out.getNumChannels(); ++c)
            {
                const float x = out.getSample (c, i) * g, a = std::abs (x);
                pk = juce::jmax (pk, a);
                hits += a > 0.8f ? 1 : 0;
                out.setSample (c, i, SynthEngine::safetyClip (x)); // tracks are each clipped already; the sum can still overload
            }
        }
        if (pk > peakLevel.load()) peakLevel = pk;
        if (hits > 0) limiterHits += hits;
        if (running) pos += n;
        if (L > 0 && pos >= L) { pos -= L; loopBase += L; }
        position = (double) pos / rate;
    }

    // Loop recording, audio thread: plays this take's earlier passes into `midi`, then stores what is played live now
    // (absolute sample position since bar 1 of the first pass) in the preallocated loopTake. No allocation, no lock.
    void loopOverdub (juce::MidiBuffer& midi, juce::int64 L, int cut, int n, const juce::MidiBuffer* live, const juce::MidiBuffer* pads)
    {
        const auto pass = loopBase / L;
        const int stored = loopCount.load();
        // MidiBuffer::addEvent is O(n) per insert and the buffer holds about MidiQueue's cap: past it, a flood of
        // replayed events in one block is dropped (measured: 8192 at once took 45 ms in a 5 ms block)
        for (int k = 0, added = 0; k < stored && added < MidiQueue::maxPerBlock; ++k)
        {
            const auto& e = loopTake[(size_t) k];
            const auto p = e.pos / L, at = e.pos % L;
            if (p < pass && at >= pos && at < pos + cut) { midi.addEvent (e.data, e.size, (int) (at - pos)); ++added; }
            else if (cut < n && p <= pass && at < n - cut) { midi.addEvent (e.data, e.size, cut + (int) at); ++added; } // next pass, after the wrap
        }
        int k = stored;
        for (const auto* b : { live, pads })
            if (b != nullptr)
                for (const auto meta : *b)
                {
                    const auto& m = meta.getMessage();
                    const auto at = pos + meta.samplePosition;
                    if (at < 0 || k >= loopTakeMax || m.getRawDataSize() > 3
                        || ! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isChannelPressure())) continue; // count-in, full
                    auto& e = loopTake[(size_t) k++];
                    e.pos = loopBase + at;
                    e.size = m.getRawDataSize();
                    std::memcpy (e.data, m.getRawData(), (size_t) e.size);
                }
        loopCount = k;
    }

    // Removes track i: the ones after it move up, and the freed track (now last, beyond count) is reset so "+ Track"
    // gets a clean one. The caller makes sure the audio thread isn't rendering meanwhile.
    void removeTrack (int i)
    {
        if (i < 0 || i >= count || count <= 1) return;
        std::rotate (tracks.begin() + i, tracks.begin() + i + 1, tracks.begin() + count);
        auto& t = *tracks[(size_t) --count];
        t.engine.params.copyFrom (SynthParams());
        t.engine.setInstrument (nullptr);
        t.clip.clear();
        t.audio.clear();
        t.mute = false; t.solo = false; t.idx = 0;
        if (selected >= count) selected = count - 1;
    }

    int activeVoices() const { int v = 0; for (int i = 0; i < count; ++i) v += tracks[(size_t) i]->engine.activeVoices(); return v; }

    double length() const // end of the last event of any track, seconds
    {
        double e = 0;
        for (int i = 0; i < count; ++i) e = juce::jmax (e, tracks[(size_t) i]->clip.getEndTime(), (double) tracks[(size_t) i]->audio.end() / rate);
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
            if (t.mute || (anySolo && ! t.solo) || (t.clip.getNumEvents() == 0 && t.audio.empty())) continue;
            const auto part = renderOffline (t.clip, t.engine.params, sampleRate, t.engine.instrument(), &t.audio); // shortcut: audio assumed at sampleRate (= rate)
            if (part.getNumSamples() > mix.getNumSamples()) mix.setSize (2, part.getNumSamples(), true, true);
            for (int c = 0; c < 2; ++c) mix.addFrom (c, 0, part, c, 0, part.getNumSamples());
        }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < mix.getNumSamples(); ++i) mix.setSample (c, i, SynthEngine::safetyClip (mix.getSample (c, i) * master.load()));
        return mix;
    }

    std::array<std::unique_ptr<Track>, maxTracks> tracks;
    std::atomic<int> count { 1 }, selected { 0 }, recordTrack { -1 };
    std::atomic<double> bpm { 120.0 };
    std::atomic<bool> metronome { true };
    std::atomic<juce::int64> loopLen { 0 };     // samples; > 0 = the transport cycles over [0, loopLen) (loop recording)
    static constexpr int loopTakeMax = 8192;     // shortcut: events per loop take, more are dropped; raise if dense playing fills it
    struct LoopEvent { juce::int64 pos = 0; juce::uint8 data[3] {}; int size = 0; };
    std::vector<LoopEvent> loopTake = std::vector<LoopEvent> ((size_t) loopTakeMax); // written by the audio thread; GUI reads loopCount events once stopped
    std::atomic<int> loopCount { 0 };
    std::atomic<float> master { 1.0f };
    std::atomic<int> voiceTrack { -1 };          // >= 0: the mic is being recorded into voiceTake for this track
    juce::AudioBuffer<float> voiceTake { 1, 0 }; // allocated by the GUI before a voice take, never by the audio thread
    std::atomic<juce::int64> voiceEnd { 0 };     // samples of voiceTake written          // master volume 0..1, after the track sum (metronome included), before the master clip
    std::atomic<bool> run { false };
    std::atomic<juce::int64> startPos { 0 };    // where the transport starts, in samples (negative = count-in)
    std::atomic<double> songZeroWall { 0.0 };   // wall-clock time (MidiQueue::now) at which the song's 0 s is heard
    std::atomic<double> position { 0.0 };       // seconds, for the GUI's playhead
    std::atomic<float> peakLevel { 0.0f };      // master, before its safety clip
    std::atomic<juce::int64> limiterHits { 0 };
    std::atomic<bool> audioRunning { false };   // what the audio thread last did: the GUI waits for it before touching clips
    double rate = 48000.0;
    juce::int64 pos = 0;                         // audio thread
    juce::int64 loopBase = 0;                    // audio thread: samples of the loop passes already completed
    juce::SmoothedValue<float> masterSmoothed { 1.0f }; // audio thread
    int clickLeft = 0;
    double clickHz = 1000.0, clickPhase = 0.0;
};
