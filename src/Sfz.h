#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <regex>
#include "Synth.h"

// Loads a sampled instrument from a subset of SFZ (the plain-text format most free sample libraries ship in) or
// from a single audio file. Supported: #include, #define, <control> default_path/note_offset/octave_offset/set_ccN,
// <global>/<master>/<group>/<region> inheritance, sample, key,
// lokey, hikey, pitch_keycenter, lovel, hivel, tune, transpose, volume, loop_mode, loop_start, loop_end, amp_veltrack,
// ampeg_release, lorand, hirand, seq_length, seq_position, group, off_by (fast choke), pitch_keytrack; *_onccN, amplitude_onccN
// and xfin/xfout_locc/hicc evaluated at the controllers' set_cc defaults (regions silent there are skipped); sw_last vs
// sw_default picks the default articulation. One region plays per note (no layering).
// Release-triggered regions are skipped; anything else unknown is ignored.
namespace Sfz
{
// "c4" = 60 (SFZ convention), "c#4", "db4", "c-1" = 0, or a plain number. -1 if unreadable.
inline int noteNumber (juce::String s)
{
    s = s.trim().toLowerCase();
    if (s.isEmpty()) return -1;
    if (s.containsOnly ("0123456789")) return juce::jlimit (0, 127, s.getIntValue());
    const int base[] { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
    if (s[0] < 'a' || s[0] > 'g') return -1;
    int n = base[s[0] - 'a'], i = 1;
    if (s[i] == '#') { ++n; ++i; } else if (s[i] == 'b' && s.length() > 2) { --n; ++i; }
    const auto octave = s.substring (i);
    if (! octave.containsOnly ("-0123456789") || octave.isEmpty()) return -1;
    const int note = (octave.getIntValue() + 1) * 12 + n;
    return note >= 0 && note <= 127 ? note : -1;
}

// Strips comments and expands #include "file" (relative to the including file) and #define $NAME value.
inline juce::String preprocess (const juce::String& text, const juce::File& dir, std::map<juce::String, juce::String>& defines, int depth = 0)
{
    if (depth > 8) throw std::invalid_argument ("#include nested too deeply");
    auto clean = text.replace ("\r", "");
    while (clean.contains ("/*"))
    {
        const int a = clean.indexOf ("/*"), b = clean.indexOf (a, "*/");
        clean = clean.substring (0, a) + (b < 0 ? juce::String() : clean.substring (b + 2));
    }
    juce::String out;
    for (auto line : juce::StringArray::fromLines (clean))
    {
        if (line.contains ("//")) line = line.upToFirstOccurrenceOf ("//", false, false);
        const auto t = line.trim();
        if (t.startsWith ("#define"))
        {
            const auto rest = t.fromFirstOccurrenceOf ("#define", false, false).trim();
            defines[rest.upToFirstOccurrenceOf (" ", false, false).trim()] = rest.fromFirstOccurrenceOf (" ", false, false).trim();
            continue;
        }
        if (t.startsWith ("#include"))
        {
            const auto name = t.fromFirstOccurrenceOf ("\"", false, false).upToLastOccurrenceOf ("\"", false, false).replaceCharacter ('\\', '/');
            const auto f = dir.getChildFile (name);
            if (! f.existsAsFile()) throw std::invalid_argument ("#include \"" + name.toStdString() + "\" (file not found)");
            out << preprocess (f.loadFileAsString(), f.getParentDirectory(), defines, depth + 1) << "\n";
            continue;
        }
        for (auto& [k, v] : defines) line = line.replace (k, v); // shortcut: no longest-match ordering; fine while names don't prefix each other
        out << line << "\n";
    }
    return out;
}

// Parses SFZ text into regions (audio not loaded yet). Error message on failure.
inline std::vector<SampleRegion> parse (const juce::String& text, const juce::File& dir, juce::String& error)
{
    using Opcodes = std::map<std::string, std::string>;
    Opcodes control, global, master, group, region, *current = &global;
    std::map<int, double> cc; // controller defaults from set_ccN: decide which CC-gated regions are audible
    std::vector<SampleRegion> out;
    bool inRegion = false;

    auto finishRegion = [&]
    {
        if (! inRegion) return;
        inRegion = false;
        Opcodes o;
        for (auto* level : { &global, &master, &group, &region })
        {
            if (level->count ("key")) for (auto k : { "lokey", "hikey", "pitch_keycenter" }) o.erase (k); // key= overrides inherited ranges
            for (auto& [k, v] : *level) o[k] = v;
        }
        if (o["trigger"] == "release" || o["sample"].empty() || o["sample"][0] == '*') return; // release noise, built-in generators
        SampleRegion r;
        const auto path = juce::String (control["default_path"] + o["sample"]).replaceCharacter ('\\', '/');
        r.file = dir.getChildFile (path);
        // Every conversion names its opcode in the error, so a broken file says what to fix.
        auto number = [&] (const std::string& k, const std::string& v)
        {
            try { return std::stod (v); }
            catch (const std::exception&) { throw std::invalid_argument (k + "=" + v); }
        };
        auto ccValue = [&] (const std::string& key, const std::string& prefix) // "ampeg_release_oncc104" -> cc 104's default
        {
            return cc.count (std::stoi (key.substr (prefix.size()))) ? cc[std::stoi (key.substr (prefix.size()))] : 0.0;
        };
        auto num = [&] (const char* k, double def) // value plus its _onccN modulation at the controllers' defaults
        {
            double v = o.count (k) ? number (k, o[k]) : def;
            const std::string prefix = std::string (k) + "_oncc";
            for (auto& [key, val] : o)
                if (key.rfind (prefix, 0) == 0) v += number (key, val) * ccValue (key, prefix) / 127.0;
            return v;
        };
        // Static gain from CC crossfades and amplitude_onccN at the controllers' defaults: 0 = silent, skip it
        double staticGain = num ("amplitude", 100) / 100.0;
        for (auto& [key, val] : o)
        {
            if (key.rfind ("amplitude_oncc", 0) == 0) staticGain *= number (key, val) / 100.0 * ccValue (key, "amplitude_oncc") / 127.0;
            for (const std::string dirn : { "xfin", "xfout" })
                if (key.rfind (dirn + "_locc", 0) == 0 || key.rfind (dirn + "_hicc", 0) == 0)
                {
                    const auto n = key.substr (dirn.size() + 5);
                    if (key.rfind (dirn + "_hicc", 0) == 0 && o.count (dirn + "_locc" + n)) continue; // handle each pair once
                    const double lo = o.count (dirn + "_locc" + n) ? number (key, o[dirn + "_locc" + n]) : 0.0;
                    const double hi = o.count (dirn + "_hicc" + n) ? number (key, o[dirn + "_hicc" + n]) : 127.0;
                    const double x = cc.count (std::stoi (n)) ? cc[std::stoi (n)] : 0.0;
                    const double t = hi > lo ? juce::jlimit (0.0, 1.0, (x - lo) / (hi - lo)) : (x >= hi ? 1.0 : 0.0);
                    staticGain *= dirn == "xfin" ? t : 1.0 - t;
                }
        }
        if (staticGain < 1e-4) return;
        const auto swDefault = o.count ("sw_default") ? o["sw_default"] : control["sw_default"];
        if (o.count ("sw_last") && ! swDefault.empty() && o["sw_last"] != swDefault) return; // other articulations (keyswitches)
        auto note = [&] (const char* k, int def)
        {
            if (! o.count (k)) return def;
            if (o[k] == "-1") return -1; // SFZ: "no key" (region triggered by a controller, e.g. pedal noise)
            const int n = noteNumber (o[k]);
            if (n < 0) throw std::invalid_argument (std::string (k) + "=" + o[k]);
            return n;
        };
        if (o.count ("key")) r.lokey = r.hikey = r.root = note ("key", 60);
        r.lokey = note ("lokey", r.lokey);
        r.hikey = note ("hikey", r.hikey);
        if (r.lokey < 0 || r.hikey < 0) return; // not played from the keyboard: unsupported, skipped
        const int offset = (int) (control.count ("note_offset") ? number ("note_offset", control["note_offset"]) : 0)
                         + 12 * (int) (control.count ("octave_offset") ? number ("octave_offset", control["octave_offset"]) : 0);
        r.root = note ("pitch_keycenter", r.root) - (int) num ("transpose", 0) + offset;
        r.lokey = juce::jlimit (0, 127, r.lokey + offset);
        r.hikey = juce::jlimit (0, 127, r.hikey + offset);
        r.lovel = (int) num ("lovel", 1);
        r.hivel = (int) num ("hivel", 127);
        r.tuneCents = num ("tune", 0);
        r.volumeDb = num ("volume", 0) + juce::Decibels::gainToDecibels (staticGain);
        r.oneShot = o["loop_mode"] == "one_shot";
        r.loop = o["loop_mode"] == "loop_continuous" || o["loop_mode"] == "loop_sustain";
        r.loopStart = (juce::int64) num ("loop_start", 0);
        r.loopEnd = o.count ("loop_end") ? (juce::int64) num ("loop_end", 0) + 1 : 0; // SFZ loop_end is inclusive
        r.ampVeltrack = juce::jlimit (0.0f, 1.0f, (float) num ("amp_veltrack", 100) / 100.0f);
        r.release = o.count ("ampeg_release") ? juce::jmax (0.001f, (float) num ("ampeg_release", 0)) : -1.0f;
        r.lorand = (float) num ("lorand", 0);
        r.hirand = (float) num ("hirand", 1);
        r.seqLength = juce::jmax (1, (int) num ("seq_length", 1));
        r.seqPosition = juce::jlimit (1, r.seqLength, (int) num ("seq_position", 1));
        r.group = (int) num ("group", 0);
        r.offBy = (int) num ("off_by", 0);
        r.keytrack = num ("pitch_keytrack", 100) != 0.0;
        out.push_back (r);
    };

    try
    {
        std::map<juce::String, juce::String> defines;
        const auto clean = preprocess (text, dir, defines);
        const std::regex token (R"((<\w+>)|([A-Za-z0-9_]+)=)");
        for (auto line : juce::StringArray::fromLines (clean))
        {
            const auto s = line.toStdString();
            std::vector<std::smatch> found;
            for (auto it = std::sregex_iterator (s.begin(), s.end(), token); it != std::sregex_iterator(); ++it) found.push_back (*it);
            for (size_t i = 0; i < found.size(); ++i)
            {
                const auto& m = found[i];
                if (m[1].matched)
                {
                    const auto h = m[1].str();
                    finishRegion();
                    if (h == "<region>")       { region.clear(); current = &region; inRegion = true; }
                    else if (h == "<group>")   { group.clear(); current = &group; }
                    else if (h == "<master>")  { master.clear(); group.clear(); current = &master; }
                    else if (h == "<global>")  { global.clear(); current = &global; }
                    else if (h == "<control>") { current = &control; }
                    else current = nullptr; // unsupported header (<curve>, <effect>...): ignore its opcodes
                    continue;
                }
                // a value runs to the next opcode/header on the line, so sample paths may contain spaces
                const auto from = (size_t) (m.position (0) + m.length (0));
                const auto to = i + 1 < found.size() ? (size_t) found[i + 1].position (0) : s.size();
                const auto key = m[2].str(), value = juce::String (s.substr (from, to - from)).trim().toStdString();
                if (key.rfind ("set_cc", 0) == 0)        cc[std::stoi (key.substr (6))] = std::stod (value);
                else if (key.rfind ("set_hdcc", 0) == 0) cc[std::stoi (key.substr (8))] = std::stod (value) * 127.0; // 0..1 scale
                else if (current != nullptr) (*current)[key] = value;
            }
        }
        finishRegion();
    }
    catch (const std::exception& e) { error = "malformed SFZ: cannot read " + juce::String (e.what()); return {}; }
    if (out.empty()) error = "no playable regions in the SFZ file";
    return out;
}

// Loads an .sfz (all its samples) or a single audio file (root C4 = 60, whole keyboard). Runs on a background
// thread in the app: whole sample sets are read into memory. Fails with a message, never half-loads.
inline juce::SynthesiserSound::Ptr load (const juce::File& file, juce::String& error)
{
    std::vector<SampleRegion> regions;
    if (file.hasFileExtension ("sfz"))
        regions = parse (file.loadFileAsString(), file.getParentDirectory(), error);
    else
        regions.push_back ({ file });
    if (error.isNotEmpty()) return nullptr;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::map<juce::String, std::pair<std::shared_ptr<const juce::AudioBuffer<float>>, double>> cache;
    for (auto& r : regions)
    {
        auto& entry = cache[r.file.getFullPathName()];
        if (entry.first == nullptr)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (r.file));
            if (reader == nullptr) { error = "cannot read " + r.file.getFullPathName(); return nullptr; }
            auto buf = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
            reader->read (buf.get(), 0, (int) reader->lengthInSamples, 0, true, buf->getNumChannels() > 1);
            entry = { buf, reader->sampleRate };
        }
        r.audio = entry.first;
        r.fileRate = entry.second;
        if (r.loopEnd > r.audio->getNumSamples() || r.loopStart >= r.loopEnd) r.loop = false; // bad loop points: play once
    }
    auto* inst = new InstrumentSound();
    inst->name = file.getFileNameWithoutExtension();
    float loudest = 0.0f; // libraries are recorded at their own levels (Salamander drums peaked at -25 dBFS here)
    for (auto& r : regions)
        loudest = juce::jmax (loudest, r.audio->getMagnitude (0, r.audio->getNumSamples()) * juce::Decibels::decibelsToGain ((float) r.volumeDb));
    inst->gain = loudest > 0.0f ? juce::jmin (4.0f, 1.0f / loudest) : 1.0f; // at most +12 dB
    inst->regions = std::move (regions);
    return inst;
}
// What a note plays, in a few words, for the drum-pad view: from the loaded kit's sample name for that key
// ("kick_OH_FF_1" -> "kick", "hihatClosed_..." -> "hihat Closed"), else the General MIDI drum name, else the note name.
inline juce::String drumName (int note, const InstrumentSound* inst)
{
    if (inst != nullptr)
        for (auto& r : inst->regions)
            if (r.lokey == note && r.hikey == note) // drum kits map one sound per key
            {
                const auto base = r.file.getFileNameWithoutExtension().upToFirstOccurrenceOf ("_", false, false);
                juce::String words;
                for (int i = 0; i < base.length(); ++i)
                {
                    if (i > 0 && juce::CharacterFunctions::isUpperCase (base[i]) && ! juce::CharacterFunctions::isUpperCase (base[i - 1])) words << ' ';
                    words << juce::String::charToString (base[i]);
                }
                return words;
            }
    static const char* gm[] { "Acoustic Bass Drum", "Bass Drum", "Side Stick", "Snare", "Hand Clap", "Electric Snare", "Low Floor Tom",
                              "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat", "Low-Mid Tom", "Hi-Mid Tom",
                              "Crash", "High Tom", "Ride", "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash", "Cowbell", "Crash 2",
                              "Vibraslap", "Ride 2", "Hi Bongo", "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga", "High Timbale",
                              "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle", "Short Guiro",
                              "Long Guiro", "Claves", "Hi Wood Block", "Low Wood Block", "Mute Cuica", "Open Cuica", "Mute Triangle", "Open Triangle" };
    static_assert (std::size (gm) == 81 - 35 + 1, "General MIDI percussion is notes 35..81");
    if (note >= 35 && note <= 81) return gm[note - 35];
    return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
}
} // namespace Sfz
