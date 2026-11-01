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

// A track's audio: every file dropped on (or recorded into) the track, one after the other in `buf`, and up to
// maxRegions regions on the song's timeline that play parts of it. A region plays buf samples [in, out) so that `in`
// sounds at song sample `start`; split and duplicate make regions that share the same samples (nothing is copied).
// The GUI changes `buf` and adds/removes regions only while the transport is stopped; start/in/out may move while
// playing (a block may see a half-updated pair: harmless).
struct AudioClip
{
    static constexpr int maxRegions = 32, peakBlock = 256;
    static constexpr double fadeSeconds = 0.005; // a cut in the middle of a sound would click

    struct Region
    {
        std::atomic<juce::int64> start { 0 };
        std::atomic<int> in { 0 }, out { 0 };
        int lo = 0, hi = 0;                                   // GUI: its file's extent in buf, so trimming stays inside it
        std::atomic<bool> fadeIn { true }, fadeOut { true };  // false where another region continues it seamlessly (a split)
        juce::int64 end() const { return start.load() + out.load() - in.load(); }
    };

    juce::AudioBuffer<float> buf { 1, 0 };
    std::vector<float> peaks; // GUI: max |sample| per peakBlock of buf, for drawing
    std::array<Region, maxRegions> regions;
    std::atomic<int> count { 0 };

    bool empty() const { return count.load() == 0; }
    juce::int64 end() const { juce::int64 e = 0; for (int i = 0; i < count; ++i) e = juce::jmax (e, regions[(size_t) i].end()); return e; } // song sample after the last one heard

    void clear() { count = 0; buf.setSize (1, 0); peaks.clear(); } // transport stopped

    // Transport stopped. Appends `b` to buf (mono becomes stereo if either is stereo); returns where it starts in buf.
    int append (const juce::AudioBuffer<float>& b)
    {
        const int lo = buf.getNumSamples(), chans = juce::jmax (lo > 0 ? buf.getNumChannels() : 1, b.getNumChannels());
        juce::AudioBuffer<float> next (chans, lo + b.getNumSamples());
        for (int c = 0; c < chans; ++c)
        {
            if (lo > 0) next.copyFrom (c, 0, buf, juce::jmin (c, buf.getNumChannels() - 1), 0, lo);
            next.copyFrom (c, lo, b, juce::jmin (c, b.getNumChannels() - 1), 0, b.getNumSamples());
        }
        buf = std::move (next);
        peaks.resize ((size_t) (buf.getNumSamples() + peakBlock - 1) / peakBlock, 0.0f);
        for (int i = lo - lo % peakBlock; i < buf.getNumSamples(); ++i)
        {
            auto& pk = peaks[(size_t) (i / peakBlock)];
            if (i % peakBlock == 0) pk = 0.0f;
            for (int c = 0; c < chans; ++c) pk = juce::jmax (pk, std::abs (buf.getSample (c, i)));
        }
        return lo;
    }

    // Transport stopped. A region over buf [in, out) inside [lo, hi), heard from song sample `start`; -1 if full or empty.
    int addRegion (juce::int64 start, int in, int out, int lo, int hi)
    {
        hi = juce::jlimit (0, buf.getNumSamples(), hi);
        lo = juce::jlimit (0, hi, lo);
        out = juce::jlimit (lo, hi, out);
        in = juce::jlimit (lo, out, in);
        if (count >= maxRegions || in >= out) return -1;
        auto& r = regions[(size_t) count.load()];
        r.start = juce::jmax<juce::int64> (0, start); r.in = in; r.out = out; r.lo = lo; r.hi = hi;
        ++count;
        refreshFades();
        return count - 1;
    }

    int add (const juce::AudioBuffer<float>& b, juce::int64 at) // a new file: appended and placed at `at`
    {
        if (count >= maxRegions || b.getNumSamples() == 0) return -1;
        const int lo = append (b);
        return addRegion (at, lo, buf.getNumSamples(), lo, buf.getNumSamples());
    }

    int duplicate (int i, juce::int64 at) { const auto& r = regions[(size_t) i]; return addRegion (at, r.in, r.out, r.lo, r.hi); }

    // Transport stopped. Cuts region i at song sample `at`: it keeps the left part, a new region the right; -1 if `at` isn't inside.
    int split (int i, juce::int64 at)
    {
        auto& r = regions[(size_t) i];
        if (at <= r.start.load() || at >= r.end() || count >= maxRegions) return -1;
        const int cut = r.in.load() + (int) (at - r.start.load()), out = r.out.load();
        r.out = cut; // count is still the old one: the new region below can't be heard before it's complete
        return addRegion (at, cut, out, r.lo, r.hi);
    }

    void remove (int i) // transport stopped
    {
        for (int k = i; k + 1 < count; ++k)
        {
            auto &d = regions[(size_t) k], &s = regions[(size_t) k + 1];
            d.start = s.start.load(); d.in = s.in.load(); d.out = s.out.load(); d.lo = s.lo; d.hi = s.hi;
        }
        if (i >= 0 && i < count) --count;
        refreshFades();
    }

    // GUI, after any edit: no fade where a region is continued seamlessly by another (same samples, right after it).
    void refreshFades()
    {
        for (int i = 0; i < count; ++i)
        {
            auto& r = regions[(size_t) i];
            bool joinedIn = false, joinedOut = false;
            for (int j = 0; j < count; ++j)
            {
                const auto& o = regions[(size_t) j];
                if (j == i) continue;
                joinedIn |= o.end() == r.start.load() && o.out.load() == r.in.load();
                joinedOut |= r.end() == o.start.load() && r.out.load() == o.in.load();
            }
            r.fadeIn = ! joinedIn;
            r.fadeOut = ! joinedOut;
        }
    }

    // Adds what sounds at song samples p0 .. p0+n into dst[dstStart ..]; wrapAt > 0: samples from wrapAt on are a loop
    // starting again at 0. Real-time safe.
    void addTo (juce::AudioBuffer<float>& dst, int dstStart, int n, juce::int64 p0, double rate, juce::int64 wrapAt = 0) const
    {
        const float fade = (float) juce::jmax (1.0, fadeSeconds * rate);
        const int last = buf.getNumChannels() - 1, regionsNow = count.load();
        for (int ri = 0; ri < regionsNow; ++ri)
        {
            const auto& r = regions[(size_t) ri];
            const juce::int64 s = r.start.load(), i0 = r.in.load(), o = juce::jmin (r.out.load(), buf.getNumSamples());
            if (wrapAt <= 0 && (p0 + n <= s || p0 >= s + o - i0)) continue; // not in this block
            const bool fi = r.fadeIn.load(), fo = r.fadeOut.load();
            for (int k = 0; k < n; ++k)
            {
                const auto p = wrapAt > 0 && p0 + k >= wrapAt ? p0 + k - wrapAt : p0 + k;
                const auto q = p - s + i0; // the buffer sample heard at song sample p
                if (q < i0 || q >= o) continue;
                const float g = juce::jmin (1.0f, fi ? (float) (q - i0 + 1) / fade : 1.0f, fo ? (float) (o - q) / fade : 1.0f);
                for (int c = 0; c < dst.getNumChannels(); ++c) dst.addSample (c, dstStart + k, g * buf.getSample (juce::jmin (c, last), (int) q));
            }
        }
    }
};

// Renders a take offline with the same engine and settings as the live synth (adds release tail).
// instrument: the sampler set to play the take with (nullptr = the synth); its sample data is shared, not copied.
// audio: optional region played through the same engine (effects, Volume, pan), at sampleRate.
inline juce::AudioBuffer<float> renderOffline (const juce::MidiMessageSequence& take, const SynthParams& params, double sampleRate,
                                              juce::SynthesiserSound::Ptr instrument = nullptr, const AudioClip* audio = nullptr)
{
    SynthEngine engine;
    engine.params.copyFrom (params);
    engine.setInstrument (instrument);
    engine.prepare (sampleRate);
    const bool hasAudio = audio != nullptr && ! audio->empty();
    const int total = (int) ((juce::jmax (take.getEndTime(), hasAudio ? (double) audio->end() / sampleRate : 0.0) + params.release.load() + 0.5) * sampleRate);
    juce::AudioBuffer<float> in (2, 512);
    juce::AudioBuffer<float> out (2, total);
    juce::MidiBuffer midi;
    int idx = 0;
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = juce::jmin (512, total - pos);
        midi.clear();
        idx = addEventsInRange (take, idx, pos, n, sampleRate, midi, pos); // Synthesiser wants absolute buffer positions
        in.clear();
        if (hasAudio) audio->addTo (in, 0, n, pos, sampleRate);
        engine.render (out, pos, n, midi, hasAudio ? &in : nullptr);
    }
    return out;
}

// An audio file (WAV, MP3, FLAC, Ogg, AIFF; mono or its first 2 channels) at `rate`. Empty if missing, unreadable or
// longer than maxSeconds.
inline juce::AudioBuffer<float> readAudioFile (const juce::File& f, double rate, double maxSeconds)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0 || r->sampleRate <= 0 || r->lengthInSamples > (juce::int64) (r->sampleRate * maxSeconds))
        return juce::AudioBuffer<float> (1, 0);
    const int channels = juce::jlimit (1, 2, (int) r->numChannels);
    juce::AudioBuffer<float> raw (channels, (int) r->lengthInSamples);
    r->read (&raw, 0, raw.getNumSamples(), 0, true, channels > 1);
    if (std::abs (r->sampleRate - rate) < 1.0) return raw;
    const double ratio = r->sampleRate / rate;
    juce::AudioBuffer<float> out (channels, (int) (raw.getNumSamples() / ratio));
    for (int c = 0; c < channels; ++c)
    {
        juce::LagrangeInterpolator li;
        li.process (ratio, raw.getReadPointer (c), out.getWritePointer (c), out.getNumSamples());
    }
    return out;
}

inline bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, (unsigned int) audio.getNumChannels(), 24, {}, 0));
    if (writer == nullptr) return false;
    stream.release(); // the writer owns it now
    return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}
