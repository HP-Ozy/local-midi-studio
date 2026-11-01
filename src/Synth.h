#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// Written by the GUI thread, read by the audio thread once per block.
struct SynthParams
{
    std::atomic<int> wave { 1 };       // 0 sine, 1 saw, 2 square, 3 triangle, 4 noise, 5 pulse
    std::atomic<int> filterType { 0 }; // 0 low-pass, 1 high-pass, 2 band-pass
    std::atomic<float> attack { 0.005f }, decay { 0.2f }, sustain { 0.7f }, release { 0.3f };
    std::atomic<float> cutoff { 6000.0f }, resonance { 0.7f }, gain { 0.6f };
    std::atomic<int> lfoTarget { 0 };  // 0 pitch (vibrato), 1 filter cutoff, 2 volume (tremolo), 3 pulse width (PWM)
    std::atomic<float> pulseWidth { 0.5f }; // wave 5 (pulse) duty cycle, 0.05..0.95
    std::atomic<float> lfoRate { 5.0f }, lfoDepth { 0.0f };
    std::atomic<float> modWheel { 0.0f }; // CC1 / 127, written by the audio thread from incoming MIDI; adds to lfoDepth
    std::atomic<float> chorusMix { 0.0f }, delayMix { 0.0f }, delayTime { 0.35f }, delayFeedback { 0.35f },
                       reverbMix { 0.0f }, reverbSize { 0.5f }; // effects: a mix of 0 switches the effect off
    std::atomic<float> pan { 0.0f };   // -1 left .. +1 right (balance: the far side fades, centre = x1 on both)
    std::atomic<float> tune { 0.0f }, fine { 0.0f };          // whole voice: semitones, cents
    std::atomic<int> osc2Wave { 1 };                           // second oscillator, same waves as `wave`
    std::atomic<float> osc2Tune { 0.0f }, osc2Fine { 0.0f }, osc2Mix { 0.0f }; // relative to osc 1; mix 0 = osc 1 only

    // Built from forEach, so a new parameter can't be forgotten (the WAV export copies the sound with this).
    void copyFrom (const SynthParams& o)
    {
        std::vector<double> v;
        const_cast<SynthParams&> (o).forEach ([&] (const char*, auto& x, auto, auto) { v.push_back ((double) x.load()); });
        size_t i = 0;
        forEach ([&] (const char*, auto& x, auto lo, auto) { x = static_cast<decltype (lo)> (v[i++]); });
        modWheel = o.modWheel.load();
    }

    // Every user-settable sound parameter with its key (presets, MIDI map) and valid range. modWheel is
    // performance state, not sound, so it isn't here.
    template <typename F> void forEach (F&& f)
    {
        f ("wave", wave, 0, 5);            f ("filterType", filterType, 0, 2);  f ("lfoTarget", lfoTarget, 0, 3);
        f ("attack", attack, 0.001f, 5.0f); f ("decay", decay, 0.001f, 5.0f);  f ("sustain", sustain, 0.0f, 1.0f);
        f ("release", release, 0.001f, 8.0f);
        f ("cutoff", cutoff, 20.0f, 20000.0f); f ("resonance", resonance, 0.5f, 10.0f);
        f ("lfoRate", lfoRate, 0.1f, 20.0f);   f ("lfoDepth", lfoDepth, 0.0f, 1.0f); f ("pulseWidth", pulseWidth, 0.05f, 0.95f);
        f ("gain", gain, 0.0f, 1.0f);      f ("pan", pan, -1.0f, 1.0f);
        f ("chorusMix", chorusMix, 0.0f, 1.0f);
        f ("delayMix", delayMix, 0.0f, 1.0f); f ("delayTime", delayTime, 0.02f, 1.5f); f ("delayFeedback", delayFeedback, 0.0f, 0.9f);
        f ("reverbMix", reverbMix, 0.0f, 1.0f); f ("reverbSize", reverbSize, 0.0f, 1.0f);
        f ("tune", tune, -24.0f, 24.0f);      f ("fine", fine, -100.0f, 100.0f);
        f ("osc2Wave", osc2Wave, 0, 5);       f ("osc2Tune", osc2Tune, -24.0f, 24.0f); f ("osc2Fine", osc2Fine, -100.0f, 100.0f);
        f ("osc2Mix", osc2Mix, 0.0f, 1.0f);
    }
};

// Band-limited wavetables for saw, square and triangle (exact Fourier series of the naive shapes).
// Each table holds only harmonics 1..H; a note uses the largest H with H * f0 below Nyquist, so nothing aliases.
// Every count up to 16, then three tables per octave: the top harmonic stays above 0.38 x sample rate,
// or is already the last one that fits below Nyquist (no dull highs between tables).
struct Wavetables
{
    static constexpr int size = 8192; // 4096 left linear-interpolation error at -87 dBc on narrow pulses, low notes

    // Built once, on first use: call it from a non-audio thread first (the Voice constructor does).
    static const Wavetables& get() { static const Wavetables w; return w; }

    int tableFor (double dt) const // dt = f0 / sampleRate
    {
        const double maxH = 0.5 / juce::jmax (dt, 1e-9);
        const auto it = std::upper_bound (harmonics.begin(), harmonics.end(), maxH, [] (double v, int h) { return v < h; });
        return juce::jmax (0, (int) (it - harmonics.begin()) - 1);
    }

    float read (int w, double t, int table) const // w: 1 saw, 2 square, 3 triangle
    {
        const auto& tab = tables[(size_t) w - 1][(size_t) table];
        const double x = t * size;
        const int i = (int) x;
        return tab[(size_t) i] + (float) (x - i) * (tab[(size_t) i + 1] - tab[(size_t) i]);
    }

    std::vector<int> harmonics; // H per table, ascending

private:
    Wavetables()
    {
        for (int h = 1; h <= 16; ++h) harmonics.push_back (h); // every count: integer gaps would drop harmonics on high notes
        for (int i = 1; 16.0 * std::pow (2.0, i / 3.0) < size / 2; ++i)
            harmonics.push_back ((int) std::lround (16.0 * std::pow (2.0, i / 3.0)));

        std::vector<double> sinTab (size); // sin(2*pi*k*i/size) == sinTab[(k*i) % size], exact
        for (int i = 0; i < size; ++i) sinTab[(size_t) i] = std::sin (juce::MathConstants<double>::twoPi * i / size);
        const double pi = juce::MathConstants<double>::pi;
        for (int w = 1; w <= 3; ++w)
            for (int H : harmonics)
            {
                std::vector<double> acc (size, 0.0);
                for (int k = 1; k <= H; ++k)
                {
                    // saw 2t-1 = -(2/pi) sum sin/k ; square = (4/pi) sum_odd sin/k ; triangle = (8/pi^2) sum_odd cos/k^2
                    if (w != 1 && k % 2 == 0) continue;
                    const double a = w == 1 ? -2.0 / (pi * k) : w == 2 ? 4.0 / (pi * k) : 8.0 / (pi * pi * k * k);
                    const int shift = w == 3 ? size / 4 : 0; // cos(x) = sin(x + pi/2)
                    for (int i = 0; i < size; ++i) acc[(size_t) i] += a * sinTab[(size_t) (((juce::int64) k * i + shift) % size)];
                }
                std::vector<float> tab (size + 1); // + guard sample for interpolation
                for (int i = 0; i < size; ++i) tab[(size_t) i] = (float) acc[(size_t) i];
                tab[size] = tab[0];
                tables[(size_t) w - 1].push_back (std::move (tab));
            }
    }

    std::array<std::vector<std::vector<float>>, 3> tables;
};

// Linear ADSR, same shape as juce::ADSR. Replaces it because juce::ADSR::setParameters, which we call every block to
// follow the knobs, resets the release rate to sustain/release: a note released above sustain took 0.49 s instead of
// 0.2, and with sustain 0 the release ended at once (a cut). Here the release always lasts `release` seconds from the
// level the key was let go at, and sustain changes glide (full scale in 20 ms) instead of jumping.
class Envelope
{
public:
    void setSampleRate (double sr) { rate = sr; glideStep = (float) (1.0 / (0.02 * sr)); }
    void setParameters (float a, float d, float s, float r) { attack = a; decay = d; sustain = s; release = r; }
    void noteOn()  { stage = Stage::attack; }
    void noteOff() { if (stage != Stage::idle) { stage = Stage::release; releaseFrom = value; } }
    void reset()   { stage = Stage::idle; value = 0.0f; }
    bool isActive() const { return stage != Stage::idle; }

    float getNextSample()
    {
        switch (stage)
        {
            case Stage::idle: return 0.0f;
            case Stage::attack:
                value += step (1.0f, attack);
                if (value >= 1.0f) { value = 1.0f; stage = Stage::decay; }
                break;
            case Stage::decay:
                value -= step (1.0f - sustain, decay);
                if (value <= sustain) stage = Stage::sustain; // no snap: the sustain stage glides to the exact level
                break;
            case Stage::sustain:
                value += juce::jlimit (-glideStep, glideStep, sustain - value);
                break;
            case Stage::release:
                value -= step (releaseFrom, release);
                if (value <= 0.0f) reset();
                break;
        }
        return value;
    }

private:
    float step (float distance, float seconds) const { return seconds > 0.0f ? distance / (seconds * (float) rate) : 1.0f; }

    enum class Stage { idle, attack, decay, sustain, release } stage = Stage::idle;
    float value = 0.0f, attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.3f, releaseFrom = 0.0f, glideStep = 0.001f;
    double rate = 48000.0;
};

struct AnySound : juce::SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

class Voice : public juce::SynthesiserVoice
{
public:
    // Fixed seed per voice: noise is reproducible run to run (WAV export, tests) yet uncorrelated between voices.
    Voice (SynthParams& p, int index) : params (p), rng (index + 1) { Wavetables::get(); }

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<AnySound*> (s) != nullptr; }

    void setCurrentPlaybackSampleRate (double sr) override
    {
        SynthesiserVoice::setCurrentPlaybackSampleRate (sr);
        if (sr <= 0) return;
        adsr.setSampleRate (sr);
        filter.prepare ({ sr, 4096, 1 });
        cutoff.reset (sr, 0.02); // 20 ms glide so knob moves don't click
        lfoDepthSmoothed.reset (sr, 0.02);
        resonanceSmoothed.reset (sr, 0.02);
        widthSmoothed.reset (sr, 0.02);
        mix2Smoothed.reset (sr, 0.02);
        residualDecay = (float) std::exp (-1.0 / (0.002 * sr)); // 2 ms time constant
    }

    void startNote (int note, float velocity, juce::SynthesiserSound*, int pitchWheel) override
    {
        level = velocity * 0.3f; // headroom for many voices before the master soft-clip
        bend = pitchWheel;
        updatePitch();
        phase = phase2 = 0.0;
        lfoPhase = 0.0; // LFO restarts with each note: reproducible renders
        filter.reset();
        cutoff.setCurrentAndTargetValue (targetCutoff());
        lfoDepthSmoothed.setCurrentAndTargetValue (lfoDepth());
        widthSmoothed.setCurrentAndTargetValue (params.pulseWidth);
        mix2Smoothed.setCurrentAndTargetValue (params.osc2Mix);
        resonanceSmoothed.setCurrentAndTargetValue (params.resonance);
        filter.setResonance (params.resonance);
        updateParams();
        lastCutoff = -1.0f; // force the first coefficient update
        mod = lfoAt (0.0, lfoDepth()); // LFO state at the note's first sample
        modStep = {};
        lfoCountdown = 0;
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) adsr.noteOff();
        else
        {
            // Hard stop (voice stealing): don't drop to 0 in one sample, let the last value fade out.
            residual += last;
            last = 0.0f;
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int value) override { bend = value; updatePitch(); }
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        bool active = isVoiceActive();
        if (! active && residual == 0.0f) return;
        if (active) updateParams();
        const int w = params.wave.load(), w2 = params.osc2Wave.load();
        for (int i = 0; i < num; ++i)
        {
            float s = 0.0f;
            if (active)
            {
                // LFO at control rate: sin/exp2 once per lfoBlock samples, linear in between (no steps). At the
                // 20 Hz maximum the interpolation error is ~2e-4 of the depth (-73 dB); it cut vibrato cost by most.
                if (lfoCountdown == 0)
                {
                    lfoCountdown = lfoBlock;
                    lfoPhase += lfoStep * lfoBlock;
                    if (lfoPhase >= 1.0) lfoPhase -= std::floor (lfoPhase);
                    const auto target = lfoAt (lfoPhase, lfoDepthSmoothed.skip (lfoBlock));
                    for (size_t k = 0; k < 4; ++k) modStep[k] = (target[k] - mod[k]) / lfoBlock;
                }
                --lfoCountdown;
                for (size_t k = 0; k < 4; ++k) mod[k] += modStep[k];
                const float pitchMul = mod[0], cutMul = mod[1], amp = mod[2], width = widthSmoothed.getNextValue() + mod[3];
                if (resonanceSmoothed.isSmoothing()) filter.setResonance (resonanceSmoothed.getNextValue()); // a jump clicks
                const float fc = juce::jlimit (20.0f, (float) getSampleRate() * 0.45f, cutoff.getNextValue() * cutMul);
                if (fc != lastCutoff) { filter.setCutoffFrequency (fc); lastCutoff = fc; } // a tan() per call: skip when unchanged
                const float pw = juce::jlimit (0.05f, 0.95f, width), m2 = mix2Smoothed.getNextValue();
                float o = osc (w, phase, dt * pitchMul, table, pw);
                // mix 0: osc 1 bit-identical, osc 2 not computed (its phase waits; the mix ramps in from 0, so no click)
                if (m2 > 0.0f) o += m2 * (osc (w2, phase2, dt2 * pitchMul, table2, pw) - o);
                s = last = filter.processSample (0, o * level * amp * adsr.getNextSample());
                if (! adsr.isActive()) { clearCurrentNote(); active = false; last = 0.0f; }
            }
            s += residual;
            residual = std::abs (residual) < 1e-6f ? 0.0f : residual * residualDecay;
            for (int c = 0; c < out.getNumChannels(); ++c)
                out.addSample (c, start + i, s);
            if (! active && residual == 0.0f) break;
        }
    }

private:
    // What the LFO does at a phase and depth: { pitch multiplier, cutoff multiplier, amplitude, width offset }.
    std::array<float, 4> lfoAt (double ph, float d) const
    {
        std::array<float, 4> m { 1.0f, 1.0f, 1.0f, 0.0f };
        if (d <= 0.0f) return m;
        const float l = (float) std::sin (ph * juce::MathConstants<double>::twoPi);
        if (lfoTarget == 0)      m[0] = std::exp2 (d * l / 12.0f);    // +-1 semitone at full depth
        else if (lfoTarget == 1) m[1] = std::exp2 (2.0f * d * l);     // +-2 octaves
        else if (lfoTarget == 2) m[2] = 1.0f - d * 0.5f * (1.0f - l); // down to 1 - depth
        else                     m[3] = 0.45f * d * l;               // PWM
        return m;
    }

    float targetCutoff() const { return juce::jmin (params.cutoff.load(), (float) getSampleRate() * 0.45f); }

    void updatePitch()
    {
        const double semis = (bend - 8192) / 8192.0 * 2.0 + params.tune + params.fine / 100.0; // +-2 semitone bend range
        const auto note = getCurrentlyPlayingNote();
        dt = juce::MidiMessage::getMidiNoteInHertz (note) * std::pow (2.0, semis / 12.0) / getSampleRate();
        dt2 = dt * std::pow (2.0, (params.osc2Tune + params.osc2Fine / 100.0) / 12.0);
        table = Wavetables::get().tableFor (dt * 1.06); // room for +1 semitone of vibrato without aliasing
        table2 = Wavetables::get().tableFor (dt2 * 1.06);
    }

    float lfoDepth() const { return juce::jlimit (0.0f, 1.0f, params.lfoDepth.load() + params.modWheel.load()); }

    void updateParams()
    {
        adsr.setParameters (params.attack, params.decay, params.sustain, params.release);
        using T = juce::dsp::StateVariableTPTFilterType;
        const T types[] { T::lowpass, T::highpass, T::bandpass };
        filter.setType (types[juce::jlimit (0, 2, params.filterType.load())]);
        resonanceSmoothed.setTargetValue (params.resonance);
        cutoff.setTargetValue (targetCutoff());
        lfoDepthSmoothed.setTargetValue (lfoDepth());
        widthSmoothed.setTargetValue (params.pulseWidth);
        mix2Smoothed.setTargetValue (params.osc2Mix);
        updatePitch(); // tuning knobs act on held notes; the phase runs on, so no click
        lfoStep = juce::jlimit (0.01, 50.0, (double) params.lfoRate.load()) / getSampleRate();
        lfoTarget = params.lfoTarget.load();
    }

public:
    // One sample of waveform w (0 sine, 1 saw, 2 square, 3 triangle, 5 pulse) at phase t in [0, 1) with phase step dt.
    static float shape (int w, double t, double dt, double width = 0.5)
    {
        return w == 0 ? (float) std::sin (t * juce::MathConstants<double>::twoPi)
             : w == 5 ? pulse (t, width, Wavetables::get().tableFor (dt))
                      : Wavetables::get().read (w, t, Wavetables::get().tableFor (dt));
    }

    // Band-limited pulse of any width = saw minus the same saw shifted by the width:
    // high (2 - 2w) for a fraction w of the cycle, low (-2w) otherwise; zero mean, 2 peak to peak.
    static float pulse (double t, double width, int table)
    {
        const double t2 = t + width;
        return Wavetables::get().read (1, t, table) - Wavetables::get().read (1, t2 >= 1.0 ? t2 - 1.0 : t2, table);
    }

private:
    float osc (int w, double& ph, double step, int tab, float width)
    {
        const float v = w == 0 ? (float) std::sin (ph * juce::MathConstants<double>::twoPi)
                      : w == 5 ? pulse (ph, width, tab)
                      : w < 4  ? Wavetables::get().read (w, ph, tab)
                               : rng.nextFloat() * 2.0f - 1.0f;
        ph += step;
        if (ph >= 1.0) ph -= 1.0;
        return v;
    }

    SynthParams& params;
    Envelope adsr;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 6000.0f };
    juce::SmoothedValue<float> lfoDepthSmoothed, widthSmoothed, resonanceSmoothed, mix2Smoothed;
    juce::Random rng;
    double phase = 0.0, dt = 0.0, phase2 = 0.0, dt2 = 0.0, lfoPhase = 0.0, lfoStep = 0.0;
    static constexpr int lfoBlock = 16;
    std::array<float, 4> mod { 1.0f, 1.0f, 1.0f, 0.0f }, modStep {}; // interpolated LFO effect, see lfoAt
    int lfoCountdown = 0;
    float lastCutoff = -1.0f;
    int lfoTarget = 0;
    float level = 0.0f;
    float last = 0.0f, residual = 0.0f, residualDecay = 0.0f; // last output, and what's left of a cut-off note
    int bend = 8192, table = 0, table2 = 0;
};

// ---- Sampler: recorded audio mapped to keys and velocities (loaded by Sfz.h, played by SampleVoice) ----
struct SampleRegion
{
    juce::File file;
    int lokey = 0, hikey = 127, lovel = 1, hivel = 127, root = 60;
    double tuneCents = 0, volumeDb = 0;
    bool oneShot = false, loop = false;      // one-shot: plays to the end, ignores note-off (drums)
    juce::int64 loopStart = 0, loopEnd = 0;  // in file samples, end exclusive
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // shared by regions using the same file
    double fileRate = 44100.0;
    float ampVeltrack = 1.0f;                // 0..1: how much velocity changes the level (SFZ amp_veltrack / 100)
    float release = -1.0f;                   // seconds; < 0 = use the Release knob
    float lorand = 0.0f, hirand = 1.0f;      // random alternate takes: played when lorand <= r < hirand
    int seqLength = 1, seqPosition = 1;      // round robin: played on the seqPosition-th hit of every seqLength
    int group = 0, offBy = 0;                // choke: starting a region of group G stops regions with offBy == G
    bool keytrack = true;                    // false: never transposed (noises, drums)
};

struct InstrumentSound : juce::SynthesiserSound
{
    juce::String name;
    std::vector<SampleRegion> regions;
    float gain = 1.0f; // same for every region: brings the loudest sample of the set to full scale (keeps its balance)
    std::array<std::atomic<int>, 128> hits {};      // per-key hit counter for round robin
    std::array<std::atomic<int>, 256> chokeEpoch {}; // bumped when a region of that group starts (lock-free choke)

    bool appliesToNote (int note) override { for (auto& r : regions) if (note >= r.lokey && note <= r.hikey) return true; return false; }
    bool appliesToChannel (int) override   { return true; }
    // First region matching key, velocity, the random draw r in [0,1) and the key's round-robin turn.
    const SampleRegion* find (int note, int velocity127, float r = 0.0f)
    {
        const int hit = hits[(size_t) juce::jlimit (0, 127, note)]++;
        for (auto& g : regions)
            if (note >= g.lokey && note <= g.hikey && velocity127 >= g.lovel && velocity127 <= g.hivel
                && r >= g.lorand && r < g.hirand && hit % g.seqLength == g.seqPosition - 1)
                return &g;
        return nullptr;
    }
};

class SampleVoice : public juce::SynthesiserVoice
{
public:
    explicit SampleVoice (SynthParams& p) : params (p) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<InstrumentSound*> (s) != nullptr; }

    void setCurrentPlaybackSampleRate (double sr) override
    {
        SynthesiserVoice::setCurrentPlaybackSampleRate (sr);
        if (sr <= 0) return;
        env.setSampleRate (sr);
        residualDecay = (float) std::exp (-1.0 / (0.002 * sr)); // same 2 ms declick as the synth voices
    }

    void startNote (int note, float velocity, juce::SynthesiserSound* s, int pitchWheel) override
    {
        sound = static_cast<InstrumentSound*> (s);
        region = sound->find (note, juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f)), rng.nextFloat());
        if (region == nullptr || region->audio == nullptr || region->audio->getNumSamples() < 2) { clearCurrentNote(); return; }
        if (region->group > 0) ++sound->chokeEpoch[(size_t) (region->group & 255)];
        chokedAt = region->offBy > 0 ? sound->chokeEpoch[(size_t) (region->offBy & 255)].load() : 0;
        noteNumber = note;
        bend = pitchWheel;
        updateStep();
        pos = 0.0;
        const float vel = (1.0f - region->ampVeltrack) + region->ampVeltrack * velocity * velocity; // vel^2 = SFZ's default curve
        gain = 0.3f * sound->gain * vel * juce::Decibels::decibelsToGain ((float) region->volumeDb);
        env.setParameters (0.001f, 0.0f, 1.0f, releaseTime());
        env.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) { if (region == nullptr || ! region->oneShot) env.noteOff(); return; }
        residual[0] += last[0]; residual[1] += last[1]; // hard stop (stealing): fade the last value out
        last[0] = last[1] = 0.0f;
        env.reset();
        clearCurrentNote();
    }

    void pitchWheelMoved (int value) override { bend = value; updateStep(); }
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        bool active = isVoiceActive() && region != nullptr;
        if (! active && residual[0] == 0.0f && residual[1] == 0.0f) return;
        // Only touch the region while the note holds the sound (after a stop the residual alone fades out).
        const juce::AudioBuffer<float>* a = active ? region->audio.get() : nullptr;
        const int len = a != nullptr ? a->getNumSamples() : 0, chans = a != nullptr ? juce::jmin (2, a->getNumChannels()) : 0;
        const bool looping = active && region->loop && region->loopEnd > region->loopStart + 1;
        const auto loopLen = active ? region->loopEnd - region->loopStart : 0;
        auto sampleAt = [&] (int c, juce::int64 i)
        {
            if (looping && i >= region->loopEnd) i -= loopLen;
            return a->getSample (c, (int) juce::jlimit<juce::int64> (0, len - 1, i));
        };
        if (active)
        {
            env.setParameters (0.001f, 0.0f, 1.0f, releaseTime());
            if (region->offBy > 0 && sound->chokeEpoch[(size_t) (region->offBy & 255)].load() != chokedAt)
            {
                env.setParameters (0.001f, 0.0f, 1.0f, 0.005f); // choked (e.g. open hi-hat by the closed one): 5 ms fade
                env.noteOff();
                chokedAt = sound->chokeEpoch[(size_t) (region->offBy & 255)].load();
            }
        }
        for (int i = 0; i < num; ++i)
        {
            float s[2] = { 0.0f, 0.0f };
            if (active)
            {
                const auto i0 = (juce::int64) pos;
                const float t = (float) (pos - (double) i0), e = env.getNextSample() * gain;
                for (int c = 0; c < chans; ++c)
                    s[c] = last[c] = e * hermite (sampleAt (c, i0 - 1), sampleAt (c, i0), sampleAt (c, i0 + 1), sampleAt (c, i0 + 2), t);
                if (chans == 1) s[1] = last[1] = s[0];
                pos += step;
                if (looping && pos >= (double) region->loopEnd) pos -= (double) loopLen;
                if ((! looping && pos >= len - 1) || ! env.isActive())
                {
                    residual[0] += last[0]; residual[1] += last[1]; // a sample that ends abruptly still fades
                    last[0] = last[1] = 0.0f;
                    env.reset();
                    clearCurrentNote();
                    active = false;
                }
            }
            for (int c = 0; c < 2; ++c)
            {
                s[c] += residual[c];
                residual[c] = std::abs (residual[c]) < 1e-6f ? 0.0f : residual[c] * residualDecay;
            }
            for (int c = 0; c < out.getNumChannels(); ++c) out.addSample (c, start + i, s[juce::jmin (c, 1)]);
            if (! active && residual[0] == 0.0f && residual[1] == 0.0f) break;
        }
    }

    // 4-point, 3rd-order Hermite interpolation (good enough for the +-6 semitone transpositions of sample sets).
    static float hermite (float xm1, float x0, float x1, float x2, float t)
    {
        const float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b = w + a;
        return ((a * t - b) * t + c) * t + x0;
    }

private:
    void updateStep()
    {
        const double semis = (region->keytrack ? noteNumber - region->root + (bend - 8192) / 8192.0 * 2.0 : 0.0) + region->tuneCents / 100.0;
        step = std::pow (2.0, semis / 12.0) * region->fileRate / getSampleRate();
    }

    float releaseTime() const { return region->release >= 0.0f ? region->release : params.release.load(); }

    SynthParams& params;
    InstrumentSound* sound = nullptr;
    const SampleRegion* region = nullptr;
    juce::Random rng { 12345 };
    int chokedAt = 0;
    Envelope env;
    double pos = 0.0, step = 1.0;
    float gain = 0.0f, last[2] {}, residual[2] {}, residualDecay = 0.0f;
    int noteNumber = 60, bend = 8192;
};

// GUI-independent engine: juce::Synthesiser gives voice allocation, stealing and sustain pedal.
class SynthEngine
{
public:
    explicit SynthEngine (int voices = 32)
    {
        for (int i = 0; i < voices; ++i) synth.addVoice (new Voice (params, i));
        for (int i = 0; i < voices; ++i) synth.addVoice (new SampleVoice (params));
        setInstrument (nullptr);
    }

    // Message thread. nullptr = the synth. Order matters: no new notes on the old sound, cut what is playing
    // (2 ms declick) so the voices drop their references here, then free the old sound outside the audio thread.
    void setInstrument (juce::SynthesiserSound::Ptr instrument)
    {
        auto old = current;
        current = instrument != nullptr ? instrument : juce::SynthesiserSound::Ptr (new AnySound());
        synth.clearSounds();
        synth.allNotesOff (0, false);
        synth.addSound (current);
    }

    juce::SynthesiserSound::Ptr instrument() const { return dynamic_cast<InstrumentSound*> (current.get()) != nullptr ? current : nullptr; }

    void prepare (double sampleRate)
    {
        synth.setCurrentPlaybackSampleRate (sampleRate);
        gainSmoothed.reset (sampleRate, 0.02); // a Volume jump clicked at 14x the waveform's own slope
        gainSmoothed.setCurrentAndTargetValue (params.gain);
        panSmoothed.reset (sampleRate, 0.02);
        panSmoothed.setCurrentAndTargetValue (params.pan);

        // Effects: everything allocated here, never on the audio thread; processed in chunks of <= maxChunk.
        rate = sampleRate;
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxChunk, 2 };
        chorus.prepare (spec);
        chorus.setRate (0.8f); chorus.setDepth (0.3f); chorus.setCentreDelay (8.0f); chorus.setFeedback (0.0f);
        delay.setMaximumDelayInSamples ((int) (1.6 * sampleRate));
        delay.prepare (spec);
        reverb.setSampleRate (sampleRate);
        delayMixSmoothed.reset (sampleRate, 0.02);
        chorusGain.reset (sampleRate, 0.05); // same ramp as dsp::Chorus's own dry/wet smoothing
        chorusGain.setCurrentAndTargetValue (1.0f);
        delayTimeSmoothed.reset (sampleRate, 0.1); // time changes glide like tape instead of crackling
        delayMixSmoothed.setCurrentAndTargetValue (params.delayMix);
        delayTimeSmoothed.setCurrentAndTargetValue (params.delayTime);
    }

    // `in`: optional audio (num samples from 0, mono or stereo, e.g. a voice) mixed with the voices before effects and Volume
    void render (juce::AudioBuffer<float>& buf, int start, int num, const juce::MidiBuffer& midi, const juce::AudioBuffer<float>* in = nullptr)
    {
        buf.clear (start, num);
        for (const auto meta : midi) // mod wheel is engine-wide, so notes started later get it too
            if (meta.getMessage().isControllerOfType (1)) params.modWheel = meta.getMessage().getControllerValue() / 127.0f;
        synth.renderNextBlock (buf, midi, start, num);
        if (in != nullptr) for (int c = 0; c < buf.getNumChannels(); ++c) buf.addFrom (c, start, *in, juce::jmin (c, in->getNumChannels() - 1), 0, num);
        applyEffects (buf, start, num);
        gainSmoothed.setTargetValue (params.gain);
        panSmoothed.setTargetValue (params.pan);
        float blockPeak = 0;
        int hits = 0;
        auto* const* ch = buf.getArrayOfWritePointers();
        const bool stereo = buf.getNumChannels() == 2;
        for (int i = start; i < start + num; ++i)
        {
            const float g = gainSmoothed.getNextValue(), p = panSmoothed.getNextValue();
            for (int c = 0; c < buf.getNumChannels(); ++c)
            {
                // Balance pan: the signal is already stereo (reverb), so the far side fades and the centre stays x1 exactly
                const float x = ch[c][i] * g * (stereo ? juce::jmin (1.0f, c == 0 ? 1.0f - p : 1.0f + p) : 1.0f);
                const float a = std::abs (x);
                blockPeak = juce::jmax (blockPeak, a);
                hits += a > 0.8f ? 1 : 0;
                ch[c][i] = safetyClip (x);
            }
        }
        // Diagnostics for the GUI: it resets peakLevel itself; a racing lost maximum only affects the display.
        if (blockPeak > peakLevel.load()) peakLevel = blockPeak;
        if (hits > 0) limiterHits += hits;
        int n = 0; // counted here so the GUI never touches the Synthesiser (its lock is held for the whole render)
        for (int i = 0; i < synth.getNumVoices(); ++i) n += synth.getVoice (i)->isVoiceActive() ? 1 : 0;
        active = n;
    }

    int activeVoices() const { return active; }

    // Chorus -> delay -> reverb, before Volume and the safety clip. Always running, like send effects: a mix of 0
    // only silences the wet part (dry stays exactly x1), and turning one up never starts from an empty buffer
    // (a delay switched on mid-note would otherwise start its echo abruptly mid-waveform: a click).
    void applyEffects (juce::AudioBuffer<float>& buf, int start, int num)
    {
        delayMixSmoothed.setTargetValue (params.delayMix);
        delayTimeSmoothed.setTargetValue (params.delayTime);
        const int chans = juce::jmin (2, buf.getNumChannels());
        auto* const* ch = buf.getArrayOfWritePointers();
        const float fb = params.delayFeedback;

        for (int pos = start; pos < start + num; pos += maxChunk)
        {
            const int n = juce::jmin (maxChunk, start + num - pos);
            // dsp::Chorus mixes dry and wet linearly; at 50/50 the partly decorrelated halves lose ~3 dB (measured
            // -3.09 dB), so engaging the chorus made the sound quieter. Equal-power compensation, x1 exactly at mix 0.
            const float w = 0.5f * params.chorusMix; // 0.5 = equal dry/wet, the fullest chorus
            chorus.setMix (w);
            chorusGain.setTargetValue (1.0f / std::sqrt ((1.0f - w) * (1.0f - w) + w * w));
            juce::dsp::AudioBlock<float> block (ch, (size_t) chans, (size_t) pos, (size_t) n);
            chorus.process (juce::dsp::ProcessContextReplacing<float> (block));
            for (int i = pos; i < pos + n; ++i)
            {
                const float g = chorusGain.getNextValue();
                for (int c = 0; c < chans; ++c) ch[c][i] *= g;
            }

            for (int i = pos; i < pos + n; ++i)
            {
                delay.setDelay ((float) (delayTimeSmoothed.getNextValue() * rate));
                const float m = delayMixSmoothed.getNextValue();
                for (int c = 0; c < chans; ++c)
                {
                    const float in = ch[c][i], d = delay.popSample (c);
                    delay.pushSample (c, in + fb * d);
                    ch[c][i] = in + m * d;
                }
            }

            juce::Reverb::Parameters p; // juce::Reverb smooths its gains; it scales wet x3 and dry x2 internally
            p.roomSize = params.reverbSize;
            p.damping = 0.5f;
            p.wetLevel = 0.33f * params.reverbMix;
            p.dryLevel = 0.5f;
            p.width = 1.0f;
            reverb.setParameters (p);
            if (chans == 2) reverb.processStereo (ch[0] + pos, ch[1] + pos, n);
            else            reverb.processMono (ch[0] + pos, n);
        }
    }

    // Linear up to 0.8 (-2 dBFS) so normal playing is untouched; above, a tanh knee with the same slope
    // at the threshold that approaches 1.0 (reaches it only by float rounding). A safety net, not a sound.
    static float safetyClip (float x)
    {
        constexpr float t = 0.8f;
        const float a = std::abs (x);
        return a <= t ? x : std::copysign (t + (1.0f - t) * std::tanh ((a - t) / (1.0f - t)), x);
    }

    std::atomic<int> active { 0 };
    std::atomic<float> peakLevel { 0.0f };   // highest level before the safety clip since the GUI last reset it
    std::atomic<juce::int64> limiterHits { 0 }; // samples the safety clip has shaped, ever
    SynthParams params;
    juce::Synthesiser synth;
    juce::SmoothedValue<float> gainSmoothed { 0.6f }, panSmoothed;
    juce::SynthesiserSound::Ptr current; // AnySound (synth) or an InstrumentSound (sampler)

    static constexpr int maxChunk = 512;
    double rate = 48000.0;
    juce::dsp::Chorus<float> chorus;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 2 };
    juce::Reverb reverb;
    juce::SmoothedValue<float> delayMixSmoothed, delayTimeSmoothed, chorusGain { 1.0f };
};
