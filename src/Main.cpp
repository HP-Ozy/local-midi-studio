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
#include "Steps.h"
#include "Notes.h"

class MainComponent : public juce::AudioAppComponent,
                      private juce::MidiInputCallback,
                      private juce::MidiKeyboardState::Listener,
                      private juce::Timer
{
public:
    MainComponent()
        : keyboard (keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard),
          selector (deviceManager, 0, 1, 2, 2, true, false, true, false) // 0..1 input: the microphone for voice takes
    {
        auto saved = juce::XmlDocument::parse (settingsFile());
        setAudioChannels (1, 2, saved.get());
        enableAllMidiInputs();
        deviceManager.addMidiInputDeviceCallback ({}, this);
        keyboardState.addListener (this);

        song.count = 2; // track 1 = keys, track 2 = drums (the pads always play it)
        addCombo (wave, "Waveform", { "Sine", "Saw", "Square", "Triangle", "Noise", "Pulse" }, &SynthParams::wave); // index = SynthParams::wave
        addCombo (filterType, "Filter", { "Low-pass", "High-pass", "Band-pass" }, &SynthParams::filterType);
        addCombo (lfoTarget, "LFO to", { "Pitch", "Filter", "Volume", "Width" }, &SynthParams::lfoTarget); // mod wheel adds to LFO depth
        addKnob ("Tune",      "tune",      -24.0, 24.0,  &SynthParams::tune);
        addKnob ("Fine",      "fine",      -100,  100,   &SynthParams::fine);
        addKnob ("Width",     "pulseWidth", 0.05,  0.95,  &SynthParams::pulseWidth); // pulse wave only
        addCombo (osc2WaveBox, "Osc 2", { "Sine", "Saw", "Square", "Triangle", "Noise", "Pulse" }, &SynthParams::osc2Wave);
        addKnob ("Tune 2",    "osc2Tune",  -24.0, 24.0,  &SynthParams::osc2Tune);
        addKnob ("Fine 2",    "osc2Fine",  -100,  100,   &SynthParams::osc2Fine);
        addKnob ("Mix",       "osc2Mix",    0.0,  1.0,   &SynthParams::osc2Mix);
        shapeView.params = [this]() -> SynthParams& { return sel().params; };
        addAndMakeVisible (shapeView);
        addKnob ("Attack",    "attack",    0.001, 5.0,   &SynthParams::attack,    0.3);
        addKnob ("Decay",     "decay",     0.001, 5.0,   &SynthParams::decay,     0.5);
        addKnob ("Sustain",   "sustain",   0.0,   1.0,   &SynthParams::sustain);
        addKnob ("Release",   "release",   0.001, 8.0,   &SynthParams::release,   0.5);
        addKnob ("Cutoff",    "cutoff",    20.0,  20000, &SynthParams::cutoff,    1000);
        addKnob ("Resonance", "resonance", 0.5,   10.0,  &SynthParams::resonance, 2.0);
        addKnob ("LFO rate",  "lfoRate",   0.1,   20.0,  &SynthParams::lfoRate,   3.0);
        addKnob ("LFO depth", "lfoDepth",  0.0,   1.0,   &SynthParams::lfoDepth);
        addKnob ("Volume",    "gain",      0.0,   1.0,   &SynthParams::gain);
        addKnob ("Pan",       "pan",      -1.0,   1.0,   &SynthParams::pan);
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
        stepsButton.setClickingTogglesState (true);
        stepsButton.setTooltip ("Step sequencer: click the squares to build a one-bar drum pattern, repeated over the bars");
        stepsButton.onClick = [this]
        {
            const bool on = stepsButton.getToggleState();
            refreshSteps();
            steps.setVisible (on); stepBars.setVisible (on);
            pads.setVisible (! on); padLayout.setVisible (! on); learnPadsButton.setVisible (! on);
        };
        stepBars.setRange (1.0, 64.0, 1.0);
        stepBars.setValue (4.0, juce::dontSendNotification);
        stepBars.setTextValueSuffix (" bars");
        stepBars.onValueChange = [this] { writeSteps(); };
        steps.nameOf = [this] (int r) { return pads.nameOf (pads.sounds[(size_t) r]); };
        steps.onChange = [this] { writeSteps(); };
        loadPads();
        for (auto* c : std::initializer_list<juce::Component*> { &pads, &padsLabel, &padLayout, &learnPadsButton, &stepsButton, &steps, &stepBars })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        steps.setVisible (false); stepBars.setVisible (false);
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
        songView.onDropAudio = [this] (const juce::File& f, int track, double at) { return dropAudio (f, track, at); };
        songView.onRegionEdit = [this] (int track, int region, int what, double at) { return editRegion (track, region, what, at); };
        songView.onTrackMenu = [this] (int i)
        {
            juce::PopupMenu m;
            m.addItem (1, "Delete track " + juce::String (i + 1), i > drumTrack); // tracks 1 (keys) and 2 (drums the pads play) stay
            m.addItem (2, "Clear track " + juce::String (i + 1) + " (notes and audio)");
            m.showMenuAsync (juce::PopupMenu::Options(), [this, i] (int item) { if (item == 1) deleteTrack (i); else if (item == 2) clearTrack (i); });
        };
        bpmSlider.setRange (40.0, 240.0, 1.0);
        bpmSlider.setValue (song.bpm.load(), juce::dontSendNotification);
        bpmSlider.setTextValueSuffix (" BPM");
        bpmSlider.onValueChange = [this] { song.bpm = bpmSlider.getValue(); if (stepsOwnClip()) writeSteps(); }; // a step pattern follows the tempo
        clickToggle.setToggleState (song.metronome.load(), juce::dontSendNotification);
        clickToggle.onClick = [this] { song.metronome = clickToggle.getToggleState(); };
        masterVolume.setRange (0.0, 1.0);
        masterVolume.setValue (song.master.load(), juce::dontSendNotification);
        masterVolume.onValueChange = [this] { song.master = (float) masterVolume.getValue(); };
        masterVolume.setTooltip ("Master volume (all tracks, also the exported song)");
        addTrackButton.onClick = [this]
        {
            if (song.count >= Song::maxTracks) { log ("All " + juce::String (Song::maxTracks) + " tracks are in use."); return; }
            ++song.count; // engines exist already: nothing is allocated for the audio thread
            selectTrack (song.count - 1);
            songView.repaint();
        };
        songHint.setText ("Click a track to select it  |  drop audio files on it  |  Ctrl+wheel: zoom", juce::dontSendNotification);
        editNotesButton.setClickingTogglesState (true);
        editNotesButton.onClick = [this]
        {
            const bool on = editNotesButton.getToggleState();
            roll.setVisible (on); songView.setVisible (! on);
            loadRoll();
            songHint.setText (on ? "Click: add  |  drag: move / stretch  |  right-click: delete  |  wheel: up-down, Shift: time, Ctrl: zoom"
                                 : "Click a track to select it  |  drop audio files on it  |  Ctrl+wheel: zoom", juce::dontSendNotification);
        };
        roll.onCommit = [this] { commitRoll(); };
        quantizeButton.onClick = [this] { quantizeTrack(); };
        undoButton.onClick = [this] { undo (false); };
        redoButton.onClick = [this] { undo (true); };
        loopToggle.setTooltip ("Loop recording: Rec cycles over these bars, every pass is added to the track and heard on the next");
        voiceButton.setTooltip ("Records the microphone into the selected track while the song plays; click again (or Stop) to finish");
        voiceButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8b1e1e));
        voiceButton.onClick = [this] { if (recording) stopAll(); else startRecording (true); };
        loopBars.setRange (1.0, 16.0, 1.0);
        loopBars.setValue (4.0, juce::dontSendNotification);
        loopBars.setTextValueSuffix (" bars");
        quantizeButton.setTooltip ("Move the selected track's notes to the nearest 1/16");
        roll.setVisible (false);
        saveSongButton.onClick = [this] { chooseSaveSong(); };
        openSongButton.onClick = [this] { chooseOpenSong(); };
        newSongButton.onClick = [this] { chooseNewSong(); };
        setDefaultButton.onClick = [this] { saveDefault(); };
        newSongButton.setTooltip ("Start again from the default sounds, effects and tracks (no notes)");
        setDefaultButton.setTooltip ("Make the current sounds, effects, instruments and tracks the starting point of New song");
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
                                                                 &addTrackButton, &songHint, &kitBox, &keysBox, &keysLabel, &drumVolume, &masterVolume, &masterLabel,
                                                                 &saveSongButton, &openSongButton, &settingsButton, &logButton, &lastLog,
                                                                 &editNotesButton, &quantizeButton, &undoButton, &redoButton, &loopToggle, &loopBars, &voiceButton, &newSongButton, &setDefaultButton })
        {
            if (c != &keyboard) c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        roll.setWantsKeyboardFocus (false);
        addChildComponent (roll);
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
        micBuf.assign ((size_t) juce::jmax (blockSize, 1), 0.0f);
        const auto* d = deviceManager.getCurrentAudioDevice();
        hasMic = d != nullptr && d->getActiveInputChannels().countNumberOfSetBits() > 0;
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();
        const double now = MidiQueue::now();
        midi.clear(); keysMidi.clear(); padMidi.clear();
        queue.popInto (midi, info.numSamples, currentRate, now);
        router.split (midi, keysMidi, padMidi); // pads -> drum track, the rest -> the selected track
        const float* mic = nullptr; // the input arrives in channel 0 of the same buffer: keep it before the song overwrites it
        if (hasMic && info.numSamples <= (int) micBuf.size() && info.buffer->getNumChannels() > 0)
        {
            std::copy_n (info.buffer->getReadPointer (0, info.startSample), info.numSamples, micBuf.data());
            mic = micBuf.data();
        }
        song.render (*info.buffer, info.startSample, info.numSamples, keysMidi, now, drumTrack, &padMidi, mic);

        // Callback time vs the time this buffer lasts: over 100 % is a guaranteed dropout (WASAPI shared reports none).
        const double ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
        if (ms > worstBlockMs.load()) worstBlockMs = ms;
        if (ms > info.numSamples * 1000.0 / currentRate) ++overBudget;
    }

    void releaseResources() override {}

    // Knob groups of the sound panels: title, accent colour, optional selector, knobs by parameter key.
    struct SoundGroup { juce::String title; juce::Colour colour; juce::ComboBox* combo; juce::StringArray keys; juce::Component* view = nullptr; };
    std::vector<SoundGroup> soundGroups()
    {
        return { { "OSCILLATOR", juce::Colour (0xfff0a030), &wave,       { "tune", "fine", "pulseWidth" } },
                 { "OSC 2",      juce::Colour (0xfff0a030), &osc2WaveBox, { "osc2Tune", "osc2Fine", "osc2Mix" } },
                 { "ENVELOPE",   juce::Colour (0xff5ec27a), nullptr,     { "attack", "decay", "sustain", "release" } },
                 { "SHAPE",      juce::Colour (0xffdde2e7), nullptr,     {}, &shapeView },
                 { "FILTER",     juce::Colour (0xff3fa7d6), &filterType, { "cutoff", "resonance" } },
                 { "LFO",        juce::Colour (0xffb07ee8), &lfoTarget,  { "lfoRate", "lfoDepth" } },
                 { "EFFECTS",    juce::Colour (0xffe8687a), nullptr,     { "chorusMix", "delayTime", "delayFeedback", "delayMix", "reverbSize", "reverbMix" } },
                 { "OUTPUT",     juce::Colour (0xffdde2e7), nullptr,     { "gain", "pan" } } };
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
        masterLabel.setBounds (bar.removeFromLeft (46));
        masterVolume.setBounds (bar.removeFromLeft (74)); // wider squeezes "Open song" at 1280 px
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
        stepsButton.setBounds (padHeader.removeFromLeft (60));
        learnPadsButton.setBounds (padHeader.removeFromRight (130));
        padHeader.removeFromRight (6);
        padLayout.setBounds (padHeader.removeFromRight (80));
        stepBars.setBounds (padHeader.withTrimmedLeft (10).removeFromRight (150));
        left.removeFromTop (6);
        pads.setBounds (left);
        steps.setBounds (left);
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
        auto slotsOf = [] (const SoundGroup& gr) { return gr.keys.size() + (gr.combo != nullptr ? 2 : 0) + (gr.view != nullptr ? 3 : 0); }; // a selector takes two slots, a view three
        for (auto [from, to] : { std::pair { 0, 4 }, std::pair { 4, 8 } }) // two rows of sound panels
        {
            auto line = r.removeFromTop (128);
            int slots = 0;
            for (int g = from; g < to; ++g) slots += slotsOf (groups[(size_t) g]);
            const int w = (line.getWidth() - 6 * (to - from - 1)) / juce::jmax (1, slots);
            for (int g = from; g < to; ++g)
            {
                const auto& gr = groups[(size_t) g];
                const int n = slotsOf (gr);
                auto area = panel (g == to - 1 ? line : line.removeFromLeft (n * w), gr.title, gr.colour).withTrimmedTop (18); // 18: knob labels
                line.removeFromLeft (6);
                const int sw = area.getWidth() / n;
                if (gr.combo != nullptr) gr.combo->setBounds (area.removeFromLeft (2 * sw).withSizeKeepingCentre (2 * sw - 12, 26));
                if (gr.view != nullptr) gr.view->setBounds (area.withTop (area.getY() - 18)); // no knob labels above it
                for (auto& key : gr.keys) knobs_.at (key).slider->setBounds (area.removeFromLeft (sw));
            }
            r.removeFromTop (6);
        }

        auto songArea = panel (r, "SONG", juce::Colour (0xff3fa7d6));
        auto songHeader = songArea.removeFromTop (26);
        addTrackButton.setBounds (songHeader.removeFromLeft (80));
        songHeader.removeFromLeft (6);
        for (auto [b, w] : { std::pair<juce::Component*, int> { &editNotesButton, 90 }, { &quantizeButton, 80 }, { &undoButton, 56 }, { &redoButton, 56 }, { &loopToggle, 54 }, { &loopBars, 118 }, { &voiceButton, 96 } })
        { b->setBounds (songHeader.removeFromLeft (w)); songHeader.removeFromLeft (6); }
        setDefaultButton.setBounds (songHeader.removeFromRight (90));
        songHeader.removeFromRight (6);
        newSongButton.setBounds (songHeader.removeFromRight (90));
        songHeader.removeFromLeft (4);
        songHint.setBounds (songHeader);
        songArea.removeFromTop (4);
        songView.setBounds (songArea);
        roll.setBounds (songArea);
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
        // focus back to the piano whenever nothing has it (after a dialog or a click elsewhere): computer keys and Space keep working
        if (keyboard.isShowing() && juce::Component::getCurrentlyFocusedComponent() == nullptr && getPeer() != nullptr && getPeer()->isFocused()) keyboard.grabKeyboardFocus();
        const double pos = song.position.load();
        if (playing && ! recording && pos > song.length() + 2.0) stopTransport(); // played to the end (+ release tails)
        songView.setPlayhead ((playing || recording) ? pos : -1.0);
        if (roll.isVisible()) { roll.playhead = songView.playhead; roll.repaint(); }
        if (const int s = (playing || recording) && pos >= 0.0 ? (int) (std::fmod (pos, song.barSeconds()) / song.barSeconds() * Steps::steps) : -1; s != steps.playStep)
        { steps.playStep = s; steps.repaint(); }
        recButton.setButtonText (! recording ? "Rec" : pos < 0.0 ? "count-in " + juce::String ((int) std::ceil (-pos / song.beatSeconds()))
                                                                 : "REC  " + juce::String (pos, 1) + " s");
        voiceButton.setButtonText (recording && song.voiceTrack.load() >= 0 ? "Stop voice" : "Rec voice");
        songView.repaint();
        pads.tick();
        shapeView.check();
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
            s << "  |  mic: " << (hasMic ? deviceManager.getAudioDeviceSetup().inputDeviceName : juce::String ("none"));
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
        if (! recording || song.loopLen.load() > 0 || song.voiceTrack.load() >= 0 || ! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isChannelPressure())) return;
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

    void startRecording (bool voice = false) // voice: the microphone instead of MIDI
    {
        stopAll();
        {
            const juce::ScopedLock sl (takeLock);
            take.clear();
        }
        song.songZeroWall = 1e18; // nothing counts until the audio thread starts the transport
        if (voice)
        {
            if (! hasMic) { openSettings(); log ("No microphone: pick it under \"Input\" in these settings, close them, then press Rec voice again."); return; }
            if (loopToggle.getToggleState()) { log ("Voice takes don't loop: untick Loop first."); return; }
            song.voiceTake.setSize (1, (int) (maxVoiceSeconds * currentRate), false, true); // here, never on the audio thread
            song.voiceEnd = 0;
            song.voiceTrack = song.selected.load(); // the track's notes keep playing; its old voice doesn't
            song.recordTrack = -1;
            song.loopLen = 0;
            song.startPos = -(juce::int64) std::llround (song.barSeconds() * currentRate);
            song.run = true;
            recording = true;
            log ("Recording the microphone into track " + juce::String (song.selected + 1) + " after a one-bar count-in (max "
                 + juce::String ((int) maxVoiceSeconds / 60) + " min)... press Stop when done.");
            return;
        }
        song.recordTrack = song.selected.load();
        song.startPos = -(juce::int64) std::llround (song.barSeconds() * currentRate); // one bar of count-in
        const int bars = (int) loopBars.getValue();
        song.loopLen = loopToggle.getToggleState() ? std::llround (bars * song.barSeconds() * currentRate) : 0; // MIDI thread: loop takes are kept by the audio thread
        song.run = true;
        recording = true;
        log ("Recording track " + juce::String (song.selected + 1) + " after a one-bar count-in" + (song.loopLen > 0 ? ", looping " + juce::String (bars) + " bars" : juce::String())
             + "... press Stop when done.");
    }

    void stopRecording()
    {
        if (! recording) return;
        recording = false;
        const double end = juce::jmax (0.0, song.position.load());
        stopTransport();
        if (const int v = song.voiceTrack.exchange (-1); v >= 0) { stopVoiceTake (v); return; }
        const int rec = song.recordTrack.exchange (-1);
        if (const auto L = song.loopLen.exchange (0); L > 0) { stopLoopTake (rec, L); return; }
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
        setClip (rec, take); // Undo brings back what the track had before this take
        writeSong (songsFolder().getChildFile ("autosave.lmsong")); // a take is never lost, even if the app is closed unsaved
        take.clear();
        log ("Track " + juce::String (rec + 1) + " recorded (" + juce::String (end, 1) + " s). Play to hear the song.");
    }

    // Voice take: what the mic recorded, moved earlier by the round-trip latency, becomes the track's voice.
    void stopVoiceTake (int v)
    {
        // shortcut: latency = what the driver reports (buffer in + out + device latencies), not measured with a loopback cable
        juce::int64 shift = 0;
        if (auto* d = deviceManager.getCurrentAudioDevice())
            shift = 2 * d->getCurrentBufferSizeSamples() + d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
        const auto len = song.voiceEnd.load() - shift;
        if (len < (juce::int64) (0.2 * currentRate)) { song.voiceTake.setSize (1, 0); log ("Nothing recorded: the track keeps its voice."); return; }
        juce::AudioBuffer<float> voice (1, (int) len);
        voice.copyFrom (0, 0, song.voiceTake, 0, (int) shift, (int) len);
        song.voiceTake.setSize (1, 0); // free the minutes reserved for the take
        const auto backup = audioFolder (songsFolder()).getChildFile ("voice-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav");
        backup.getParentDirectory().createDirectory();
        const bool kept = writeWav (voice, currentRate, backup); // every take kept as a file: replacing a voice never loses one
        song.tracks[(size_t) v]->audio.add (voice, 0); // transport stopped: the audio thread doesn't read it; older takes stay (delete them by right-click)
        writeSong (songsFolder().getChildFile ("autosave.lmsong"));
        songView.repaint();
        log ("Track " + juce::String (v + 1) + ": voice recorded (" + juce::String (len / currentRate, 1) + " s, latency "
             + juce::String (shift * 1000.0 / currentRate, 1) + " ms compensated)" + (kept ? ", also saved as " + backup.getFileName() : juce::String (", WARNING: backup WAV not written")) + ".");
    }

    // Loop take: the passes (stored by the audio thread, stopped now) folded into one, added to what the track had.
    void stopLoopTake (int rec, juce::int64 L)
    {
        juce::MidiMessageSequence played;
        for (int k = 0; k < song.loopCount; ++k)
        {
            const auto& e = song.loopTake[(size_t) k];
            played.addEvent (juce::MidiMessage (e.data, e.size, (double) e.pos / song.rate));
        }
        auto notes = Notes::read (played);
        if (notes.empty() || rec < 0) { log ("Nothing recorded: the track keeps what it had."); songView.repaint(); return; }
        Notes::fold (notes, (double) L / song.rate);
        auto all = Notes::read (song.tracks[(size_t) rec]->clip);
        all.insert (all.end(), notes.begin(), notes.end());
        setClip (rec, Notes::write (all, song.tracks[(size_t) rec]->clip)); // shortcut: pedal/CC/bend played in a loop take are not kept
        writeSong (songsFolder().getChildFile ("autosave.lmsong"));
        log ("Track " + juce::String (rec + 1) + ": " + juce::String ((int) notes.size()) + " notes from the loop added" + (song.loopCount >= Song::loopTakeMax ? " (take full: later notes dropped)" : "") + ".");
    }

    void startPlayback()
    {
        stopAll();
        song.loopLen = 0;
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

    bool keyPressed (const juce::KeyPress& k) override // keys the piano doesn't use come here
    {
        if (k == juce::KeyPress::spaceKey) { if (playing || recording) stopAll(); else startPlayback(); return true; } // Play / Stop
        const bool del = k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey, dup = k == juce::KeyPress ('d', juce::ModifierKeys::ctrlModifier, 0);
        if ((del || dup) && ! recording && songView.selRegion >= 0) // the selected audio region
        {
            songView.edit (songView.selTrack, songView.selRegion, del ? SongView::deleteIt : SongView::duplicateIt, 0.0);
            return true;
        }
        return false;
    }

    // ---- Song files (.lmsong in Documents\LocalMIDIStudio\Songs) ----
    static juce::File songsFolder() { return takesFolder().getChildFile ("Songs"); }

    static juce::File audioFolder (const juce::File& songDir) { return songDir.getChildFile ("Audio"); } // voices live here, not among the songs

    bool writeSong (const juce::File& f) // the song, and each track's voice as Audio\<name>-trackN.wav
    {
        audioFolder (f.getParentDirectory()).createDirectory();
        const auto stem = f.getFileNameWithoutExtension();
        bool ok = true;
        for (int i = 0; i < song.count; ++i)
            if (const auto& a = song.tracks[(size_t) i]->audio; ! a.empty()) // the whole file: trimming stays undoable after reopening
                ok &= writeWav (a.buf, song.rate, audioFolder (f.getParentDirectory()).getChildFile (SongFile::audioName (stem, i)));
        return f.replaceWithText (SongFile::toJson (song, instrumentFiles, true, stem)) && ok;
    }

    // An audio file (WAV, MP3, FLAC, Ogg, AIFF; mono or the first 2 channels), converted to the current sample rate.
    // Empty if missing, unreadable or longer than maxVoiceSeconds (logged).
    juce::AudioBuffer<float> loadAudio (const juce::File& f, int track)
    {
        auto out = readAudioFile (f, song.rate, maxVoiceSeconds);
        if (out.getNumSamples() == 0)
            log ("Track " + juce::String (track + 1) + ": " + f.getFileName() + " missing, unreadable or longer than " + juce::String ((int) maxVoiceSeconds / 60) + " min, left out.");
        return out;
    }

    // A file dropped on the song: becomes `track`'s audio from `seconds` (track == count: a new track). Replaces what
    // the track had (its file stays on disk).
    // Runs `change` with the transport stopped (the audio thread then reads no track buffer or region list), then
    // resumes playing from the same place if it was playing.
    template <typename F> auto whileStopped (F change)
    {
        const bool wasPlaying = playing;
        const double pos = song.position.load();
        stopTransport();
        auto result = change();
        if (wasPlaying && pos >= 0.0) { song.recordTrack = -1; song.startPos = std::llround (pos * currentRate); song.run = true; playing = true; }
        songView.repaint();
        return result;
    }

    static constexpr double maxTrackAudioSeconds = 30 * 60.0; // shortcut: all files on one track together, in memory; raise if needed

    // A file dropped on the song: added to `track` from `seconds` (track == count: a new track). Returns its region or -1.
    int dropAudio (const juce::File& f, int track, double seconds)
    {
        if (recording) { log ("Stop recording first."); return -1; }
        if (track >= song.count)
        {
            if (song.count >= Song::maxTracks) { log ("All " + juce::String (Song::maxTracks) + " tracks are in use: drop it on a track."); return -1; }
            track = song.count;
        }
        auto& a = song.tracks[(size_t) track]->audio;
        if (a.count >= AudioClip::maxRegions) { log ("Track " + juce::String (track + 1) + " has " + juce::String (AudioClip::maxRegions) + " audio regions already: use another track."); return -1; }
        auto buf = loadAudio (f, track);
        if (buf.getNumSamples() == 0) return -1;
        if (a.buf.getNumSamples() + buf.getNumSamples() > maxTrackAudioSeconds * song.rate) { log ("Track " + juce::String (track + 1) + " is full (30 min of audio): use another track."); return -1; }
        const int k = whileStopped ([&]
        {
            if (track == song.count) ++song.count; // engines exist already: nothing is allocated for the audio thread
            return a.add (buf, std::llround (juce::jmax (0.0, seconds) * song.rate));
        });
        selectTrack (track);
        log ("Track " + juce::String (track + 1) + ": " + f.getFileName() + " at " + juce::String (seconds, 2) + " s (" + juce::String (buf.getNumSamples() / song.rate, 1)
             + " s long). Drag it to move, drag its edges to trim, right-click to split or duplicate, Ctrl+wheel to zoom.");
        return k;
    }

    void deleteTrack (int i)
    {
        if (recording || i <= drumTrack || i >= song.count) return;
        whileStopped ([&]
        {
            const juce::ScopedLock sl (deviceManager.getAudioCallbackLock()); // the live keys still render while stopped: no block mid-shuffle
            song.removeTrack (i);
            std::rotate (instrumentFiles.begin() + i, instrumentFiles.begin() + i + 1, instrumentFiles.end());
            instrumentFiles.back() = juce::File();
            return 0;
        });
        history.clear(); // its steps name tracks by number, which just changed
        songView.selTrack = songView.selRegion = -1;
        selectTrack (juce::jmin (i, song.count - 1));
        log ("Track " + juce::String (i + 1) + " deleted (Undo history cleared).");
    }

    void clearTrack (int i)
    {
        if (recording) return;
        setClip (i, {}); // undoable
        whileStopped ([&] { song.tracks[(size_t) i]->audio.clear(); return 0; });
        songView.selTrack = songView.selRegion = -1;
        log ("Track " + juce::String (i + 1) + " cleared (Undo brings back the notes; the audio files are still on disk).");
    }

    // Split / duplicate / delete / untrim a region (SongView::Edit). Returns the region to select afterwards (-1: none).
    int editRegion (int track, int region, int what, double seconds)
    {
        auto& a = song.tracks[(size_t) track]->audio;
        auto& r = a.regions[(size_t) region];
        const auto at = std::llround (seconds * song.rate);
        const juce::String name = "Track " + juce::String (track + 1) + ": ";
        if (what == SongView::untrim) // not structural: only the region's own atomics change
        {
            r.start = juce::jmax<juce::int64> (0, r.start.load() - (r.in.load() - r.lo));
            r.in = r.lo; r.out = r.hi;
            a.refreshFades();
            songView.repaint();
            return region;
        }
        if (what != SongView::deleteIt && a.count >= AudioClip::maxRegions) { log (name + "already " + juce::String (AudioClip::maxRegions) + " audio regions (the most a track holds)."); return region; }
        const int k = whileStopped ([&]
        {
            if (what == SongView::splitHere) return a.split (region, at);
            if (what == SongView::duplicateIt) return a.duplicate (region, r.end()); // right after it: ready to loop
            a.remove (region);
            return -1;
        });
        if (what == SongView::splitHere && k < 0) log (name + "split it inside the region, not at its edge.");
        if (what == SongView::deleteIt) log (name + "audio region deleted (its file is still on disk).");
        return what == SongView::splitHere && k < 0 ? region : k;
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

    // ---- Default setup: Documents\LocalMIDIStudio\Songs\default.lmsong, a song saved without notes ----
    static juce::File defaultSongFile() { return songsFolder().getChildFile ("default.lmsong"); }

    void saveDefault()
    {
        stopAll();
        defaultSongFile().getParentDirectory().createDirectory();
        log (defaultSongFile().replaceWithText (SongFile::toJson (song, instrumentFiles, false))
                 ? "Default saved: New song now starts from these sounds, effects and tracks."
                 : "ERROR: could not save " + defaultSongFile().getFullPathName());
    }

    void chooseNewSong()
    {
        if (song.length() <= 0.0) { newSong(); return; }
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New song",
                                            "Notes not saved with Save song are lost. Continue?", "New song", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([safe = SafePointer<MainComponent> (this)] (int ok)
                                            { if (ok == 1 && safe != nullptr) safe->newSong(); }));
    }

    void newSong()
    {
        SongFile::Loaded l;
        const bool mine = defaultSongFile().existsAsFile() && SongFile::parse (defaultSongFile().loadFileAsString(), l).isEmpty();
        if (! mine)
        {
            if (defaultSongFile().existsAsFile()) log ("default.lmsong is not valid: using the built-in default.");
            l = SongFile::Loaded();
            SongFile::parse (R"({"format":"LocalMIDIStudio.song","version":1,"tracks":[{},{}]})", l);
        }
        applySong (l);
        for (auto& t : song.tracks) t->clip.clear(); // a hand-edited default may contain notes
        if (! mine) loadDefaultKit();
        songFile = juce::File(); // Save song asks for a new name
        songView.repaint();
        log (mine ? "New song from your default." : "New song from the built-in default (use Set default to choose your own).");
    }

    void openSong (const juce::File& f)
    {
        SongFile::Loaded l;
        if (const auto err = SongFile::parse (f.loadFileAsString(), l); err.isNotEmpty()) { log ("Song not opened (" + err + "): the current song is unchanged."); return; }
        applySong (l, f.getParentDirectory());
        songFile = f;
        log ("Song opened: " + f.getFullPathName() + " (" + juce::String ((int) l.tracks.size()) + " tracks)");
    }

    void applySong (const SongFile::Loaded& l, const juce::File& folder = {}) // folder: where the song's voice WAVs are
    {
        stopAll(); // the audio thread doesn't read clips once stopped
        history.clear(); // an Undo would bring back a clip of the previous song
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
            auto wav = audioFolder (folder).getChildFile (src != nullptr ? src->audio : juce::String());
            if (! wav.existsAsFile()) wav = folder.getChildFile (wav.getFileName()); // older songs: next to the song
            t.audio.clear();
            if (src != nullptr && src->audio.isNotEmpty() && folder != juce::File())
                if (const auto pool = loadAudio (wav, i); pool.getNumSamples() > 0)
                {
                    t.audio.append (pool);
                    const int len = pool.getNumSamples();
                    auto smp = [&] (double sec) { return sec < 0.0 ? len : (int) juce::jmin<juce::int64> (len, std::llround (sec * song.rate)); }; // -1: the end
                    for (const auto& r : src->regions) t.audio.addRegion (std::llround (r[0] * song.rate), smp (r[1]), smp (r[2]), smp (r[3]), smp (r[4]));
                }
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
        masterVolume.setValue (l.master); // sends it to the song
        drumVolume.setValue (drums().params.gain.load(), juce::dontSendNotification);
        selectTrack (juce::jmin (l.selected, song.count - 1));
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
        history.clear();
        for (auto& t : song.tracks) { t->clip.clear(); t->audio.clear(); } // transport stopped: the audio thread doesn't read them now
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

    // What the selected track's synth sound looks like: two cycles of osc 1 mixed with osc 2, and the envelope.
    struct ShapeView : juce::Component
    {
        void check() // repaint only when a sound parameter changed
        {
            std::vector<double> v;
            params().forEach ([&] (const char*, auto& x, auto, auto) { v.push_back ((double) x.load()); });
            if (v != shown) { shown = std::move (v); repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto& p = params();
            auto r = getLocalBounds().toFloat();
            auto wr = r.removeFromLeft (r.getWidth() * 0.55f).reduced (2, 6), er = r.reduced (6, 6);
            g.setColour (juce::Colour (0xff1b1f24));
            g.fillRoundedRectangle (wr, 4.0f);
            g.fillRoundedRectangle (er, 4.0f);

            const double dt = 110.0 / 48000.0, ratio = std::pow (2.0, (p.osc2Tune + p.osc2Fine / 100.0) / 12.0);
            const float m = p.osc2Mix, width = p.pulseWidth;
            juce::Random rng (1);
            auto sample = [&] (int w, double t, double d) { return w == 4 ? rng.nextFloat() * 2.0f - 1.0f : Voice::shape (w, t - std::floor (t), d, width); };
            juce::Path wave;
            for (int x = 0; x <= (int) wr.getWidth(); ++x)
            {
                const double t = 2.0 * x / wr.getWidth(); // two cycles of osc 1
                const float v = (1.0f - m) * sample (p.wave, t, dt) + (m > 0 ? m * sample (p.osc2Wave, t * ratio, dt * ratio) : 0.0f);
                const float y = wr.getCentreY() - juce::jlimit (-1.0f, 1.0f, v / 1.25f) * wr.getHeight() * 0.45f; // 1.25: band-limited overshoot
                if (x == 0) wave.startNewSubPath (wr.getX() + x, y); else wave.lineTo (wr.getX() + x, y);
            }
            g.setColour (juce::Colour (0xfff0a030));
            g.strokePath (wave, juce::PathStrokeType (1.5f));

            const float a = p.attack, d = p.decay, s = p.sustain, rel = p.release, hold = 0.3f * (a + d + rel) + 0.05f;
            const float k = er.getWidth() / (a + d + hold + rel);
            auto at = [&] (float time, float level) { return juce::Point<float> (er.getX() + time * k, er.getBottom() - level * er.getHeight() * 0.9f); };
            juce::Path env;
            env.startNewSubPath (at (0, 0));
            env.lineTo (at (a, 1)); env.lineTo (at (a + d, s)); env.lineTo (at (a + d + hold, s)); env.lineTo (at (a + d + hold + rel, 0));
            g.setColour (juce::Colour (0xff5ec27a));
            g.strokePath (env, juce::PathStrokeType (1.5f));
        }

        std::function<SynthParams&()> params;
        std::vector<double> shown;
    };

    // Step sequencer: a row per pad (its sound), 16 sixteenth notes of one bar; click a square to turn it on or off.
    struct StepGrid : juce::Component
    {
        static constexpr int label = 78;
        juce::Rectangle<float> cell (int r, int s) const
        {
            const float w = (float) (getWidth() - label) / Steps::steps, h = (float) getHeight() / Steps::rows;
            return { (float) label + (float) s * w, (float) r * h, w, h };
        }

        void paint (juce::Graphics& g) override
        {
            g.setFont (juce::Font (12.0f));
            for (int r = 0; r < Steps::rows; ++r)
            {
                g.setColour (juce::Colours::lightgrey);
                g.drawFittedText (nameOf ? nameOf (r) : juce::String(), juce::Rectangle<float> (0, cell (r, 0).getY(), (float) label - 4, cell (r, 0).getHeight()).toNearestInt(),
                                  juce::Justification::centredLeft, 1, 0.8f);
                for (int s = 0; s < Steps::steps; ++s)
                {
                    const auto c = pattern[(size_t) r][(size_t) s] ? juce::Colour (0xffe8892b)
                                 : juce::Colour ((s / 4) % 2 == 0 ? 0xff2a3138 : 0xff343d46); // beats in alternating shades
                    g.setColour (s == playStep ? c.brighter (0.5f) : c);
                    g.fillRoundedRectangle (cell (r, s).reduced (1.5f), 3.0f);
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            for (int r = 0; r < Steps::rows; ++r)
                for (int s = 0; s < Steps::steps; ++s)
                    if (cell (r, s).contains (e.position))
                    {
                        pattern[(size_t) r][(size_t) s] = ! pattern[(size_t) r][(size_t) s];
                        repaint();
                        if (onChange) onChange();
                        return;
                    }
        }

        Steps::Pattern pattern {};
        int playStep = -1;
        std::function<juce::String (int)> nameOf;
        std::function<void()> onChange;
    };

    // The grid or its bars changed: the drum clip becomes the pattern repeated over the bars.
    void writeSteps()
    {
        auto& clip = song.tracks[(size_t) drumTrack]->clip;
        if (recording) { log ("Stop recording before editing the steps."); refreshSteps(); return; }
        if (clip.getNumEvents() > 0 && ! Steps::same (clip, stepsWritten)) // recorded or opened drums: never replaced silently
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Step sequencer",
                                                "The drum track has notes that were recorded or opened. Replace them with this pattern?", "Replace", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([safe = SafePointer<MainComponent> (this)] (int ok)
            {
                if (safe == nullptr) return;
                if (ok != 1) { safe->refreshSteps(); return; }
                safe->stepsWritten = safe->song.tracks[(size_t) drumTrack]->clip; // the grid owns the clip now
                safe->writeSteps();
            }));
            return;
        }
        stepsWritten = Steps::render (steps.pattern, pads.sounds, song.barSeconds(), (int) stepBars.getValue());
        setClip (drumTrack, stepsWritten);
    }

    void refreshSteps() // the grid shows the drum clip's first bar (a recorded take: rounded to the steps)
    {
        const auto& clip = song.tracks[(size_t) drumTrack]->clip;
        steps.pattern = Steps::read (clip, pads.sounds, song.barSeconds());
        if (clip.getNumEvents() > 0) stepBars.setValue (juce::jlimit (1.0, 64.0, std::ceil (clip.getEndTime() / song.barSeconds())), juce::dontSendNotification);
        steps.repaint();
    }

    bool stepsOwnClip() { const auto& c = song.tracks[(size_t) drumTrack]->clip; return c.getNumEvents() > 0 && Steps::same (c, stepsWritten); }

    static juce::File padsFile() { return settingsFile().getSiblingFile ("pads.json"); }

    void rebuildRouter() // pads' (channel, note) -> their kit sound, read by the audio thread
    {
        router.clear();
        for (int i = 0; i < 16; ++i) router.set (pads.channels[(size_t) i], pads.notes[(size_t) i], pads.sounds[(size_t) i]);
        pads.repaint();
        if (stepsOwnClip()) writeSteps(); else refreshSteps(); // a pad's new sound: its row keeps its steps
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
    // Tracks over time. Ctrl+wheel zooms around the mouse, wheel scrolls in time, double-click empty space fits the song.
    // An audio file dropped on a track (or below the last one, for a new track) is added there as a region: drag it to
    // move it (Shift: snap to 1/16), drag its edges to trim it, right-click it to split, duplicate, delete or untrim.
    struct SongView : juce::Component, juce::FileDragAndDropTarget
    {
        SongView (Song& s, const juce::MidiMessageSequence& t, juce::CriticalSection& l) : song (s), take (t), takeLock (l) {}

        static constexpr int header = 170, rulerH = 14;
        enum Edit { splitHere = 1, duplicateIt, deleteIt, untrim };

        double fitLen() const { return juce::jmax (8.0, song.length() + 1.0, playhead + 1.0); }
        double visible() const { return viewSecs > 0.0 ? viewSecs : drag.track >= 0 ? drag.len : fitLen(); } // fit is frozen while dragging
        float contentW() const { return juce::jmax (1.0f, (float) getWidth() - header); }
        float xOf (double t) const { return (float) header + (float) ((t - viewStart) / visible()) * contentW(); }
        double timeAt (float x) const { return viewStart + (x - (float) header) / contentW() * visible(); }
        float rowH() const { return (float) (getHeight() - rulerH) / (float) juce::jmax (1, song.count.load()); }
        float rowY (int i) const { return (float) rulerH + i * rowH(); }
        int rowAt (float y) const { return (int) std::floor ((y - (float) rulerH) / rowH()); }
        juce::Rectangle<float> muteBox (int i) const { return { (float) header - 54, rowY (i) + 4, 22, juce::jmin (22.0f, rowH() - 8) }; }
        juce::Rectangle<float> soloBox (int i) const { return muteBox (i).translated (26, 0); }
        juce::Rectangle<float> regionRect (int i, int k) const
        {
            const auto& r = song.tracks[(size_t) i]->audio.regions[(size_t) k];
            const float x0 = xOf ((double) r.start.load() / song.rate);
            return { x0, rowY (i) + 2, juce::jmax (4.0f, xOf ((double) r.end() / song.rate) - x0), rowH() - 4 };
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff1d2126));
            const int n = song.count;
            static const juce::uint32 palette[] { 0xfff0a030, 0xffe8687a, 0xff5ec27a, 0xff3fa7d6, 0xffb07ee8, 0xffe8c547, 0xff4fc9b0, 0xffd67ad0 };
            { // bars (and beats when there is room), numbered in the ruler
                const double bar = song.barSeconds(), beat = song.beatSeconds();
                const bool beats = (double) contentW() / visible() * beat > 18.0;
                const int every = juce::jmax (1, (int) std::ceil (40.0 / ((double) contentW() / visible() * bar))); // a number at least every 40 px
                for (int b = juce::jmax (0, (int) std::floor (viewStart / bar)); b * bar < viewStart + visible(); ++b)
                    for (int q = 0; q < (beats ? 4 : 1); ++q)
                    {
                        const float x = xOf (b * bar + q * beat);
                        if (x < (float) header) continue;
                        g.setColour (juce::Colour (q == 0 ? 0xff3e4750 : 0xff2a3037));
                        g.drawVerticalLine ((int) x, (float) rulerH, (float) getHeight());
                        if (q == 0 && b % every == 0)
                        {
                            g.setColour (juce::Colours::grey);
                            g.setFont (juce::Font (10.0f));
                            g.drawText (juce::String (b + 1), (int) x + 2, 0, 40, rulerH, juce::Justification::centredLeft);
                        }
                    }
            }
            for (int i = 0; i < n; ++i)
            {
                const auto row = juce::Rectangle<float> (0, rowY (i), (float) getWidth(), rowH());
                auto& t = *song.tracks[(size_t) i];
                const juce::Colour colour (palette[i % 8]);
                if (i == song.selected) { g.setColour (colour.withAlpha (0.14f)); g.fillRect (row); }
                g.setColour (juce::Colour (0xff3a434b));
                g.drawHorizontalLine ((int) row.getBottom() - 1, 0, (float) getWidth());
                {
                    juce::Graphics::ScopedSaveState clip (g);
                    g.reduceClipRegion (row.withTrimmedLeft ((float) header).toNearestInt());
                    for (int k = 0; k < t.audio.count; ++k) // audio regions and their waveforms (the part heard)
                    {
                        const auto r = regionRect (i, k);
                        if (r.getRight() < (float) header || r.getX() > (float) getWidth()) continue;
                        const auto& reg = t.audio.regions[(size_t) k];
                        g.setColour (colour.withAlpha (0.22f));
                        g.fillRoundedRectangle (r, 3.0f);
                        g.setColour (colour.withAlpha (0.8f));
                        const double perPx = (double) (reg.out.load() - reg.in.load()) / juce::jmax (1.0f, r.getWidth()) / AudioClip::peakBlock;
                        for (int px = juce::jmax (0, (int) ((float) header - r.getX())); px < (int) r.getWidth() && r.getX() + px < (float) getWidth(); ++px)
                        {
                            const auto b0 = (size_t) (reg.in.load() / AudioClip::peakBlock + px * perPx), b1 = juce::jmin (t.audio.peaks.size(), b0 + (size_t) juce::jmax (1.0, perPx));
                            float pk = 0;
                            for (auto b = b0; b < b1; ++b) pk = juce::jmax (pk, t.audio.peaks[b]);
                            const float h = juce::jmin (1.0f, pk) * (r.getHeight() - 4) * 0.5f;
                            g.drawVerticalLine ((int) r.getX() + px, r.getCentreY() - h, r.getCentreY() + h + 1);
                        }
                        const bool sel = i == selTrack && k == selRegion;
                        g.setColour (sel ? juce::Colours::white : colour.withAlpha (0.8f));
                        g.drawRoundedRectangle (r, 3.0f, sel ? 2.0f : 1.0f);
                    }
                    // notes: this track's clip, or the take being recorded on it
                    const bool rec = song.recordTrack == i;
                    const juce::ScopedLock sl (takeLock);
                    const auto& seq = rec ? take : t.clip;
                    int lo = 127, hi = 0;
                    for (auto* e : seq) if (e->message.isNoteOn()) { lo = juce::jmin (lo, e->message.getNoteNumber()); hi = juce::jmax (hi, e->message.getNoteNumber()); }
                    const float nh = (row.getHeight() - 6) / (float) juce::jmax (1, hi - lo + 1);
                    for (int k = 0; lo <= hi && k < seq.getNumEvents(); ++k)
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
                g.setColour (juce::Colour (0xff1d2126)); // the header on top of whatever is scrolled under it
                g.fillRect (row.withWidth ((float) header).withTrimmedBottom (1));
                if (i == song.selected) { g.setColour (colour.withAlpha (0.14f)); g.fillRect (row.withWidth ((float) header).withTrimmedBottom (1)); }
                g.setColour (colour);
                g.fillRect (row.withWidth (5.0f).reduced (0, 2)); // track colour stripe
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
            }
            if (playhead > 0.0 && xOf (playhead) >= (float) header) { g.setColour (juce::Colours::white); g.drawVerticalLine ((int) xOf (playhead), 0, (float) getHeight()); }
        }

        void setPlayhead (double t) // keeps it in view while zoomed in
        {
            playhead = t;
            if (t >= 0.0 && viewSecs > 0.0 && drag.track < 0 && (t > viewStart + viewSecs || t < viewStart)) viewStart = juce::jmax (0.0, t - 0.1 * viewSecs);
        }

        enum class Grab { none, move, left, right };
        std::pair<int, Grab> grabAt (int i, juce::Point<float> p) const // region under p (the topmost) and which part of it
        {
            if (i < 0 || i >= song.count || p.x < (float) header) return { -1, Grab::none };
            for (int k = song.tracks[(size_t) i]->audio.count; --k >= 0;)
                if (const auto r = regionRect (i, k); r.expanded (4, 0).contains (p))
                    return { k, p.x < r.getX() + 6 ? Grab::left : p.x > r.getRight() - 6 ? Grab::right : Grab::move };
            return { -1, Grab::none };
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const auto g = grabAt (rowAt (e.position.y), e.position).second;
            setMouseCursor (g == Grab::move ? juce::MouseCursor::DraggingHandCursor : g == Grab::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor);
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            const float delta = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
            if (delta == 0.0f) return;
            const double secs = visible();
            if (e.mods.isCtrlDown()) // zoom, keeping the time under the mouse where it is
            {
                const double at = timeAt (e.position.x), next = juce::jlimit (0.25, 3600.0, secs * (delta > 0 ? 0.8 : 1.25));
                viewStart = juce::jmax (0.0, at - (e.position.x - (float) header) / contentW() * next);
                viewSecs = next;
            }
            else { viewSecs = secs; viewStart = juce::jmax (0.0, viewStart - delta * secs * 0.25); }
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (e.position.x >= (float) header && grabAt (rowAt (e.position.y), e.position).first < 0) { viewSecs = 0.0; viewStart = 0.0; repaint(); }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = rowAt (e.position.y);
            if (i < 0 || i >= song.count) return;
            auto& t = *song.tracks[(size_t) i];
            if (muteBox (i).contains (e.position)) t.mute = ! t.mute;
            else if (soloBox (i).contains (e.position)) t.solo = ! t.solo;
            else if (song.recordTrack < 0 && song.voiceTrack < 0) // not while recording: the take belongs to one track
            {
                if (onSelect) onSelect (i);
                if (e.position.x < (float) header && e.mods.isPopupMenu()) { if (onTrackMenu) onTrackMenu (i); return; }
                const auto [k, g] = grabAt (i, e.position);
                selTrack = i; selRegion = k;
                if (k >= 0 && e.mods.isPopupMenu())
                {
                    const auto& r = t.audio.regions[(size_t) k];
                    const double at = timeAt (e.position.x);
                    juce::PopupMenu m;
                    m.addItem (splitHere, "Split here");
                    m.addItem (duplicateIt, "Duplicate (Ctrl+D)");
                    m.addItem (deleteIt, "Delete (Del)");
                    m.addItem (untrim, "Untrim (whole file)", r.in.load() > r.lo || r.out.load() < r.hi);
                    m.showMenuAsync (juce::PopupMenu::Options(), [this, i, k, at] (int item) { if (item > 0) edit (i, k, item, at); });
                }
                else if (k >= 0)
                {
                    const auto& r = t.audio.regions[(size_t) k];
                    drag = { i, k, g, visible(), r.start.load(), r.in.load(), r.out.load() };
                }
            }
            repaint();
        }

        void edit (int track, int region, int what, double at) // via the owner: structural edits need the transport stopped
        {
            if (onRegionEdit && track < song.count && region >= 0 && region < song.tracks[(size_t) track]->audio.count)
            { selTrack = track; selRegion = onRegionEdit (track, region, what, at); }
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (drag.track < 0) return;
            auto& a = song.tracks[(size_t) drag.track]->audio;
            auto& r = a.regions[(size_t) drag.region];
            auto d = std::llround ((timeAt (e.position.x) - timeAt (e.mouseDownPosition.x)) * song.rate); // samples
            const int minLen = (int) (0.01 * song.rate);
            if (drag.grab == Grab::move)
            {
                auto s = juce::jmax<juce::int64> (0, drag.start + d);
                if (e.mods.isShiftDown()) { const double step = song.beatSeconds() / 4 * song.rate; s = std::llround (std::round ((double) s / step) * step); }
                r.start = s;
            }
            else if (drag.grab == Grab::left) // the start moves with the cut, so what stays keeps its place in the song
            {
                d = juce::jlimit<juce::int64> (juce::jmax<juce::int64> (r.lo - drag.in, -drag.start), drag.out - minLen - drag.in, d);
                r.in = drag.in + (int) d;
                r.start = drag.start + d;
            }
            else r.out = juce::jlimit (drag.in + minLen, r.hi, drag.out + (int) d);
            a.refreshFades();
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override { drag.track = -1; repaint(); }

        bool isInterestedInFileDrag (const juce::StringArray& files) override
        {
            for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;mp3;flac;ogg;aif;aiff")) return true;
            return false;
        }
        void filesDropped (const juce::StringArray& files, int x, int y) override
        {
            for (auto& f : files)
                if (juce::File (f).hasFileExtension ("wav;mp3;flac;ogg;aif;aiff") && onDropAudio)
                {
                    const int track = juce::jlimit (0, song.count.load(), rowAt ((float) y));
                    if (const int k = onDropAudio (juce::File (f), track, juce::jmax (0.0, timeAt ((float) x))); k >= 0) { selTrack = song.selected; selRegion = k; }
                    repaint();
                    return; // one file per drop
                }
        }

        Song& song;
        const juce::MidiMessageSequence& take;
        juce::CriticalSection& takeLock;
        std::function<juce::String (int)> trackName;
        std::function<void (int)> onSelect;
        std::function<int (const juce::File&, int, double)> onDropAudio; // file, track (count = a new one), seconds -> new region or -1
        std::function<int (int, int, int, double)> onRegionEdit;         // track, region, Edit, seconds -> region to select (-1 none)
        std::function<void (int)> onTrackMenu;                            // right-click on a track's name
        double playhead = -1.0, viewStart = 0.0, viewSecs = 0.0;         // viewSecs 0: the whole song fits
        int selTrack = -1, selRegion = -1;
        struct Drag { int track = -1, region = -1; Grab grab = Grab::none; double len = 0; juce::int64 start = 0; int in = 0, out = 0; } drag;
    };

    // Piano roll of the selected track: click empty space to add a note (drag to make it longer), drag a note to move it,
    // drag its right edge to change its length, right-click to delete. Wheel: notes up/down; Shift+wheel: time; Ctrl+wheel: zoom.
    struct PianoRoll : juce::Component
    {
        static constexpr int rowH = 12;
        float x (double t) const { return (float) keysW + (float) ((t - viewStart) * pxPerSec); }
        double timeAt (float px) const { return viewStart + (px - (float) keysW) / pxPerSec; }
        int noteAt (float py) const { return juce::jlimit (0, 127, topNote - (int) std::floor (py / rowH)); }
        double snap (double t) const { return std::round (t / grid) * grid; }
        juce::Rectangle<float> rect (const Notes::Note& n) const
        {
            return { x (n.start), (float) ((topNote - n.note) * rowH), juce::jmax (3.0f, (float) ((n.end - n.start) * pxPerSec)), (float) rowH };
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff1d2126));
            for (int n = topNote, y = 0; n >= 0 && y < getHeight(); --n, y += rowH)
            {
                const bool black = juce::MidiMessage::isMidiNoteBlack (n);
                g.setColour (black ? juce::Colour (0xff181b1f) : juce::Colour (0xff23282e));
                g.fillRect (keysW, y, getWidth() - keysW, rowH);
                g.setColour (black ? juce::Colour (0xff30363d) : juce::Colour (0xffc8cdd2));
                g.fillRect (0, y, keysW - 2, rowH - 1);
                if (nameOf || n % 12 == 0)
                {
                    g.setColour (black ? juce::Colours::white : juce::Colours::black);
                    g.setFont (juce::Font (10.0f));
                    g.drawText (nameOf ? nameOf (n) : juce::MidiMessage::getMidiNoteName (n, true, true, 3), 2, y, keysW - 6, rowH, juce::Justification::centredLeft, true);
                }
            }
            const int first = (int) std::floor (viewStart / grid);
            for (int s = juce::jmax (0, first); x (s * grid) < (float) getWidth(); ++s) // steps, beats, bars
            {
                g.setColour (juce::Colour (s % 16 == 0 ? 0xff6a7580 : s % 4 == 0 ? 0xff3e4750 : 0xff2b3138));
                g.drawVerticalLine ((int) x (s * grid), 0.0f, (float) getHeight());
            }
            for (auto& n : notes)
            {
                const auto r = rect (n);
                if (r.getRight() < (float) keysW || r.getX() > (float) getWidth()) continue;
                g.setColour (colour.withAlpha (0.45f + 0.55f * (float) n.velocity / 127.0f));
                g.fillRoundedRectangle (r.reduced (0.5f), 2.0f);
                g.setColour (colour.brighter (0.6f));
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
            }
            if (playhead >= 0.0) { g.setColour (juce::Colours::white); g.drawVerticalLine ((int) x (playhead), 0.0f, (float) getHeight()); }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.position.x < (float) keysW) return;
            int hit = -1;
            for (int i = (int) notes.size(); --i >= 0;) if (rect (notes[(size_t) i]).contains (e.position)) { hit = i; break; }
            if (e.mods.isPopupMenu())
            {
                if (hit >= 0) { notes.erase (notes.begin() + hit); repaint(); if (onCommit) onCommit(); }
                return;
            }
            if (hit < 0) // a new note, one step long, on the step under the mouse
            {
                const double t = juce::jmax (0.0, std::floor (timeAt (e.position.x) / grid) * grid);
                notes.push_back ({ t, t + grid, noteAt (e.position.y), 100, channel });
                hit = (int) notes.size() - 1;
                before = {};
                resizing = true;
            }
            else
            {
                before = notes[(size_t) hit];
                resizing = e.position.x > rect (notes[(size_t) hit]).getRight() - 6.0f;
            }
            dragging = hit;
            grab = timeAt (e.position.x) - notes[(size_t) hit].start;
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0) return;
            auto& n = notes[(size_t) dragging];
            if (resizing) n.end = juce::jmax (n.start + grid, snap (timeAt (e.position.x)));
            else
            {
                const double len = n.end - n.start;
                n.start = juce::jmax (0.0, snap (timeAt (e.position.x) - grab));
                n.end = n.start + len;
                n.note = noteAt (e.position.y);
            }
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging < 0) return;
            const auto& n = notes[(size_t) dragging];
            const bool changed = ! before || before->start != n.start || before->end != n.end || before->note != n.note;
            dragging = -1;
            if (changed && onCommit) onCommit();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (e.mods.isCtrlDown()) pxPerSec = juce::jlimit (10.0, 4000.0, pxPerSec * (w.deltaY > 0 ? 1.25 : 0.8));
            else if (e.mods.isShiftDown() || w.deltaX != 0.0f)
                viewStart = juce::jmax (0.0, viewStart - (w.deltaX != 0.0f ? w.deltaX : w.deltaY) * 300.0 / pxPerSec);
            else topNote = juce::jlimit (getHeight() / rowH, 127, topNote + (w.deltaY > 0 ? 2 : -2));
            repaint();
        }

        std::vector<Notes::Note> notes;
        std::optional<Notes::Note> before; // the dragged note as it was (none: a new note)
        double grid = 0.125, viewStart = 0.0, pxPerSec = 100.0, playhead = -1.0, grab = 0.0;
        int topNote = 84, channel = 1, keysW = 44, dragging = -1;
        bool resizing = false;
        juce::Colour colour { 0xfff0a030 };
        std::function<juce::String (int)> nameOf; // drum track: the kit's sound names on the keys
        std::function<void()> onCommit;           // the notes changed (mouse up / delete)
    };

    void loadRoll (bool resetView = true) // the piano roll shows the selected track's clip
    {
        static const juce::uint32 palette[] { 0xfff0a030, 0xffe8687a, 0xff5ec27a, 0xff3fa7d6, 0xffb07ee8, 0xffe8c547, 0xff4fc9b0, 0xffd67ad0 };
        const int t = song.selected;
        const bool drumsSel = t == drumTrack;
        roll.notes = Notes::read (song.tracks[(size_t) t]->clip);
        roll.grid = song.barSeconds() / 16.0;
        roll.channel = drumsSel ? 10 : 1;
        roll.colour = juce::Colour (palette[t % 8]);
        roll.keysW = drumsSel ? 96 : 44;
        roll.nameOf = drumsSel ? std::function<juce::String (int)> ([this] (int n) { return pads.nameOf (n); }) : nullptr;
        if (! resetView) { roll.repaint(); return; } // after an edit the view stays where the user put it
        if (! roll.notes.empty())
        {
            int hi = 0;
            for (auto& n : roll.notes) hi = juce::jmax (hi, n.note);
            roll.topNote = juce::jlimit (roll.getHeight() / PianoRoll::rowH, 127, hi + 3);
        }
        roll.pxPerSec = juce::jmax (10.0, (roll.getWidth() - roll.keysW) / (4.0 * song.barSeconds())); // four bars across
        roll.viewStart = 0.0;
        roll.repaint();
    }

    void commitRoll() // piano-roll edits go into the selected track
    {
        if (recording) { log ("Stop recording before editing notes."); loadRoll(); return; }
        const int t = song.selected;
        setClip (t, Notes::write (roll.notes, song.tracks[(size_t) t]->clip));
    }

    void quantizeTrack()
    {
        if (recording) { log ("Stop recording before quantizing."); return; }
        const int t = song.selected;
        auto notes = Notes::read (song.tracks[(size_t) t]->clip);
        if (notes.empty()) { log ("Track " + juce::String (t + 1) + " has no notes to quantize."); return; }
        Notes::quantize (notes, song.barSeconds() / 16.0);
        setClip (t, Notes::write (notes, song.tracks[(size_t) t]->clip));
        log ("Track " + juce::String (t + 1) + ": " + juce::String ((int) notes.size()) + " notes moved to the nearest 1/16 (Undo to go back).");
    }

    // Replaces a track's notes (undoable); while playing, the song carries on from where it was.
    void setClip (int track, const juce::MidiMessageSequence& next, bool undoable = true)
    {
        const bool wasPlaying = playing;
        const double pos = song.position.load();
        stopTransport(); // the audio thread doesn't read clips once stopped
        if (undoable) history.changed (track, song.tracks[(size_t) track]->clip);
        song.tracks[(size_t) track]->clip = next;
        if (wasPlaying && pos >= 0.0) { song.recordTrack = -1; song.startPos = std::llround (pos * currentRate); song.run = true; playing = true; }
        songView.repaint();
        loadRoll (false);
        if (track == drumTrack) refreshSteps();
    }

    void undo (bool redo)
    {
        if (recording) { log ("Stop recording first."); return; }
        Notes::History::Step s;
        const auto current = [this] (int t) { return song.tracks[(size_t) t]->clip; };
        if (! (redo ? history.redo (current, s) : history.undo (current, s))) { log (redo ? "Nothing to redo." : "Nothing to undo."); return; }
        setClip (s.track, s.clip, false);
        log ((redo ? "Redo" : "Undo") + juce::String (" on track ") + juce::String (s.track + 1) + " (" + juce::String ((int) history.undos.size()) + " undo, "
             + juce::String ((int) history.redos.size()) + " redo left).");
    }

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
             + (inst != nullptr ? static_cast<InstrumentSound*> (inst.get())->name : juce::String ("Synth"))
             + (! song.tracks[(size_t) i]->audio.empty() ? " + audio" : "");
    }

    void selectTrack (int i)
    {
        song.selected = i;
        for (auto& r : refreshers) r(); // knobs and menus now show this track's sound
        presetBox.setSelectedId (0, juce::dontSendNotification);
        showInstrumentName();
        refreshSteps(); // songs and MIDI files land here after replacing the clips
        loadRoll();
        log ("Selected track" + juce::String (i + 1) + ": the keys, knobs, presets and Rec now work on it (the pads always play the drums).");
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
        s->setRange (lo, hi, hi >= 24 ? 1.0 : 0.0); // semitones, cents and Hz in whole steps
        if (mid > 0) s->setSkewFactorFromMidPoint (mid);
        s->setNumDecimalPlacesToDisplay (hi >= 24 ? 0 : 3);
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
        l->setMinimumHorizontalScale (0.6f); // narrow knobs: squeeze "Delay time" rather than cut it to "Delay ..."
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
    juce::ComboBox wave, filterType, lfoTarget, presetBox, osc2WaveBox;
    ShapeView shapeView;
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
    juce::Slider masterVolume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label masterLabel { {}, "Master" };
    juce::TextButton addTrackButton { "+ Track" }, saveSongButton { "Save song" }, openSongButton { "Open song" }, newSongButton { "New song" }, setDefaultButton { "Set default" };
    juce::File songFile; // where the song was last saved or opened
    juce::Label songHint;
    DrumPads pads;
    juce::ComboBox kitBox, keysBox;
    juce::Label keysLabel;
    juce::Slider drumVolume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label padsLabel;
    juce::ComboBox padLayout;
    juce::TextButton learnPadsButton { "Learn pads" }, stepsButton { "Steps" };
    StepGrid steps;
    juce::Slider stepBars { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::MidiMessageSequence stepsWritten; // the last clip the grid wrote: if the drum clip still equals it, the grid owns it
    PianoRoll roll;
    juce::TextButton editNotesButton { "Edit notes" }, quantizeButton { "Quantize" }, undoButton { "Undo" }, redoButton { "Redo" };
    Notes::History history;
    juce::ToggleButton loopToggle { "Loop" };
    juce::TextButton voiceButton { "Rec voice" };
    static constexpr double maxVoiceSeconds = 600.0; // shortcut: a voice take reserves 10 min (~115 MB at 48 kHz) while recording
    std::vector<float> micBuf;                        // audio thread: this block's microphone samples
    std::atomic<bool> hasMic { false };
    juce::Slider loopBars { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
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
