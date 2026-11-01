# Passaggio di consegne — Local MIDI Studio (01/11/2026)

Sei un agente di sviluppo. Continui un progetto già avviato: **Local MIDI Studio**, un'app desktop Windows nativa e offline (C++20, JUCE, CMake) per suonare una tastiera **Alesis VI49** e sentire il suono in cuffia. Il progetto è in `F:\claude\LocalMIDIStudio`. Il repo GitHub è **privato**: `HP-Ozy/local-midi-studio`, licenza GPL-3.0.
Rispondi all'utente in **italiano**, in modo semplice: non è un programmatore audio.

## Prima di toccare qualsiasi cosa
1. Leggi `docs/ENGINEERING.md`, cicli da C1 a C24, e `README.md`.
2. Regole di lavoro, già concordate con l'utente e obbligatorie:
   - **misura prima e dopo** ogni modifica al DSP;
   - nel callback audio: niente allocazioni, lock, file I/O o log;
   - priorità P0 → P3;
   - per ogni nuova logica, un test in `tests/RenderTest.cpp` con **controllo negativo**: dimostra che il test fallisce sulla versione sbagliata;
   - a fine ciclo, una sezione "Cxx" in `ENGINEERING.md` con misure, test, cosa NON è verificato e cosa viene dopo.
3. Stile "ponytail": la modifica più piccola che risolve davvero il problema, nessuna astrazione non richiesta, nessuna dipendenza nuova per poche righe.
4. Mai dichiarare verificato ciò che non hai verificato. In particolare l'hardware e la GUI: i clic simulati (PostMessage) **non premono** i TextButton JUCE, quindi le prove nell'interfaccia le fa l'utente.

## Build e test
- Toolchain: JUCE **7.0.12** come submodule (JUCE 8 rifiuta MinGW), **MinGW GCC 16**, Ninja, link statico. Su questo PC **non c'è MSVC**.
- Prima **chiudi l'exe in esecuzione**, altrimenti il link fallisce. Poi: `cmake --build build` (dalla cartella del progetto).
- Test: `build\RenderTest_artefacts\Release\RenderTest.exe` deve finire con `ALL TESTS PASSED`. Altre modalità:
  - `--stress N`: N minuti di stress test real-time;
  - `--probe file.sfz|file.mid`: esamina uno strumento o un file MIDI;
  - `RenderTest <cartella>`: scrive i WAV di riferimento.
- App: `build\LocalMIDIStudio_artefacts\Release\Local MIDI Studio.exe`.
- Trappola PowerShell 5: `Get-Content -Raw` legge l'UTF-8 come ANSI. Usa lo strumento Edit oppure `[IO.File]` con UTF8.
- Due sessioni hanno modificato `Main.cpp` in parallelo il 01/11: prima di grandi modifiche controlla `git status` e `git diff`.

## Architettura (file principali in `src/`)
- `Synth.h`:
  - SynthParams, parametri atomici percorribili con `forEach`;
  - wavetable band-limited, Envelope propria, filtro TPT, LFO a control-rate, effetti come mandate;
  - SampleVoice e InstrumentSound per il sampler;
  - SynthEngine con 32 voci synth e 32 voci sample.
- `MidiQueue.h`: coda MIDI lock-free. `Recording.h`: MIDI file, render offline, WAV, `readMidiParts`.
- `Presets.h`: JSON e 9 preset di fabbrica. `MidiMap.h`: MIDI Learn CC → parametro. `Sfz.h`: un sottoinsieme di SFZ v2. `Library.h`: catalogo di 8 strumenti CC0 scaricabili.
- `Song.h`:
  - Song con 8 tracce preallocate; ognuna ha il suo SynthEngine e la sua clip in secondi;
  - PadRouter: pad → traccia Drums (traccia 2), lock-free;
  - count-in, metronomo, mixdown.
- `SongFile.h`: file `.lmsong` in JSON, validato in una copia prima di applicarlo.
- `Steps.h`: step sequencer. `Notes.h`: piano roll e quantize.
- `Main.cpp`: tutta la GUI.
  - Ogni modifica a una clip passa da `setClip()`: ferma il trasporto, sostituisce la clip, salva un livello di undo e riparte dalla stessa posizione.
  - Le clip si toccano solo a trasporto fermo: `stopTransport()` aspetta `audioRunning`.

## Cosa è fatto
- Fasi 1–5 del piano iniziale:
  - MIDI con hot-plug e monitor; synth polifonico con filtri, LFO, effetti e clipping di sicurezza;
  - impostazioni audio; preset; MIDI Learn;
  - sampler SFZ con Salamander Grand Piano e una batteria in `F:\claude\samples`;
  - registrazione, esportazione WAV e salvataggio/apertura della canzone.
- Song mode a 8 tracce, pad per la batteria, step sequencer, piano roll, quantize, undo.
- Stress test di 30 minuti superato.
- Verificato sull'hardware vero dall'utente: solo **tasti VI49 → suono in cuffia**.

## Cosa resta (in ordine di priorità consigliato)
1. **Mixer**: volume e pan per traccia, più un **master volume** globale. Il piano lo chiede già: "Track mixing", "Stereo pan", "Master volume".
   - Il pan va aggiunto al motore come parametro smussato.
   - Va salvato nel `.lmsong`, che oggi salva il suono con `Presets::toJson`: un nuovo parametro in `forEach` finisce lì da solo.
   - Misura i click con la metrica esistente.
2. **Editor del suono, parti mancanti del piano**:
   - **intonazione dell'oscillatore** (semitoni e cents);
   - **mix di due oscillatori** (secondo oscillatore + mix);
   - **visualizzazione della forma d'onda e dell'inviluppo**.
   - Tutto con smoothing e misure prima/dopo; il costo CPU va confrontato con lo stress test.
3. **Registrazione, estensioni del piano**:
   - **loop recording** (registrazione a ciclo su N battute);
   - **undo/redo a più livelli**: oggi è uno solo, `undoClip`/`undoTrack` in `Main.cpp`.
4. **Tracce vocali (audio dal microfono)**, chieste dall'utente. Serve:
   - un ingresso audio nelle impostazioni;
   - una registrazione su un buffer preallocato o una FIFO verso un thread che scrive il WAV;
   - la riproduzione allineata a `songZeroWall`;
   - il salvataggio del WAV accanto al `.lmsong`.
5. **Sample player per un singolo WAV importato**: oggi root, range di tasti e velocity e loop si impostano solo via SFZ. Il piano chiede di poterli configurare per i campioni personali. Basta una piccola finestra che generi la regione.
6. **Violino e chitarra elettrica gratuiti** per la Library (`src/Library.h`): l'utente li ha chiesti.
   - Servono licenze CC0 o CC-BY verificate, da documentare in `CREDITS`.
   - **Chiedi all'utente il permesso prima di ogni download**, indicando nome, fonte e dimensione.
7. **Fase 6, packaging**:
   - uno zip di release con exe statico, README e CREDITS;
   - verifica che parta fuori dalla cartella di build;
   - completare nel README la guida all'Alesis VI49. Pad e manopole sono da imparare dall'hardware; consiglio: pad sul canale 10 dall'editor Alesis.
8. **Latenza**: oggi circa 20 ms con WASAPI shared, mentre il piano punta a circa 10 ms. Prova con l'utente la modalità Exclusive / Low Latency e un buffer da 128–256. ASIO non è compilato: serve l'SDK Steinberg, quindi documenta il limite.

## Da far provare all'utente (mai provato dal vivo)
- MIDI Learn con le manopole vere; Learn pads.
- Download dalla Library.
- Registrazione in Song mode; Save/Open song; Open MIDI.
- Step sequencer; piano roll, Quantize, Undo.

## Vincoli di sicurezza
- **Non modificare impostazioni di sistema.** Per esempio il risparmio energetico USB: le fa l'utente. Se la VI49 tace, controlla per prima cosa la "sospensione selettiva USB".
- Non muovere il mouse dell'utente.
- Nessun download senza un suo "sì" esplicito.
- Fai commit o push solo se te lo chiede.
