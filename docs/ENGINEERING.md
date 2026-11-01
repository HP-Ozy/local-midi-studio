# Engineering rules & roadmap

Binding rules for every change to the synth engine. The roadmap below is the current gap list, ordered by priority.

## Method (every change)
Inspect → measure baseline → design → small change → test → compare before/after → document. One topic per change.
- No "sounds better" without numbers (FFT, aliasing dB, CPU ms, click/NaN checks) and, when it matters, a level-matched WAV before/after in `docs/measurements/`.
- Don't replace working algorithms without a written technical reason.
- Hardware not tested = say so. Offline tests (`tests/RenderTest.cpp`) are the baseline; real VI49 + headphones are verified by the user.
- Targets come from benchmarks on this PC, never declared up front.

## Real-time rules (audio callback)
No allocation, file I/O, logging, GUI, network, waits, or locks that can block. Preallocate; use atomics / lock-free FIFOs for GUI→audio and MIDI→audio.
Buffer duration ≠ latency (128 samples @ 48 kHz = 2.67 ms of buffer only).

## Signal chain
Oscillator → Envelope → Filter → Effects → Gain staging → Output. 32-bit float inside, 48 kHz default (44.1/96 supported), stereo out.
Headroom comes from gain staging, not from a limiter; the limiter is an optional safety net.

## Report after each cycle
Problem · cause · fix · files · test commands · before/after numbers · remaining limits · next step.

---

## Current state (v0.1, 31/10/2026)
`src/Synth.h` = engine (juce::Synthesiser, 32 voices, band-limited wavetable saw/square/triangle (since C7), noise, juce::ADSR, TPT SVF LP/HP/BP, smoothed cutoff, pitch bend ±2, tanh master).
`src/Main.cpp` = GUI + devices. `tests/RenderTest.cpp` = offline tests + CPU (32 voices ≈ 1.3 % of one core).

## Roadmap (gaps found by reading the code, not yet measured)

### P0 — real-time safety & stability
1. ~~Locks in the callback~~ — **done 31/10/2026**, see cycle log.
2. ~~Click on voice stealing~~ — **done 31/10/2026**, see cycle C3.
3. ~~Gain staging~~ — **done 31/10/2026**, see cycle C4.
4. ~~Numeric guards~~ — **done 31/10/2026**, see cycle C5.
5. ~~Diagnostics~~ — **done 31/10/2026**, see cycle C6. **P0 complete.**

### P1 — quality
~~Aliasing measured by FFT; band-limited triangle~~ (done, C7); ~~mod wheel (CC1); LFO with smoothing~~ (done, C8); ~~PWM~~ (done, C9); ~~ADSR timing accuracy test; click test on fast knob moves; WAV reference files~~ (done, C10). **P1 complete.**

### P2 — experience
~~Presets (versioned JSON), MIDI Learn~~ (done, C11–C12), ~~effects (delay, reverb, chorus)~~ (done, C13), saturation, EQ, level meter, ~~CPU-measured optimisations~~ (LFO + filter coefficients, C15), ~~30-min stress run~~ (done, C14).

### P3 — evolution
Wavetable (mipmapped), modulation matrix, FM / granular, ~~sampler~~ (core done, C16), spectrum view.

Recording / WAV export (master prompt Phase 5) slots in after P0 and before P2 effects.

---

## Cycle log

### C1 — P0.1 locks on the audio thread (31/10/2026)
- **Problem**: the audio callback could wait on locks held by other threads.
- **Cause**: `MidiMessageCollector` (JUCE `juce_MidiMessageCollector.cpp:51,74`) and `MidiKeyboardState::processNextMidiBuffer` (`juce_MidiKeyboardState.cpp:140`) share a `CriticalSection` with the MIDI/GUI threads; the GUI side runs listeners and allocates while holding it. Also the GUI voice counter called `Synthesiser::getVoice()`, which takes the lock held for the whole render.
- **Fix**: `src/MidiQueue.h` — `AbstractFifo` (1024 slots), producers serialised by a `SpinLock` the audio thread never touches; events keep their arrival timing inside the block (one block of fixed latency, as before); max 256 events per block because `MidiBuffer::addEvent` is O(n) per insert; overflow drops and counts, never blocks. Hardware MIDI lights the on-screen keys from the MIDI thread; on-screen keys reach the synth through a `MidiKeyboardState::Listener`. Voice count is now an atomic written at the end of each render.
- **Files**: `src/MidiQueue.h` (new), `src/Main.cpp`, `src/Synth.h`, `tests/RenderTest.cpp`.
- **Test**: `cmake --build build && ctest --test-dir build --output-on-failure` (3/3 runs pass).
- **Before/after** (producer stalled 5 ms inside the lock, as a preempted GUI thread would be): audio-thread MIDI read **old 4.0–5.0 ms blocked → new 0.05–0.09 ms** with a full queue (256-event cap; uncapped 1023 events took 0.72 ms). Event placement: exact to the sample (test: 0/287/383/479). Synth render unchanged: 32 voices ≈ 1.3 % of one core.
- **Limits**: `AudioDeviceManager`'s own callback lock remains (contended only while switching devices). Uncontended `Synthesiser` lock remains (nothing else takes it now). Not verified on hardware: VI49 keys and on-screen piano → sound after this change.
- **Next**: P0.2 click on voice stealing.
- **Hardware check (later, 31/10/2026)**: user confirmed VI49 keys → sound in headphones with this build. Setup note: the VI49 enumerated but sent nothing until Windows USB selective suspend was disabled and the PC rebooted (added to README troubleshooting).

### C2 — Recording, playback, WAV export (31/10/2026)
- **Goal**: master prompt Phase 5 (user asked for it before the remaining P0 items).
- **Design**: recording on MIDI/GUI threads only (`takeLock`, never the audio thread), same clock as `MidiQueue`; playback = immutable copy published by atomic pointer, GUI frees it only after the audio thread stops reporting it in use (no lock shared with audio); stopping mid-note sends All Notes Off; held notes and sustain pedal are closed at Stop. Every take auto-saves to `Documents\LocalMIDIStudio\take-*.mid` (120 BPM, 960 PPQ → seconds map exactly to ticks). WAV export = offline render with the same `SynthEngine` and a copy of the live params, 48 kHz / 24-bit, release tail included.
- **Files**: `src/Recording.h` (new: `addEventsInRange`, `writeMidiFile`, `renderOffline`, `writeWav`), `src/Main.cpp` (transport, piano roll, playback hand-off), `src/Synth.h` (`SynthParams::copyFrom`), `tests/RenderTest.cpp`, `CMakeLists.txt` (test links juce_audio_formats).
- **Tests**: block placement (event at sample 480 → block [256,512) offset 224), MIDI file round trip (4 notes, timing < 1 ms), offline render (silent before first note, sound during, silent after tail, exact length), WAV round trip (48 kHz, 2 ch, 24-bit, length). All pass.
- **End-to-end in the real app** (posted clicks, cursor untouched): Rec → 2 on-screen notes → Stop → `take-20261031-215200.mid` saved (126 B) → Export WAV → 904 KB (≈3.1 s) → Play: playhead moves, `voices 2`. A real VI49 note arriving during the test was recorded too.
- **Limits**: WAV render runs on the GUI thread (fine for short takes); no opening of old takes, metronome, quantize, multi-track; recorded timing uses driver timestamps (not yet measured against the audio clock).
- **Next**: P0.2 click on voice stealing.

### C3 — P0.2 click on voice stealing (31/10/2026)
- **Problem**: audible click when more notes are played than there are voices.
- **Cause**: `juce_Synthesiser.cpp:326` — a stolen voice gets `stopNote(0, false)` and the new note starts at the same instant; our `Voice` did `adsr.reset()`, so the old note fell from its current value to 0 in one sample.
- **Fix** (`src/Synth.h`, `Voice`): on a hard stop the last output value becomes a residual that decays exponentially (2 ms time constant) and is added to the voice output, so the waveform stays continuous. The voice keeps rendering the residual even when inactive, until it is below 1e-6. One float per voice, one multiply per sample.
- **Test**: `tests/RenderTest.cpp` — 1-voice engine, held sine C4, C5 steals the voice; largest sample-to-sample jump across 20 waveform phases vs the steady sine slope.
- **Before/after**: worst jump **0.1244 (20.2× the C4 slope) → 0.0124 (2.0×)**; 2.0× is exactly the natural slope of the new note (one octave up), i.e. no step left. Render cost 32 voices: 12.7–13.1 ms → 14.7–15.0 ms per second of audio (1.3 % → 1.5 % of one core, extra per-sample branch and residual math).
- **Limits**: the old note is not crossfaded (its waveform is replaced by a smooth decay of its last value) — inaudible at 2 ms for normal material; a true crossfade would need a second oscillator/filter state per voice. Steal choice is still JUCE's (oldest note, protects lowest/highest). Not verified by ear on hardware.
- **Next**: P0.3 gain staging (THD of the always-on tanh).
### C4 — P0.3 gain staging (31/10/2026)
- **Problem**: the output stage coloured every sound, single notes and especially chords.
- **Cause**: `SynthEngine::render` applied `tanh(x * gain)` to every sample. `tanh` is non-linear everywhere: harmonics on one note, intermodulation between the notes of a chord.
- **Fix** (`src/Synth.h`): `SynthEngine::safetyClip` — exactly linear up to 0.8 (−2 dBFS); above, a scaled `tanh` knee with matching value and slope at the threshold, bounded at 1.0 (touches 1.0 only by float rounding for huge inputs). Volume of normal playing unchanged (one default note: tanh(0.126) vs 0.126, −0.04 dB). Per-voice headroom (velocity × 0.3) kept: an 8-note full-velocity sine chord stays below the knee with default Volume.
- **Test** (`tests/RenderTest.cpp`): THD of one full-velocity A4 sine (harmonics 2–9, 1 s = 440 exact cycles); chord rendered together vs sum of its notes rendered alone (4 and 8 notes); knee properties (identity below 0.8, continuity, bound, symmetry). 10/10 runs pass.
- **Before/after**: one-note THD **−57.6 dB → −166.5 dB**; 4-note chord error **−31.0 dB → −143.4 dB**; 8-note **−25.5 dB → −141.3 dB** (after = float rounding floor). CPU unchanged (14.6 ms per second of audio, 32 voices).
- **Also fixed**: the P0.1 stall test was flaky (1/3 runs) because it slept 1 ms hoping the producer already held the lock; now the producer raises a flag from inside the lock and the measurement waits for it.
- **Limits**: overloads (very loud dense chords or Volume high) are still shaped by the knee, now only above −2 dBFS; no visible clip indicator yet (P0.5 diagnostics). Not verified by ear.
- **Next**: P0.4 numeric guards (NaN/Inf, DC, extreme filter settings, sample rates and block sizes).
### C5 — P0.4 numeric robustness (31/10/2026)
- **Goal**: prove the engine stays finite, bounded and quiet at the edges, at every supported rate and block size.
- **Test** (`tests/RenderTest.cpp`): 900 cases = rates 44.1/48/96 kHz × blocks 1/37/64/256/512 × 5 waves × 3 filter types × cutoff 20 Hz/20 kHz × resonance 0.5/10; each plays notes 0/21/60/108/127 (velocity 0.01 and 1), pitch bend to both extremes, cutoff slammed 20 Hz ↔ 20 kHz every block, then All Notes Off. Checks: every sample finite, peak ≤ 1, no voice left active, last block < 1e-4. Plus DC offset of each wave over exactly 440 cycles of A4, and A4 = 440 Hz at each rate. Whole suite ≈ 8 s; 10/10 runs pass.
- **Result**: 900/900 clean, no change needed for NaN/Inf, stability or stuck voices. Tonal DC 0.000000 of RMS at all rates; pitch exact at all rates.
- **Defect found**: the noise oscillator used `juce::Random()` seeded from the clock, so the same take exported twice gave different WAVs and noise renders weren't reproducible (seen as DC varying 0.0013–0.0065 between runs). **Fix** (`src/Synth.h`): fixed seed per voice (`index + 1`) — identical every run, still uncorrelated between voices. New test: two engines render identical noise. The noise DC check now uses its statistical bound (4/√N) instead of a fixed 0.01, which would have failed ~3 % of runs.
- **Limits**: if the sample rate changes while a note is held, that note keeps its old pitch increment until the next note-on (device switches stop audio, so not seen in practice). Denormals not measured (voices stop rendering when silent; residual cut at 1e-6).
- **Next**: P0.5 diagnostics (worst-case callback time, clip indicator, dropped MIDI count).
### C6 — P0.5 diagnostics (31/10/2026)
- **Problem**: no way to see dropouts (WASAPI shared reports no underruns), limiter activity or lost MIDI.
- **Fix**:
  - `src/Synth.h`: `SynthEngine` records the peak level *before* the safety clip (`peakLevel`, reset by the GUI) and counts the samples the clip shapes (`limiterHits`). One compare and one add per sample; atomics touched once per block.
  - `src/Main.cpp`: every audio callback is timed with the high-resolution counter; worst time kept in an atomic, and callbacks longer than the buffer duration counted as `over budget` (a certain dropout). Status bar now two lines: device/rate/latency (+ driver underruns when the driver reports them), then CPU, worst block vs budget (2 s hold), over-budget count, level in dBFS (falling peak), voices, `LIMITING` (1 s hold) and `MIDI dropped` when non-zero. Turns orange on any problem.
- **Test**: one normal note → 0 limiter hits, peak 0.189 (−14.5 dBFS pre-clip); 32-note cluster at full Volume → 13 374 hits, peak 3.66 (+11 dB) before the clip. Live app: idle worst block 0.02 of 10.0 ms; one held note level −20.1 dBFS, voices 1, over budget 0. Render cost unchanged (14.5 ms per second of audio, 32 voices). 5/5 runs pass.
- **Limits**: worst-block timing measures our callback only, not the driver/OS path, so `over budget 0` doesn't rule out dropouts caused elsewhere; level is the pre-clip peak of the synth output only. No 30-minute stress run yet (P2).
- **Next**: P1 — FFT aliasing measurement (naive vs PolyBLEP), band-limited triangle, PWM, mod wheel, LFO.
### C7 — P1 oscillator aliasing (31/10/2026)
- **Measurement first** (`tests/RenderTest.cpp`): oscillator alone (`Voice::shape`), 65 536-point FFT, Blackman-Harris window (−92 dB sidelobes), worst non-harmonic peak below 20 kHz in dBc, at A2/A4/A6/C8, 48 kHz.
- **Finding** (contrary to the roadmap's guess that the triangle was the problem): the 2-point PolyBLEP saw/square only gained ~12 dB over naive — **saw −59.5 / −47.3 / −35.5 / −28.8 dBc**, square similar; naive triangle −96 / −73 / −49 / −34 dBc. Spurious tones at −29 dBc on high notes are clearly audible.
- **Fix** (`src/Synth.h`): `Wavetables` — band-limited tables for saw, square, triangle built from the exact Fourier series of the previous shapes (same phase/sign), 4096 samples + guard, linear interpolation. One table per harmonic count 1…16, then three per octave (36 per wave). A note uses the largest count whose top harmonic is below Nyquist, chosen once per pitch change (`updatePitch`, so pitch bend too), never per sample. Built once at engine construction (15 ms, ≈1.8 MB), never on the audio thread; exact integer sine-table lookups. PolyBLEP code deleted.
- **Before/after** (worst alias, dBc): saw −59.5/−47.3/−35.5/−28.8 → **−97.5/−101.3/−122.5/−100.4**; square −59.5/−48.5/−37.8/−28.8 → **−97.5/−101.4/−122.5/−100.4**; triangle −96.2/−72.7/−49.3/−33.5 → **−103.6/−101.4/−125.8/−100.4** (≈ measurement floor). CPU 32 voices: 14.5 → 15.5 ms per second of audio (within noise of ±0.5 ms seen across runs). All other tests unchanged (THD, chord linearity, 900-case robustness, DC, pitch, determinism).
- **Also caught by the new selection test**: integer gaps in the first version's table spacing (…5, 6, 8…) dropped harmonics that would have fitted on high notes; fixed by one table per count up to 16. Guarantee now: top harmonic ≥ 0.38 × sample rate (18.2 kHz at 48 kHz), or no further harmonic fits below Nyquist; very low notes are capped at 2047 harmonics by the table size.
- **Limits**: band-limited saw/square show Gibbs overshoot (peak ≈ 1.18 instead of 1.0 per voice), which is the correct band-limited waveform; switching tables during a pitch bend changes the top harmonic count abruptly (tiny high-frequency step, not measured); not compared by ear (should sound cleaner on high notes, especially with pitch bend). No oversampling needed.
- **Next**: P1 — mod wheel + LFO (vibrato/filter), then PWM.
### C8 — P1 LFO + mod wheel (31/10/2026)
- **Design** (`src/Synth.h`): one sine LFO per voice, restarted at note-on (reproducible renders), rate 0.1–20 Hz, depth 0–1, target pitch (±1 semitone at full depth), filter cutoff (±2 octaves, clamped 20 Hz…0.45·fs) or volume (tremolo down to 1 − depth). Mod wheel (CC1) is read by `SynthEngine::render` from the block's MIDI before the voices run and stored engine-wide (`SynthParams::modWheel`), so notes started after the wheel moved get it; it adds to the depth knob. Effective depth smoothed over 20 ms. Wavetable chosen for +1 semitone so vibrato can't alias. GUI: *LFO to* combo, *LFO rate* and *LFO depth* knobs (`src/Main.cpp`); recorded takes keep CC1, so playback and WAV export reproduce the wheel.
- **Tests** (`tests/RenderTest.cpp`): vibrato measured from interpolated zero crossings of a sine A4: **415.3…466.2 Hz** (target 415.305…466.164, < 0.5 %), **10 LFO cycles in 2 s** at 5 Hz; mod wheel 127 on a note started after the wheel → same vibrato; wheel 0 → frequency flat (< 0.1 Hz spread); tremolo depth 0.5 → level ratio **0.500**; filter LFO on a low-passed saw → brightness (RMS of sample difference) swings **×5.2**; wheel slammed 0→127 with tremolo → no sample jump above the steady slope; the 900-case robustness sweep now also runs the LFO at 20 Hz, full depth, all targets: 900/900 clean. 5/5 runs pass.
- **Mistake in my first filter test**: plain RMS of an A2 saw barely moves (0.059…0.075) because its strongest harmonics sit below the lowest cutoff the LFO reaches; the filter was working, the metric was wrong.
- **Cost**: 32 voices, LFO off: 15.3 → 17.5–19.0 ms per second of audio (extra per-sample branch/clamp); 32 voices all with vibrato: **51 ms** (5.1 % of one core: `sin` + `exp2` per sample per voice). Within budget; candidate for a measured P2 optimisation (phasor LFO, polynomial exp2, control-rate updates).
- **Limits**: single sine shape; one LFO for everything; mod wheel is engine-wide (not per MIDI channel); vibrato depth fixed at ±1 semitone; not verified by ear or with the VI49's wheel.
- **Next**: P1 — PWM (needs a band-limited pulse: two saw tables), ADSR timing test, fast-knob click test.
### C9 — P1 pulse wave + PWM (31/10/2026)
- **Design** (`src/Synth.h`): wave 5 *Pulse* = band-limited saw minus the same saw shifted by the width (`Voice::pulse`): high 2−2w for a fraction w of the cycle, low −2w, zero mean, 2 peak-to-peak; alias-free by construction because both saws come from the C7 tables (no new tables). `SynthParams::pulseWidth` 0.05–0.95 smoothed over 20 ms; LFO target 3 *Width* adds ±0.45 × depth (classic PWM), clamped 0.05–0.95. GUI (`src/Main.cpp`): *Pulse* in Waveform, *Width* knob, *Width* in LFO to; also ships the shortened "LFO to" label left pending from C8. Existing wave/target indices unchanged.
- **Tests** (`tests/RenderTest.cpp`): pulse 25 % spectrum vs theory |h_k| ∝ |sin(πkw)|/k: **h2/h1 0.7071** (want 0.7071), **h4/h1, h8/h1 ≈ 2e-9** (want 0); PWM via LFO sweeps the measured duty cycle **0.05…0.95**; aliasing rows for pulse 25 % and 10 %; robustness sweep now 1080 cases (pulse + width target): 1080/1080 clean; tonal DC 0 for the pulse too. 5/5 runs pass.
- **Finding → fix**: narrow pulse on low notes measured **−87.3 dBc** (pulse 10 %, 110 Hz), above the −90 dBc bar. Cause: linear-interpolation error on the ~200 harmonics of low-note tables at 4096 points, relatively larger because a 10 % pulse's fundamental is 10 dB weaker (sin(0.1π) = 0.31). Fix: tables 4096 → **8192** points. After: pulse 10 % **−99.9 / −99.5 / −115.7 / −99.6 dBc**; saw at 110 Hz improved too (−97.5 → −103.8). Cost: table build 15 → **79 ms** at engine construction (39 tables per wave), memory ≈ 1.8 → **3.8 MB**; render CPU unchanged (17.9 ms per second of audio, 32 voices).
- **Limits**: pulse uses two table reads per sample; width changes on held notes move the edge position, which is inherently discontinuous (smoothed over 20 ms, not separately click-tested); not verified by ear.
- **Next**: P1 remaining — ADSR timing accuracy test, fast-knob click test, WAV reference files.
### C10 — P1 envelope timing, fast-knob clicks, reference WAVs (31/10/2026)
- **Suspicion from the source** (`juce_ADSR.h:259,263,195`): `setParameters` — which we call every block to follow the knobs — resets the release rate to `sustain / release`, and with sustain 0 ends the release at once; in sustain the level jumps to a new sustain value.
- **Measured** (`tests/RenderTest.cpp`; envelope = per-cycle peak of a 1-voice sine C8): A (attack 0.1, decay 0.2 → 0.5, release 0.3): correct (half 0.0501 s, peak 0.0996 s, mid-decay 0.744, sustain 0.495, mid-release 0.250). **B** (released at 0.5 with sustain 0.2, release 0.2 s): **0.49 s**. **C** (sustain 0, released at 0.8): **cut in 0.0002 s**. Fast-knob test on a held sine (worst step in 50 ms after slamming a knob): **Volume 14–15×**, **Sustain 4.7×**, **Resonance 1.34×** the steady slope; Cutoff 1.04×, LFO depth 0.99× fine.
- **Fixes** (`src/Synth.h`): own linear `Envelope` (same shape as juce::ADSR; release always lasts `release` seconds from the level at key-up, whatever the knobs do; sustain changes glide full scale in 20 ms); master Volume smoothed 20 ms in `SynthEngine::render`; resonance smoothed 20 ms per voice.
- **After**: B **0.1963 s** (expected 0.1960 to the 1 % mark), C fades **0.2965 s** (0.2963) with mid-release 0.402 (0.402); Volume 0.98/1.00×, Sustain 1.00×, Resonance 1.01×, Cutoff 1.04×, LFO depth 0.99×. All earlier results unchanged (THD, chord linearity, 1080-case sweep, aliasing, vibrato/tremolo/PWM, voice steal). 5/5 runs pass.
- **Mistakes of mine, corrected**: (1) test A first "failed" on release 0.2942 s vs 0.3 — a linear release from L crosses the 1 % end mark at R·(1 − 0.01/L) = 0.294 s, so the engine was right; expectations now use that formula. (2) Resonance at 1.06× was the resonant filter ringing louder for a moment, not a discontinuity: the click metric is now worst step / peak against a clean sine's 2·sin(πf/fs). A negative control (sine → square mid-note) proves it detects real clicks: **10.5×** — the old raw metric scored that same click **1.00×**, i.e. it would have missed it. (3) A `printf` of a temporary JUCE String printed garbage paths in the reference writer; fixed.
- **Reference WAVs**: `RenderTest <folder>` writes 8 renders through the Export WAV path, normalized to −20 dBFS RMS (never clipped): `docs/measurements/p1-refs/01_saw_chord … 08_pluck_sustain0.wav` (saw chord, square bass, PWM, mod-wheel vibrato, filter sweep, high saw arpeggio, triangle tremolo, sustain-0 plucks). Keep them to compare by ear after future DSP changes.
- **Limits**: waveform and filter-type switches mid-note are inherently discontinuous (not smoothed); not verified by ear.
- **Next**: P2 — presets (versioned JSON), MIDI Learn, effects, 30-minute stress run, CPU optimisation of the LFO path.
### C11 — P2 presets (31/10/2026)
- **Design**: one parameter table, `SynthParams::forEach` (key, value, valid range), shared by presets and MIDI Learn. `src/Presets.h`: JSON via JUCE (`{"format":"LocalMIDIStudio.preset","version":1,"name",...,"params":{...}}`). Loading validates at the file boundary: other formats / newer versions / malformed JSON / missing params refused with a message and the sound untouched; unknown keys ignored; missing keys keep their value; values clamped to range. 9 built-in presets defined in code and loaded through the same JSON path (Init, Analog Synth, Synth Bass, Ambient Pad, Electronic Lead, Electric Piano (synth), Organ (synth), Pluck, PWM Strings) — names mark imitations; Grand Piano and Drum Kit from the master prompt need samples (sampler not built) so they are not faked. GUI (`src/Main.cpp`): preset menu (Built-in / Yours) + *Save preset* (name dialog → `Documents\LocalMIDIStudio\Presets\<legal name>.json`); every control re-syncs from `engine.params` after a load (engine smoothing handles the jumps).
- **Tests**: round trip of all 13 parameters; clamping (cutoff 99999 → 20000, wave 9 → 5, sustain −3 → 0), unknown key ignored, missing key kept; version 2, garbage text, other format, missing params all refused with the sound untouched; **9/9** factory presets load and play (finite, ≤ 1, audible, all voices released). **In the real app** (posted clicks, cursor untouched): menu lists the 9 sounds; choosing *Synth Bass* set Square / 0.003 / 0.250 / 0.300 / 0.100 / 600 / 4.000 on every control.
- **Limits**: the *Save preset* dialog was not exercised automatically; no delete/rename in the app (files can be removed from the folder); presets store the sound only, not the MIDI map.

### C12 — P2 MIDI Learn (31/10/2026)
- **Design**: `src/MidiMap.h` — (MIDI channel, CC) → parameter key; one CC per parameter and one parameter per CC (re-binding replaces both), JSON persistence (`%APPDATA%\LocalMIDIStudio\midi-map.json`) validated on load (bad entries skipped, wrong format → empty map). GUI: right-click a knob → *MIDI Learn* / *Forget MIDI mapping*; the next CC binds (sustain CC64 and channel-mode CC120–127 excluded); bound CCs move the on-screen knob through `proportionOfLengthToValue`, so the knob's own scale (e.g. logarithmic cutoff) applies and the usual smoothing prevents zipper; label shows `Name CCnn`. All map access is on the message thread (MIDI thread posts CCs with `callAsync`).
- **Tests**: bind, lookup, re-learn moves a parameter, re-use takes a CC from its old parameter, forget, JSON round trip, hand-edited file with out-of-range/malformed entries, garbage and wrong format. 5/5 runs pass.
- **Not verified**: learning from the real VI49 knobs (needs the hardware; no virtual MIDI port here) and the right-click menu in the running app. Recorded takes store raw CCs, so playback/WAV export do not re-apply the knob mapping.
- **Next**: P2 — effects (delay, reverb, chorus), 30-minute stress run, LFO CPU optimisation.
### C13 — P2 effects: chorus, delay, reverb (01/11/2026)
- **Design** (`src/Synth.h`, `SynthEngine::applyEffects`): voices → chorus (`juce::dsp::Chorus`) → delay (`juce::dsp::DelayLine`, linear interpolation, feedback) → reverb (`juce::Reverb`, stereo) → Volume → safety clip. Six parameters added to the `forEach` table (so presets, WAV export and MIDI Learn get them for free): chorus mix, delay time/feedback/mix, room size, reverb mix. All allocation in `prepare`; processing in ≤ 512-sample chunks. Effects **always run** like sends: a mix of 0 only silences the wet part. First design skipped effects at mix 0 — rejected before testing because a delay switched on mid-note would start its echo abruptly from an empty buffer (a click 0.35 s later, outside the click test's window). `copyFrom` rewritten from `forEach` so new parameters can't be forgotten by the WAV export.
- **Tests** (`tests/RenderTest.cpp`): delay echo lag by cross-correlation **0.2500 s** (set 0.25), echo1/dry **1.000**, echo2/echo1 **0.500** (feedback 0.5); reverb tail to −40 dB **0.49 s** (size 0.3) vs **2.31 s** (size 0.9), L/R correlation 0.68 / 0.35 (stereo); chorus correlation with dry 0.70 (modulated); clicks on the three mix knobs 0.95–1.00× a clean sine; every effect flat out (feedback 0.9, room 1, delay time swinging 0.02↔1.5 s) at 44.1/96 kHz with blocks 1/37/512/1000: finite and bounded. With all mixes at 0 the dry tests are unchanged (THD −166.5 dB, chord −143 dB): bit-transparent. 5/5 runs pass.
- **Findings → fixes**: (1) chorus at full mix measured **−3.09 dB** quieter (dsp::Chorus mixes linearly; 50/50 of partly decorrelated halves loses power) → equal-power compensation 1/√((1−w)²+w²), smoothed like the chorus's own mix, exactly ×1 at mix 0 → **−0.08 dB**. (2) My first delay timing measurement (half-amplitude onset) said 0.2492 s; cross-correlation shows the echo is exact — the method was imprecise, not the delay.
- **Cost**: effects always on add ≈ 3 ms per second of audio regardless of voice count (32 voices: 18 → 21 ms, 2.1 % of one core).
- **Presets**: Ambient Pad (chorus + large reverb), PWM Strings (chorus + reverb), Electric Piano (light chorus), Electronic Lead (delay); 9/9 still load and play.
- **Limits**: chorus is the same on L and R (width comes from the reverb); no tempo sync for the delay; saturation and EQ not done; not verified by ear.
- **Next**: P2 — 30-minute stress run, LFO CPU optimisation, level meter.
### C14 — P2 30-minute real-time stress run (01/11/2026)
- **Harness** (`tests/RenderTest.cpp`, `RenderTest --stress <minutes>`): the real default output device (Cuffie, WASAPI shared, 48 kHz, 480-sample buffer = 10 ms budget) driven by the app's `SynthEngine` and lock-free `MidiQueue`; every block is checked (finite, peak) and then **silenced** before reaching the device. A "keyboard" thread sends random notes/velocities, note-offs, mod wheel, pitch bend, sustain pedal, 40-note steal storms and All Notes Off every 5–45 ms; a "GUI" thread moves a random parameter of the 19 in the table every 50 ms (effects and LFO included) and loads a random factory preset every ~10 s. One line per minute: worst callback time, over-budget callbacks, bad samples, peak, max voices, CPU, process private memory (psapi). Pass = no over-budget callback, no bad sample, peak ≤ 1, no voice left after All Notes Off + 2 s, memory growth < 5 MB after the first minute.
- **Result**: **PASSED**. 180 196 callbacks over 30 minutes; worst callback **2.464 ms of 10 ms** (per-minute worst 1.84–2.46 ms, no upward trend); **0** over-budget; **0** bad samples; peak 1.000 (the safety clip holding full scale during random max-Volume steal storms); 32 voices reached every minute; **0** voices left; memory **7.2 → 7.2 MB** (+0.01 MB) — no leak; 78 378 MIDI events, 28 801 knob moves, 144 preset loads; **0** MIDI dropped. Log: `docs/measurements/stress-30min.log`. Trial run of 2 min beforehand: same picture.
- **No code change needed.**
- **Limits**: measures our callback only (not the driver path); WASAPI shared reports no driver xruns (−1), so a dropout caused outside our code would not show; the user's VI49 and the GUI were not in the loop (random MIDI and parameter writes stand in for them); one machine, one buffer size (480).
- **Next**: P2 — LFO CPU optimisation (vibrato path 51–55 ms/s for 32 voices), level meter; or P3.
### C15 — P2 CPU: control-rate LFO, filter coefficient updates (01/11/2026)
- **Benchmark first** (`tests/RenderTest.cpp`): ms to render 1 s with 32 sounding voices, median of 5 runs, per LFO target (the old single-run figure varied ±1–2 ms). Baseline, stable to ±0.5 ms over 3 runs: LFO off **21.4**, vibrato **55**, filter LFO **93.5**, tremolo 27.7, PWM 29.0 — the filter LFO, not vibrato, was the worst case.
- **Step 1 — control-rate LFO** (`src/Synth.h`, `Voice::lfoAt` + interpolation): sin/exp2 once per 16 samples (3 kHz), the four LFO effects (pitch multiplier, cutoff multiplier, amplitude, width offset) interpolated linearly in between — no steps; theoretical error at the 20 Hz maximum ≈ 2·10⁻⁴ of the depth (−73 dB). Result: off 22.0, vibrato **26.2**, filter **30.6**, tremolo 23.4, PWM 25.6. Accuracy tests unchanged: vibrato 415.3…466.2 Hz and 10 cycles, tremolo 0.500, filter ×5.2, PWM 0.05…0.95, wheel slam 0.99×, 1080/1080 robustness.
- **Step 2 — skip redundant filter coefficient updates**: `setCutoffFrequency` costs a `tan()` and was called every sample on every voice even with a still cutoff; now only when the value changes. Result: off **12.9**, vibrato **16.8**, filter **28.5**, tremolo **13.9**, PWM **17.3**. THD −166.5 dB, aliasing and the Cutoff click test identical.
- **Overall**: −40 % (off) to −70 % (filter LFO). Real device, 2-minute stress: worst callback **2.2–2.46 → 0.99 ms** of 10 ms, 0 over budget, 0 bad samples, no leak. 5/5 runs pass.
- **Limits**: the filter LFO still updates coefficients every sample (cutoff genuinely changes each sample); left as is at 2.9 % of one core worst case. The "off" path gained ~0.6 ms from interpolating four values before step 2 recovered it and more.
- **Next**: level meter, or P3 (sampler for Grand Piano / Drum Kit, wavetable, modulation matrix).
### C16 — P3 sampler (01/11/2026)
- **Design** (`src/Synth.h`): `SampleRegion` (file, key range, velocity range, root, tune, volume, one-shot, loop points, shared audio), `InstrumentSound` (picks the first region matching note + velocity), `SampleVoice` (4-point Hermite resampling, file-rate → output-rate conversion, pitch bend, vel² gain = SFZ's default curve, envelope attack 1 ms / release = the Release knob, one-shot ignores note-off, loops, the same 2 ms residual declick on stealing and when a sample ends abruptly). The engine now holds 32 synth + 32 sample voices in the one `juce::Synthesiser`; `setInstrument` (message thread) removes the old sound, cuts sounding notes so voices drop their references there, then adds the new one — big sample sets are never freed on the audio thread. Effects, Volume, limiter, recording and WAV export (`renderOffline` takes the instrument; sample data shared, not copied) all apply.
- **Loader** (`src/Sfz.h`): SFZ subset — `<control> default_path`, `<global>/<group>/<region>` inheritance (a region's `key=` overrides inherited ranges), `sample` (paths with spaces), `key/lokey/hikey/pitch_keycenter` (numbers or note names, c4 = 60), `lovel/hivel`, `tune`, `transpose`, `volume`, `loop_mode/loop_start/loop_end`, `//` and `/* */` comments; release-triggered regions skipped; unknown headers/opcodes ignored. Or a single WAV/FLAC/OGG/AIFF (root C4, whole keyboard). Validated: unreadable numbers, missing files, no regions → error message, nothing loaded; bad loop points → plays once. Audio read fully into memory on a background thread (`juce::Thread::launch`), installed on the message thread. GUI: *Load instrument…* / *Synth*, current instrument shown.
- **Tests** (`tests/RenderTest.cpp`, synthetic WAVs + an SFZ written at test time): note names; parse (4 regions, inheritance, key override, space in path, release region skipped); pitch — A4 sample at 44.1 kHz played as C5 at 48 kHz: **523 Hz** (523.25); velocity layers × vel²: level ratio **24.8** (25); one-shot keeps playing after an early note-off, a normal region releases; 1 s loop held 3 s: level **0.064 → 0.064**, seam 1.00× a clean sine (no click); Hermite error vs the ideal sine at +5 semitones **−110.9 dB**; switching instrument mid-note leaves 0 voices; missing file / bad number / empty SFZ → three clear errors. 5/5 runs pass; synth results unchanged.
- **Mistake of mine**: the first pitch check counted cycles past the end of the transposed 1 s sample (it lasts 0.84 s at +3 semitones) and read 388 Hz; the interpolation test at +5 semitones (−110.9 dB vs the ideal at the expected frequency) already showed the pitch was right.
- **Limits**: one region per note (no layered/stacked regions, no round-robin `seq_*`, no `#define/#include`, no `ampeg_*`/`amp_veltrack` per region — the Release knob applies), whole sets loaded into RAM (a 400 MB library takes ~1–1.5 GB as float), no filter on sampler voices, WAV export tail = Release + 0.5 s (long one-shots may be cut). Not yet tried with a real sample library or by ear.
- **Libraries found** (not downloaded — waiting for the user's OK): Salamander Grand Piano V3 (Yamaha C5, 16 layers, CC-BY 3.0, 16-bit/44.1 kHz package 394 MB, plain SFZ); drums: Salamander Drumkit (CC-BY-SA 3.0, ~370 MB, plain SFZ) or Gogodze Phu Vol II (CC0, 133 MB, but Karoryfer SFZ use `#define`/`#include`, not supported yet).
### C17 — P3 sampler on real libraries (01/11/2026)
- **Downloaded** (user approved): Salamander Grand Piano V3 44.1 kHz/16-bit (412 MB .tar.xz, CC BY 3.0) and Salamander Drumkit (388 MB .tar.bz2, CC BY-SA 3.0) into `F:\claude\samples`, credits in `F:\claude\samples\CREDITS.txt` and the README. Windows `tar.exe` hung on these archives (0 MB extracted, idle) — extracted with Python `tarfile` and the `data` filter (no absolute paths / `..` / links out).
- **What the real files needed** (opcode census of both SFZ): drums — `lorand/hirand` (462×), `seq_length/seq_position`, `amp_veltrack`, `group/off_by` (hi-hat choke), `ampeg_release`; piano — `amp_veltrack`, `ampeg_release`, `pitch_keytrack`, and `lokey=-1` (regions triggered by controllers, e.g. pedal noise). **Added** (`src/Synth.h`, `src/Sfz.h`): random alternate takes, per-key round robin (atomic counters), velocity tracking, per-region release overriding the knob, no-transpose regions, lock-free choke (atomic per-group epoch; a choked voice fades in 5 ms, checked once per block), `lokey=-1` regions skipped. Error messages now name the opcode and value (`cannot read lokey=-1` found the piano problem at once). Each instrument is normalized on load by one common gain bringing its loudest sample to full scale (≤ +12 dB) — keeps the library's balance.
- **Tests** (synthetic, `tests/RenderTest.cpp`): round robin loud/soft/loud (0.109/0.030/0.118), random takes 22/18 of 40, `amp_veltrack=0` ratio 1.00, `ampeg_release=0.05` beats a 0.3 s knob (tail 0), `pitch_keytrack=0` stays at 440 Hz five semitones up, open hi-hat choked by the closed one (level 0), normalization gain exactly 1/0.8; earlier sampler and synth results unchanged; 5/5 runs.
- **Real libraries** (`RenderTest --probe <sfz>`): drums 532 regions, keys 35–64, 1.7 s, +636 MB, finite and bounded; piano 480 regions, keys 21–108, 0.7–2.0 s, **+2.2 GB**, finite and bounded, louder with velocity on every note tested (e.g. note 61: 0.006 / 0.050 / 0.151 at velocity 0.2 / 0.6 / 1.0).
- **Mistakes of mine, corrected**: the first probe let the previous note's 1 s release leak into the next measurement and showed velocity 0.2 louder than 0.6 — a fresh engine per measurement fixed it; the interpolation test's ideal amplitude had to include the new normalization gain.
- **Honest limits**: normalization did not raise these two libraries (each already has a near-full-scale sample — cymbals, forte notes); their quiet feel is the shared per-voice headroom (0.3) × Volume 0.6, the same as the synth — raise *Volume*. Kick at velocity 0.2 is nearly silent (the library's softest layer × `amp_veltrack=93`). Release-trigger regions (damper noise) and controller-triggered regions are not played. Choke latency up to one audio block (~10 ms at 480 samples). Not yet heard by the user.
- **Next**: in-app instrument library (download, extract, load: guitars, sax, trumpet, violins…), then song mode.
### C18 — P3 SFZ v2 subset + in-app instrument library (01/11/2026)
- **Why**: the user asked to download and load more instruments (guitars, saxophones, trumpets, violins). Free ones with clear licences: Karoryfer's CC0 libraries hosted as repositories by the sfzinstruments GitHub organisation (ZIP via `archive/HEAD.zip`, all 8 checked: HTTP 200, `application/zip`, no length header). No CC0 trumpet/violin in that set; orchestral options (VSCO 2 CE, Sonatina) are large or not directly downloadable — left out for now.
- **SFZ features real Karoryfer files need** (read from Weresax's `Sax.sfz` + maps, text only): `#include` (relative, nested ≤ 8, missing file named in the error), `#define $VAR`, `<master>`, `<control> note_offset/octave_offset`, `set_ccN`/`set_hdccN` defaults driving any `*_onccN` (e.g. `ampeg_release=0.001` + `ampeg_release_oncc104=2` at cc104 = 20 → 0.316 s), `amplitude_onccN` and `xfin/xfout_locc/hicc` evaluated at those defaults — regions silent there are skipped (Weresax: the `*noise` breath region and the dynamic-mic layer), built-in `*` generators skipped, `sw_last` vs `sw_default` picks the default articulation. Added in `src/Sfz.h`; still one region per note.
- **Library** (`src/Library.h`): catalog (id, name, kind, licence, ~size, URL); library folder (saved in `%APPDATA%\LocalMIDIStudio\library-folder.txt`, default `Documents\LocalMIDIStudio\Instruments`; set to `F:\claude\samples` for this user); `install` streams the ZIP with progress on a background thread, honours a cancel flag, extracts with `juce::ZipFile` (which refuses entries outside the target), deletes the archive. GUI (`src/Main.cpp`): *Library...* menu — installed `.sfz` grouped per library, catalog items (disabled while a download runs or when installed), cancel, choose/open folder; progress in the instrument label; *Load instrument...* and the library share one background loader.
- **Tests** (`tests/RenderTest.cpp`, offline): Karoryfer-style SFZ written at test time (#define, #include of two mic maps, master, set_cc, xfades, amplitude_oncc on `*noise`, two articulations, note_offset) → exactly the 2 audible regions, keys shifted, release 0.001 + 2·20/127; missing `#include` named in the error; catalog URLs/licences/unique ids; install from a locally built ZIP via a file URL (bytes reported = file size, `.sfz` found and loads, archive removed); a ZIP entry `../escaped.sfz` refused ("Entry ../escaped.sfz is outside the target directory"), nothing written outside; cancel. Real piano/drums still load (`--probe`). 5/5 runs pass.
- **Bug found by the cancel test**: the partial archive was deleted while its stream was still open — Windows refuses, so the file stayed; now the stream is closed first.
- **Not verified**: a real download through the app, and the *Library...* menu itself — posted clicks can't press a JUCE button (it fires only if the real pointer is over it on release; the preset combo worked because it opens on press), and moving the user's mouse was not acceptable. Layered regions (several per note), keyswitch changes while playing, CC modulation in real time and `.7z`/`.tar.*` archives are not supported.
- **Next**: song mode (tracks, timeline, metronome), then step sequencer / piano roll / mixer, then vocal tracks and song export.
### C19 — Drum pad view (01/11/2026)
- **Why**: the user wanted to see what each VI49 pad plays. The VI49's factory pad notes and pad layout could not be confirmed from reliable sources (reviews only), so nothing is assumed: the view learns from the hardware.
- **Design** (`src/Main.cpp` `DrumPads`, `src/Sfz.h` `drumName`): 16 cells (pad 1 bottom-left; 4×4 or 2×8), default notes 36–51 until learnt; each cell shows the sound name for its note — from the loaded kit's sample file name for that single key (`kick_OH_FF_1` → *kick*, `hihatClosed_…` → *hihat Closed*), else the General MIDI percussion name (35–81, table checked by a `static_assert` on its length), else the note name (keyboard convention, middle C = C3); lit by incoming note-ons (velocity sets brightness, ~30 Hz fade); click plays the note through the on-screen keyboard path (queued, recorded). *Learn pads*: cells light in blue one by one, the next note received is stored for that cell; *Cancel learning* restores the saved map. Saved in `%APPDATA%\LocalMIDIStudio\pads.json`, validated on load (16 notes in 0–127, else defaults).
- **Tests**: `drumName` on a synthetic kit — *kick*, *hihat Closed*, keyed (non-drum) region falls back to GM *Low Tom*, GM *Snare* / *Acoustic Bass Drum* / *Open Triangle*, note 100 → *E6*. 5/5 runs. Screenshot of the running app: the 16 GM names in the left column.
- **Mistake of mine**: the test first expected *E7* for note 100 (SFZ's octave convention); the app deliberately uses the on-screen keyboard's convention, so *E6* is right.
- **Not verified**: learning and lighting with the real VI49 pads (needs the user's hands); click-to-play in the running app (posted clicks reach `mouseDown` on custom components, but not tried).
### C20 — Song mode core + drums on their own track (01/11/2026)
- **Why**: the user wanted pads to play drums while the keys play another instrument, instruments chosen next to the keyboard, and (earlier) a way to compose songs from several parts.
- **Engine** (`src/Song.h`, GUI-independent): up to 8 tracks built up front (no allocation when adding), each its own `SynthEngine` + MIDI clip in seconds; live MIDI to the selected track, pad MIDI to the drum track; mute/solo (muted tracks still render so envelopes stay consistent); transport with one-bar count-in and metronome (accented bar start); master safety clip after the sum; `songZeroWall` = wall time of song 0 s, recomputed every block with the same rule MidiQueue uses to place events, so a recorded note lands exactly where it was heard; `mixdown` renders audible tracks offline with their engines and sums. `PadRouter`: lock-free (channel, note) → kit sound table; `split` sends pad notes (re-mapped) to the drum track and everything else to the keys.
- **App** (`src/Main.cpp`): the single engine became `Song` (track 1 Keys, track 2 Drums); knobs/combos rebound with pointers-to-member so they always act on the selected track (refreshed on selection); presets, *Load instrument*, *Library...*, *Keys play:* act on the selected track; *Drums* row (kit picker, auto-loaded last kit or `<library>/drums/ALL.sfz`, volume); pads store trigger channel+note (learnt) and a kit sound (right-click menu of the kit's sounds, or GM if no kit), saved in `pads.json`, router rebuilt on change; track view (rows: name + instrument, M/S, notes, recording take in red, playhead; click selects, not while recording); transport: Rec = record the selected track after count-in while the others play (replaces its clip; pads recorded as kit sounds only onto the drum track), Play = whole song, auto-stop 2 s after the end, *Export song* = mixdown WAV; BPM box, Click toggle, + Track. The old single-take playback path and per-take `.mid` auto-save were removed.
- **Tests** (`tests/RenderTest.cpp`): tracks sound at their times (0.5 s / 1.0 s, silence before), mute and solo, live notes only to the selected track, count-in click at the first sample and clip starting exactly at bar 1, `songZeroWall` = 201.9947 (expected), timing identity (recorded time = heard position within ½ sample), stop releases all voices, a recording track doesn't play its old clip, mixdown length with/without a muted track and bounded; router: pad (ch 10, note 36) → kit 35 at the same sample positions, key/CC to keys, same note on another channel not a pad; pads reach the drum track while track 0 is selected. 5/5 runs. App screenshot: kit auto-loaded (532 regions), pads named from the kit, two tracks, BPM/Click/+Track, *Keys play*.
- **Mistake of mine**: the routing test first ignored the 20 ms Volume smoothing (a gain set to 0 just before a note still lets ~20 ms through); it now waits for the smoothing.
- **Not verified**: clicking in the new GUI — posted clicks stopped reaching the app in this session (even the on-screen keyboard that accepted them before), and the user's mouse was not moved; the real VI49 pads' channel (factory pads may share channel 1 and notes with low keys — then those keys play drums; documented). Song save/load not done yet; no editing (quantize, piano roll), no audio/vocal tracks yet.
- **Next**: save/load song, then drum step sequencer, piano-roll editing + quantize, mixer, vocal (audio) tracks.
### C21 — Open MIDI + genre sketches (01/11/2026, parallel session)
- Done in another session on the same files at the same time: *Open MIDI...* (`readMidiParts` in `src/Recording.h`: channel 10 → Drums track, each other file track → its own track, tempo → BPM; asks before replacing clips), `tools/genres.py` (stdlib; 8-bar house / hip hop / rock / bossa nova / reggae sketches to `Documents\LocalMIDIStudio\Genres`), `RenderTest --probe x.mid` lists parts. Its tests run in the same suite (5/5 with C22). Details in the README.

### C22 — Save / open song (01/11/2026)
- **Format** (`src/SongFile.h`): `.lmsong` JSON — format name + version, bpm, metronome, selected track, and per track: its sound (the preset JSON, so it gets the preset validation), instrument path, mute, solo, events as `[seconds, status, data1, data2]`.
- **Validation at the file boundary**: other format / newer version / no tracks / more than 8 tracks / malformed or invalid events (non-channel status such as SysEx, wrong length for the status, data byte > 127, negative or non-finite or > 24 h time) refuse the whole file with a message naming the track; parsing goes into a separate `SongFile::Loaded`, the current song is replaced only after the whole file validated.
- **App** (`src/Main.cpp`): *Save song* (save dialog, overwrite warning, `Documents\LocalMIDIStudio\Songs`), *Open song* (stops the transport first; at least 2 tracks so track 2 stays the pads' drum track; instruments reloaded in the background; a moved instrument is found again by folder + file name in the library, else the track falls back to the synth with a message); every recorded take also writes `Songs\autosave.lmsong`.
- **Tests**: round trip of 3 tracks (note on/off, pitch bend, CC on channel 10, channel pressure; sounds; mute/solo; bpm; metronome; selected; an unloaded instrument saved as empty) — identical bytes and times; 8/8 bad files refused. 5/5 runs.
- **Not verified**: the save/open dialogs and applying a loaded song in the running app (GUI clicks can't be automated in this session). Pad learning/assignments stay app settings (not per song).
- **Next**: drum step sequencer, piano-roll editing + quantize, mixer, vocal (audio) tracks.

### C23 — Drum step sequencer (01/11/2026)
- **Model** (`src/Steps.h`): 16 rows (one per pad, playing the pad's kit sound) x 16 sixteenth steps of one bar, repeated over 1..64 bars, rendered into the drum track's clip (channel 10, velocity 100, half-step notes). No new file format: songs save the clip; the grid reads the clip's first bar back, rounded to the nearest step.
- **App**: *Steps* toggle in the DRUMS panel swaps the pads for the grid + a bars box; the playing step is highlighted. A clip the grid did not write (recorded, opened song/MIDI) is never replaced silently: the first edit asks. Pattern clips follow BPM changes and pad sound changes; recorded tracks stay in seconds (not re-timed). Editing while playing stops and restarts the transport at the same position; editing while recording is refused.
- **Tests**: 2-bar pattern -> 28 notes, kick of bar 2 at exactly 2.5 s, round trip identical, a take ±12 ms off reads as the same pattern, `same()` tells equal / jittered / different-length clips apart. All tests pass.
- **Not verified**: the grid in the running app (posted clicks don't press JUCE buttons) — screenshot shows the Steps button only.

### C24 — Piano-roll editing, quantize, undo (01/11/2026)
- **Bug found first** (from C23): restarting the transport mid-song (an edit while playing) reset every track to event 0, so `addEventsInRange` fired every note already passed in the first block. Test: a note held across the resume point sounded at 0.113 RMS in the first 150 ms; `Song::render` now skips events before `startPos` on start → 0.0000. (Notes held across the resume point are not re-struck.)
- **Model** (`src/Notes.h`): clip ↔ list of notes (start/end s, key, velocity, channel); writing keeps every non-note event (pedal, CC, pitch bend) and adds note-offs before note-ons, so back-to-back repeats of a key both sound (negative control: ons first → second note 0.000). Quantize: starts to the nearest 1/16, lengths to whole steps (≥ 1).
- **App**: *Edit notes* swaps the track rows for a piano roll of the selected track (drum track: kit sound names on the keys, channel 10): click = add a 1/16 note (drag to lengthen), drag = move (snapped to 1/16), right edge = length, right-click = delete; wheel / Shift / Ctrl = pitch / time / zoom. *Quantize* = selected track. *Undo* = one level, covers takes, edits, quantize and the step grid (pressing it again redoes); cleared on Open song / Open MIDI. All clip changes go through `setClip` (stop, replace, resume at the same position).
- **Tests**: round trip (2 notes, unreleased note gets 0.25 s, CC + pitch bend kept), repeated key, quantize, resume. All tests pass.
- **Not verified**: the editor in the running app (screenshot shows the buttons only; posted clicks don't reach JUCE buttons). Velocity editing, multi-select, copy/paste not done.

### C25 — Mixer: pan per track, master volume (01/11/2026)
- **Already there**: per-track volume = each track's *Volume* knob (`gain`, smoothed since C10), saved in the song. Not duplicated in a separate mixer view.
- **Pan** (`src/Synth.h`): `SynthParams::pan` −1…+1 in the `forEach` table, so presets, song files, MIDI Learn, WAV export and the stress test's random knob moves get it for free. Applied with Volume, before the track's safety clip, smoothed 20 ms. Balance law (the signal is already stereo because of the reverb): the far side fades linearly, the near side stays ×1, so **centre is bit-identical to before** (THD −166.5 dB, chord −143.4/−141.3 dB, unchanged). Old presets/songs without `pan` keep the track centred (missing keys keep their value).
- **Master** (`src/Song.h`): `Song::master` 0…1 on the track sum + metronome, smoothed 20 ms, before the master safety clip; also applied in `mixdown` (Export song). Saved as `"master"` in `.lmsong` (clamped; missing = 1, so older songs open unchanged). GUI: *Pan* knob in OUTPUT (selected track), *Master* slider in the top bar.
- **Tests** (`tests/RenderTest.cpp`): pan centre L = R, hard left L = centre L and R = 0, +0.5 → L **0.500** / R **1.000** of centre; master 0.5 → live **0.500**, mixdown **0.500**; click test on Pan 0→1 **0.97×** and 1→0 **1.00×** a clean sine; song file round trip keeps pan and master; a song without `master` opens at 1 and centred. **Negative control**: same build with the pan ramp set to 0 → Pan **inf×** (left drops to 0 in one sample) and **17.1×**, `clicks == 0` fails. 3/3 runs `ALL TESTS PASSED`.
- **Cost**: CPU median 32 voices LFO off **13.0** ms/s (C15: 12.9), vibrato 16.9, filter 28.6, tremolo 14.2, PWM 17.1–17.8 — within noise. (A first baseline this session read 19.2 because the app was running at the same time.) Real device 2-minute stress: worst block **1.065 ms** of 10, 0 over budget, 0 bad samples, no leak.
- **Not verified**: Pan knob / Master slider by hand and by ear (screenshot shows them; posted clicks don't press JUCE controls); pan is part of the track's sound like Volume, so loading a preset saved with a pan also moves the track. No master click test of its own (same `SmoothedValue` ramp as the tested pan/Volume). Not done: mixer strip view with all tracks at once, level meters per track, pan law choice.
- **Next**: sound editor — oscillator tuning (semitones/cents), second oscillator + mix, waveform/envelope display.

### C26 — Sound editor: tuning, second oscillator, shape view (01/11/2026)
- **Tuning** (`src/Synth.h`): `tune` (−24…+24 semitones, whole steps in the GUI) and `fine` (±100 cents) for the whole voice, added to the pitch-bend semitones in `Voice::updatePitch`. `updatePitch` now also runs once per block (`updateParams`), so turning Tune on a held note acts within one block; the phase runs on, so the pitch changes without a waveform step (a pitch change is not a click and the step/peak metric can't judge it: a higher note has a steeper natural slope).
- **Osc 2**: `osc2Wave` (same 6 waves), `osc2Tune` / `osc2Fine` relative to osc 1, `osc2Mix` 0…1 smoothed 20 ms per voice: out = osc1 + mix·(osc2 − osc1). Own phase and **own band-limited table** chosen from its own pitch (`table2`). At mix 0 osc 2 is not computed at all and osc 1 is **bit-identical** to before (THD −166.5 dB, chord −143.4/−141.3 dB, aliasing rows unchanged). Its phase waits while the mix is 0 (first version kept it running per sample — removed: the mix ramps in from 0, so a waiting phase can't click). Same LFO pitch, pulse width and filter as osc 1. All in `forEach`: presets, songs, MIDI Learn, WAV export and the stress test's random knob moves get them. Factory presets are written from a default `SynthParams`, so loading one resets tune/osc 2 (and pan); user presets saved before today keep the current tune/osc 2 values (missing keys keep their value).
- **GUI** (`src/Main.cpp`): OSCILLATOR = Waveform + Tune + Fine + Width; new OSC 2 panel = wave + Tune 2 + Fine 2 + Mix; SHAPE panel (`ShapeView`): two cycles of osc 1 mixed with osc 2 (drawn with the engine's own `Voice::shape`, noise as random) and the ADSR outline (attack, decay, a hold, release in proportion). It repaints only when a parameter of the selected track changes (30 Hz check of the `forEach` values). Second row is now FILTER, LFO, EFFECTS, OUTPUT; knob labels squeeze (min scale 0.6) instead of being cut to "Delay ...".
- **Tests** (`tests/RenderTest.cpp`): Tune +12 → **880.00 Hz**; −1 st +50 c → **427.47** (427.474); osc 2 alone +7 st → **659.26** (659.255); Tune moved on a held note **440.00 → 880.00 Hz**; mix 0 with osc 2 = noise +5 st → output **identical** to the plain saw; osc 2 saw +24 st on A6 (7040 Hz): fold-back at 12.8 kHz **−144.1 dBc**; Osc 2 mix 0→1 (to a sine an octave down) **0.95×** a clean sine. **Negative controls** (same build with the mix ramp at 0 and osc 2 on osc 1's table): click **5.53×** → `clicks == 0` fails; fold-back **−14.0 dBc** (predicted ≈ −14: the 5th harmonic at 1/5) → `alias < -90` fails. ALL TESTS PASSED.
- **Cost**: the PC was busy this time (other apps), so absolute CPU figures drifted (LFO off 13.0 → 14–20 ms/s between runs). Interleaved A/B, same machine state, old single-oscillator loop vs new: LFO off **14.4/13.9/16.2 vs 19.7/14.5/16.0** ms, no consistent difference; new bench column *osc 2* (32 voices, detuned saw, mix 0.5) **18.3–21.4** ms/s, i.e. a second oscillator costs roughly +2–5 ms per second of audio (≤ 0.5 % of a core). Real device 2-minute stress: worst block **1.078 ms** of 10, 0 over budget, 0 bad samples, no leak.
- **Not verified**: the new knobs and the shape view by hand and by ear (screenshot shows the panels at default values; posted clicks don't move JUCE controls). Not done: osc 2 hard sync / FM / ring mod, separate osc 2 level per voice, envelope view with real time axis, waveform view of sampler instruments.
- **Next**: loop recording, multi-level undo/redo.

### C27 — Multi-level undo/redo, loop recording (01/11/2026)
- **Undo/redo** (`src/Notes.h` `History`, `src/Main.cpp`): replaces the single `undoClip`/`undoTrack`. Every `setClip` (takes, piano-roll edits, quantize, step grid, loop takes) pushes what the track had; *Undo* swaps it back and puts the replaced clip on the redo side, *Redo* the reverse; a new change clears redo; 50 steps (oldest dropped); cleared on Open song / Open MIDI as before. Undo no longer doubles as redo: it goes further back.
- **Loop recording** (`src/Song.h`, `src/Main.cpp`): *Loop* + *bars* (1–16) in the SONG header. With Loop on, Rec still counts in one bar, then the transport cycles over the bars; the recording track **also plays its old clip** (overdub) and every pass is heard on the next ones. Design: the audio thread itself keeps what is played live for the recording track (keys if it is selected, pads if it is the drum track — same rule as normal takes) in a preallocated `loopTake` (8192 events, absolute sample position) and replays earlier passes from it — no allocation, no lock, sample-exact (the wall-clock `record()` path is skipped while looping). The wrap may fall inside a block: clip events are read as [pos, L) + [0, rest) with the right buffer offsets, the metronome wraps too, and clip notes still sounding at the loop end get a note-off at the wrap (live keys are not cut). At Stop the passes are folded into one loop length (`Notes::fold`: a note still sounding at the loop end is cut there) and **added** to the track's notes through `setClip` (undoable), then autosaved.
- **Tests** (`tests/RenderTest.cpp`): history — 3 changes on 2 tracks undone newest first back to the originals, redo, new change clears redo, cap 50; fold — passes 0/1/2 land at 0.25/0.25/0.75 s, a note past the loop end ends at 2.00; engine — 0.5 s loop (wrap inside a 256-sample block), one live note in pass 0 → onsets **4901 / 28901 / 52901** (exactly 24000 samples apart), 2 events stored, silence between; a clip note 0.3–0.9 s is silent after the wrap (**0.000000**) and sounding before (0.104). **Negative controls** (same build without replaying earlier passes and without the wrap note-off): onsets 4901 / 62464 → fails; after the wrap **0.089** (hanging note) → fails. Full take scanned each block: first version **45.7 ms** for a 5.33 ms block (8192 events replayed at one instant: `MidiBuffer::addEvent` is O(n) per insert, and its capacity would have been exceeded = allocation on the audio thread) → replay capped at `MidiQueue::maxPerBlock` (256) per block → **0.085 ms**. ALL TESTS PASSED (3/3).
- **Not verified**: Loop / bars / Redo by hand (screenshot shows them; posted clicks don't press JUCE buttons); with the real VI49 and pads. The 2-minute stress harness drives `SynthEngine`, not `Song`, so it doesn't exercise this; not run this cycle.
- **Limits**: pedal, CC and pitch bend played in a loop take are not kept (notes only, `shortcut:` in `stopLoopTake`); more than 8192 events in one loop take are dropped (log says so); more than 256 replayed events in one block are dropped; the loop always starts at bar 1; no "replace" mode (loop takes always add).
- **Next**: vocal (audio) tracks, single-WAV sample player, packaging.

### C28 — Voice (audio) tracks (01/11/2026)
- **What**: any track can hold a recorded voice (mono) next to its notes. *Voice* (SONG header) + Rec = record the microphone into the selected track after the usual one-bar count-in; the track's notes and the other tracks keep playing; Stop makes it the track's voice (replacing the old one). The voice plays through the track's engine (`SynthEngine::render` gained an optional mono input, mixed before effects), so Volume, Pan, the effects (reverb on a voice), mute/solo and the safety clip apply, live and in *Export song* (`renderOffline` takes the voice too). Track rows show "+ voice"; the status bar shows the active mic or "mic: none".
- **Real time** (`src/Song.h`, `src/Main.cpp`): the input arrives in channel 0 of the callback buffer and is copied to a preallocated `micBuf` before the song overwrites it; the audio thread writes it at the song's own sample position into `voiceTake`, which the GUI allocates (10 min, ~115 MB) when Rec is pressed and frees at Stop — no allocation, lock or file I/O in the callback, and no wall-clock alignment needed (the audio thread knows the position). Playback copies the block of each track's voice into a preallocated per-track buffer (handles the loop wrap too). Nothing is recorded during the count-in. A track being voice-recorded doesn't play its old voice.
- **Latency**: at Stop the take is moved earlier by `2 × buffer + input latency + output latency` as reported by the driver (what you heard was output that late, and the mic arrives that late). **Not measured** — needs a loopback check (play a click, record it back, compare); WASAPI shared may misreport.
- **Files**: `Save song` writes each voice as `<song name>-trackN.wav` (mono 24-bit) next to the `.lmsong`, which refers to it by file name only; loading validates the name (a plain `.wav` file name, no path — a song can't point elsewhere) and converts other sample rates (Lagrange). Every voice take is also written to `Songs\voice-YYYYMMDD-HHMMSS.wav` before it replaces anything, so a replaced voice is never lost (voice takes aren't in Undo). Autosave after a voice take writes `autosave-trackN.wav` too.
- **Settings**: the audio device selector now offers 0–1 input channel and the app asks for 1 input; with the saved settings of this PC the input is still none until the user picks a microphone in *Audio settings*.
- **Tests** (`tests/RenderTest.cpp`): voice silent before its tone (0.00000), tone at 0.212 RMS, Volume 0.3 → ratio **0.500**, mute → 0, song length = voice length; mic capture after a 12 000-sample count-in → **36 128** samples written, every one at its exact song sample, old voice silent while recording; mixdown includes the voice (peak 0.300); song file round trip names `my song-track1.wav`; `..\x.wav`, `C:/x.wav`, `sub/x.wav`, `x.exe` refused (4/4). **Negative controls** (same build with the take written one sample late, the old voice playing during a take, the name check off): aligned 0 and 0.212 → fail; 0/4 refused → fail. ALL TESTS PASSED.
- **Not verified**: a real voice take (choosing the input, singing, hearing it back) — needs the user's microphone, not recorded without asking; latency compensation (see above); input and output on two different devices under WASAPI shared (JUCE supports it; not tried here). No input monitoring (you don't hear yourself through the app; use the headset's or Windows' monitoring).
- **Limits**: one voice per track, mono, from song 0 s; no editing, trimming or moving of voices; voices aren't part of Undo (but each take is kept as a file); a voice is assumed to be at the current device rate during playback (converted on load only); Open MIDI keeps existing voices; voice takes refuse Loop.
- **Next**: single-WAV sample player, violin/guitar for the Library (needs the user's OK for downloads), packaging, latency.

**C28 follow-up (UI):** the *Voice* tick box was hard to find, so it is now a red **Rec voice** button in the SONG header. One click records the mic, the second click stops. With no microphone it opens Audio settings. No engine change; tests still pass (ALL TESTS PASSED). The click itself is not checked: posted clicks do not reach JUCE buttons.

## C29 — Audio files on the timeline (drop, move, trim)

**Asked for:** drag a downloaded audio file onto the song at a chosen position, then edit it.

**What changed:**
- `AudioClip` (Recording.h) replaces the mono voice buffer on each track. It holds:
  - the buffer, mono or stereo, at the song rate;
  - `start` (song sample) and the part heard, `in`/`out`. These are atomics, so dragging works while playing;
  - a fixed 5 ms fade at both cuts;
  - a peak overview for drawing.
- `addTo` is the one reader, shared by the live render (including the loop wrap), the offline mixdown and the export.
- `SynthEngine::render`'s `in` is now an AudioBuffer: stereo audio stays stereo through effects, Volume and Pan.
- `readAudioFile`: WAV/MP3/FLAC/Ogg/AIFF, first 2 channels, Lagrange resampling to the song rate, 10 min cap. MP3 needs `JUCE_USE_MP3AUDIOFORMAT=1`.
- SongView:
  - a file can be dropped on a track, or below the last track for a new one;
  - drag the body to move it (Shift snaps to 1/16);
  - drag an edge to trim it non-destructively (a left trim keeps the rest in place);
  - right-click for *Delete audio* or *Untrim*;
  - the waveform is drawn.
- Song file: `audioStart`/`audioIn`/`audioOut` in seconds, validated (finite, ≥ 0, ≤ 24 h). The whole file is saved in `Audio\`, so a trim can still be undone after reopening.
- Open MIDI now also clears the tracks' audio. Before, an old voice survived.

**Measured:**
- AudioClip placement is sample-exact; silent outside the region; stereo kept; `end()` is right.
- Worst step at the two cuts (both near a sine peak) is 1.00× a clean sine. Without the fade it is 17.36×.
- The loop wrap reads from the start again.
- Cost: 0.004 ms per 256-sample block for 8 stereo tracks (budget 5.33 ms).
- A 44.1 kHz stereo WAV is read at 48 kHz: 48000 samples, 220/220 cycles, stereo kept.
- A real MP3 made with ffmpeg decoded: 2 ch, 3.03 s. This is an optional check with `LMS_MP3=<file>`.
- A region moved by 0.25 s plays 0.25 s later; song length follows.
- Song file round trip is exact; 3/3 bad positions refused.
- Stress (SynthEngine) ran 3 min: worst block 1.0 of 10 ms, 0 over budget.

**Negative controls, each failed and was then restored:**
- no fade (17.36×);
- `in` ignored when placing (placed 0);
- loop wrap ignored;
- position validation removed (0/3);
- no resampling (44100 samples, 240 cycles).

**Not verified:**
- drag & drop from Explorer and the mouse edits: posted input doesn't reach JUCE, so the user tries it;
- waveform drawing with a real file;
- a 10-minute MP3 (loading happens on the GUI thread and may freeze the window for a moment).

**Limits:**
- one audio region per track, so for more audio use more tracks (8 max);
- dropping on a track replaces its audio;
- audio edits aren't in Undo, but the original file stays on disk;
- no time-stretch or pitch-shift, no per-clip gain or fades beyond the 5 ms; the track's Volume, Pan and effects apply;
- the song file assumes the audio is at the current rate (resampled on load).

**Next:** the remaining list (sample player, Library violin/guitar with the user's OK for downloads, packaging, latency).

**C29 follow-up (UI):** Space = Play / Stop (also stops a recording), handled by `MainComponent::keyPressed`; the piano passes on the keys it does not use. The piano now takes the keyboard focus back whenever nothing has it (after a dialog), instead of only once at start. Not checked: a posted WM_KEYDOWN does not reach JUCE, so the user tries it.

## C30 — Audio regions (split, duplicate, delete), zoom, delete track

**Asked for:**
- Ctrl+wheel to zoom in on a track and edit where audio starts and ends;
- cut and duplicate audio, plus other useful edits;
- delete tracks.

**What changed:**
- `AudioClip` is now a pool plus regions:
  - every file dropped on (or recorded into) a track is appended to one buffer;
  - up to 32 regions (start/in/out atomics, lo/hi = their file's extent) play parts of that buffer;
  - split and duplicate share samples, so nothing is copied;
  - dropping a second file on a track adds it instead of replacing, and a voice take is added too.
- Fades are 5 ms only at real cuts. `refreshFades` turns them off where another region continues seamlessly, so a split sounds identical to the unsplit region.
- Structural edits (add/split/duplicate/delete) run in `whileStopped`, which resumes from the same place.
- SongView:
  - a bar ruler (beats when there is room);
  - Ctrl+wheel zooms around the mouse; the wheel scrolls in time; double-click on empty space fits the song;
  - the playhead stays in view;
  - the selected region gets a white outline;
  - right-click on a region offers Split here, Duplicate (Ctrl+D), Delete (Del), Untrim;
  - right-click on a track name offers Delete track (tracks 3+) and Clear track (undoable notes, audio gone).
- `Song::removeTrack` rotates the tracks up and resets the freed one. Main holds `getAudioCallbackLock()` around it, because the live keys render even while stopped. Undo history is cleared, since it names tracks by number.
- Song file: `"regions":[[start,in,out,lo,hi],...]` in seconds. The old audioStart/In/Out still loads as one region. Validated: 5 finite numbers, ≥ 0 (out/hi may be −1 = the end), ≤ 24 h, ≤ 32 regions.

**Measured:**
- A split plays bit-identical to the unsplit region.
- Split at or outside an edge does nothing.
- A duplicate placed right after plays the same samples.
- Delete silences the region; the audio is kept once (48000 samples).
- A second mono file on a stereo track goes into both channels at its place.
- Song file: 2 regions round trip; old format gives 1 region; 6/6 bad positions refused.
- Delete track: the later tracks move up and the freed one is reset.
- Cost unchanged: 0.003 ms per block for 8 stereo tracks.

**Negative controls, each failed and was then restored:**
- fades also at the split joint (split ≠ unsplit);
- freed track not cleared (clean 0).

**Not verified:**
- every mouse/wheel/keyboard gesture: posted input doesn't reach JUCE, so the user tries it;
- holding the audio callback lock during Delete track: briefly blocks the audio thread, not measured.

**Limits:**
- audio edits aren't in Undo;
- a track's files together are capped at 30 min in memory;
- deleting regions doesn't free their samples until the song is reopened (the saved WAV holds the whole pool);
- tracks 1 and 2 can't be deleted (keys and the drums the pads play), only cleared.
