// Offline render test: drives the real engine with MIDI, no hardware or audio device needed.
// Also: `RenderTest <folder>` writes reference WAVs, `RenderTest --stress <minutes>` runs the real-time stress test.
#ifdef _WIN32
 #define NOMINMAX
 #include <windows.h>
 #include <psapi.h> // process memory for the stress test's leak check
#endif
#include <juce_audio_devices/juce_audio_devices.h>
#include "Synth.h"
#include "MidiQueue.h"
#include "Recording.h"
#include "Presets.h"
#include "MidiMap.h"
#include "Sfz.h"
#include "Library.h"
#include "Song.h"
#include "SongFile.h"
#include <thread>
#include <set>

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

constexpr double sr = 48000.0;
constexpr int block = 256;

// Renders `seconds`, sending `events` at the first sample. Returns the rendered mono channel.
static std::vector<float> run (SynthEngine& e, double seconds, std::initializer_list<juce::MidiMessage> events = {})
{
    juce::AudioBuffer<float> buf (2, block);
    std::vector<float> out;
    juce::MidiBuffer midi;
    for (auto& m : events) midi.addEvent (m, 0);
    for (int done = 0; done < seconds * sr; done += block)
    {
        e.render (buf, 0, block, midi);
        midi.clear();
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
    }
    return out;
}

static float rms (const std::vector<float>& v)  { double s = 0; for (auto x : v) s += x * x; return (float) std::sqrt (s / v.size()); }
static float peak (const std::vector<float>& v) { float p = 0; for (auto x : v) p = std::max (p, std::abs (x)); return p; }

static std::unique_ptr<SynthEngine> fresh (int wave = 1)
{
    auto e = std::make_unique<SynthEngine>();
    e->params.wave = wave;
    e->prepare (sr);
    return e;
}

using M = juce::MidiMessage;

// Reference renders for listening before/after a change: `RenderTest <folder>`. Same offline path as Export WAV,
// each normalized to -20 dBFS RMS so comparisons are at equal loudness.
static void writeReferences (const juce::File& dir)
{
    struct Note { int n; double on, off; float vel; };
    auto make = [&] (const char* name, std::vector<Note> notes, auto setParams, std::vector<M> extra = {})
    {
        juce::MidiMessageSequence take;
        for (auto& x : notes)
        {
            take.addEvent (M::noteOn (1, x.n, x.vel).withTimeStamp (x.on));
            take.addEvent (M::noteOff (1, x.n).withTimeStamp (x.off));
        }
        for (auto& m : extra) take.addEvent (m);
        take.updateMatchedPairs();
        SynthParams p;
        setParams (p);
        auto audio = renderOffline (take, p, 48000.0);
        double sq = 0;
        for (int i = 0; i < audio.getNumSamples(); ++i) sq += (double) audio.getSample (0, i) * audio.getSample (0, i);
        const float rmsNow = (float) std::sqrt (sq / audio.getNumSamples());
        float g = juce::Decibels::decibelsToGain (-20.0f) / juce::jmax (rmsNow, 1e-9f);
        g = juce::jmin (g, 0.99f / juce::jmax (audio.getMagnitude (0, audio.getNumSamples()), 1e-9f)); // never clip the file
        audio.applyGain (g);
        const auto f = dir.getChildFile (juce::String (name) + ".wav");
        const auto path = f.getFullPathName(); // keep the String alive while printf reads its buffer
        std::printf ("%s %s (%.1f s, gain %+.1f dB)\n", writeWav (audio, 48000.0, f) ? "wrote" : "FAILED", path.toRawUTF8(),
                     audio.getNumSamples() / 48000.0, juce::Decibels::gainToDecibels (g));
    };
    dir.createDirectory();
    make ("01_saw_chord", { { 48, 0, 2.5, 0.8f }, { 52, 0, 2.5, 0.8f }, { 55, 0, 2.5, 0.8f }, { 60, 0, 2.5, 0.8f } },
          [] (SynthParams& p) { p.wave = 1; p.cutoff = 3000.0f; });
    make ("02_square_bass", { { 33, 0, 0.45, 1 }, { 36, 0.5, 0.95, 1 }, { 40, 1.0, 1.45, 1 }, { 31, 1.5, 1.95, 1 } },
          [] (SynthParams& p) { p.wave = 2; p.cutoff = 800.0f; p.resonance = 3.0f; p.decay = 0.2f; p.sustain = 0.4f; p.release = 0.1f; });
    make ("03_pulse_pwm", { { 57, 0, 3, 0.8f }, { 61, 0, 3, 0.8f }, { 64, 0, 3, 0.8f } },
          [] (SynthParams& p) { p.wave = 5; p.lfoTarget = 3; p.lfoDepth = 0.8f; p.lfoRate = 1.0f; p.cutoff = 5000.0f; });
    std::vector<M> wheel;
    for (int k = 0; k <= 20; ++k) wheel.push_back (M::controllerEvent (1, 1, k * 127 / 20).withTimeStamp (1.0 + k * 0.05));
    make ("04_sine_vibrato_modwheel", { { 69, 0, 3, 0.9f } }, [] (SynthParams& p) { p.wave = 0; p.lfoRate = 5.5f; }, wheel);
    make ("05_filter_sweep", { { 45, 0, 4, 1 } },
          [] (SynthParams& p) { p.wave = 1; p.lfoTarget = 1; p.lfoDepth = 1.0f; p.lfoRate = 0.5f; p.cutoff = 800.0f; p.resonance = 6.0f; });
    std::vector<Note> arp;
    int i = 0;
    for (int n : { 84, 88, 91, 96, 100, 103, 108, 103, 100, 96 }) { arp.push_back ({ n, i * 0.25, i * 0.25 + 0.22, 0.9f }); ++i; }
    make ("06_high_saw_arpeggio", arp, [] (SynthParams& p) { p.wave = 1; p.cutoff = 20000.0f; p.release = 0.1f; });
    make ("07_triangle_tremolo", { { 62, 0, 2.5, 0.9f }, { 66, 0, 2.5, 0.9f }, { 69, 0, 2.5, 0.9f } },
          [] (SynthParams& p) { p.wave = 3; p.lfoTarget = 2; p.lfoDepth = 0.7f; p.lfoRate = 6.0f; });
    std::vector<Note> plucks;
    i = 0;
    for (int n : { 60, 64, 67, 72, 67, 64, 60 }) { plucks.push_back ({ n, i * 0.3, i * 0.3 + 0.12, 0.9f }); ++i; }
    make ("08_pluck_sustain0", plucks, [] (SynthParams& p) { p.wave = 3; p.decay = 0.25f; p.sustain = 0.0f; p.release = 0.2f; p.cutoff = 3000.0f; });
}

static double memoryMb() // this process's private memory
{
   #ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc)))
        return pmc.PrivateUsage / (1024.0 * 1024.0);
   #endif
    return 0.0;
}

// `RenderTest --probe <file.sfz>`: loads a real sample library, reports load time/memory/regions and plays a few
// notes across its range at three velocities, checking every sample is finite and bounded.
// `RenderTest --probe <file.mid>`: lists the parts the app would open (drums, then one line per track).
static int runProbe (const juce::File& f)
{
    if (f.hasFileExtension ("mid;midi"))
    {
        juce::FileInputStream in (f);
        MidiParts mp;
        if (! in.openedOk() || ! readMidiParts (in, mp)) { std::printf ("PROBE FAILED: not a MIDI file\n"); return 1; }
        auto line = [] (const char* name, const juce::MidiMessageSequence& s)
        {
            int n = 0; for (auto* e : s) n += e->message.isNoteOn() ? 1 : 0;
            std::printf ("  %-10s %4d notes, ends %.2f s\n", name, n, s.getEndTime());
        };
        std::printf ("%s: %.1f BPM\n", f.getFileName().toRawUTF8(), mp.bpm);
        line ("Drums", mp.drums);
        for (auto& p : mp.parts) line (p.name.toRawUTF8(), p.seq);
        std::printf ("PROBE OK\n");
        return 0;
    }
    const double m0 = memoryMb(), t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    auto inst = Sfz::load (f, error);
    if (inst == nullptr) { std::printf ("PROBE FAILED: %s\n", error.toRawUTF8()); return 1; }
    const auto* s = static_cast<InstrumentSound*> (inst.get());
    int lo = 127, hi = 0;
    for (auto& r : s->regions) { lo = std::min (lo, r.lokey); hi = std::max (hi, r.hikey); }
    std::printf ("Loaded %s: %zu regions, keys %d..%d, %.1f s, +%.0f MB\n", f.getFileName().toRawUTF8(), s->regions.size(), lo, hi,
                 (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, memoryMb() - m0);
    juce::AudioBuffer<float> b (2, 480);
    bool ok = true;
    for (int note = lo; note <= hi; note += std::max (1, (hi - lo) / 8))
        for (float vel : { 0.2f, 0.6f, 1.0f })
        {
            SynthEngine e; // fresh each time: the previous note's release tail must not leak into this measurement
            e.setInstrument (inst);
            e.prepare (48000.0);
            juce::MidiBuffer ev;
            ev.addEvent (juce::MidiMessage::noteOn (1, note, vel), 0);
            float pk = 0;
            for (int i = 0; i < 50; ++i) // 0.5 s
            {
                e.render (b, 0, 480, ev);
                if (i == 0) ev.clear();
                if (i == 25) ev.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                for (int c = 0; c < 2; ++c) for (int k = 0; k < 480; ++k) { const float x = b.getSample (c, k); ok &= std::isfinite (x); pk = std::max (pk, std::abs (x)); }
            }
            std::printf ("  note %3d vel %.1f: peak %.3f\n", note, vel, pk);
            ok &= pk <= 1.0f;
        }
    std::printf (ok ? "PROBE OK\n" : "PROBE FAILED: non-finite or overloaded output\n");
    return ok ? 0 : 1;
}

// Real-time stress: the real default output device, the app's engine and lock-free MIDI path, a "keyboard"
// thread hammering MIDI and a "GUI" thread turning knobs and switching presets. Every block is checked and then
// silenced, so nothing reaches the headphones. Prints one line per minute; returns 0 if nothing went wrong.
static int runStress (int minutes)
{
    struct Stress : juce::AudioIODeviceCallback
    {
        SynthEngine engine;
        MidiQueue queue;
        juce::MidiBuffer midi;
        double rate = 48000.0;
        std::atomic<double> worstMs { 0.0 }, budgetMs { 10.0 };
        std::atomic<juce::int64> blocks { 0 }, overBudget { 0 }, badSamples { 0 };
        std::atomic<float> peakOut { 0.0f };
        std::atomic<int> maxVoices { 0 };

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            rate = d->getCurrentSampleRate();
            budgetMs = d->getCurrentBufferSizeSamples() * 1000.0 / rate;
            engine.prepare (rate);
            midi.ensureSize (MidiQueue::capacity * 16);
        }
        void audioDeviceStopped() override {}

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            juce::AudioBuffer<float> buf (out, numOut, n); // refers to the device buffers, no allocation
            midi.clear();
            queue.popInto (midi, n, rate, MidiQueue::now());
            engine.render (buf, 0, n, midi);
            float pk = 0;
            int bad = 0;
            for (int c = 0; c < numOut; ++c)
                for (int i = 0; i < n; ++i) { const float x = out[c][i]; bad += std::isfinite (x) ? 0 : 1; pk = std::max (pk, std::abs (x)); }
            buf.clear(); // silent: checked, never heard
            const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
            if (ms > worstMs.load()) worstMs = ms;
            if (ms > budgetMs.load()) ++overBudget;
            if (pk > peakOut.load()) peakOut = pk;
            badSamples += bad;
            maxVoices = std::max (maxVoices.load(), engine.activeVoices());
            ++blocks;
        }
    } s;


    juce::AudioDeviceManager dm;
    const auto err = dm.initialiseWithDefaultDevices (0, 2);
    auto* dev = dm.getCurrentAudioDevice();
    if (err.isNotEmpty() || dev == nullptr) { std::printf ("Stress: no audio device (%s)\n", err.toRawUTF8()); return 2; }
    std::printf ("Stress: %d min on '%s', %.0f Hz, %d-sample buffer (%.1f ms budget), output silenced\n", minutes,
                 dev->getName().toRawUTF8(), dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples(),
                 dev->getCurrentBufferSizeSamples() * 1000.0 / dev->getCurrentSampleRate());
    dm.addAudioCallback (&s);

    std::atomic<bool> stop { false };
    std::atomic<juce::int64> midiEvents { 0 }, knobMoves { 0 }, presetLoads { 0 };
    std::thread keyboard ([&]
    {
        juce::Random r (1);
        std::vector<int> held;
        auto send = [&] (const juce::MidiMessage& m) { s.queue.push (m, MidiQueue::now()); ++midiEvents; };
        for (int step = 0; ! stop; ++step)
        {
            const int what = r.nextInt (100);
            if (what < 45)      { const int n = 24 + r.nextInt (85); send (M::noteOn (1, n, 0.1f + 0.9f * r.nextFloat())); held.push_back (n); }
            else if (what < 85) { if (! held.empty()) { const auto k = (size_t) r.nextInt ((int) held.size()); send (M::noteOff (1, held[k])); held.erase (held.begin() + (long) k); } }
            else if (what < 90) send (M::controllerEvent (1, 1, r.nextInt (128)));   // mod wheel
            else if (what < 95) send (M::pitchWheel (1, r.nextInt (16384)));
            else if (what < 98) send (M::controllerEvent (1, 64, r.nextBool() ? 127 : 0)); // sustain pedal
            else if (what < 99) for (int k = 0; k < 40; ++k) { const int n = 24 + r.nextInt (85); send (M::noteOn (1, n, 1.0f)); held.push_back (n); } // steal storm
            else                { send (M::allNotesOff (1)); held.clear(); }
            std::this_thread::sleep_for (std::chrono::milliseconds (5 + r.nextInt (40)));
        }
    });
    std::thread gui ([&]
    {
        juce::Random r (2);
        for (int step = 0; ! stop; ++step)
        {
            int k = 0;
            const int target = r.nextInt (19);
            s.engine.params.forEach ([&] (const char*, auto& v, auto lo, auto hi)
            {
                if (k++ == target) v = static_cast<decltype (lo)> (lo + (hi - lo) * r.nextDouble());
            });
            ++knobMoves;
            if (step % 200 == 199) // every ~10 s
            {
                juce::String name;
                const auto& f = Presets::factory()[(size_t) r.nextInt ((int) Presets::factory().size())];
                Presets::fromJson (Presets::factoryJson (f), s.engine.params, name);
                ++presetLoads;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
    });

    const double memStart = memoryMb();
    double memAfterWarmup = 0, worstEver = 0;
    for (int minute = 1; minute <= minutes; ++minute)
    {
        juce::Thread::sleep (60000);
        const double w = s.worstMs.exchange (0.0);
        worstEver = std::max (worstEver, w);
        const double mem = memoryMb();
        if (minute == 1) memAfterWarmup = mem;
        std::printf ("min %2d | worst block %.3f of %.1f ms | over budget %lld | bad samples %lld | peak %.3f | max voices %d | CPU %.1f%% | MIDI %lld, knobs %lld, presets %lld | memory %.1f MB | driver xruns %d\n",
                     minute, w, s.budgetMs.load(), (long long) s.overBudget.load(), (long long) s.badSamples.load(), s.peakOut.load(),
                     s.maxVoices.load(), dm.getCpuUsage() * 100.0, (long long) midiEvents.load(), (long long) knobMoves.load(),
                     (long long) presetLoads.load(), mem, dev->getXRunCount());
        std::fflush (stdout);
    }
    stop = true;
    keyboard.join();
    gui.join();
    s.queue.push (M::allNotesOff (1), MidiQueue::now());
    s.engine.params.release = 0.3f;
    juce::Thread::sleep (2000); // let every release finish
    const int voicesLeft = s.engine.activeVoices();
    dm.removeAudioCallback (&s);
    dm.closeAudioDevice();

    const double memGrowth = memoryMb() - memAfterWarmup;
    const bool ok = s.overBudget == 0 && s.badSamples == 0 && s.peakOut <= 1.0f && voicesLeft == 0 && memGrowth < 5.0;
    std::printf ("STRESS %s: %lld blocks, worst block %.3f ms (budget %.1f), over budget %lld, bad samples %lld, peak %.3f, voices left %d, "
                 "memory %.1f -> %.1f MB after warm-up (growth %+.2f MB), MIDI dropped %d\n",
                 ok ? "PASSED" : "FAILED", (long long) s.blocks.load(), worstEver, s.budgetMs.load(), (long long) s.overBudget.load(),
                 (long long) s.badSamples.load(), s.peakOut.load(), voicesLeft, memStart, memAfterWarmup, memGrowth, s.queue.dropped.load());
    return ok ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 2 && juce::String (argv[1]) == "--stress") return runStress (juce::String (argv[2]).getIntValue());
    if (argc > 2 && juce::String (argv[1]) == "--probe") return runProbe (juce::File (juce::String (argv[2])));
    if (argc > 1) { writeReferences (juce::File (juce::String (argv[1]))); return 0; }

    const auto tw = juce::Time::getMillisecondCounterHiRes();
    const auto& tables = Wavetables::get();
    const double buildMs = juce::Time::getMillisecondCounterHiRes() - tw;
    std::printf ("Wavetables: %zu tables per wave, built in %.0f ms\n", tables.harmonics.size(), buildMs);

    // Table choice: never a harmonic above Nyquist, and no dull highs: the top harmonic is above 0.38 x rate,
    // or the next one wouldn't fit below Nyquist anyway
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (double f0 = 8.0; f0 < 0.5 * rate; f0 *= 1.01)
        {
            const int H = tables.harmonics[(size_t) tables.tableFor (f0 / rate)];
            CHECK (H * f0 < rate / 2);
            if (f0 * tables.harmonics.back() >= 0.38 * rate) // very low notes: capped by table size instead
                CHECK (H * f0 >= 0.38 * rate || (H + 1) * f0 >= rate / 2);
        }

    // Note on/off and velocity
    auto e = fresh();
    const float loud = rms (run (*e, 0.2, { M::noteOn (1, 60, 1.0f) }));
    CHECK (loud > 0.01f);
    CHECK (e->activeVoices() == 1);
    run (*e, 0.005, { M::noteOff (1, 60) });
    const auto tail = run (*e, 1.0);
    CHECK (e->activeVoices() == 0);
    CHECK (peak ({ tail.end() - block, tail.end() }) < 1e-4f);

    auto soft = fresh();
    CHECK (rms (run (*soft, 0.2, { M::noteOn (1, 60, 0.3f) })) < loud * 0.6f);

    // Channels 1-16 all play
    auto ch = fresh();
    run (*ch, 0.05, { M::noteOn (16, 64, 1.0f), M::noteOn (9, 40, 1.0f) });
    CHECK (ch->activeVoices() == 2);

    // Sustain pedal holds released notes, pedal-up releases them
    auto sus = fresh();
    run (*sus, 0.1, { M::controllerEvent (1, 64, 127), M::noteOn (1, 60, 1.0f) });
    run (*sus, 0.5, { M::noteOff (1, 60) });
    CHECK (sus->activeVoices() == 1);
    run (*sus, 0.005, { M::controllerEvent (1, 64, 0) });
    run (*sus, 1.0);
    CHECK (sus->activeVoices() == 0);

    // Polyphony, voice stealing, no clipping, nothing stuck after all-notes-off
    auto poly = fresh();
    run (*poly, 0.05, { M::noteOn (1, 48, 1.0f), M::noteOn (1, 52, 1.0f), M::noteOn (1, 55, 1.0f), M::noteOn (1, 60, 1.0f) });
    CHECK (poly->activeVoices() == 4);
    juce::MidiBuffer many;
    for (int n = 30; n < 70; ++n) many.addEvent (M::noteOn (1, n, 1.0f), 0); // 40 notes > 32 voices
    juce::AudioBuffer<float> buf (2, block);
    poly->render (buf, 0, block, many);
    const auto loudest = run (*poly, 0.3);
    CHECK (poly->activeVoices() == 32);
    CHECK (peak (loudest) <= 1.0f);
    run (*poly, 0.005, { M::allNotesOff (1) });
    run (*poly, 1.0);
    CHECK (poly->activeVoices() == 0);

    // Pitch: sine A4 must be 440 Hz (count rising zero crossings over 1 s)
    auto pitch = fresh (0);
    pitch->params.cutoff = 20000.0f;
    const auto s = run (*pitch, 1.1, { M::noteOn (1, 69, 1.0f) });
    int crossings = 0;
    for (size_t i = (size_t) (0.1 * sr); i < (size_t) (1.1 * sr) && i < s.size(); ++i)
        crossings += (s[i - 1] < 0 && s[i] >= 0) ? 1 : 0;
    CHECK (std::abs (crossings - 440) <= 1);

    // Deterministic rendering for the same input
    auto a = fresh(), b = fresh();
    CHECK (run (*a, 0.3, { M::noteOn (1, 57, 0.8f) }) == run (*b, 0.3, { M::noteOn (1, 57, 0.8f) }));
    auto na = fresh (4), nb = fresh (4); // noise too (seeded per voice, not from the clock)
    CHECK (run (*na, 0.3, { M::noteOn (1, 57, 0.8f), M::noteOn (1, 60, 0.8f) }) == run (*nb, 0.3, { M::noteOn (1, 57, 0.8f), M::noteOn (1, 60, 0.8f) }));

    // Performance: time to render 1 s with 32 voices, as a share of real time
    auto perf = fresh();
    perf->render (buf, 0, block, many);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    run (*perf, 1.0);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::printf ("32 voices: 1 s of audio rendered in %.1f ms (%.1f%% of one core)\n", ms, ms / 10.0);
    CHECK (ms < 500.0);

    // MidiQueue: events land at the sample matching their arrival time (480-sample block @ 48 kHz)
    {
        MidiQueue q;
        juce::MidiBuffer out;
        const double t = 100.0;
        q.push (M::noteOn (1, 60, 1.0f), t - 0.004);  // 192 samples old
        q.push (M::noteOn (1, 62, 1.0f), t - 0.002);  // 96 samples old
        q.push (M::noteOff (1, 60), t);
        q.push (M::noteOn (1, 64, 1.0f), t - 1.0);    // stale: clamped to 0
        q.popInto (out, 480, sr, t);
        std::vector<int> pos;
        for (const auto meta : out) pos.push_back (meta.samplePosition);
        CHECK ((pos == std::vector<int> { 0, 287, 383, 479 }));

        for (int i = 0; i < 2000; ++i) q.push (M::noteOn (1, 60, 1.0f), t);
        CHECK (q.dropped == 2000 - (MidiQueue::capacity - 1)); // full queue drops, never blocks or allocates
    }

    // Old path: a producer stalled while holding MidiKeyboardState's lock (e.g. GUI thread preempted) stalls the audio thread too.
    juce::MidiBuffer mb;
    mb.ensureSize (MidiQueue::capacity * 16);
    {
        struct Stall : juce::MidiKeyboardState::Listener
        {
            std::atomic<bool> inside { false };
            void handleNoteOn (juce::MidiKeyboardState*, int, int, float) override { inside = true; juce::Thread::sleep (5); } // runs inside the lock
            void handleNoteOff (juce::MidiKeyboardState*, int, int, float) override {}
        } stall;
        juce::MidiKeyboardState ks;
        ks.addListener (&stall);
        std::thread producer ([&] { ks.noteOn (1, 60, 1.0f); });
        while (! stall.inside) std::this_thread::yield(); // the producer now holds the lock
        const auto a = juce::Time::getMillisecondCounterHiRes();
        ks.processNextMidiBuffer (mb, 0, 480, true);
        const double oldMs = juce::Time::getMillisecondCounterHiRes() - a;
        producer.join();
        ks.removeListener (&stall);

        // New path: the consumer takes no lock; measure its heaviest case, a full queue.
        MidiQueue q;
        for (int i = 0; i < MidiQueue::capacity; ++i) q.push (M::noteOn (1, 60, 1.0f), 100.0);
        mb.clear();
        const auto b = juce::Time::getMillisecondCounterHiRes();
        q.popInto (mb, 480, sr, 100.0);
        const double newMs = juce::Time::getMillisecondCounterHiRes() - b;
        std::printf ("Audio-thread MIDI read with a stalled producer: old %.3f ms (blocked), new %.3f ms (full queue, capped at 256 events)\n", oldMs, newMs);
        CHECK (oldMs > 2.0);  // the hazard is real in the old path
        CHECK (newMs < 0.5);
    }

    // Gain staging: the output stage must be linear for normal playing (no colouring), bounded on overload.
    {
        auto sine = [] (std::initializer_list<int> notes)
        {
            auto e = fresh (0);
            e->params.cutoff = 20000.0f;
            juce::MidiBuffer ev;
            for (int n : notes) ev.addEvent (M::noteOn (1, n, 1.0f), 0);
            juce::AudioBuffer<float> b (2, block);
            std::vector<float> out;
            for (int done = 0; done < 1.5 * sr; done += block)
            {
                e->render (b, 0, block, ev);
                ev.clear();
                out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + block);
            }
            return std::vector<float> (out.begin() + (int) (0.5 * sr), out.begin() + (int) (1.5 * sr)); // 1 s at sustain
        };
        auto dft = [] (const std::vector<float>& x, double f)
        {
            double re = 0, im = 0;
            for (size_t n = 0; n < x.size(); ++n)
            {
                const double a = juce::MathConstants<double>::twoPi * f * (double) n / sr;
                re += x[n] * std::cos (a);
                im -= x[n] * std::sin (a);
            }
            return std::sqrt (re * re + im * im);
        };
        // THD of one full-velocity A4 (1 s = exactly 440 cycles, so no window needed)
        const auto a4 = sine ({ 69 });
        double harm = 0;
        for (int k = 2; k <= 9; ++k) harm += std::pow (dft (a4, 440.0 * k), 2.0);
        const double thdDb = 20.0 * std::log10 (std::sqrt (harm) / dft (a4, 440.0) + 1e-12);

        // Chord vs sum of its notes rendered alone: any difference is distortion from the output stage
        auto chordErrDb = [&] (std::initializer_list<int> notes)
        {
            const auto chord = sine (notes);
            std::vector<float> sum (chord.size(), 0.0f);
            for (int n : notes) { const auto one = sine ({ n }); for (size_t i = 0; i < sum.size(); ++i) sum[i] += one[i]; }
            double err = 0, ref = 0;
            for (size_t i = 0; i < sum.size(); ++i) { err += std::pow (chord[i] - sum[i], 2.0); ref += std::pow (sum[i], 2.0); }
            return 10.0 * std::log10 (err / ref + 1e-30);
        };
        const double chord4 = chordErrDb ({ 57, 61, 64, 69 }), chord8 = chordErrDb ({ 45, 52, 57, 61, 64, 69, 73, 76 });
        std::printf ("Output stage: one-note THD %.1f dB, 4-note chord error %.1f dB, 8-note chord error %.1f dB\n", thdDb, chord4, chord8);
        CHECK (thdDb < -100.0);
        CHECK (chord4 < -100.0);

        // Safety clip: identity below 0.8, continuous (value and slope) at the knee, bounded below 1, odd
        CHECK (SynthEngine::safetyClip (0.8f) == 0.8f && SynthEngine::safetyClip (-0.5f) == -0.5f);
        CHECK (std::abs (SynthEngine::safetyClip (0.8001f) - 0.8001f) < 1e-6f);
        CHECK (SynthEngine::safetyClip (1.0f) < 1.0f && SynthEngine::safetyClip (100.0f) <= 1.0f);
        CHECK (SynthEngine::safetyClip (-100.0f) == -SynthEngine::safetyClip (100.0f));

        // Diagnostics counters: a normal note never touches the limiter; a full-volume 32-note cluster does
        auto quiet = fresh();
        run (*quiet, 0.3, { M::noteOn (1, 60, 1.0f) });
        CHECK (quiet->limiterHits == 0 && quiet->peakLevel > 0.05f && quiet->peakLevel < 0.8f);
        auto loudE = fresh();
        loudE->params.gain = 1.0f;
        juce::MidiBuffer cluster;
        for (int n = 40; n < 72; ++n) cluster.addEvent (M::noteOn (1, n, 1.0f), 0);
        juce::AudioBuffer<float> cb (2, block);
        loudE->render (cb, 0, block, cluster);
        run (*loudE, 0.3);
        std::printf ("Limiter: normal note %lld hits (peak %.3f), 32-note cluster %lld hits (peak %.2f before the clip)\n",
                     (long long) quiet->limiterHits.load(), quiet->peakLevel.load(), (long long) loudE->limiterHits.load(), loudE->peakLevel.load());
        CHECK (loudE->limiterHits > 0 && loudE->peakLevel > 0.8f);
    }

    // Numeric robustness: every rate x block size x wave x filter x extreme cutoff/resonance, with extreme notes,
    // velocities, pitch bend both ways and the cutoff slammed 20 Hz <-> 20 kHz every block.
    {
        int cases = 0, bad = 0;
        for (double rate : { 44100.0, 48000.0, 96000.0 })
            for (int bs : { 1, 37, 64, 256, 512 })
                for (int w = 0; w < 6; ++w)
                    for (int ft = 0; ft < 3; ++ft)
                        for (float cut : { 20.0f, 20000.0f })
                            for (float res : { 0.5f, 10.0f })
                            {
                                SynthEngine e;
                                e.params.wave = w; e.params.filterType = ft; e.params.cutoff = cut; e.params.resonance = res;
                                e.params.lfoTarget = cases % 4; e.params.lfoDepth = 1.0f; e.params.lfoRate = 20.0f; // LFO flat out too
                                e.prepare (rate);
                                juce::AudioBuffer<float> b (2, bs);
                                juce::MidiBuffer ev;
                                bool finite = true;
                                float pk = 0;
                                auto step = [&] (int blocks)
                                {
                                    for (int i = 0; i < blocks; ++i)
                                    {
                                        e.params.cutoff = (i & 1) ? 20.0f : 20000.0f;
                                        e.render (b, 0, bs, ev);
                                        ev.clear();
                                        for (int c = 0; c < 2; ++c)
                                            for (int s = 0; s < bs; ++s)
                                            {
                                                const float x = b.getSample (c, s);
                                                finite &= std::isfinite (x);
                                                pk = std::max (pk, std::abs (x));
                                            }
                                    }
                                };
                                const int perSecond = (int) (rate / bs);
                                for (int n : { 0, 21, 60, 108, 127 }) ev.addEvent (M::noteOn (1, n, n == 60 ? 0.01f : 1.0f), 0);
                                step (perSecond / 10);
                                ev.addEvent (M::pitchWheel (1, 16383), 0); step (perSecond / 10);
                                ev.addEvent (M::pitchWheel (1, 0), 0);     step (perSecond / 10);
                                ev.addEvent (M::allNotesOff (1), 0);       step (perSecond / 2); // release 0.3 s + margin
                                float tail = 0;
                                for (int c = 0; c < 2; ++c) for (int s = 0; s < bs; ++s) tail = std::max (tail, std::abs (b.getSample (c, s)));
                                ++cases;
                                if (! finite || pk > 1.0f || e.activeVoices() != 0 || tail > 1e-4f)
                                {
                                    if (++bad <= 5)
                                        std::printf ("  bad: rate %.0f block %d wave %d filter %d cut %.0f res %.1f -> finite %d peak %.3f voices %d tail %.2g\n",
                                                     rate, bs, w, ft, cut, res, (int) finite, pk, e.activeVoices(), tail);
                                }
                            }
        std::printf ("Robustness: %d/%d cases clean\n", cases - bad, cases);
        CHECK (bad == 0);
    }

    // DC offset and pitch at every sample rate: A4 per waveform, mean over exactly 440 cycles, zero crossings of the sine
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        float worstDc = 0;
        int sineCrossings = 0;
        for (int w = 0; w < 6; ++w)
        {
            SynthEngine e;
            e.params.wave = w; e.params.cutoff = 20000.0f;
            e.prepare (rate);
            juce::AudioBuffer<float> b (2, 1);
            juce::MidiBuffer ev;
            ev.addEvent (M::noteOn (1, 69, 1.0f), 0);
            const int skip = (int) (0.5 * rate), len = (int) rate;
            double sum = 0, sq = 0;
            float prev = 0;
            for (int i = 0; i < skip + len; ++i)
            {
                e.render (b, 0, 1, ev);
                ev.clear();
                const float x = b.getSample (0, 0);
                if (i >= skip)
                {
                    sum += x; sq += (double) x * x;
                    if (w == 0 && prev < 0 && x >= 0) ++sineCrossings;
                }
                prev = x;
            }
            const float dc = (float) (std::abs (sum / len) / std::sqrt (sq / len));
            if (w == 4) CHECK (dc < 4.0f / std::sqrt ((float) len)); // noise: its 1 s mean is random, allow 4 sigma (1/sqrt(N))
            else worstDc = std::max (worstDc, dc);
        }
        std::printf ("Rate %.0f: worst tonal DC %.6f of RMS, sine A4 %d Hz\n", rate, worstDc, sineCrossings);
        CHECK (worstDc < 1e-4f);
        CHECK (std::abs (sineCrossings - 440) <= 1);
    }

    // Aliasing: worst non-harmonic peak below 20 kHz relative to the fundamental (dBc), oscillator alone,
    // 65536-point FFT with a Blackman-Harris window (sidelobes -92 dB = measurement floor).
    {
        constexpr int order = 16, N = 1 << order;
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window ((size_t) N, juce::dsp::WindowingFunction<float>::blackmanHarris, false);
        auto worstAliasDbc = [&] (int w, double f0, bool naive, double width = 0.5)
        {
            std::vector<float> data (2 * N, 0.0f);
            const double dt = f0 / sr;
            double t = 0;
            for (int i = 0; i < N; ++i)
            {
                data[(size_t) i] = ! naive ? Voice::shape (w, t, dt, width)
                                 : w == 1  ? (float) (2.0 * t - 1.0)
                                 : w == 2  ? (t < 0.5 ? 1.0f : -1.0f)
                                 : w == 5  ? (float) (t >= 1.0 - width ? 2.0 - 2.0 * width : -2.0 * width)
                                           : (float) (4.0 * std::abs (t - 0.5) - 1.0);
                t += dt;
                if (t >= 1.0) t -= 1.0;
            }
            window.multiplyWithWindowingTable (data.data(), (size_t) N);
            fft.performFrequencyOnlyForwardTransform (data.data());
            const double binHz = sr / N;
            auto isHarmonic = [&] (int bin)
            {
                const double k = std::round (bin * binHz / f0);
                return k >= 1 && std::abs (bin - k * f0 / binHz) <= 10.0;
            };
            float fund = 0, worst = 0;
            for (int b = (int) (30 / binHz); b < (int) (20000 / binHz); ++b)
            {
                if (std::abs (b - f0 / binHz) <= 10.0) fund = std::max (fund, data[(size_t) b]);
                else if (! isHarmonic (b)) worst = std::max (worst, data[(size_t) b]);
            }
            return 20.0 * std::log10 (worst / fund + 1e-12);
        };
        struct Osc { int w; double width; const char* name; };
        for (const Osc o : { Osc { 1, 0.5, "saw" }, Osc { 2, 0.5, "square" }, Osc { 3, 0.5, "triangle" },
                             Osc { 5, 0.25, "pulse25" }, Osc { 5, 0.1, "pulse10" } })
        {
            std::printf ("Aliasing %-8s", o.name);
            for (int note : { 45, 69, 93, 108 })
            {
                const double f0 = juce::MidiMessage::getMidiNoteInHertz (note);
                const double now = worstAliasDbc (o.w, f0, false, o.width);
                std::printf ("  %5.0f Hz: naive %6.1f / now %6.1f dBc", f0, worstAliasDbc (o.w, f0, true, o.width), now);
                CHECK (now < -90.0); // before band-limited tables: PolyBLEP saw/square -29..-60, naive triangle -34..-96
            }
            std::printf ("\n");
        }
    }

    // Pulse width shapes the spectrum as theory says: |h_k| ~ |sin(pi k w)| / k. At w = 0.25: h2/h1 = 0.7071, h4 = 0.
    {
        const double f0 = 440.0;
        std::vector<float> x ((size_t) sr);
        double t = 0;
        for (auto& v : x) { v = Voice::shape (5, t, f0 / sr, 0.25); t += f0 / sr; if (t >= 1.0) t -= 1.0; }
        auto h = [&] (int k)
        {
            double re = 0, im = 0;
            for (size_t n = 0; n < x.size(); ++n)
            {
                const double a = juce::MathConstants<double>::twoPi * k * f0 * (double) n / sr;
                re += x[n] * std::cos (a); im -= x[n] * std::sin (a);
            }
            return std::sqrt (re * re + im * im);
        };
        const double h1 = h (1), r2 = h (2) / h1, r4 = h (4) / h1, r8 = h (8) / h1;
        std::printf ("Pulse 25%%: h2/h1 %.4f (want 0.7071), h4/h1 %.1e, h8/h1 %.1e (want 0)\n", r2, r4, r8);
        CHECK (std::abs (r2 - 0.70711) < 0.005 && r4 < 1e-4 && r8 < 1e-4);
    }

    // PWM: LFO on width sweeps the duty cycle (fraction of time the pulse is high) across 0.05..0.95
    {
        auto e = fresh (5);
        e->params.cutoff = 20000.0f; e->params.lfoTarget = 3; e->params.lfoDepth = 1.0f; e->params.lfoRate = 2.0f;
        const auto x = run (*e, 1.3, { M::noteOn (1, 57, 1.0f) });
        double dmin = 1, dmax = 0;
        const size_t win = 480; // 10 ms = 2.2 cycles of A3
        for (size_t i = (size_t) (0.3 * sr); i + win < x.size(); i += win)
        {
            int high = 0;
            for (size_t k = i; k < i + win; ++k) high += x[k] > 0 ? 1 : 0;
            dmin = std::min (dmin, high / (double) win); dmax = std::max (dmax, high / (double) win);
        }
        std::printf ("PWM: duty cycle swept %.2f..%.2f (want ~0.05..0.95)\n", dmin, dmax);
        CHECK (dmin < 0.12 && dmax > 0.88);
    }

    // LFO and mod wheel. Instantaneous frequency per cycle from interpolated rising zero crossings of a sine A4.
    {
        auto cycleFreqs = [] (const std::vector<float>& x, double fromSec)
        {
            std::vector<double> f;
            double lastT = -1;
            for (size_t i = (size_t) (fromSec * sr); i < x.size(); ++i)
                if (x[i - 1] < 0 && x[i] >= 0)
                {
                    const double t = (double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                    if (lastT >= 0) f.push_back (sr / (t - lastT));
                    lastT = t;
                }
            return f;
        };
        auto sineEngine = [] (int target, float depth, float rate)
        {
            auto e = fresh (0);
            e->params.cutoff = 20000.0f;
            e->params.lfoTarget = target; e->params.lfoDepth = depth; e->params.lfoRate = rate;
            return e;
        };

        // Vibrato: full depth = +-1 semitone around 440 Hz, at the set rate
        auto vib = sineEngine (0, 1.0f, 5.0f);
        const auto vf = cycleFreqs (run (*vib, 2.3, { M::noteOn (1, 69, 1.0f) }), 0.3);
        const double fmin = *std::min_element (vf.begin(), vf.end()), fmax = *std::max_element (vf.begin(), vf.end());
        int lfoCycles = 0;
        for (size_t i = 1; i < vf.size(); ++i) lfoCycles += (vf[i - 1] < 440.0 && vf[i] >= 440.0) ? 1 : 0;
        std::printf ("Vibrato: %.1f..%.1f Hz (want 415.3..466.2), %d LFO cycles in 2 s (want 10)\n", fmin, fmax, lfoCycles);
        CHECK (std::abs (fmin / 415.305 - 1.0) < 0.005 && std::abs (fmax / 466.164 - 1.0) < 0.005);
        CHECK (lfoCycles >= 9 && lfoCycles <= 11);

        // Mod wheel adds depth, also for notes started after it moved; wheel at 0 = no vibrato
        auto wheel = sineEngine (0, 0.0f, 5.0f);
        run (*wheel, 0.01, { M::controllerEvent (1, 1, 127) });
        const auto wf = cycleFreqs (run (*wheel, 2.3, { M::noteOn (1, 69, 1.0f) }), 0.3);
        CHECK (std::abs (*std::max_element (wf.begin(), wf.end()) / 466.164 - 1.0) < 0.005);
        auto still = sineEngine (0, 0.0f, 5.0f);
        run (*still, 0.01, { M::controllerEvent (1, 1, 0) });
        const auto sf = cycleFreqs (run (*still, 1.3, { M::noteOn (1, 69, 1.0f) }), 0.3);
        CHECK (*std::max_element (sf.begin(), sf.end()) - *std::min_element (sf.begin(), sf.end()) < 0.1);

        // Tremolo: depth 0.5 swings the level between 50 % and 100 % (per-cycle peaks)
        auto trem = sineEngine (2, 0.5f, 4.0f);
        const auto tx = run (*trem, 2.3, { M::noteOn (1, 69, 1.0f) });
        float pmin = 1e9f, pmax = 0;
        for (size_t i = (size_t) (0.3 * sr); i + 110 < tx.size(); i += 109) // ~one 440 Hz cycle per window
        {
            const float p = *std::max_element (tx.begin() + (long) i, tx.begin() + (long) i + 110);
            pmin = std::min (pmin, p); pmax = std::max (pmax, p);
        }
        std::printf ("Tremolo: level ratio %.3f (want 0.500)\n", pmin / pmax);
        CHECK (std::abs (pmin / pmax - 0.5f) < 0.02f);

        // Filter LFO: low-passed saw gets clearly brighter and darker. Brightness = RMS of the sample-to-sample
        // difference (weights highs); plain RMS barely moves because the low harmonics sit below the cutoff range.
        auto filt = fresh (1);
        filt->params.cutoff = 1000.0f; filt->params.lfoTarget = 1; filt->params.lfoDepth = 1.0f; filt->params.lfoRate = 2.0f;
        const auto fx = run (*filt, 2.3, { M::noteOn (1, 45, 1.0f) });
        float bmin = 1e9f, bmax = 0;
        for (size_t i = (size_t) (0.3 * sr); i + 961 < fx.size(); i += 960)
        {
            double acc = 0;
            for (size_t k = i + 1; k <= i + 960; ++k) acc += std::pow (fx[k] - fx[k - 1], 2.0);
            const float b = (float) std::sqrt (acc / 960);
            bmin = std::min (bmin, b); bmax = std::max (bmax, b);
        }
        std::printf ("Filter LFO: brightness swings %.4f..%.4f (x%.1f)\n", bmin, bmax, bmax / bmin);
        CHECK (bmax > 4.0f * bmin);

        // Slamming the wheel 0 -> 127 with tremolo must not click (depth is smoothed)
        auto jump = sineEngine (2, 0.0f, 4.0f);
        juce::AudioBuffer<float> jb (2, block);
        juce::MidiBuffer jev;
        jev.addEvent (M::noteOn (1, 69, 1.0f), 0);
        float prevS = 0, steady = 0, worst = 0;
        for (int i = 0; i < 60; ++i)
        {
            if (i == 40) jev.addEvent (M::controllerEvent (1, 1, 127), 0);
            jump->render (jb, 0, block, jev);
            jev.clear();
            for (int s = 0; s < block; ++s)
            {
                const float d = std::abs (jb.getSample (0, s) - prevS);
                if (i >= 20 && i < 40) steady = std::max (steady, d);
                if (i >= 40) worst = std::max (worst, d);
                prevS = jb.getSample (0, s);
            }
        }
        CHECK (worst <= steady * 1.05f);

        // CPU with vibrato on all 32 voices
        auto perfV = fresh();
        perfV->params.lfoDepth = 1.0f;
        perfV->render (buf, 0, block, many);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        run (*perfV, 1.0);
        std::printf ("32 voices with vibrato: 1 s rendered in %.1f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
    }

    // CPU benchmark: ms to render 1 s with 32 sounding voices, median of 5 runs, per LFO target (saw, effects off)
    {
        auto bench = [&] (int target, float depth)
        {
            std::vector<double> t;
            for (int k = 0; k < 5; ++k)
            {
                auto e = fresh();
                e->params.lfoTarget = target; e->params.lfoDepth = depth; e->params.wave = target == 3 ? 5 : 1;
                e->render (buf, 0, block, many);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                run (*e, 1.0);
                t.push_back (juce::Time::getMillisecondCounterHiRes() - t0);
            }
            std::sort (t.begin(), t.end());
            return t[2];
        };
        std::printf ("CPU (32 voices, 1 s, median of 5): LFO off %.1f ms | vibrato %.1f | filter %.1f | tremolo %.1f | PWM %.1f\n",
                     bench (0, 0.0f), bench (0, 1.0f), bench (1, 1.0f), bench (2, 1.0f), bench (3, 1.0f));
    }

    // Effects: delay echo timing/levels, reverb tail vs size and stereo width, chorus modulation, extremes.
    {
        auto stereo = [] (SynthEngine& e, std::vector<std::pair<double, M>> events, double seconds)
        {
            std::vector<float> l, r;
            juce::AudioBuffer<float> b (2, block);
            for (int done = 0; done < seconds * sr; done += block)
            {
                juce::MidiBuffer ev;
                for (auto& [t, m] : events) if (t * sr >= done && t * sr < done + block) ev.addEvent (m, (int) (t * sr) - done);
                e.render (b, 0, block, ev);
                l.insert (l.end(), b.getReadPointer (0), b.getReadPointer (0) + block);
                r.insert (r.end(), b.getReadPointer (1), b.getReadPointer (1) + block);
            }
            return std::make_pair (l, r);
        };
        auto seg = [] (const std::vector<float>& x, double a, double b2) { return std::vector<float> (x.begin() + (long) (a * sr), x.begin() + (long) (b2 * sr)); };
        auto onset = [] (const std::vector<float>& x, double a, double b2, float level)
        {
            for (size_t i = (size_t) (a * sr); i < (size_t) (b2 * sr); ++i) if (std::abs (x[i]) > level) return (double) i / sr;
            return -1.0;
        };

        // Delay: 30 ms sine burst; first echo after delayTime at the dry level (mix 1), the next one x feedback
        auto d = fresh (0);
        d->params.cutoff = 20000.0f; d->params.attack = 0.001f; d->params.release = 0.005f;
        d->params.delayMix = 1.0f; d->params.delayTime = 0.25f; d->params.delayFeedback = 0.5f;
        d->prepare (sr);
        const auto dx = stereo (*d, { { 0.0, M::noteOn (1, 69, 1.0f) }, { 0.03, M::noteOff (1, 69) } }, 1.0).first;
        const float dry = peak (seg (dx, 0.0, 0.1)), e1 = peak (seg (dx, 0.2, 0.35)), e2 = peak (seg (dx, 0.45, 0.6));
        double lag = 0, best = -1; // cross-correlation of the burst with the signal 0.24..0.26 s later
        for (int L = (int) (0.24 * sr); L <= (int) (0.26 * sr); ++L)
        {
            double acc = 0;
            for (int i = 0; i < (int) (0.04 * sr); ++i) acc += (double) dx[(size_t) i] * dx[(size_t) (i + L)];
            if (acc > best) { best = acc; lag = L / sr; }
        }
        std::printf ("Delay: echo after %.4f s (0.25), echo1/dry %.3f (1.0), echo2/echo1 %.3f (0.5)\n", lag, e1 / dry, e2 / e1);
        CHECK (std::abs (lag - 0.25) < 0.5 / sr + 1e-9 && std::abs (e1 / dry - 1.0f) < 0.03f && std::abs (e2 / e1 - 0.5f) < 0.03f);

        // Reverb: tail after a 0.2 s note lasts longer with a bigger room; L and R decorrelated (stereo width)
        auto tail = [&] (float size, float& corr)
        {
            auto e = fresh (0);
            e->params.cutoff = 20000.0f; e->params.release = 0.01f; e->params.reverbMix = 1.0f; e->params.reverbSize = size;
            e->prepare (sr);
            const auto [l, r] = stereo (*e, { { 0.0, M::noteOn (1, 69, 1.0f) }, { 0.2, M::noteOff (1, 69) } }, 6.0);
            double ll = 0, rr = 0, lr = 0;
            for (size_t i = (size_t) (0.4 * sr); i < (size_t) (0.8 * sr); ++i) { ll += l[i] * l[i]; rr += r[i] * r[i]; lr += l[i] * r[i]; }
            corr = (float) (lr / std::sqrt (ll * rr));
            const float start = rms (seg (l, 0.25, 0.26));
            for (double t = 0.25; t + 0.01 < 6.0; t += 0.01)
                if (rms (seg (l, t, t + 0.01)) < start * 0.01f) return t - 0.25; // -40 dB
            return 6.0;
        };
        float cSmall = 0, cBig = 0;
        const double tSmall = tail (0.3f, cSmall), tBig = tail (0.9f, cBig);
        std::printf ("Reverb: -40 dB after %.2f s (size 0.3) vs %.2f s (size 0.9); L/R correlation %.2f / %.2f\n", tSmall, tBig, cSmall, cBig);
        CHECK (tSmall > 0.1 && tBig > 1.5 * tSmall && cSmall < 0.9f && cBig < 0.9f);

        // Chorus: same level, but the sound is modulated (no longer identical to the dry note)
        auto dryE = fresh (0), chE = fresh (0);
        chE->params.chorusMix = 1.0f;
        chE->prepare (sr);
        const auto a = seg (stereo (*dryE, { { 0.0, M::noteOn (1, 57, 1.0f) } }, 2.0).first, 0.5, 2.0);
        const auto c = seg (stereo (*chE, { { 0.0, M::noteOn (1, 57, 1.0f) } }, 2.0).first, 0.5, 2.0);
        double ac = 0, aa = 0, cc = 0;
        for (size_t i = 0; i < a.size(); ++i) { ac += a[i] * c[i]; aa += a[i] * a[i]; cc += c[i] * c[i]; }
        std::printf ("Chorus: correlation with dry %.3f, level %.2f dB\n", ac / std::sqrt (aa * cc), 10 * std::log10 (cc / aa));
        CHECK (ac / std::sqrt (aa * cc) < 0.99 && std::abs (10 * std::log10 (cc / aa)) < 1.0); // before equal-power compensation: -3.09 dB

        // Extremes: every effect flat out (feedback 0.9, biggest room), odd and oversized blocks: finite, bounded
        int bad = 0;
        for (double rate : { 44100.0, 96000.0 })
            for (int bs : { 1, 37, 512, 1000 })
            {
                SynthEngine e;
                e.params.wave = 1; e.params.chorusMix = 1; e.params.delayMix = 1; e.params.delayFeedback = 0.9f;
                e.params.delayTime = 0.02f; e.params.reverbMix = 1; e.params.reverbSize = 1; e.params.gain = 1;
                e.prepare (rate);
                juce::AudioBuffer<float> b (2, bs);
                juce::MidiBuffer ev;
                for (int n : { 36, 48, 55, 60, 64, 67, 72 }) ev.addEvent (M::noteOn (1, n, 1.0f), 0);
                bool ok = true;
                for (int i = 0; i < (int) (2.0 * rate / bs); ++i)
                {
                    e.params.delayTime = (i / 50) % 2 ? 1.5f : 0.02f; // swing the delay time too
                    e.render (b, 0, bs, ev);
                    ev.clear();
                    for (int s = 0; s < bs; ++s) ok &= std::isfinite (b.getSample (0, s)) && std::abs (b.getSample (1, s)) <= 1.0f;
                }
                bad += ok ? 0 : 1;
            }
        std::printf ("Effects flat out: %d/8 cases finite and bounded\n", 8 - bad);
        CHECK (bad == 0);
    }

    // ADSR timing. Envelope = per-12-sample peak of a 1-voice sine C8 (4186 Hz, one cycle per window), normalized
    // by the full level of a reference note. Segments are block-quantized, so note-off time is taken from the render length.
    // A linear release from level L crosses the 1 % end mark at R * (1 - 0.01 / L), not at R.
    {
        auto envOf = [] (SynthEngine& e, double onFor, double offFor, double& tOff)
        {
            auto x = run (e, onFor, { M::noteOn (1, 108, 1.0f) });
            tOff = (double) x.size() / sr;
            const auto y = run (e, offFor, { M::noteOff (1, 108) });
            x.insert (x.end(), y.begin(), y.end());
            std::vector<float> env;
            for (size_t i = 0; i + 12 <= x.size(); i += 12)
                env.push_back (std::abs (*std::max_element (x.begin() + (long) i, x.begin() + (long) i + 12,
                                                            [] (float a, float b) { return std::abs (a) < std::abs (b); })));
            return env;
        };
        auto at = [] (const std::vector<float>& env, double t) { return env[std::min (env.size() - 1, (size_t) (t * sr / 12))]; };
        auto firstTime = [] (const std::vector<float>& env, double from, auto pred)
        {
            for (size_t i = (size_t) (from * sr / 12); i < env.size(); ++i) if (pred (env[i])) return (i + 0.5) * 12 / sr;
            return -1.0;
        };
        auto engine = [] (float a, float d, float s, float r)
        {
            auto e = fresh (0);
            e->params.cutoff = 20000.0f;
            e->params.attack = a; e->params.decay = d; e->params.sustain = s; e->params.release = r;
            return e;
        };
        double tOff = 0;
        // A: attack 0.1, decay 0.2 to 0.5, release 0.3
        auto ea = engine (0.1f, 0.2f, 0.5f, 0.3f);
        auto envA = envOf (*ea, 0.6, 0.5, tOff);
        const float full = *std::max_element (envA.begin(), envA.end());
        for (auto& v : envA) v /= full;
        const double tHalf = firstTime (envA, 0.0, [] (float v) { return v >= 0.5f; });
        const double tPeak = firstTime (envA, 0.0, [] (float v) { return v >= 0.995f; });
        const double tEndA = firstTime (envA, tOff, [] (float v) { return v < 0.01f; }) - tOff;
        std::printf ("ADSR A: half %.4f s (0.05), peak %.4f s (0.1), mid-decay %.3f (0.75), sustain %.3f (0.5), mid-release %.3f (0.25), release %.4f s (0.294)\n",
                     tHalf, tPeak, at (envA, 0.2), at (envA, 0.45), at (envA, tOff + 0.15), tEndA);
        CHECK (std::abs (tHalf - 0.05) < 0.002 && std::abs (tPeak - 0.1) < 0.002);
        CHECK (std::abs (at (envA, 0.2) - 0.75f) < 0.02f && std::abs (at (envA, 0.45) - 0.5f) < 0.02f);
        CHECK (std::abs (at (envA, tOff + 0.15) - 0.25f) < 0.02f && std::abs (tEndA - 0.3 * (1 - 0.01 / 0.5)) < 0.003);

        // B: released mid-attack (level ~0.5, above sustain 0.2): release must still take 0.2 s
        auto eb = engine (1.0f, 0.2f, 0.2f, 0.2f);
        auto envB = envOf (*eb, 0.5, 0.6, tOff);
        for (auto& v : envB) v /= full;
        const double tEndB = firstTime (envB, tOff, [] (float v) { return v < 0.01f; }) - tOff;
        std::printf ("ADSR B (release from %.2f, sustain 0.2): release %.4f s (want %.4f)\n", at (envB, tOff - 0.001), tEndB, 0.2 * (1 - 0.01 / at (envB, tOff - 0.001)));
        CHECK (std::abs (tEndB - 0.2 * (1 - 0.01 / at (envB, tOff - 0.001))) < 0.003);

        // C: sustain 0 ("pluck"), released mid-decay (~0.8): must fade over 0.3 s, not cut
        auto ec = engine (0.005f, 1.0f, 0.0f, 0.3f);
        auto envC = envOf (*ec, 0.2, 0.6, tOff);
        for (auto& v : envC) v /= full;
        const float startC = at (envC, tOff - 0.001);
        const double tEndC = firstTime (envC, tOff, [] (float v) { return v < 0.01f; }) - tOff;
        std::printf ("ADSR C (sustain 0, release from %.2f): mid-release %.3f (want %.3f), release %.4f s (want %.4f)\n",
                     startC, at (envC, tOff + 0.15), startC / 2, tEndC, 0.3 * (1 - 0.01 / startC));
        CHECK (std::abs (tEndC - 0.3 * (1 - 0.01 / startC)) < 0.003 && std::abs (at (envC, tOff + 0.15) - startC / 2) < 0.03f);
    }

    // Fast knob moves must not click: held sine A4 at sustain, slam one knob, look at the next 50 ms.
    // A clean sine of any amplitude A has max sample-to-sample step 2*A*sin(pi*f/sr); a click is a step the
    // amplitude doesn't explain. So the pass/fail metric is (worst step / peak) against that ideal. The raw
    // "worst step vs steady slope" is printed too: it also rises when a resonant filter rings louder for a moment.
    {
        auto knobClick = [] (const char* name, auto setup, auto slam)
        {
            auto e = fresh (0);
            setup (*e);
            e->prepare (sr);
            juce::AudioBuffer<float> b (2, block);
            juce::MidiBuffer ev;
            ev.addEvent (M::noteOn (1, 69, 1.0f), 0);
            float prev = 0, before = 0, during = 0, after = 0, duringPeak = 0;
            const int change = (int) (0.5 * sr / block), settle = change + (int) (0.3 * sr / block), end = settle + (int) (0.1 * sr / block);
            for (int i = 0; i < end; ++i)
            {
                if (i == change) slam (*e);
                e->render (b, 0, block, ev);
                ev.clear();
                for (int s = 0; s < block; ++s)
                {
                    const float dlt = std::abs (b.getSample (0, s) - prev);
                    prev = b.getSample (0, s);
                    if (i >= change - (int) (0.1 * sr / block) && i < change) before = std::max (before, dlt);
                    else if (i >= change && i < change + (int) (0.05 * sr / block) + 1)
                    {
                        during = std::max (during, dlt);
                        duringPeak = std::max (duringPeak, std::abs (prev));
                    }
                    else if (i >= settle) after = std::max (after, dlt);
                }
            }
            const float vsSteady = during / std::max (before, after);
            const float vsIdeal = (during / duringPeak) / (2.0f * std::sin (juce::MathConstants<float>::pi * 440.0f / (float) sr));
            std::printf ("  knob %-10s step/peak %.2fx a clean sine's (raw worst step %.2fx the steady slope)\n", name, vsIdeal, vsSteady);
            return vsIdeal;
        };
        std::printf ("Fast knob moves:\n");
        int clicks = 0;
        clicks += knobClick ("Volume",    [] (SynthEngine&) {}, [] (SynthEngine& e) { e.params.gain = 0.1f; }) > 1.05f;
        clicks += knobClick ("Volume up", [] (SynthEngine& e) { e.params.gain = 0.1f; }, [] (SynthEngine& e) { e.params.gain = 1.0f; }) > 1.05f;
        clicks += knobClick ("Sustain",   [] (SynthEngine& e) { e.params.sustain = 0.2f; }, [] (SynthEngine& e) { e.params.sustain = 1.0f; }) > 1.05f;
        clicks += knobClick ("Cutoff",    [] (SynthEngine& e) { e.params.cutoff = 200.0f; }, [] (SynthEngine& e) { e.params.cutoff = 20000.0f; }) > 1.05f;
        clicks += knobClick ("Resonance", [] (SynthEngine& e) { e.params.cutoff = 500.0f; }, [] (SynthEngine& e) { e.params.resonance = 10.0f; }) > 1.05f;
        clicks += knobClick ("LFO depth", [] (SynthEngine& e) { e.params.lfoTarget = 2; }, [] (SynthEngine& e) { e.params.lfoDepth = 1.0f; }) > 1.05f;
        clicks += knobClick ("Chorus mix", [] (SynthEngine&) {}, [] (SynthEngine& e) { e.params.chorusMix = 1.0f; }) > 1.05f;
        clicks += knobClick ("Delay mix",  [] (SynthEngine&) {}, [] (SynthEngine& e) { e.params.delayMix = 1.0f; }) > 1.05f;
        clicks += knobClick ("Reverb mix", [] (SynthEngine&) {}, [] (SynthEngine& e) { e.params.reverbMix = 1.0f; }) > 1.05f;
        CHECK (clicks == 0);
        // Negative control: switching sine -> square mid-note is a real discontinuity; the metric must flag it
        const float control = knobClick ("(control)", [] (SynthEngine&) {}, [] (SynthEngine& e) { e.params.wave = 2; });
        CHECK (control > 1.5f);
    }

    // Voice stealing must not click: 1-voice engine, held sine, a second note steals the voice.
    // Measure the largest sample-to-sample jump in the steal block across 20 waveform phases,
    // against the steady slope of the sine before the steal.
    {
        float worstJump = 0, steadySlope = 0;
        for (int k = 0; k < 20; ++k)
        {
            SynthEngine one (1);
            one.params.wave = 0;
            one.params.cutoff = 20000.0f;
            one.prepare (sr);
            juce::AudioBuffer<float> b (2, block);
            juce::MidiBuffer ev;
            ev.addEvent (M::noteOn (1, 60, 1.0f), 0);
            float prev = 0;
            for (int i = 0; i < 40 + k; ++i) // ~0.2 s: past attack/decay, at sustain
            {
                one.render (b, 0, block, ev);
                ev.clear();
                for (int s = 0; s < block; ++s) { steadySlope = std::max (steadySlope, std::abs (b.getSample (0, s) - prev)); prev = b.getSample (0, s); }
            }
            ev.addEvent (M::noteOn (1, 72, 1.0f), 37 + k * 9);
            for (int i = 0; i < 2; ++i) // the steal block and the next one
            {
                one.render (b, 0, block, ev);
                ev.clear();
                for (int s = 0; s < block; ++s) { worstJump = std::max (worstJump, std::abs (b.getSample (0, s) - prev)); prev = b.getSample (0, s); }
            }
        }
        std::printf ("Voice steal: worst jump %.4f, steady sine slope %.4f (ratio %.1f)\n", worstJump, steadySlope, worstJump / steadySlope);
        CHECK (worstJump < 2.0f * steadySlope * 1.05f); // the new note is an octave up, so its own slope is 2x; before the fix: 20x
    }

    // ---- Recording ----
    juce::MidiMessageSequence takeSeq;
    takeSeq.addEvent (M::noteOn (1, 60, 0.8f).withTimeStamp (0.5));
    takeSeq.addEvent (M::noteOff (1, 60).withTimeStamp (1.0));
    takeSeq.addEvent (M::noteOn (1, 64, 0.8f).withTimeStamp (1.25));
    takeSeq.addEvent (M::noteOff (1, 64).withTimeStamp (1.5));
    takeSeq.updateMatchedPairs();

    // Playback placement: an event at 0.010 s (sample 480) lands in block [256, 512) at offset 224 (+ buffer offset)
    {
        juce::MidiMessageSequence one;
        one.addEvent (M::noteOn (1, 60, 1.0f).withTimeStamp (0.010));
        juce::MidiBuffer b;
        int idx = addEventsInRange (one, 0, 0, 256, sr, b);
        CHECK (idx == 0 && b.getNumEvents() == 0);
        idx = addEventsInRange (one, idx, 256, 256, sr, b, 7);
        CHECK (idx == 1 && b.getNumEvents() == 1 && b.getFirstEventTime() == 224 + 7);
    }

    // MIDI file round trip keeps notes and timing (to 1 ms)
    {
        juce::MemoryOutputStream mos;
        writeMidiFile (takeSeq, mos);
        juce::MemoryInputStream mis (mos.getData(), mos.getDataSize(), false);
        juce::MidiFile file;
        CHECK (file.readFrom (mis));
        file.convertTimestampTicksToSeconds();
        std::vector<std::pair<int, double>> notes;
        for (auto* e : *file.getTrack (0))
            if (e->message.isNoteOnOrOff()) notes.push_back ({ e->message.getNoteNumber(), e->message.getTimeStamp() });
        CHECK (notes.size() == 4);
        const double want[] { 0.5, 1.0, 1.25, 1.5 };
        for (size_t i = 0; i < notes.size() && i < 4; ++i) CHECK (std::abs (notes[i].second - want[i]) < 0.001);
    }

    // Opening a type-1 file: tempo from track 0, channel 10 -> drums from any track, empty tracks skipped, times in seconds
    {
        juce::MidiMessageSequence conductor, bass, kit, empty;
        conductor.addEvent (M::tempoMetaEvent (666667), 0); // 90 BPM
        bass.addEvent (M::textMetaEvent (3, "Bass"), 0);
        bass.addEvent (M::noteOn (2, 40, 0.8f), 960);       // beat 2 = 0.667 s at 90 BPM
        bass.addEvent (M::noteOff (2, 40), 1920);
        kit.addEvent (M::noteOn (10, 36, 1.0f), 0);
        kit.addEvent (M::noteOff (10, 36), 240);
        kit.addEvent (M::noteOn (3, 72, 0.5f), 480);        // a melody note sharing the drum track stays a part
        kit.addEvent (M::noteOff (3, 72), 720);
        empty.addEvent (M::textMetaEvent (3, "Nothing"), 0);
        juce::MidiFile f;
        f.setTicksPerQuarterNote (960);
        for (auto* s : { &conductor, &bass, &kit, &empty }) f.addTrack (*s);
        juce::MemoryOutputStream mos;
        f.writeTo (mos);
        juce::MemoryInputStream mis (mos.getData(), mos.getDataSize(), false);
        MidiParts mp;
        CHECK (readMidiParts (mis, mp));
        CHECK (std::abs (mp.bpm - 90.0) < 0.01);
        CHECK (mp.drums.getNumEvents() == 2 && mp.drums.getEventPointer (0)->message.getNoteNumber() == 36);
        CHECK (mp.parts.size() == 2 && mp.parts[0].name == "Bass" && mp.parts[1].name.isEmpty());
        if (mp.parts.size() == 2)
        {
            CHECK (std::abs (mp.parts[0].seq.getEventPointer (0)->message.getTimeStamp() - 2.0 / 3.0) < 0.001);
            CHECK (mp.parts[0].seq.getIndexOfMatchingKeyUp (0) == 1);
            CHECK (mp.parts[1].seq.getNumEvents() == 2);
        }
        juce::MemoryInputStream junk ("not midi", 8, false);
        CHECK (! readMidiParts (junk, mp));
    }

    // Offline render (WAV export path): sound where notes are, silence before and after the tail
    {
        SynthParams p;
        const auto audio = renderOffline (takeSeq, p, sr);
        auto section = [&] (double a, double b)
        {
            std::vector<float> v (audio.getReadPointer (0) + (int) (a * sr), audio.getReadPointer (0) + (int) (b * sr));
            return v;
        };
        CHECK (audio.getNumSamples() == (int) ((1.5 + p.release + 0.5) * sr));
        CHECK (peak (section (0.0, 0.49)) < 1e-6f);
        CHECK (rms (section (0.6, 0.9)) > 0.01f);
        CHECK (peak (section (2.2, 2.29)) < 1e-4f);

        // WAV file round trip: format and length preserved
        const auto wavFile = juce::File::createTempFile (".wav");
        CHECK (writeWav (audio, sr, wavFile));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (wavFile.createInputStream().release(), true));
        CHECK (reader != nullptr && reader->sampleRate == sr && reader->numChannels == 2 && reader->bitsPerSample == 24
               && reader->lengthInSamples == audio.getNumSamples());
        reader.reset();
        wavFile.deleteFile();
    }

    // ---- Presets: round trip, validation at the file boundary, factory sounds ----
    {
        auto values = [] (SynthParams& p) { std::vector<double> v; p.forEach ([&] (const char*, auto& x, auto, auto) { v.push_back ((double) x.load()); }); return v; };
        SynthParams a;
        a.wave = 5; a.filterType = 2; a.lfoTarget = 3; a.attack = 0.7f; a.decay = 1.3f; a.sustain = 0.25f; a.release = 2.0f;
        a.cutoff = 1234.5f; a.resonance = 6.5f; a.lfoRate = 3.3f; a.lfoDepth = 0.4f; a.pulseWidth = 0.2f; a.gain = 0.9f;
        SynthParams b;
        juce::String name;
        CHECK (Presets::fromJson (Presets::toJson (a, "Test"), b, name).isEmpty() && name == "Test");
        CHECK (values (a) == values (b));

        SynthParams c; // out of range clamped, missing keys untouched, unknown keys ignored
        c.decay = 0.9f;
        CHECK (Presets::fromJson (R"({"format":"LocalMIDIStudio.preset","version":1,"name":"x","params":{"cutoff":99999,"wave":9,"sustain":-3,"bogus":1}})", c, name).isEmpty());
        CHECK (c.cutoff == 20000.0f && c.wave == 5 && c.sustain == 0.0f && c.decay == 0.9f);

        SynthParams d; // refused files leave the sound alone
        const auto before = values (d);
        CHECK (Presets::fromJson (R"({"format":"LocalMIDIStudio.preset","version":2,"params":{"cutoff":100}})", d, name).isNotEmpty());
        CHECK (Presets::fromJson ("not json at all {", d, name).isNotEmpty());
        CHECK (Presets::fromJson (R"({"format":"SomethingElse","version":1,"params":{"cutoff":100}})", d, name).isNotEmpty());
        CHECK (Presets::fromJson (R"({"format":"LocalMIDIStudio.preset","version":1})", d, name).isNotEmpty());
        CHECK (values (d) == before);

        int ok = 0; // every factory sound loads and plays: finite, bounded, audible, releases
        for (const auto& f : Presets::factory())
        {
            auto e = fresh();
            juce::String n;
            const bool loaded = Presets::fromJson (Presets::factoryJson (f), e->params, n).isEmpty() && n == f.name;
            const auto x = run (*e, 1.5, { M::noteOn (1, 48, 0.9f), M::noteOn (1, 55, 0.9f), M::noteOn (1, 64, 0.9f) });
            run (*e, 0.005, { M::allNotesOff (1) });
            run (*e, e->params.release + 0.5);
            bool finite = true;
            for (float v : x) finite &= std::isfinite (v);
            ok += (loaded && finite && peak (x) <= 1.0f && rms (x) > 0.005f && e->activeVoices() == 0) ? 1 : 0;
        }
        std::printf ("Presets: round trip, validation ok; %d/%zu factory presets load and play\n", ok, Presets::factory().size());
        CHECK (ok == (int) Presets::factory().size());
    }

    // ---- MIDI Learn map ----
    {
        MidiMap m;
        m.bind (1, 21, "cutoff");
        m.bind (1, 22, "resonance");
        CHECK (m.find (1, 21) == "cutoff" && m.find (2, 21).isEmpty() && m.find (1, 99).isEmpty());
        m.bind (1, 23, "cutoff");                 // re-learning a parameter moves it to the new knob
        CHECK (m.find (1, 21).isEmpty() && m.find (1, 23) == "cutoff");
        m.bind (1, 22, "attack");                 // re-using a knob takes it from the old parameter
        CHECK (m.find (1, 22) == "attack" && m.forParam ("resonance") == nullptr && m.bindings.size() == 2);
        const auto back = MidiMap::fromJson (m.toJson());
        CHECK (back.bindings.size() == 2 && back.find (1, 23) == "cutoff" && back.find (1, 22) == "attack");
        m.forget ("attack");
        CHECK (m.find (1, 22).isEmpty() && m.bindings.size() == 1);
        // hand-edited files: bad entries skipped, wrong format -> empty
        const auto mixed = MidiMap::fromJson (R"({"format":"LocalMIDIStudio.midimap","version":1,"bindings":[{"channel":1,"cc":20,"param":"gain"},{"channel":17,"cc":5,"param":"x"},{"channel":1,"cc":200,"param":"y"},{"channel":2,"cc":7}]})");
        CHECK (mixed.bindings.size() == 1 && mixed.find (1, 20) == "gain");
        CHECK (MidiMap::fromJson ("garbage").bindings.empty() && MidiMap::fromJson (R"({"format":"other","version":1})").bindings.empty());
        std::printf ("MIDI map: bind/rebind/forget/persist/validation ok\n");
    }

    // ---- Sampler: SFZ subset, pitch, velocity layers, one-shot, loops, interpolation quality, errors ----
    {
        CHECK (Sfz::noteNumber ("c4") == 60 && Sfz::noteNumber ("C#4") == 61 && Sfz::noteNumber ("db4") == 61 && Sfz::noteNumber ("a0") == 21
               && Sfz::noteNumber ("c-1") == 0 && Sfz::noteNumber ("bb3") == 58 && Sfz::noteNumber ("b3") == 59 && Sfz::noteNumber ("64") == 64
               && Sfz::noteNumber ("h4") == -1 && Sfz::noteNumber ("") == -1);

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lms-sampler-test");
        dir.deleteRecursively();
        dir.getChildFile ("samples").createDirectory();
        auto writeTone = [&] (const char* name, float amp, double seconds, bool noise)
        {
            juce::AudioBuffer<float> b (2, (int) (seconds * 44100));
            juce::Random r (7);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float v = amp * (noise ? r.nextFloat() * 2 - 1 : (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / 44100.0));
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            writeWav (b, 44100.0, dir.getChildFile ("samples").getChildFile (name));
        };
        writeTone ("loud.wav", 0.8f, 1.0, false);
        writeTone ("soft layer.wav", 0.2f, 1.0, false); // a space in the path
        writeTone ("drum.wav", 0.8f, 0.2, true);
        writeTone ("loop.wav", 0.5f, 1.0, false);      // 44100 samples = exactly 440 cycles: seamless loop
        const auto sfz = dir.getChildFile ("test.sfz");
        sfz.replaceWithText (R"(// test instrument
<control> default_path=samples/
<group> lokey=c4 hikey=c6 pitch_keycenter=a4   /* inherited
   by the next two regions */
<region> sample=loud.wav lovel=64
<region> sample=soft layer.wav hivel=63
<group>
<region> key=36 sample=drum.wav loop_mode=one_shot
<region> key=d2 sample=loop.wav loop_mode=loop_continuous loop_start=0 loop_end=44099
<region> key=40 sample=missing-release.wav trigger=release
)");
        juce::String err;
        const auto regions = Sfz::parse (sfz.loadFileAsString(), dir, err);
        CHECK (err.isEmpty() && regions.size() == 4);
        if (regions.size() == 4)
        {
            CHECK (regions[0].lokey == 60 && regions[0].hikey == 84 && regions[0].root == 69 && regions[0].lovel == 64);
            CHECK (regions[1].file.getFileName() == "soft layer.wav" && regions[1].hivel == 63);
            CHECK (regions[2].lokey == 36 && regions[2].hikey == 36 && regions[2].root == 36 && regions[2].oneShot);
            CHECK (regions[3].root == 38 && regions[3].loop && regions[3].loopEnd == 44100);
        }
        auto inst = Sfz::load (sfz, err);
        CHECK (inst != nullptr && err.isEmpty());

        auto engineWith = [&] { auto e = fresh(); e->setInstrument (inst); return e; };
        // pitch: A4 sample (44.1 kHz) played 3 semitones up at 48 kHz -> 523.25 Hz
        {
            auto e = engineWith();
            const auto x = run (*e, 1.2, { M::noteOn (1, 72, 1.0f) });
            int crossings = 0;
            for (size_t i = (size_t) (0.1 * sr); i < (size_t) (0.6 * sr); ++i) crossings += (x[i - 1] < 0 && x[i] >= 0) ? 1 : 0; // the 1 s sample lasts 0.84 s up here
            std::printf ("Sampler: A4 sample played as C5 -> %d cycles in 0.5 s (want 261.6 = 523.25 Hz)\n", crossings);
            CHECK (std::abs (crossings - 261.6) <= 1.0);
        }
        // velocity layers: vel 1.0 -> loud layer (0.8), vel 0.4 -> soft layer (0.2), times the vel^2 curve: ratio 25
        {
            auto a = engineWith(), b = engineWith();
            const float ra = rms (run (*a, 0.5, { M::noteOn (1, 69, 1.0f) })), rb = rms (run (*b, 0.5, { M::noteOn (1, 69, 0.4f) }));
            std::printf ("Sampler: velocity layers level ratio %.2f (want 25.0)\n", ra / rb);
            CHECK (std::abs (ra / rb / 25.0f - 1.0f) < 0.05f);
        }
        // one-shot drum ignores an early note-off; a normal region releases
        {
            auto d = engineWith();
            run (*d, 0.01, { M::noteOn (1, 36, 1.0f) });
            const auto tailD = run (*d, 0.15, { M::noteOff (1, 36) });
            auto n = engineWith();
            run (*n, 0.05, { M::noteOn (1, 69, 1.0f) });
            run (*n, 0.5, { M::noteOff (1, 69) }); // release 0.3 s
            CHECK (rms ({ tailD.end() - 2400, tailD.end() }) > 0.01f && n->activeVoices() == 0);
        }
        // loop: held 3 s on a 1 s sample keeps its level, and the loop seam doesn't click
        {
            auto l = engineWith();
            const auto x = run (*l, 3.0, { M::noteOn (1, 38, 1.0f) });
            const float early = rms ({ x.begin() + (long) (0.2 * sr), x.begin() + (long) (0.7 * sr) }),
                        late = rms ({ x.begin() + (long) (2.4 * sr), x.begin() + (long) (2.9 * sr) });
            float worst = 0, pk = 0;
            for (size_t i = (size_t) (0.1 * sr); i < x.size(); ++i) { worst = std::max (worst, std::abs (x[i] - x[i - 1])); pk = std::max (pk, std::abs (x[i])); }
            const float vsIdeal = (worst / pk) / (2.0f * std::sin (juce::MathConstants<float>::pi * 440.0f / (float) sr));
            std::printf ("Sampler: loop level %.3f -> %.3f, worst step %.2fx a clean sine's\n", early, late, vsIdeal);
            CHECK (std::abs (late / early - 1.0f) < 0.01f && vsIdeal < 1.05f);
        }
        // interpolation quality: +5 semitones (resampling ratio 1.226) vs the ideal sine
        {
            auto e = engineWith();
            const auto x = run (*e, 0.6, { M::noteOn (1, 74, 1.0f) });
            // the file rate cancels out (the pitch is musical); the set is normalized so its loudest sample (0.8) hits full scale
            const double f = 440.0 * std::pow (2.0, 5.0 / 12.0), A = 0.8 * 0.3 * 0.6 * static_cast<InstrumentSound*> (inst.get())->gain;
            CHECK (std::abs (static_cast<InstrumentSound*> (inst.get())->gain - 1.0f / 0.8f) < 1e-4f);
            double errSq = 0, sigSq = 0;
            for (size_t i = (size_t) (0.1 * sr); i < (size_t) (0.5 * sr); ++i)
            {
                const double ideal = A * std::sin (juce::MathConstants<double>::twoPi * f * (double) i / sr);
                errSq += (x[i] - ideal) * (x[i] - ideal); sigSq += ideal * ideal;
            }
            const double errDb = 10.0 * std::log10 (errSq / sigSq);
            std::printf ("Sampler: Hermite interpolation error at +5 semitones %.1f dB\n", errDb);
            CHECK (errDb < -60.0);
        }
        // switching instrument mid-note is safe and leaves nothing sounding
        {
            auto e = engineWith();
            run (*e, 0.1, { M::noteOn (1, 69, 1.0f), M::noteOn (1, 38, 1.0f) });
            e->setInstrument (nullptr);
            run (*e, 0.1);
            CHECK (e->activeVoices() == 0);
        }
        // Real libraries' extras: round robin, random takes, amp_veltrack, ampeg_release, pitch_keytrack, choke
        {
            const auto feat = dir.getChildFile ("feat.sfz");
            feat.replaceWithText (R"(<control> default_path=samples/
<region> key=50 seq_length=2 seq_position=1 sample=loud.wav
<region> key=50 seq_length=2 seq_position=2 sample=soft layer.wav
<region> key=52 lorand=0 hirand=0.5 sample=loud.wav
<region> key=52 lorand=0.5 hirand=1 sample=soft layer.wav
<region> key=54 amp_veltrack=0 sample=loud.wav
<region> key=56 sample=loop.wav loop_mode=loop_continuous loop_start=0 loop_end=44099 ampeg_release=0.05
<region> lokey=90 hikey=95 pitch_keycenter=90 pitch_keytrack=0 sample=loud.wav
<region> key=40 sample=loop.wav loop_mode=loop_continuous loop_start=0 loop_end=44099 group=1 off_by=2
<region> key=41 sample=drum.wav loop_mode=one_shot group=2
)");
            juce::String e2;
            auto fi = Sfz::load (feat, e2);
            CHECK (fi != nullptr);
            auto en = fresh();
            en->setInstrument (fi);
            auto hitLevel = [&] (int note, float vel)
            {
                const float v = rms (run (*en, 0.1, { M::noteOn (1, note, vel) }));
                run (*en, 0.005, { M::noteOff (1, note) });
                run (*en, 0.6);
                return v;
            };
            const float r1 = hitLevel (50, 1.0f), r2 = hitLevel (50, 1.0f), r3 = hitLevel (50, 1.0f);
            int loud = 0, soft = 0;
            for (int k = 0; k < 40; ++k) (hitLevel (52, 1.0f) > (r1 + r2) / 2 ? loud : soft)++;
            const float v1 = hitLevel (54, 1.0f), v2 = hitLevel (54, 0.3f);
            std::printf ("Sampler extras: round robin %.3f/%.3f/%.3f, random takes %d loud / %d soft, veltrack 0 ratio %.2f\n",
                         r1, r2, r3, loud, soft, v1 / v2);
            CHECK (r1 > 3 * r2 && r3 > 3 * r2 && loud >= 10 && soft >= 10 && std::abs (v1 / v2 - 1.0f) < 0.02f);

            en->params.release = 0.3f; // ampeg_release=0.05 must win over the knob
            run (*en, 0.2, { M::noteOn (1, 56, 1.0f) });
            const auto relTail = run (*en, 0.1, { M::noteOff (1, 56) });
            const float relPeak = peak ({ relTail.begin() + (long) (0.07 * sr), relTail.end() });
            CHECK (relPeak < 1e-3f);

            const auto kt = run (*en, 0.6, { M::noteOn (1, 95, 1.0f) }); // 5 semitones above the root, keytrack off: still 440 Hz
            run (*en, 0.005, { M::noteOff (1, 95) }); run (*en, 0.6);
            int crossings = 0;
            for (size_t i = (size_t) (0.1 * sr); i < (size_t) (0.6 * sr); ++i) crossings += (kt[i - 1] < 0 && kt[i] >= 0) ? 1 : 0;
            CHECK (std::abs (crossings - 220) <= 1);

            run (*en, 0.2, { M::noteOn (1, 40, 1.0f) });                 // "open hi-hat" ringing
            const auto ch = run (*en, 0.35, { M::noteOn (1, 41, 1.0f) }); // "closed hi-hat" chokes it; its 0.2 s hit ends
            const float after = rms ({ ch.begin() + (long) (0.25 * sr), ch.begin() + (long) (0.33 * sr) });
            std::printf ("Sampler extras: keytrack off %d Hz (440), release override tail %.1e, choked level %.1e\n", crossings * 2, relPeak, after);
            CHECK (after < 1e-3f && en->activeVoices() == 0);
        }
        // Karoryfer-style files (as in Weresax): #include/#define, <master>, set_cc defaults driving _oncc, CC crossfades,
        // amplitude_oncc, built-in *noise, keyswitch default articulation, note_offset
        {
            dir.getChildFile ("maps").createDirectory();
            dir.getChildFile ("maps/condenser.sfz").replaceWithText ("<group> xfout_locc120=0\n<region> sample=$S/loud.wav key=60\n");
            dir.getChildFile ("maps/dynamic.sfz").replaceWithText ("<group> xfin_hicc120=127\n<region> sample=$S/soft layer.wav key=60\n");
            const auto k = dir.getChildFile ("karo.sfz");
            k.replaceWithText (R"(#define $S samples
<control>
set_cc104=20
note_offset=2
<global> ampeg_release=0.001 ampeg_release_oncc104=2 sw_default=24
<master>
<region> sample=*noise lokey=0 hikey=127 amplitude_oncc122=100
#include "maps/dynamic.sfz"
#include "maps/condenser.sfz"
<group> sw_last=25
<region> sample=$S/drum.wav key=70
<group> sw_last=24
<region> sample=$S/loop.wav key=70
)");
            juce::String ek;
            const auto kr = Sfz::parse (k.loadFileAsString(), dir, ek);
            std::printf ("Karoryfer-style: %zu regions, error '%s'\n", kr.size(), ek.toRawUTF8());
            CHECK (ek.isEmpty() && kr.size() == 2);                       // noise (silent) and dynamic (xfin at cc 0) and articulation 25 skipped
            if (kr.size() == 2)
            {
                CHECK (kr[0].file.getFileName() == "loud.wav" && kr[0].lokey == 62 && kr[0].root == 62); // condenser, shifted by note_offset
                CHECK (std::abs (kr[0].release - (0.001f + 2.0f * 20.0f / 127.0f)) < 1e-4f);         // ampeg_release via set_cc104
                CHECK (kr[1].file.getFileName() == "loop.wav" && kr[1].lokey == 72);                   // the default articulation
            }
            dir.getChildFile ("maps/bad.sfz").replaceWithText ("#include \"nowhere.sfz\"\n");
            juce::String eb;
            Sfz::parse ("#include \"maps/bad.sfz\"", dir, eb);
            CHECK (eb.contains ("nowhere.sfz"));
        }

        // bad input: missing sample, unreadable numbers, no regions -> clear errors, nothing loaded
        auto loadText = [&] (const char* text) { sfz.replaceWithText (text); juce::String e; auto p = Sfz::load (sfz, e); return std::make_pair (p == nullptr, e); };
        const auto missing = loadText ("<region> sample=nope.wav");
        const auto garbage = loadText ("<region> sample=samples/loud.wav lovel=abc");
        const auto empty = loadText ("// nothing here");
        std::printf ("Sampler errors: '%s' | '%s' | '%s'\n", missing.second.toRawUTF8(), garbage.second.toRawUTF8(), empty.second.toRawUTF8());
        CHECK (missing.first && garbage.first && empty.first && missing.second.isNotEmpty() && garbage.second.isNotEmpty() && empty.second.isNotEmpty());
        dir.deleteRecursively();
    }

    // ---- Drum pad names: from the kit's sample names, else General MIDI, else the note name ----
    {
        InstrumentSound kit;
        SampleRegion k; k.file = juce::File::getCurrentWorkingDirectory().getChildFile ("OH/kick_OH_FF_1.wav"); k.lokey = k.hikey = 36;
        SampleRegion h; h.file = juce::File::getCurrentWorkingDirectory().getChildFile ("hihatClosed_OH_P_3.wav"); h.lokey = h.hikey = 42;
        SampleRegion p; p.file = juce::File::getCurrentWorkingDirectory().getChildFile ("C4v8.wav"); p.lokey = 40; p.hikey = 50; // keyed range: not a drum
        kit.regions = { k, h, p };
        std::printf ("Pad names: '%s' '%s' '%s' '%s' '%s'\n", Sfz::drumName (36, &kit).toRawUTF8(), Sfz::drumName (42, &kit).toRawUTF8(),
                     Sfz::drumName (45, &kit).toRawUTF8(), Sfz::drumName (38, nullptr).toRawUTF8(), Sfz::drumName (100, nullptr).toRawUTF8());
        CHECK (Sfz::drumName (36, &kit) == "kick" && Sfz::drumName (42, &kit) == "hihat Closed" && Sfz::drumName (45, &kit) == "Low Tom"
               && Sfz::drumName (38, nullptr) == "Snare" && Sfz::drumName (35, nullptr) == "Acoustic Bass Drum" && Sfz::drumName (81, nullptr) == "Open Triangle"
               && Sfz::drumName (100, nullptr) == "E6"); // same octave naming as the on-screen keyboard (middle C = C3)
    }

    // ---- Instrument library: catalog sanity, install from a ZIP (offline, file URL), zip-slip refused, cancel ----
    {
        std::set<std::string> ids;
        for (auto& it : Library::catalog())
        {
            CHECK (juce::String (it.url).startsWith ("https://github.com/sfzinstruments/") && juce::String (it.url).endsWith ("/archive/HEAD.zip"));
            CHECK (juce::String (it.licence) == "CC0" && it.approxMb > 0 && ids.insert (it.id).second);
        }
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lms-library-test");
        root.deleteRecursively();
        root.createDirectory();
        const auto src = root.getChildFile ("src");
        src.getChildFile ("Programs").createDirectory();
        src.getChildFile ("Programs/test.sfz").replaceWithText ("<region> sample=../Samples/t.wav\n");
        juce::AudioBuffer<float> tone (2, 4410);
        for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, 0.5f * std::sin (i * 0.0627f)), tone.setSample (1, i, tone.getSample (0, i));
        src.getChildFile ("Samples").createDirectory();
        writeWav (tone, 44100.0, src.getChildFile ("Samples/t.wav"));
        auto makeZip = [&] (const juce::File& zip, std::vector<std::pair<juce::File, juce::String>> entries)
        {
            juce::ZipFile::Builder b;
            for (auto& [f, path] : entries) b.addFile (f, 6, path);
            juce::FileOutputStream out (zip);
            return b.writeToStream (out, nullptr);
        };
        const auto good = root.getChildFile ("good.zip");
        CHECK (makeZip (good, { { src.getChildFile ("Programs/test.sfz"), "repo-master/Programs/test.sfz" },
                                { src.getChildFile ("Samples/t.wav"), "repo-master/Samples/t.wav" } }));
        const auto lib = root.getChildFile ("lib");
        std::atomic<bool> noCancel { false };
        juce::int64 seen = 0;
        const auto err = Library::install (juce::URL (good).toString (false), "test", lib, [&] (juce::int64 n) { seen = n; }, noCancel);
        const auto found = Library::installed (lib);
        juce::String le;
        const bool loads = found.size() == 1 && Sfz::load (found[0], le) != nullptr;
        std::printf ("Library: install '%s', %lld bytes reported, %d .sfz found, loads %d, zip removed %d\n",
                     err.toRawUTF8(), (long long) seen, found.size(), (int) loads, (int) ! lib.getChildFile ("test.zip").exists());
        CHECK (err.isEmpty() && seen == good.getSize() && loads && ! lib.getChildFile ("test.zip").exists());

        const auto evil = root.getChildFile ("evil.zip"); // an entry pointing outside the target folder
        CHECK (makeZip (evil, { { src.getChildFile ("Programs/test.sfz"), "../escaped.sfz" } }));
        const auto evilErr = Library::install (juce::URL (evil).toString (false), "evil", lib, [] (juce::int64) {}, noCancel);
        std::printf ("Library: malicious entry -> '%s'\n", evilErr.toRawUTF8());
        CHECK (evilErr.isNotEmpty() && ! lib.getChildFile ("escaped.sfz").exists() && ! root.getChildFile ("escaped.sfz").exists());

        std::atomic<bool> cancel { true };
        CHECK (Library::install (juce::URL (good).toString (false), "c", lib, [] (juce::int64) {}, cancel) == "cancelled"
               && ! lib.getChildFile ("c.zip").exists());
        root.deleteRecursively();
    }

    // ---- Song: tracks, mute/solo, live input routing, count-in + metronome, timing of recorded notes, stop, mixdown ----
    {
        auto song = std::make_unique<Song>();
        song->count = 2;
        song->prepare (sr, block);
        song->metronome = false;
        auto& t0 = *song->tracks[0];
        auto& t1 = *song->tracks[1];
        t0.engine.params.wave = 0; t1.engine.params.wave = 1;
        t0.clip.addEvent (M::noteOn (1, 69, 1.0f).withTimeStamp (0.5)); t0.clip.addEvent (M::noteOff (1, 69).withTimeStamp (0.6));
        t1.clip.addEvent (M::noteOn (1, 57, 1.0f).withTimeStamp (1.0)); t1.clip.addEvent (M::noteOff (1, 57).withTimeStamp (1.1));
        t0.clip.updateMatchedPairs(); t1.clip.updateMatchedPairs();
        t0.engine.params.release = t1.engine.params.release = 0.05f;
        auto play = [&] (double seconds, juce::MidiBuffer live = {})
        {
            std::vector<float> x;
            juce::AudioBuffer<float> b (2, block);
            for (int done = 0; done < seconds * sr; done += block)
            {
                song->render (b, 0, block, live, 100.0 + done / sr);
                live.clear();
                x.insert (x.end(), b.getReadPointer (0), b.getReadPointer (0) + block);
            }
            return x;
        };
        auto win = [&] (const std::vector<float>& x, double a, double b2) { return rms ({ x.begin() + (long) (a * sr), x.begin() + (long) (b2 * sr) }); };
        song->startPos = 0; song->run = true;
        const auto both = play (1.5);
        song->run = false; play (0.3);
        std::printf ("Song: quiet %.4f, track 0 at 0.5 s %.3f, track 1 at 1.0 s %.3f\n", win (both, 0.1, 0.45), win (both, 0.52, 0.58), win (both, 1.02, 1.08));
        CHECK (win (both, 0.1, 0.45) < 1e-4f && win (both, 0.52, 0.58) > 0.01f && win (both, 1.02, 1.08) > 0.01f);

        t1.mute = true; song->run = true;
        const auto muted = play (1.5);
        song->run = false; play (0.3); t1.mute = false;
        t1.solo = true; song->run = true;
        const auto solo = play (1.5);
        song->run = false; play (0.3); t1.solo = false;
        CHECK (win (muted, 0.52, 0.58) > 0.01f && win (muted, 1.02, 1.08) < 1e-4f && win (solo, 0.52, 0.58) < 1e-4f && win (solo, 1.02, 1.08) > 0.01f);

        t0.engine.params.gain = 0.0f; // live notes go to the selected track only
        play (0.1);                   // let the 20 ms Volume smoothing reach 0 first
        juce::MidiBuffer live; live.addEvent (M::noteOn (1, 60, 1.0f), 0);
        song->selected = 0; const float toSilent = rms (play (0.2, live));
        juce::MidiBuffer off; off.addEvent (M::allNotesOff (1), 0); play (0.3, off);
        song->selected = 1; const float toAudible = rms (play (0.2, live));
        play (0.3, off);
        t0.engine.params.gain = 0.6f;
        CHECK (toSilent < 1e-4f && toAudible > 0.01f);

        // count-in: one bar of clicks before bar 1, then the clip starts exactly at the bar line
        song->metronome = true; song->bpm = 120.0; song->selected = 0;
        t0.clip.clear(); t0.clip.addEvent (M::noteOn (1, 69, 1.0f).withTimeStamp (0.0)); t0.clip.addEvent (M::noteOff (1, 69).withTimeStamp (0.2)); t0.clip.updateMatchedPairs();
        song->startPos = -(juce::int64) std::llround (song->barSeconds() * sr); song->run = true;
        juce::AudioBuffer<float> b (2, block);
        song->render (b, 0, block, {}, 200.0);
        const float firstClick = b.getMagnitude (0, 0, 64);
        const double zero = song->songZeroWall; // event arriving at wall time a lands at song time a - zero
        const auto rest = play (2.2);
        song->run = false; play (0.3);
        // the first block began at -2 s (bar of 4 beats at 120 BPM); the note is at song 0 s = 2 s after the start
        const auto noteStart = (size_t) std::llround ((2.0 - (double) block / sr) * sr); // rest[] starts one block in
        float before = 0; for (size_t i = noteStart - 400; i < noteStart - 10; ++i) before = std::max (before, std::abs (rest[i]));
        std::printf ("Song: count-in click %.3f, zero at wall %.4f (want %.4f), sound before bar 1 %.4f, at bar 1 %.3f\n",
                     firstClick, zero, 200.0 - (-2.0 * sr + block - 1) / sr, before, rms ({ rest.begin() + (long) noteStart + 200, rest.begin() + (long) noteStart + 2400 }));
        CHECK (firstClick > 0.1f && std::abs (zero - (200.0 - (-2.0 * sr + block - 1) / sr)) < 1e-9);
        CHECK (rms ({ rest.begin() + (long) noteStart + 200, rest.begin() + (long) noteStart + 2400 }) > 0.01f);

        // timing identity: a note arriving at wall time a, placed by MidiQueue in the block that starts at song sample p0,
        // sits at song sample p0 + (n-1) - (now-a)*rate; recording it as a - songZeroWall gives exactly that position
        {
            const double now = 300.0, a = now - 0.004; const juce::int64 p0 = 96000;
            const double zeroW = now - (double) (p0 + block - 1) / sr;
            const double heard = (double) (p0 + block - 1 - std::llround ((now - a) * sr)) / sr, recorded = a - zeroW;
            CHECK (std::abs (heard - recorded) < 0.5 / sr);
        }

        // stop releases everything; a track being recorded does not play its old clip
        song->metronome = false;
        song->recordTrack = 0; song->startPos = 0; song->run = true;
        const auto recording = play (0.3);
        song->run = false; song->recordTrack = -1; play (0.3);
        CHECK (peak (recording) < 1e-4f && song->activeVoices() == 0);

        // mixdown = the audible tracks rendered offline and summed
        t1.mute = true;
        const auto mixMuted = song->mixdown (sr);
        t1.mute = false;
        const auto mixAll = song->mixdown (sr);
        std::printf ("Song mixdown: %.2f s (all) vs %.2f s (track 1 muted)\n", mixAll.getNumSamples() / sr, mixMuted.getNumSamples() / sr);
        CHECK (mixAll.getNumSamples() > mixMuted.getNumSamples() && mixAll.getMagnitude (0, mixAll.getNumSamples()) <= 1.0f);

        // Pad router: a pad (channel 10, note 36) plays kit sound 35 on the drum track; keys and CCs stay on the keys track
        PadRouter router;
        router.set (10, 36, 35);
        juce::MidiBuffer in, keysOut, padsOut;
        in.addEvent (M::noteOn (10, 36, 0.9f), 3); in.addEvent (M::noteOn (1, 60, 0.9f), 5);
        in.addEvent (M::controllerEvent (1, 1, 64), 7); in.addEvent (M::noteOff (10, 36), 9);
        router.split (in, keysOut, padsOut);
        std::vector<std::pair<int, int>> pd, kd; // (note or -cc, position)
        for (const auto m : padsOut) pd.push_back ({ m.getMessage().getNoteNumber(), m.samplePosition });
        for (const auto m : keysOut) kd.push_back ({ m.getMessage().isController() ? -m.getMessage().getControllerNumber() : m.getMessage().getNoteNumber(), m.samplePosition });
        CHECK ((pd == std::vector<std::pair<int, int>> { { 35, 3 }, { 35, 9 } }) && (kd == std::vector<std::pair<int, int>> { { 60, 5 }, { -1, 7 } }));
        CHECK (router.soundFor (1, 36) == -1); // same note on another channel is not a pad

        // pads reach their track while another track is selected
        song->selected = 0; t0.engine.params.gain = 0.0f; t1.engine.params.gain = 0.6f;
        play (0.1);
        juce::MidiBuffer padHit; padHit.addEvent (M::noteOn (10, 60, 1.0f), 0);
        std::vector<float> px;
        juce::AudioBuffer<float> pb (2, block);
        for (int k = 0; k < 20; ++k) { song->render (pb, 0, block, {}, 400.0, 1, k == 0 ? &padHit : nullptr); px.insert (px.end(), pb.getReadPointer (0), pb.getReadPointer (0) + block); }
        CHECK (rms (px) > 0.01f);
    }

    // ---- Song file: round trip, and bad files are refused as a whole ----
    {
        auto a = std::make_unique<Song>();
        a->count = 3; a->bpm = 97.0; a->metronome = false; a->selected = 2;
        a->tracks[0]->engine.params.cutoff = 1234.0f; a->tracks[1]->engine.params.wave = 3; a->tracks[2]->engine.params.reverbMix = 0.4f;
        a->tracks[1]->mute = true; a->tracks[2]->solo = true;
        a->tracks[0]->clip.addEvent (M::noteOn (1, 60, 0.7f).withTimeStamp (0.25));
        a->tracks[0]->clip.addEvent (M::noteOff (1, 60).withTimeStamp (0.75));
        a->tracks[0]->clip.addEvent (M::pitchWheel (1, 9000).withTimeStamp (0.5));
        a->tracks[1]->clip.addEvent (M::controllerEvent (10, 64, 127).withTimeStamp (1.0));
        a->tracks[1]->clip.addEvent (M::channelPressureChange (1, 33).withTimeStamp (1.1));
        std::array<juce::File, Song::maxTracks> inst;
        inst[1] = juce::File::getCurrentWorkingDirectory().getChildFile ("kit.sfz"); // not loaded: saved as empty
        const auto json = SongFile::toJson (*a, inst);
        SongFile::Loaded b;
        const auto err = SongFile::parse (json, b);
        auto sameClip = [] (const juce::MidiMessageSequence& x, const juce::MidiMessageSequence& y)
        {
            if (x.getNumEvents() != y.getNumEvents()) return false;
            for (int i = 0; i < x.getNumEvents(); ++i)
            {
                const auto& p = x.getEventPointer (i)->message; const auto& q = y.getEventPointer (i)->message;
                if (std::abs (p.getTimeStamp() - q.getTimeStamp()) > 1e-9 || p.getRawDataSize() != q.getRawDataSize()
                    || std::memcmp (p.getRawData(), q.getRawData(), (size_t) p.getRawDataSize()) != 0) return false;
            }
            return true;
        };
        CHECK (err.isEmpty() && b.tracks.size() == 3 && b.bpm == 97.0 && ! b.metronome && b.selected == 2);
        if (b.tracks.size() == 3)
        {
            CHECK (b.tracks[0]->params.cutoff == 1234.0f && b.tracks[1]->params.wave == 3 && std::abs (b.tracks[2]->params.reverbMix - 0.4f) < 1e-6f);
            CHECK (b.tracks[1]->mute && ! b.tracks[0]->mute && b.tracks[2]->solo && b.tracks[1]->instrument.isEmpty());
            CHECK (sameClip (a->tracks[0]->clip, b.tracks[0]->clip) && sameClip (a->tracks[1]->clip, b.tracks[1]->clip) && b.tracks[2]->clip.getNumEvents() == 0);
        }
        auto refused = [] (const juce::String& text) { SongFile::Loaded l; const auto e = SongFile::parse (text, l); return e.isNotEmpty() ? e : juce::String ("ACCEPTED"); };
        const juce::String bad[] {
            R"({"format":"Other","version":1,"tracks":[{}]})",
            R"({"format":"LocalMIDIStudio.song","version":2,"tracks":[{}]})",
            R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{},{},{},{},{},{},{},{},{}]})",
            R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{"events":[[0.5,240,1,2]]}]})",   // sysex status
            R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{"events":[[-1,144,60,100]]}]})", // negative time
            R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{"events":[[0.5,144,60]]}]})",    // note-on missing a byte
            R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{"events":[[0.5,144,200,100]]}]})", // data byte > 127
            "not json {" };
        int refusedCount = 0;
        for (auto& text : bad) { const auto e = refused (text); refusedCount += e != "ACCEPTED" ? 1 : 0; if (e == "ACCEPTED") std::printf ("  accepted bad song: %s\n", text.toRawUTF8()); }
        std::printf ("Song file: round trip ok, %d/%d bad files refused (e.g. '%s')\n", refusedCount, (int) std::size (bad), refused (bad[3]).toRawUTF8());
        CHECK (refusedCount == (int) std::size (bad));
    }

    // Not a pass/fail check: proves JUCE can see the real MIDI hardware on this machine
    for (auto& d : juce::MidiInput::getAvailableDevices())
        std::printf ("MIDI input found: %s\n", d.name.toRawUTF8());

    std::printf (failures == 0 ? "ALL TESTS PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
