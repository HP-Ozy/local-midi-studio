#include <juce_audio_utils/juce_audio_utils.h>
#include "Synth.h"
#include "MidiQueue.h"
#include "Recording.h"
#include "Presets.h"
#include "MidiMap.h"
#include "Sfz.h"
#include "Library.h"
#include "Song.h"
#include "SongFile.h"

class MainComponent : public juce::AudioAppComponent,
                      private juce::MidiInputCallback,
                      private juce::MidiKeyboardState::Listener,
                      private juce::Timer
{
public:
    MainComponent()
        : keyboard (keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard),
          selector (deviceManager, 0, 0, 2, 2, true, false, true, false)
    {
        auto saved = juce::XmlDocument::parse (settingsFile());
        setAudioChannels (0, 2, saved.get());
        enableAllMidiInputs();
        deviceManager.addMidiInputDeviceCallback ({}, this);
        keyboardState.addListener (this);

        song.count = 2; // track 1 = keys, track 2 = drums (the pads always play it)
        addCombo (wave, "Waveform", { "Sine", "Saw", "Square", "Triangle", "Noise", "Pulse" }, &SynthParams::wave); // index = SynthParams::wave
        addCombo (filterType, "Filter", { "Low-pass", "High-pass", "Band-pass" }, &SynthParams::filterType);
        addCombo (lfoTarget, "LFO to", { "Pitch", "Filter", "Volume", "Width" }, &SynthParams::lfoTarget); // mod wheel adds to LFO depth
        addKnob ("Width",     "pulseWidth", 0.05,  0.95,  &SynthParams::pulseWidth); // pulse wave only
        addKnob ("Attack",    "attack",    0.001, 5.0,   &SynthParams::attack,    0.3);
        addKnob ("Decay",     "decay",     0.001, 5.0,   &SynthParams::decay,     0.5);
        addKnob ("Sustain",   "sustain",   0.0,   1.0,   &SynthParams::sustain);
        addKnob ("Release",   "release",   0.001, 8.0,   &SynthParams::release,   0.5);
        addKnob ("Cutoff",    "cutoff",    20.0,  20000, &SynthParams::cutoff,    1000);
        addKnob ("Resonance", "resonance", 0.5,   10.0,  &SynthParams::resonance, 2.0);
        addKnob ("LFO rate",  "lfoRate",   0.1,   20.0,  &SynthParams::lfoRate,   3.0);
        addKnob ("LFO depth", "lfoDepth",  0.0,   1.0,   &SynthParams::lfoDepth);
        addKnob ("Volume",    "gain",      0.0,   1.0,   &SynthParams::gain);
        addKnob ("Chorus",     "chorusMix",     0.0,  1.0, &SynthParams::chorusMix);
        addKnob ("Delay time", "delayTime",     0.02, 1.5, &SynthParams::delayTime, 0.3);
        addKnob ("Feedback",   "delayFeedback", 0.0,  0.9, &SynthParams::delayFeedback);
        addKnob ("Delay mix",  "delayMix",      0.0,  1.0, &SynthParams::delayMix);
        addKnob ("Room size",  "reverbSize",    0.0,  1.0, &SynthParams::reverbSize);
        addKnob ("Reverb mix", "reverbMix",     0.0,  1.0, &SynthParams::reverbMix);
        for (auto& gr : soundGroups())
            for (auto& key : gr.keys)
                for (auto id : { juce::Slider::rotarySliderFillColourId, juce::Slider::thumbColourId }) knobs_[key].slider->setColour (id, gr.colour);
        midiMap = MidiMap::fromJson (midiMapFile().loadFileAsString());
        updateKnobLabels();

        presetBox.setTextWhenNothingSelected ("Presets");
        presetBox.onChange = [this] { loadSelectedPreset(); };
        savePresetButton.onClick = [this] { savePreset(); };
        loadInstrumentButton.onClick = [this] { chooseInstrument(); };
        synthButton.onClick = [this] { sel().setInstrument (nullptr); showInstrumentName(); log ("Track " + juce::String (song.selected + 1) + ": Synth"); };
        libraryButton.onClick = [this] { showLibraryMenu(); };
        pads.nameOf = [this] (int n) { const auto i = drums().instrument(); return Sfz::drumName (n, static_cast<InstrumentSound*> (i.get())); };
        pads.play = [this] (int ch, int note, bool down) { if (down) keyboardState.noteOn (ch, note, 0.8f); else keyboardState.noteOff (ch, note, 0.0f); }; // routed like the real pad
        pads.onAssign = [this] (int pad) { choosePadSound (pad); };
        pads.onLearnt = [this] { learnPadsButton.setButtonText ("Learn pads"); savePads(); log ("Pads learnt and saved: they now play the drum track."); };
        padsLabel.setText ("Drums", juce::dontSendNotification);
        padLayout.addItemList ({ "4 x 4", "2 x 8" }, 1);
        padLayout.onChange = [this] { pads.wide = padLayout.getSelectedItemIndex() == 1; pads.repaint(); savePads(); };
        learnPadsButton.onClick = [this]
        {
            if (pads.learning >= 0) { pads.learning = -1; loadPads(); learnPadsButton.setButtonText ("Learn pads"); log ("Pad learning cancelled."); }
            else { pads.learning = 0; learnPadsButton.setButtonText ("Cancel learning"); log ("Learn pads: hit, on the keyboard, the pad lit in blue on screen - 16 in order (pad 1 = bottom-left)."); }
            pads.repaint();
        };
        loadPads();
        for (auto* c : std::initializer_list<juce::Component*> { &pads, &padsLabel, &padLayout, &learnPadsButton })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        showInstrumentName();
        for (auto* c : std::initializer_list<juce::Component*> { &presetBox, &savePresetButton, &libraryButton, &loadInstrumentButton, &synthButton, &instrumentLabel })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        refreshPresetList();

        monitor.setMultiLine (true);
        monitor.setReadOnly (true);
        monitor.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
        keyboard.setKeyPressBaseOctave (5); // computer keys A..K play from middle C (MIDI 60)
        for (auto* b : { &recButton, &playButton, &stopButton, &wavButton, &openMidiButton, &folderButton })
        {
            b->setWantsKeyboardFocus (false); // keep computer keys playing the on-screen piano
            addAndMakeVisible (b);
        }
        recButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8b1e1e));
        playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
        recButton.onClick = [this] { startRecording(); };
        playButton.onClick = [this] { startPlayback(); };
        stopButton.onClick = [this] { stopAll(); };
        wavButton.onClick = [this] { exportWav(); };
        openMidiButton.onClick = [this] { chooseMidiFile(); };
        folderButton.onClick = [] { takesFolder().createDirectory(); takesFolder().startAsProcess(); };
        wavButton.setButtonText ("Export song");

        // Song: tracks view, tempo, metronome, more tracks
        songView.trackName = [this] (int i) { return trackName (i); };
        songView.onSelect = [this] (int i) { selectTrack (i); };
        bpmSlider.setRange (40.0, 240.0, 1.0);
        bpmSlider.setValue (song.bpm.load(), juce::dontSendNotification);
        bpmSlider.setTextValueSuffix (" BPM");
        bpmSlider.onValueChange = [this] { song.bpm = bpmSlider.getValue(); };
        clickToggle.setToggleState (song.metronome.load(), juce::dontSendNotification);
        clickToggle.onClick = [this] { song.metronome = clickToggle.getToggleState(); };
        addTrackButton.onClick = [this]
        {
            if (song.count >= Song::maxTracks) { log ("All " + juce::String (Song::maxTracks) + " tracks are in use."); return; }
            ++song.count; // engines exist already: nothing is allocated for the audio thread
            selectTrack (song.count - 1);
            songView.repaint();
        };
        songHint.setText ("Click a track to select it", juce::dontSendNotification);
        saveSongButton.onClick = [this] { chooseSaveSong(); };
        openSongButton.onClick = [this] { chooseOpenSong(); };
        songHint.setColour (juce::Label::textColourId, juce::Colours::grey);

        // Drums section (by the pads) and what the keys play (by the keyboard)
        kitBox.onChange = [this] { if (const int k = kitBox.getSelectedId() - 100; k >= 0 && k < installedFiles.size()) loadInstrumentFile (installedFiles[k], drumTrack); };
        keysBox.onChange = [this]
        {
            const int id = keysBox.getSelectedId();
            if (id == 1) { sel().setInstrument (nullptr); instrumentFiles[(size_t) song.selected.load()] = juce::File(); showInstrumentName(); }
            else if (id >= 100 && id - 100 < installedFiles.size()) loadInstrumentFile (installedFiles[id - 100]);
        };
        keysLabel.setText ("Keys play:", juce::dontSendNotification);
        drumVolume.setRange (0.0, 1.0);
        drumVolume.setValue (drums().params.gain.load(), juce::dontSendNotification);
        drumVolume.onValueChange = [this] { drums().params.gain = (float) drumVolume.getValue(); if (song.selected == drumTrack) for (auto& r : refreshers) r(); };
        drumVolume.setTooltip ("Drum volume");
        padsLabel.setText ("Kit", juce::dontSendNotification);
        settingsButton.onClick = [this] { openSettings(); };
        logButton.setClickingTogglesState (true);
        logButton.onClick = [this] { monitor.setVisible (logButton.getToggleState()); }; // the full log covers the tracks
        lastLog.setColour (juce::Label::textColourId, juce::Colour (0xffb8c0c8));
        status.setFont (juce::Font (12.0f));
        for (auto* c : std::initializer_list<juce::Component*> { &monitor, &keyboard, &status, &songView, &bpmSlider, &clickToggle,
                                                                 &addTrackButton, &songHint, &kitBox, &keysBox, &keysLabel, &drumVolume,
                                                                 &saveSongButton, &openSongButton, &settingsButton, &logButton, &lastLog })
        {
            if (c != &keyboard) c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        monitor.setVisible (false);
        if (deviceManager.getCurrentAudioDevice() == nullptr) juce::MessageManager::callAsync ([safe = SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->openSettings(); });
        refreshInstrumentBoxes();
        loadDefaultKit();

        log ("MIDI inputs: " + juce::String (juce::MidiInput::getAvailableDevices().size()) + " found, all enabled.");
        setSize (1280, 800);
        startTimerHz (30); // smooth playhead
    }

    ~MainComponent() override
    {
        deviceManager.removeMidiInputDeviceCallback ({}, this);
        keyboardState.removeListener (this);
        *downloadCancel = true; // a running download stops and cleans up its partial file
        stopAll();
        if (auto xml = deviceManager.createStateXml())
        {
            settingsFile().getParentDirectory().createDirectory();
            xml->writeTo (settingsFile());
        }
        shutdownAudio();
    }

    void prepareToPlay (int blockSize, double sampleRate) override
    {
        song.prepare (sampleRate, blockSize);
        currentRate = sampleRate;
        for (auto* b : { &midi, &keysMidi, &padMidi }) b->ensureSize (MidiQueue::capacity * 16); // a full queue fits: no allocation in the callback
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();
        const double now = MidiQueue::now();
        midi.clear(); keysMidi.clear(); padMidi.clear();
        queue.popInto (midi, info.numSamples, currentRate, now);
        router.split (midi, keysMidi, padMidi); // pads -> drum track, the rest -> the selected track
        song.render (*info.buffer, info.startSample, info.numSamples, keysMidi, now, drumTrack, &padMidi);

        // Callback time vs the time this buffer lasts: over 100 % is a guaranteed dropout (WASAPI shared reports none).
        const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
        if (ms > worstBlockMs.load()) worstBlockMs = ms;
        if (ms > info.numSamples * 1000.0 / currentRate) ++overBudget;
    }

    void releaseResources() override {}

    // Knob groups of the sound panels: title, accent colour, optional selector, knobs by parameter key.
    struct SoundGroup { juce::String title; juce::Colour colour; juce::ComboBox* combo; juce::StringArray keys; };
    std::vector<SoundGroup> soundGroups()
    {
        return { { "OSCILLATOR", juce::Colour (0xfff0a030), &wave,       { "pulseWidth" } },
                 { "ENVELOPE",   juce::Colour (0xff5ec27a), nullptr,     { "attack", "decay", "sustain", "release" } },
                 { "FILTER",     juce::Colour (0xff3fa7d6), &filterType, { "cutoff", "resonance" } },
                 { "LFO",        juce::Colour (0xffb07ee8), &lfoTarget,  { "lfoRate", "lfoDepth" } },
                 { "EFFECTS",    juce::Colour (0xffe8687a), nullptr,     { "chorusMix", "delayTime", "delayFeedback", "delayMix", "reverbSize", "reverbMix" } },
                 { "OUTPUT",     juce::Colour (0xffdde2e7), nullptr,     { "gain" } } };
    }

    // Registers a titled panel (drawn by paint) and returns the room inside it.
    juce::Rectangle<int> panel (juce::Rectangle<int> area, const juce::String& title, juce::Colour colour)
    {
        panels.push_back ({ area, title, colour });
        return area.reduced (6).withTrimmedTop (16);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
        g.setColour (juce::Colour (0xff15181c)); // top and bottom bars
        g.fillRect (getLocalBounds().removeFromTop (barH));
        g.fillRect (getLocalBounds().removeFromBottom (footH));
        for (auto& p : panels)
        {
            g.setColour (juce::Colour (0xff252a30));
            g.fillRoundedRectangle (p.area.toFloat(), 6.0f);
            g.setColour (p.colour);
            g.fillRect (p.area.getX() + 6, p.area.getY(), p.area.getWidth() - 12, 2);
            g.setFont (juce::Font (11.0f, juce::Font::bold));
            g.drawText (p.title, p.area.reduced (8, 4).removeFromTop (14), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        panels.clear();
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (barH).reduced (8, 7); // transport, tempo, files, settings
        for (auto* b : { &recButton, &playButton, &stopButton }) { b->setBounds (bar.removeFromLeft (64)); bar.removeFromLeft (4); }
        bar.removeFromLeft (12);
        bpmSlider.setBounds (bar.removeFromLeft (140));
        bar.removeFromLeft (6);
        clickToggle.setBounds (bar.removeFromLeft (64));
        logButton.setBounds (bar.removeFromRight (56));
        bar.removeFromRight (4);
        settingsButton.setBounds (bar.removeFromRight (120));
        bar.removeFromRight (16);
        for (auto* b : { &folderButton, &wavButton, &openMidiButton, &saveSongButton, &openSongButton }) { b->setBounds (bar.removeFromRight (100)); bar.removeFromRight (4); }

        auto foot = r.removeFromBottom (footH).reduced (10, 1); // last message | diagnostics
        status.setBounds (foot.removeFromRight (foot.getWidth() / 2));
        lastLog.setBounds (foot);

        r.reduce (8, 6);
        keyboard.setBounds (r.removeFromBottom (110));
        auto keysRow = r.removeFromBottom (32).withTrimmedBottom (6); // what the keys play (the selected track's instrument)
        keysLabel.setBounds (keysRow.removeFromLeft (76));
        keysBox.setBounds (keysRow.removeFromLeft (340));

        auto left = panel (r.removeFromLeft (360), "DRUMS", juce::Colour (0xfff0a030));
        auto drumRow = left.removeFromTop (26); // kit + volume, for the pads
        padsLabel.setBounds (drumRow.removeFromLeft (34));
        drumVolume.setBounds (drumRow.removeFromRight (80));
        drumRow.removeFromRight (6);
        kitBox.setBounds (drumRow);
        left.removeFromTop (4);
        auto padHeader = left.removeFromTop (26);
        learnPadsButton.setBounds (padHeader.removeFromRight (130));
        padHeader.removeFromRight (6);
        padLayout.setBounds (padHeader.removeFromRight (80));
        left.removeFromTop (6);
        pads.setBounds (left);
        r.removeFromLeft (8);

        auto presetRow = r.removeFromTop (28);
        presetBox.setBounds (presetRow.removeFromLeft (230));
        presetRow.removeFromLeft (6);
        savePresetButton.setBounds (presetRow.removeFromLeft (120));
        presetRow.removeFromLeft (14);
        libraryButton.setBounds (presetRow.removeFromLeft (90));
        presetRow.removeFromLeft (6);
        loadInstrumentButton.setBounds (presetRow.removeFromLeft (140));
        presetRow.removeFromLeft (6);
        synthButton.setBounds (presetRow.removeFromLeft (70));
        presetRow.removeFromLeft (10);
        instrumentLabel.setBounds (presetRow);
        r.removeFromTop (6);
        const auto groups = soundGroups();
        for (auto [from, to] : { std::pair { 0, 4 }, std::pair { 4, 6 } }) // two rows of sound panels
        {
            auto line = r.removeFromTop (128);
            int slots = 0;
            for (int g = from; g < to; ++g) slots += groups[(size_t) g].keys.size() + (groups[(size_t) g].combo != nullptr ? 2 : 0); // a selector takes two slots
            const int w = (line.getWidth() - 6 * (to - from - 1)) / juce::jmax (1, slots);
            for (int g = from; g < to; ++g)
            {
                const auto& gr = groups[(size_t) g];
                const int n = gr.keys.size() + (gr.combo != nullptr ? 2 : 0);
                auto area = panel (g == to - 1 ? line : line.removeFromLeft (n * w), gr.title, gr.colour).withTrimmedTop (18); // 18: knob labels
                line.removeFromLeft (6);
                const int sw = area.getWidth() / n;
                if (gr.combo != nullptr) gr.combo->setBounds (area.removeFromLeft (2 * sw).withSizeKeepingCentre (2 * sw - 12, 26));
                for (auto& key : gr.keys) knobs_.at (key).slider->setBounds (area.removeFromLeft (sw));
            }
            r.removeFromTop (6);
        }

        auto songArea = panel (r, "SONG", juce::Colour (0xff3fa7d6));
        auto songHeader = songArea.removeFromTop (26);
        addTrackButton.setBounds (songHeader.removeFromLeft (80));
        songHeader.removeFromLeft (10);
        songHint.setBounds (songHeader);
        songArea.removeFromTop (4);
        songView.setBounds (songArea);
        monitor.setBounds (songArea);
    }

    void openSettings() // audio device, sample rate, buffer, MIDI inputs, test tone
    {
        selector.setSize (520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setNonOwned (&selector);
        o.dialogTitle = "Audio & MIDI settings";
        o.dialogBackgroundColour = findColour (juce::ResizableWindow::backgroundColourId);
        o.componentToCentreAround = this;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        const double t = m.getTimeStamp() > 0 ? m.getTimeStamp() : MidiQueue::now();
        queue.push (m, t);
        record (m, t);
        {
            const juce::ScopedValueSetter<bool> hw (fromHardware, true);
            keyboardState.processNextMidiEvent (m); // only lights up the on-screen keys
        }
        if (m.isController())
            juce::MessageManager::callAsync ([safe = SafePointer<MainComponent> (this), ch = m.getChannel(), cc = m.getControllerNumber(), v = m.getControllerValue()]
                                             { if (safe) safe->handleController (ch, cc, v); });
        if (m.isNoteOn())
            juce::MessageManager::callAsync ([safe = SafePointer<MainComponent> (this), ch = m.getChannel(), note = m.getNoteNumber(), vel = m.getFloatVelocity()]
                                             { if (safe) safe->pads.noteIn (ch, note, vel); });
        if (m.isActiveSense() || m.isMidiClock()) return;
        juce::MessageManager::callAsync ([safe = SafePointer<MainComponent> (this),
                                          text = source->getName() + "  " + m.getDescription()]
                                         { if (safe) safe->log (text); });
    }

    // On-screen piano (message thread). Hardware notes also pass through here to light the keys: skip them, they're already queued.
    void handleNoteOn (juce::MidiKeyboardState*, int ch, int note, float vel) override
    {
        if (fromHardware) return;
        const auto m = juce::MidiMessage::noteOn (ch, note, vel);
        queue.push (m, MidiQueue::now());
        record (m, MidiQueue::now());
    }

    void handleNoteOff (juce::MidiKeyboardState*, int ch, int note, float vel) override
    {
        if (fromHardware) return;
        const auto m = juce::MidiMessage::noteOff (ch, note, vel);
        queue.push (m, MidiQueue::now());
        record (m, MidiQueue::now());
    }

    void timerCallback() override
    {
        if (! focused && keyboard.isShowing()) focused = (keyboard.grabKeyboardFocus(), true);
        const double pos = song.position.load();
        if (playing && ! recording && pos > song.length() + 2.0) stopTransport(); // played to the end (+ release tails)
        songView.playhead = (playing || recording) ? pos : -1.0;
        recButton.setButtonText (! recording ? "Rec" : pos < 0.0 ? "count-in " + juce::String ((int) std::ceil (-pos / song.beatSeconds()))
                                                                 : "REC  " + juce::String (pos, 1) + " s");
        songView.repaint();
        pads.tick();
        // Diagnostics, held so they're readable: worst callback for 2 s, limiter flag for 1 s, level with a falling peak.
        const double w = worstBlockMs.exchange (0.0);
        if (w > shownWorstMs || ++worstHold > 60) { shownWorstMs = w; worstHold = 0; }
        const auto hits = song.limiterHits.load();
        limitHold = hits != lastHits ? 30 : juce::jmax (0, limitHold - 1);
        lastHits = hits;
        shownPeak = juce::jmax (song.peakLevel.exchange (0.0f), shownPeak * 0.9f);

        juce::String s;
        double budgetMs = 0;
        if (auto* d = deviceManager.getCurrentAudioDevice())
        {
            const double sr = d->getCurrentSampleRate();
            budgetMs = d->getCurrentBufferSizeSamples() * 1000.0 / sr;
            const double ms = (d->getCurrentBufferSizeSamples() + d->getOutputLatencyInSamples()) * 1000.0 / sr;
            s << d->getName() << "  |  " << sr << " Hz  |  ~" << juce::String (ms, 1) << " ms output latency";
            if (d->getXRunCount() >= 0) s << "  |  driver underruns " << d->getXRunCount();
        }
        else s << "NO AUDIO DEVICE - open Audio settings";
        s << "\nCPU " << juce::String (deviceManager.getCpuUsage() * 100.0, 1) << "%"
          << "  |  worst block " << juce::String (shownWorstMs, 2) << " of " << juce::String (budgetMs, 1) << " ms"
          << "  |  over budget " << overBudget.load()
          << "  |  level " << (shownPeak > 1e-4f ? juce::String (juce::Decibels::gainToDecibels (shownPeak), 1) + " dBFS" : juce::String ("-inf"))
          << "  |  voices " << song.activeVoices();
        if (limitHold > 0) s << "  |  LIMITING";
        if (queue.dropped > 0) s << "  |  MIDI dropped " << queue.dropped.load();
        const bool problem = limitHold > 0 || overBudget > 0 || queue.dropped > 0;
        status.setColour (juce::Label::textColourId, problem ? juce::Colours::orange : juce::Colours::white);
        status.setText (s, juce::dontSendNotification);
    }

    // ---- Recording into the selected track (MIDI and GUI threads; never the audio thread) ----
    void record (const juce::MidiMessage& m, double timeSec)
    {
        if (! recording || ! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isChannelPressure())) return;
        // Only what the recorded track hears: pads belong to the drum track (stored as their kit sound), keys to the others.
        const int sound = m.isNoteOnOrOff() ? router.soundFor (m.getChannel(), m.getNoteNumber()) : -1;
        const bool drumsRec = song.recordTrack == drumTrack;
        if (sound >= 0 && ! drumsRec) return;
        auto copy = sound < 0 ? m : m.isNoteOn() ? juce::MidiMessage::noteOn (m.getChannel(), sound, m.getVelocity())
                                                 : juce::MidiMessage::noteOff (m.getChannel(), sound);
        const double t = timeSec - song.songZeroWall; // where it was heard in the song (see Song::songZeroWall)
        if (t < 0.0) return;                           // played during the count-in
        copy.setTimeStamp (t);
        const juce::ScopedLock sl (takeLock);
        take.addEvent (copy);
    }

    void startRecording()
    {
        stopAll();
        {
            const juce::ScopedLock sl (takeLock);
            take.clear();
        }
        song.songZeroWall = 1e18; // nothing counts until the audio thread starts the transport
        song.recordTrack = song.selected.load();
        song.startPos = -(juce::int64) std::llround (song.barSeconds() * currentRate); // one bar of count-in
        song.run = true;
        recording = true;
        log ("Recording track " + juce::String (song.selected + 1) + " after a one-bar count-in... press Stop when done.");
    }

    void stopRecording()
    {
        if (! recording) return;
        recording = false;
        const double end = juce::jmax (0.0, song.position.load());
        stopTransport();
        const int rec = song.recordTrack.exchange (-1);
        const juce::ScopedLock sl (takeLock);
        take.updateMatchedPairs();
        for (int i = take.getNumEvents(); --i >= 0;) // close notes still held when Stop was pressed
            if (take.getEventPointer (i)->message.isNoteOn() && take.getIndexOfMatchingKeyUp (i) < 0)
            {
                const auto& on = take.getEventPointer (i)->message;
                take.addEvent (juce::MidiMessage::noteOff (on.getChannel(), on.getNoteNumber()).withTimeStamp (end));
            }
        bool anyNote = false;
        for (auto* e : take) anyNote |= e->message.isNoteOn();
        if (! anyNote || rec < 0) { take.clear(); log ("Nothing recorded: the track keeps what it had."); songView.repaint(); return; }
        for (int ch = 1; ch <= 16; ++ch) // release a held sustain pedal too
            take.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0).withTimeStamp (end));
        take.updateMatchedPairs();
        song.tracks[(size_t) rec]->clip = take; // transport stopped: the audio thread doesn't read clips now
        writeSong (songsFolder().getChildFile ("autosave.lmsong")); // a take is never lost, even if the app is closed unsaved
        take.clear();
        log ("Track " + juce::String (rec + 1) + " recorded (" + juce::String (end, 1) + " s). Play to hear the song.");
        songView.repaint();
    }

    void startPlayback()
    {
        stopAll();
        if (song.length() <= 0.0) { log ("Nothing to play yet: select a track and press Rec."); return; }
        song.recordTrack = -1;
        song.startPos = 0;
        song.run = true;
        playing = true;
    }

    // Stop the transport and wait until the audio thread has seen it, so clips can be changed safely.
    // If no audio device is running it never answers: the wait is bounded and nothing reads the clips then anyway.
    void stopTransport()
    {
        song.run = false;
        for (int i = 0; i < 150 && song.audioRunning.load(); ++i) juce::Thread::sleep (2);
        playing = false;
    }

    void stopAll() { stopRecording(); stopTransport(); }

    // ---- Song files (.lmsong in Documents\LocalMIDIStudio\Songs) ----
    static juce::File songsFolder() { return takesFolder().getChildFile ("Songs"); }

    bool writeSong (const juce::File& f)
    {
        f.getParentDirectory().createDirectory();
        return f.replaceWithText (SongFile::toJson (song, instrumentFiles));
    }

    void chooseSaveSong()
    {
        stopAll();
        songsFolder().createDirectory();
        chooser = std::make_unique<juce::FileChooser> ("Save the song", songFile != juce::File() ? songFile : songsFolder().getChildFile ("song.lmsong"), "*.lmsong");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() == juce::File()) return;
            const auto f = fc.getResult().withFileExtension ("lmsong");
            if (writeSong (f)) { songFile = f; log ("Song saved: " + f.getFullPathName()); }
            else log ("ERROR: could not save " + f.getFullPathName());
        });
    }

    void chooseOpenSong()
    {
        songsFolder().createDirectory();
        chooser = std::make_unique<juce::FileChooser> ("Open a song", songsFolder(), "*.lmsong");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File()) openSong (fc.getResult());
        });
    }

    void openSong (const juce::File& f)
    {
        SongFile::Loaded l;
        if (const auto err = SongFile::parse (f.loadFileAsString(), l); err.isNotEmpty()) { log ("Song not opened (" + err + "): the current song is unchanged."); return; }
        stopAll(); // the audio thread doesn't read clips once stopped
        song.count = juce::jmax (2, (int) l.tracks.size()); // track 2 stays the drums the pads play
        song.bpm = l.bpm;
        song.metronome = l.metronome;
        for (int i = 0; i < Song::maxTracks; ++i)
        {
            auto& t = *song.tracks[(size_t) i];
            auto* src = i < (int) l.tracks.size() ? l.tracks[(size_t) i].get() : nullptr;
            if (src != nullptr) t.engine.params.copyFrom (src->params); else t.engine.params.copyFrom (SynthParams());
            t.clip = src != nullptr ? src->clip : juce::MidiMessageSequence();
            t.mute = src != nullptr && src->mute;
            t.solo = src != nullptr && src->solo;
            // the saved path, else the same instrument (folder + file name) wherever the library is now, else the synth
            juce::File inst (src != nullptr && juce::File::isAbsolutePath (src->instrument) ? src->instrument : juce::String());
            if (src != nullptr && src->instrument.isNotEmpty() && ! inst.existsAsFile())
                for (auto& g : installedFiles)
                    if (g.getFileName() == inst.getFileName() && g.getParentDirectory().getFileName() == inst.getParentDirectory().getFileName()) inst = g;
            if (inst.existsAsFile()) loadInstrumentFile (inst, i);
            else
            {
                if (src != nullptr && src->instrument.isNotEmpty()) log ("Track " + juce::String (i + 1) + ": instrument not found (" + src->instrument + "), using the synth.");
                t.engine.setInstrument (nullptr);
                instrumentFiles[(size_t) i] = juce::File();
            }
        }
        bpmSlider.setValue (l.bpm, juce::dontSendNotification);
        clickToggle.setToggleState (l.metronome, juce::dontSendNotification);
        drumVolume.setValue (drums().params.gain.load(), juce::dontSendNotification);
        songFile = f;
        selectTrack (juce::jmin (l.selected, song.count - 1));
        log ("Song opened: " + f.getFullPathName() + " (" + juce::String ((int) l.tracks.size()) + " tracks)");
    }

    void exportWav()
    {
        stopAll();
        if (song.length() <= 0.0) { log ("Nothing to export: record a track first."); return; }
        // shortcut: renders on the GUI thread (a few seconds for long songs with big sample sets); move to a thread if needed
        const auto audio = song.mixdown (48000.0);
        takesFolder().createDirectory();
        const auto wavFile = takesFolder().getChildFile ("song-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav");
        log (writeWav (audio, 48000.0, wavFile) ? "Exported the song to " + wavFile.getFullPathName() + " (48 kHz, 24-bit, metronome not included)"
                                                 : "ERROR: could not write " + wavFile.getFullPathName());
    }

    // ---- Open a .mid made elsewhere (e.g. tools/genres.py): channel 10 -> Drums, each other file track -> its own track ----
    void chooseMidiFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Open a MIDI file", takesFolder(), "*.mid;*.midi");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File()) return;
            if (song.length() <= 0.0) { openMidiFile (file); return; }
            // recorded tracks live only in memory: ask before replacing them
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Open MIDI file",
                                                "This replaces the notes of every track. Continue?", "Open", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([safe = SafePointer<MainComponent> (this), file] (int ok)
                                                { if (ok == 1 && safe != nullptr) safe->openMidiFile (file); }));
        });
    }

    void openMidiFile (const juce::File& file)
    {
        MidiParts mp;
        juce::FileInputStream in (file);
        if (! in.openedOk() || ! readMidiParts (in, mp)) { log ("ERROR: not a MIDI file: " + file.getFullPathName()); return; }
        stopAll();
        for (auto& t : song.tracks) t->clip.clear(); // transport stopped: the audio thread doesn't read clips now
        song.tracks[(size_t) drumTrack]->clip = mp.drums;
        int last = drumTrack, i = 0, dropped = 0;
        for (auto& p : mp.parts)
        {
            if (i == drumTrack) ++i;
            if (i >= Song::maxTracks) { ++dropped; continue; }
            song.tracks[(size_t) i]->clip = p.seq;
            log ("Track " + juce::String (i + 1) + " <- " + (p.name.isEmpty() ? juce::String ("file track") : p.name));
            last = juce::jmax (last, i++);
        }
        song.count = last + 1;
        bpmSlider.setValue (std::round (mp.bpm)); // sends the change to the song (metronome, count-in)
        selectTrack (0);
        songView.repaint();
        log ("Opened " + file.getFileName() + (mp.drums.getNumEvents() > 0 ? " (channel 10 -> Drums)" : "")
             + (dropped > 0 ? ", " + juce::String (dropped) + " file track(s) left out: only " + juce::String (Song::maxTracks) + " tracks" : juce::String())
             + ". Pick a sound for each track, then Play.");
    }

    static juce::File takesFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LocalMIDIStudio");
    }

    // The controller's 16 pads: the kit sound each plays, lit when hit, click to play, right-click to choose the sound.
    // "Learn pads" ties each cell to a real pad (its channel + note: hit them in the order shown; pad 1 is bottom-left).
    // Learnt pads always play the drum track, whatever track is selected. Saved in pads.json.
    struct DrumPads : juce::Component
    {
        DrumPads()
        {
            const int gm[] { 36, 38, 42, 46, 37, 40, 44, 39, 41, 43, 45, 47, 49, 51, 57, 53 }; // kick, snare, hi-hats, toms, cymbals
            for (int i = 0; i < 16; ++i) { channels[(size_t) i] = 10; notes[(size_t) i] = 36 + i; sounds[(size_t) i] = gm[i]; } // until learnt
        }

        int cols() const { return wide ? 8 : 4; }
        juce::Rectangle<float> cell (int i) const
        {
            const int c = cols(), rows = 16 / c;
            const float w = (float) getWidth() / (float) c, h = juce::jmin ((float) getHeight() / (float) rows, w * 1.3f); // a bit taller than wide fills the column
            return { (float) (i % c) * w, (float) (rows - 1 - i / c) * h, w, h };
        }

        void paint (juce::Graphics& g) override
        {
            for (int i = 0; i < 16; ++i)
            {
                const auto r = cell (i).reduced (3.0f);
                const auto base = juce::Colour (0xff2a3138).interpolatedWith (juce::Colour (0xffe8892b), glow[(size_t) i]);
                g.setColour (base);
                g.fillRoundedRectangle (r, 6.0f);
                if (learning == i) { g.setColour (juce::Colour (0xff3fa7d6)); g.drawRoundedRectangle (r, 6.0f, 3.0f); }
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (14.0f, juce::Font::bold));
                g.drawFittedText (nameOf ? nameOf (sounds[(size_t) i]) : juce::String(), r.reduced (4.0f).toNearestInt().withTrimmedBottom (14),
                                  juce::Justification::centred, 2);
                g.setColour (juce::Colours::lightgrey);
                g.setFont (juce::Font (11.0f));
                g.drawText ("pad " + juce::String (i + 1) + "  ch " + juce::String (channels[(size_t) i]) + " n " + juce::String (notes[(size_t) i]),
                            r.withTrimmedTop (r.getHeight() - 16), juce::Justification::centred);
            }
        }

        int cellAt (juce::Point<float> p) const { for (int i = 0; i < 16; ++i) if (cell (i).contains (p)) return i; return -1; }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.position);
            if (i < 0) return;
            if (e.mods.isPopupMenu()) { if (onAssign) onAssign (i); return; }
            held = i;
            if (play) play (channels[(size_t) i], notes[(size_t) i], true);
        }
        void mouseUp (const juce::MouseEvent&) override { if (held >= 0 && play) play (channels[(size_t) held], notes[(size_t) held], false); held = -1; }

        void noteIn (int channel, int note, float velocity) // a pad or key was hit on the controller (message thread)
        {
            if (learning >= 0)
            {
                channels[(size_t) learning] = channel;
                notes[(size_t) learning] = note;
                if (++learning == 16) { learning = -1; if (onLearnt) onLearnt(); }
                repaint();
                return;
            }
            for (int i = 0; i < 16; ++i)
                if (notes[(size_t) i] == note && channels[(size_t) i] == channel) glow[(size_t) i] = 0.4f + 0.6f * velocity;
        }

        void tick() // ~30 Hz: fade the lights
        {
            bool changed = false;
            for (auto& g : glow) if (g > 0.0f) { g = juce::jmax (0.0f, g - 0.08f); changed = true; }
            if (changed) repaint();
        }

        std::array<int, 16> channels {}, notes {}, sounds {}; // trigger (channel, note) -> kit sound
        std::array<float, 16> glow {};
        bool wide = false;
        int learning = -1, held = -1;
        std::function<juce::String (int)> nameOf;
        std::function<void (int, int, bool)> play;
        std::function<void (int)> onAssign;
        std::function<void()> onLearnt;
    };

    static juce::File padsFile() { return settingsFile().getSiblingFile ("pads.json"); }

    void rebuildRouter() // pads' (channel, note) -> their kit sound, read by the audio thread
    {
        router.clear();
        for (int i = 0; i < 16; ++i) router.set (pads.channels[(size_t) i], pads.notes[(size_t) i], pads.sounds[(size_t) i]);
        pads.repaint();
    }

    void savePads()
    {
        rebuildRouter();
        auto list = [] (const std::array<int, 16>& a) { juce::Array<juce::var> l; for (int n : a) l.add (n); return l; };
        auto* o = new juce::DynamicObject();
        o->setProperty ("layout", pads.wide ? "2x8" : "4x4");
        o->setProperty ("channels", list (pads.channels));
        o->setProperty ("notes", list (pads.notes));
        o->setProperty ("sounds", list (pads.sounds));
        padsFile().getParentDirectory().createDirectory();
        padsFile().replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    void loadPads() // hand-editable file: anything missing or out of range keeps the default
    {
        const auto v = juce::JSON::parse (padsFile().loadFileAsString());
        auto read = [&] (const char* key, std::array<int, 16>& a, int lo, int hi)
        {
            if (auto* l = v[key].getArray(); l != nullptr && l->size() == 16)
                for (int i = 0; i < 16; ++i) if (const int n = (*l)[i]; n >= lo && n <= hi) a[(size_t) i] = n;
        };
        read ("channels", pads.channels, 1, 16);
        read ("notes", pads.notes, 0, 127);
        read ("sounds", pads.sounds, 0, 127);
        pads.wide = v["layout"].toString() == "2x8";
        padLayout.setSelectedItemIndex (pads.wide ? 1 : 0, juce::dontSendNotification);
        rebuildRouter();
    }

    void choosePadSound (int pad) // right-click on a pad: every sound of the loaded kit (or the General MIDI list)
    {
        std::map<int, juce::String> sounds;
        const auto kit = drums().instrument();
        if (const auto* inst = static_cast<InstrumentSound*> (kit.get()))
            for (auto& r : inst->regions) if (r.lokey == r.hikey) sounds[r.lokey] = Sfz::drumName (r.lokey, inst);
        if (sounds.empty()) for (int n = 35; n <= 81; ++n) sounds[n] = Sfz::drumName (n, nullptr);
        juce::PopupMenu menu;
        menu.addSectionHeader ("Pad " + juce::String (pad + 1) + " plays:");
        for (auto& [n, name] : sounds) menu.addItem (n + 1, name + "  (" + juce::String (n) + ")", true, n == pads.sounds[(size_t) pad]);
        menu.showMenuAsync (juce::PopupMenu::Options(), [this, pad] (int r)
        {
            if (r <= 0) return;
            pads.sounds[(size_t) pad] = r - 1;
            savePads();
        });
    }

    // ---- Instrument pickers: what the drum track and the selected track play (installed library instruments) ----
    static juce::File kitFile() { return settingsFile().getSiblingFile ("drum-kit.txt"); }

    void refreshInstrumentBoxes()
    {
        installedFiles = Library::installed (Library::folder());
        kitBox.clear (juce::dontSendNotification);
        keysBox.clear (juce::dontSendNotification);
        keysBox.addItem ("Synth (knobs + presets)", 1);
        for (int i = 0; i < installedFiles.size(); ++i)
        {
            const auto name = installedFiles[i].getParentDirectory().getFileName() + " / " + installedFiles[i].getFileNameWithoutExtension();
            kitBox.addItem (name, 100 + i);
            keysBox.addItem (name, 100 + i);
        }
        kitBox.setTextWhenNothingSelected (installedFiles.isEmpty() ? "no kit installed (Library...)" : "choose a drum kit");
        showInstrumentName();
    }

    void loadDefaultKit() // the last kit, else <library>/drums/ALL.sfz if it is there
    {
        auto f = juce::File (kitFile().loadFileAsString().trim());
        if (! f.existsAsFile()) f = Library::folder().getChildFile ("drums/ALL.sfz");
        if (f.existsAsFile()) loadInstrumentFile (f, drumTrack);
    }

    // Tracks as rows: name + instrument, M(ute) / S(olo), their notes over time, the playhead. Click a row to select it:
    // the keys, knobs, presets and Rec then work on that track.
    struct SongView : juce::Component
    {
        SongView (Song& s, const juce::MidiMessageSequence& t, juce::CriticalSection& l) : song (s), take (t), takeLock (l) {}

        static constexpr int header = 170;
        float rowH() const { return (float) getHeight() / (float) juce::jmax (1, song.count.load()); }
        juce::Rectangle<float> muteBox (int i) const { return { (float) header - 54, i * rowH() + 4, 22, juce::jmin (22.0f, rowH() - 8) }; }
        juce::Rectangle<float> soloBox (int i) const { return muteBox (i).translated (26, 0); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff1d2126));
            const int n = song.count;
            static const juce::uint32 palette[] { 0xfff0a030, 0xffe8687a, 0xff5ec27a, 0xff3fa7d6, 0xffb07ee8, 0xffe8c547, 0xff4fc9b0, 0xffd67ad0 };
            const double len = juce::jmax (8.0, song.length() + 1.0, playhead + 1.0);
            const float w = (float) getWidth() - header;
            auto xOf = [&] (double t) { return (float) header + (float) (juce::jmax (0.0, t) / len) * w; };
            for (int i = 0; i < n; ++i)
            {
                const auto row = juce::Rectangle<float> (0, i * rowH(), (float) getWidth(), rowH());
                auto& t = *song.tracks[(size_t) i];
                const juce::Colour colour (palette[i % 8]);
                if (i == song.selected) { g.setColour (colour.withAlpha (0.14f)); g.fillRect (row); }
                g.setColour (colour);
                g.fillRect (row.withWidth (5.0f).reduced (0, 2)); // track colour stripe
                g.setColour (juce::Colour (0xff3a434b));
                g.drawHorizontalLine ((int) row.getBottom() - 1, 0, (float) getWidth());
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (13.0f, juce::Font::bold));
                g.drawText (trackName (i), row.withWidth ((float) header - 60).withTrimmedLeft (12), juce::Justification::centredLeft, true);
                for (auto [box, on, label] : { std::tuple { muteBox (i), t.mute.load(), "M" }, std::tuple { soloBox (i), t.solo.load(), "S" } })
                {
                    g.setColour (on ? juce::Colour (0xffe8892b) : juce::Colour (0xff3a434b));
                    g.fillRoundedRectangle (box, 3.0f);
                    g.setColour (juce::Colours::white);
                    g.drawText (label, box, juce::Justification::centred);
                }
                // notes: this track's clip, or the take being recorded on it
                const bool rec = song.recordTrack == i;
                const juce::ScopedLock sl (takeLock);
                const auto& seq = rec ? take : t.clip;
                int lo = 127, hi = 0;
                for (auto* e : seq) if (e->message.isNoteOn()) { lo = juce::jmin (lo, e->message.getNoteNumber()); hi = juce::jmax (hi, e->message.getNoteNumber()); }
                if (lo > hi) continue;
                const float nh = (row.getHeight() - 6) / (float) (hi - lo + 1);
                for (int k = 0; k < seq.getNumEvents(); ++k)
                {
                    const auto& m = seq.getEventPointer (k)->message;
                    if (! m.isNoteOn()) continue;
                    const int off = seq.getIndexOfMatchingKeyUp (k);
                    const double end = off >= 0 ? seq.getEventTime (off) : juce::jmax (m.getTimeStamp(), playhead);
                    g.setColour ((rec ? juce::Colour (0xffd6453f) : colour).withAlpha (0.4f + 0.6f * m.getFloatVelocity()));
                    g.fillRect (juce::Rectangle<float> (xOf (m.getTimeStamp()), row.getY() + 3 + (float) (hi - m.getNoteNumber()) * nh,
                                                        juce::jmax (2.0f, xOf (end) - xOf (m.getTimeStamp())), juce::jmax (1.5f, nh - 1)));
                }
            }
            if (playhead > 0.0) { g.setColour (juce::Colours::white); g.drawVerticalLine ((int) xOf (playhead), 0, (float) getHeight()); }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = (int) (e.position.y / rowH());
            if (i < 0 || i >= song.count) return;
            auto& t = *song.tracks[(size_t) i];
            if (muteBox (i).contains (e.position)) t.mute = ! t.mute;
            else if (soloBox (i).contains (e.position)) t.solo = ! t.solo;
            else if (song.recordTrack < 0 && onSelect) onSelect (i); // not while recording: the take belongs to one track
            repaint();
        }

        Song& song;
        const juce::MidiMessageSequence& take;
        juce::CriticalSection& takeLock;
        std::function<juce::String (int)> trackName;
        std::function<void (int)> onSelect;
        double playhead = -1.0;
    };

    // ---- Presets: built-in ones, plus the user's as JSON files in Documents\LocalMIDIStudio\Presets ----
    static juce::File presetsFolder() { return takesFolder().getChildFile ("Presets"); }

    void refreshPresetList()
    {
        presetBox.clear (juce::dontSendNotification);
        presetBox.addSectionHeading ("Built-in");
        for (size_t i = 0; i < Presets::factory().size(); ++i) presetBox.addItem (Presets::factory()[i].name, (int) i + 1);
        userPresets = presetsFolder().findChildFiles (juce::File::findFiles, false, "*.json");
        userPresets.sort();
        if (! userPresets.isEmpty()) presetBox.addSectionHeading ("Yours");
        for (int i = 0; i < userPresets.size(); ++i) presetBox.addItem (userPresets[i].getFileNameWithoutExtension(), 1000 + i);
    }

    void loadSelectedPreset()
    {
        const int id = presetBox.getSelectedId();
        if (id <= 0) return;
        const auto json = id < 1000 ? Presets::factoryJson (Presets::factory()[(size_t) id - 1]) : userPresets[id - 1000].loadFileAsString();
        juce::String name;
        const auto error = Presets::fromJson (json, sel().params, name);
        if (error.isNotEmpty()) { log ("Preset not loaded: " + error); return; }
        for (auto& r : refreshers) r();
        log ("Preset: " + name);
    }

    void savePreset()
    {
        auto* w = new juce::AlertWindow ("Save preset", "Name for this sound:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "My sound");
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int result)
        {
            const auto name = w->getTextEditorContents ("name").trim();
            if (result != 1 || name.isEmpty()) return;
            presetsFolder().createDirectory();
            const auto file = presetsFolder().getChildFile (juce::File::createLegalFileName (name) + ".json");
            if (! file.replaceWithText (Presets::toJson (sel().params, name))) { log ("ERROR: could not save " + file.getFullPathName()); return; }
            refreshPresetList();
            presetBox.setSelectedId (1000 + userPresets.indexOf (file), juce::dontSendNotification);
            log ("Saved preset " + file.getFullPathName());
        }), true);
    }

    // ---- Sampler: an .sfz sample set or a single audio file, read on a background thread ----
    void chooseInstrument()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a sampled instrument", takesFolder(), "*.sfz;*.wav;*.flac;*.ogg;*.aif;*.aiff");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File()) loadInstrumentFile (fc.getResult());
        });
    }

    void loadInstrumentFile (const juce::File& file, int track = -1) // -1 = the selected track
    {
        if (track < 0) track = song.selected;
        log ("Loading " + file.getFullPathName() + " for track " + juce::String (track + 1) + " ...");
        instrumentLabel.setText ("Loading " + file.getFileName() + "...", juce::dontSendNotification);
        juce::Thread::launch ([safe = SafePointer<MainComponent> (this), file, track]
        {
            juce::String error;
            auto inst = Sfz::load (file, error);
            juce::MessageManager::callAsync ([safe, inst, error, file, track]
            {
                if (safe == nullptr) return;
                if (inst == nullptr) { safe->log ("Instrument not loaded: " + error); safe->showInstrumentName(); return; }
                safe->song.tracks[(size_t) track]->engine.setInstrument (inst);
                safe->instrumentFiles[(size_t) track] = file;
                if (track == drumTrack) safe->kitFile().replaceWithText (file.getFullPathName());
                const auto* s = static_cast<InstrumentSound*> (inst.get());
                safe->showInstrumentName();
                safe->log ("Track " + juce::String (track + 1) + ": " + s->name + " (" + juce::String ((int) s->regions.size()) + " regions)");
            });
        });
    }

    void showInstrumentName() // everything that names the instruments: top label, pickers, pads, track rows
    {
        const auto inst = sel().instrument();
        instrumentLabel.setText (inst != nullptr ? static_cast<InstrumentSound*> (inst.get())->name : juce::String ("Synth"), juce::dontSendNotification);
        auto idOf = [this] (int track) { const int k = installedFiles.indexOf (instrumentFiles[(size_t) track]);
                                         return song.tracks[(size_t) track]->engine.instrument() == nullptr ? 1 : k >= 0 ? 100 + k : 0; };
        keysBox.setSelectedId (idOf (song.selected), juce::dontSendNotification);
        kitBox.setSelectedId (idOf (drumTrack) == 1 ? 0 : idOf (drumTrack), juce::dontSendNotification);
        pads.repaint(); // the pad names follow the loaded kit
        songView.repaint();
    }

    SynthEngine& sel()   { return song.tracks[(size_t) song.selected.load()]->engine; } // what the keys, knobs and presets act on
    SynthEngine& drums() { return song.tracks[(size_t) drumTrack]->engine; }

    juce::String trackName (int i)
    {
        const auto inst = song.tracks[(size_t) i]->engine.instrument();
        return juce::String (i + 1) + (i == 0 ? " Keys" : i == drumTrack ? " Drums" : " Track") + ": "
             + (inst != nullptr ? static_cast<InstrumentSound*> (inst.get())->name : juce::String ("Synth"));
    }

    void selectTrack (int i)
    {
        song.selected = i;
        for (auto& r : refreshers) r(); // knobs and menus now show this track's sound
        presetBox.setSelectedId (0, juce::dontSendNotification);
        showInstrumentName();
        log ("Selected track " + juce::String (i + 1) + ": the keys, knobs, presets and Rec now work on it (the pads always play the drums).");
    }

    // ---- Instrument library: what is installed in the library folder, and free downloads ----
    void showLibraryMenu()
    {
        const auto root = Library::folder();
        const auto files = Library::installed (root);
        const auto sep = juce::File::getSeparatorString();
        std::map<juce::String, juce::PopupMenu> byFolder; // one submenu per top-level folder (one library each)
        for (int i = 0; i < files.size(); ++i)
        {
            const auto rel = files[i].getRelativePathFrom (root);
            byFolder[rel.contains (sep) ? rel.upToFirstOccurrenceOf (sep, false, false) : juce::String ("(top)")]
                .addItem (1 + i, rel.contains (sep) ? rel.fromFirstOccurrenceOf (sep, false, false) : rel);
        }
        juce::PopupMenu menu;
        menu.addSectionHeader ("Installed - " + root.getFullPathName());
        if (files.isEmpty()) menu.addItem (-1, "(nothing yet: download one below)", false);
        for (auto& [name, sub] : byFolder) menu.addSubMenu (name, sub);
        menu.addSectionHeader ("Download (free)");
        const auto& cat = Library::catalog();
        for (size_t j = 0; j < cat.size(); ++j)
        {
            const bool have = root.getChildFile (cat[j].id).isDirectory();
            menu.addItem (10000 + (int) j, juce::String (cat[j].name) + " - " + cat[j].kind + ", " + cat[j].licence + ", ~" + juce::String (cat[j].approxMb) + " MB"
                                              + (have ? "  (installed)" : ""), ! downloading && ! have);
        }
        menu.addSeparator();
        if (downloading) menu.addItem (20002, "Cancel download");
        menu.addItem (20000, "Choose library folder...");
        menu.addItem (20001, "Open library folder");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (libraryButton), [this, files, root] (int r)
        {
            if (r >= 1 && r <= files.size()) loadInstrumentFile (files[r - 1]);
            else if (r >= 10000 && r < 10000 + (int) Library::catalog().size()) startDownload (Library::catalog()[(size_t) r - 10000], root);
            else if (r == 20002) *downloadCancel = true;
            else if (r == 20001) { root.createDirectory(); root.startAsProcess(); }
            else if (r == 20000)
            {
                chooser = std::make_unique<juce::FileChooser> ("Library folder", root);
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
                {
                    if (fc.getResult() == juce::File()) return;
                    Library::setFolder (fc.getResult());
                    log ("Library folder: " + fc.getResult().getFullPathName());
                });
            }
        });
    }

    void startDownload (const Library::Item& item, const juce::File& root) // clicking the menu item is the user's go-ahead
    {
        downloading = true;
        downloadCancel = std::make_shared<std::atomic<bool>> (false);
        log ("Downloading " + juce::String (item.name) + " (" + item.licence + ", ~" + juce::String (item.approxMb) + " MB) from " + item.url);
        juce::Thread::launch ([safe = SafePointer<MainComponent> (this), url = juce::String (item.url), id = juce::String (item.id),
                               name = juce::String (item.name), mb = item.approxMb, root, cancel = downloadCancel]
        {
            juce::int64 shown = 0;
            const auto err = Library::install (url, id, root, [&] (juce::int64 n)
            {
                if (n - shown < (1 << 20)) return; // update the label every MB
                shown = n;
                juce::MessageManager::callAsync ([safe, name, n, mb]
                {
                    if (safe) safe->instrumentLabel.setText ("Downloading " + name + " " + juce::String ((int) (n >> 20)) + "/~" + juce::String (mb) + " MB", juce::dontSendNotification);
                });
            }, *cancel);
            juce::MessageManager::callAsync ([safe, err, name, id, root]
            {
                if (safe == nullptr) return;
                safe->downloading = false;
                safe->showInstrumentName();
                safe->log (err.isEmpty() ? name + " installed in " + root.getChildFile (id).getFullPathName() + " - pick it from Library..."
                                         : "Download of " + name + " failed: " + err);
            });
        });
    }

    void enableAllMidiInputs()
    {
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (! deviceManager.isMidiInputDeviceEnabled (d.identifier))
                deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
    }

    void log (const juce::String& line)
    {
        if (monitor.getTotalNumChars() > 50000) monitor.clear();
        monitor.moveCaretToEnd();
        monitor.insertTextAtCaret (line + "\n");
        lastLog.setText (line, juce::dontSendNotification);
    }

    void addKnob (const juce::String& name, const juce::String& key, double lo, double hi, std::atomic<float> SynthParams::* member, double mid = 0)
    {
        auto* s = knobs.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow));
        s->setRange (lo, hi);
        if (mid > 0) s->setSkewFactorFromMidPoint (mid);
        s->setNumDecimalPlacesToDisplay (hi > 100 ? 0 : 3);
        s->setValue ((sel().params.*member).load(), juce::dontSendNotification);
        s->onValueChange = [this, s, member] { sel().params.*member = (float) s->getValue(); }; // always the selected track
        refreshers.push_back ([this, s, member] { s->setValue ((sel().params.*member).load(), juce::dontSendNotification); });
        knobs_[key] = { s, addLabel (name, *s), name };
        s->addMouseListener (this, false); // right-click -> MIDI Learn
        addAndMakeVisible (s);
    }

    void addCombo (juce::ComboBox& box, const juce::String& name, const juce::StringArray& items, std::atomic<int> SynthParams::* member)
    {
        box.addItemList (items, 1);
        box.setSelectedItemIndex ((sel().params.*member).load(), juce::dontSendNotification);
        box.onChange = [this, &box, member] { sel().params.*member = box.getSelectedItemIndex(); };
        refreshers.push_back ([this, &box, member] { box.setSelectedItemIndex ((sel().params.*member).load(), juce::dontSendNotification); });
        addLabel (name, box);
        addAndMakeVisible (box);
    }

    juce::Label* addLabel (const juce::String& name, juce::Component& c)
    {
        auto* l = labels.add (new juce::Label ({}, name));
        l->setJustificationType (juce::Justification::centred);
        l->attachToComponent (&c, false);
        return l;
    }

    // ---- MIDI Learn: right-click a knob, move a hardware knob; bindings persist in midi-map.json ----
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu()) return;
        for (auto& [key, k] : knobs_)
            if (k.slider == e.eventComponent)
            {
                juce::PopupMenu menu;
                menu.addItem (1, "MIDI Learn: then move a knob on your keyboard");
                menu.addItem (2, "Forget MIDI mapping", midiMap.forParam (key) != nullptr);
                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (k.slider), [this, key = key] (int r)
                {
                    if (r == 1) { learnParam = key; log ("MIDI Learn: move a knob or fader on your keyboard for " + knobs_[key].name + "..."); }
                    if (r == 2) { midiMap.forget (key); saveMidiMap(); log ("MIDI mapping removed from " + knobs_[key].name); }
                });
                return;
            }
    }

    void handleController (int channel, int cc, int value) // message thread
    {
        if (learnParam.isNotEmpty() && cc != 64 && cc < 120) // not the sustain pedal (easy to hit by accident) nor channel-mode messages
        {
            midiMap.bind (channel, cc, learnParam);
            log ("MIDI Learn: CC " + juce::String (cc) + " on channel " + juce::String (channel) + " now controls " + knobs_[learnParam].name);
            learnParam = {};
            saveMidiMap();
            return;
        }
        const auto it = knobs_.find (midiMap.find (channel, cc));
        if (it != knobs_.end()) // follows the knob's own scale (e.g. log cutoff); onValueChange updates the engine
            it->second.slider->setValue (it->second.slider->proportionOfLengthToValue (value / 127.0));
    }

    void saveMidiMap()
    {
        midiMapFile().getParentDirectory().createDirectory();
        if (! midiMapFile().replaceWithText (midiMap.toJson())) log ("ERROR: could not save " + midiMapFile().getFullPathName());
        updateKnobLabels();
    }

    void updateKnobLabels()
    {
        for (auto& [key, k] : knobs_)
        {
            const auto* b = midiMap.forParam (key);
            k.label->setText (b != nullptr ? k.name + " CC" + juce::String (b->cc) : k.name, juce::dontSendNotification);
        }
    }

    static juce::File midiMapFile() { return settingsFile().getSiblingFile ("midi-map.json"); }

    static juce::File settingsFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("LocalMIDIStudio/audio-settings.xml");
    }

    Song song;
    PadRouter router;
    static constexpr int drumTrack = 1; // the pads always play this track
    std::array<juce::File, Song::maxTracks> instrumentFiles; // what each track loaded (for the pickers)
    juce::Array<juce::File> installedFiles;
    juce::MidiBuffer midi, keysMidi, padMidi;
    MidiQueue queue;
    double currentRate = 48000.0;
    static inline thread_local bool fromHardware = false; // set while hardware MIDI updates the on-screen keys
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard;
    juce::AudioDeviceSelectorComponent selector;
    juce::ComboBox wave, filterType, lfoTarget, presetBox;
    juce::TextButton savePresetButton { "Save preset" }, loadInstrumentButton { "Load instrument..." }, synthButton { "Synth" };
    juce::Label instrumentLabel;
    juce::TextButton libraryButton { "Library..." };
    bool downloading = false;                                  // one download at a time (message thread)
    std::shared_ptr<std::atomic<bool>> downloadCancel = std::make_shared<std::atomic<bool>> (false); // shared with the download thread
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<juce::File> userPresets;
    std::vector<std::function<void()>> refreshers; // put every control back in line with engine.params
    struct Knob { juce::Slider* slider; juce::Label* label; juce::String name; };
    std::map<juce::String, Knob> knobs_; // by parameter key
    MidiMap midiMap;
    juce::String learnParam;             // parameter waiting for a hardware knob
    juce::OwnedArray<juce::Slider> knobs;
    struct Panel { juce::Rectangle<int> area; juce::String title; juce::Colour colour; };
    std::vector<Panel> panels; // rebuilt by resized, drawn by paint
    static constexpr int barH = 44, footH = 36;
    juce::OwnedArray<juce::Label> labels;
    juce::TextEditor monitor;
    juce::Label status;
    bool focused = false;

    // Take being recorded (MIDI + GUI threads, guarded by takeLock) and the last saved file.
    juce::CriticalSection takeLock;
    juce::MidiMessageSequence take;
    std::atomic<bool> recording { false };
    bool playing = false;

    // Diagnostics: written by the audio thread, read and held by the GUI timer.
    std::atomic<double> worstBlockMs { 0.0 };
    std::atomic<int> overBudget { 0 };
    double shownWorstMs = 0.0;
    int worstHold = 0, limitHold = 0;
    juce::int64 lastHits = 0;
    float shownPeak = 0.0f;

    juce::TextButton recButton { "Rec" }, playButton { "Play" }, stopButton { "Stop" },
                     wavButton { "Export WAV" }, openMidiButton { "Open MIDI..." }, folderButton { "Open folder" };
    SongView songView { song, take, takeLock };
    juce::Slider bpmSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ToggleButton clickToggle { "Click" };
    juce::TextButton addTrackButton { "+ Track" }, saveSongButton { "Save song" }, openSongButton { "Open song" };
    juce::File songFile; // where the song was last saved or opened
    juce::Label songHint;
    DrumPads pads;
    juce::ComboBox kitBox, keysBox;
    juce::Label keysLabel;
    juce::Slider drumVolume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label padsLabel;
    juce::ComboBox padLayout;
    juce::TextButton learnPadsButton { "Learn pads" };
    juce::TextButton settingsButton { "Audio settings" }, logButton { "Log" };
    juce::Label lastLog;
    // shortcut: on hot-plug every input gets re-enabled, even ones the user switched off; track user choices if that annoys
    juce::MidiDeviceListConnection midiListener = juce::MidiDeviceListConnection::make ([this]
    {
        enableAllMidiInputs();
        log ("MIDI device list changed - inputs re-enabled.");
    });

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class App : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Local MIDI Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String&) override
    {
        look.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        look.setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff1b1f24));
        juce::LookAndFeel::setDefaultLookAndFeel (&look);
        window = std::make_unique<Window>();
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (new MainComponent(), true);
        window->setResizable (true, true);
        window->centreWithSize (window->getWidth(), window->getHeight());
        window->setVisible (true);
    }

    void shutdown() override { window.reset(); juce::LookAndFeel::setDefaultLookAndFeel (nullptr); }
    void systemRequestedQuit() override { quit(); }

private:
    struct Window : juce::DocumentWindow
    {
        Window() : DocumentWindow ("Local MIDI Studio", juce::Colours::black, allButtons) {}
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    // Dark theme with one orange accent: window, widgets, menus, outline, text, fill, highlighted text/fill, menu text
    juce::LookAndFeel_V4 look { { juce::Colour (0xff1b1f24), juce::Colour (0xff2b3138), juce::Colour (0xff22272d),
                                  juce::Colour (0xff3a414a), juce::Colour (0xffdde2e7), juce::Colour (0xfff0a030),
                                  juce::Colour (0xff15181c), juce::Colour (0xfff0a030), juce::Colour (0xffdde2e7) } };
    std::unique_ptr<juce::DocumentWindow> window;
};

START_JUCE_APPLICATION (App)
