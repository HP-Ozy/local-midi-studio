#pragma once
#include "Song.h"
#include "Presets.h"

// Song file (.lmsong): {"format":"LocalMIDIStudio.song","version":1,"bpm","metronome","selected",
//   "tracks":[{"sound":<preset JSON>,"instrument":"<.sfz path or empty>","mute","solo","events":[[seconds,b0,b1,b2],...]}]}.
// Files may be hand-edited: loading validates everything into a separate copy first, so a bad file never leaves a
// half-loaded song.
namespace SongFile
{
constexpr int version = 1;

inline juce::String toJson (Song& song, const std::array<juce::File, Song::maxTracks>& instruments)
{
    juce::Array<juce::var> tracks;
    for (int i = 0; i < song.count; ++i)
    {
        auto& t = *song.tracks[(size_t) i];
        juce::Array<juce::var> events;
        for (auto* e : t.clip)
        {
            const auto& m = e->message;
            if (m.getRawDataSize() > 3 || m.isMetaEvent()) continue;
            juce::Array<juce::var> ev { m.getTimeStamp() };
            for (int k = 0; k < m.getRawDataSize(); ++k) ev.add ((int) m.getRawData()[k]);
            events.add (ev);
        }
        auto* o = new juce::DynamicObject();
        o->setProperty ("sound", juce::JSON::parse (Presets::toJson (t.engine.params, "track " + juce::String (i + 1))));
        o->setProperty ("instrument", t.engine.instrument() != nullptr ? instruments[(size_t) i].getFullPathName() : juce::String());
        o->setProperty ("mute", t.mute.load());
        o->setProperty ("solo", t.solo.load());
        o->setProperty ("events", events);
        tracks.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "LocalMIDIStudio.song");
    root->setProperty ("version", version);
    root->setProperty ("bpm", song.bpm.load());
    root->setProperty ("metronome", song.metronome.load());
    root->setProperty ("selected", song.selected.load());
    root->setProperty ("tracks", tracks);
    return juce::JSON::toString (juce::var (root));
}

struct Loaded // a fully validated song, applied by the caller while the transport is stopped
{
    struct Track { SynthParams params; juce::MidiMessageSequence clip; juce::String instrument; bool mute = false, solo = false; };
    std::vector<std::unique_ptr<Track>> tracks;
    double bpm = 120.0;
    bool metronome = true;
    int selected = 0;
};

// Returns an error message (empty on success).
inline juce::String parse (const juce::String& text, Loaded& out)
{
    const auto root = juce::JSON::parse (text);
    if (root["format"].toString() != "LocalMIDIStudio.song") return "not a Local MIDI Studio song";
    const int v = root["version"];
    if (v < 1 || v > version) return "song version " + root["version"].toString() + " is not supported";
    const auto* tracks = root["tracks"].getArray();
    if (tracks == nullptr || tracks->isEmpty()) return "the song has no tracks";
    if (tracks->size() > Song::maxTracks) return "the song has " + juce::String (tracks->size()) + " tracks (max " + juce::String (Song::maxTracks) + ")";
    out.bpm = juce::jlimit (30.0, 300.0, root.getProperty ("bpm", 120.0).operator double());
    out.metronome = root.getProperty ("metronome", true);
    out.selected = juce::jlimit (0, tracks->size() - 1, (int) root.getProperty ("selected", 0));
    for (int i = 0; i < tracks->size(); ++i)
    {
        const auto& tv = (*tracks)[i];
        auto t = std::make_unique<Loaded::Track>();
        juce::String name;
        if (tv["sound"].isObject())
            if (const auto e = Presets::fromJson (juce::JSON::toString (tv["sound"]), t->params, name); e.isNotEmpty())
                return "track " + juce::String (i + 1) + ": " + e;
        t->instrument = tv["instrument"].toString();
        t->mute = tv["mute"];
        t->solo = tv["solo"];
        if (const auto* events = tv["events"].getArray())
            for (const auto& ev : *events)
            {
                const auto* a = ev.getArray();
                if (a == nullptr || a->size() < 2 || a->size() > 4) return "track " + juce::String (i + 1) + ": malformed event";
                const double time = (*a)[0];
                juce::uint8 bytes[3] {};
                for (int k = 1; k < a->size(); ++k)
                {
                    const int b = (*a)[k];
                    if (b < 0 || b > 255) return "track " + juce::String (i + 1) + ": byte out of range";
                    bytes[k - 1] = (juce::uint8) b;
                }
                const int size = a->size() - 1;
                const bool channelMessage = bytes[0] >= 0x80 && bytes[0] <= 0xEF;
                const int expected = (bytes[0] & 0xF0) == 0xC0 || (bytes[0] & 0xF0) == 0xD0 ? 2 : 3;
                if (! std::isfinite (time) || time < 0.0 || time > 24 * 3600.0 || ! channelMessage || size != expected
                    || bytes[1] > 127 || bytes[2] > 127)
                    return "track " + juce::String (i + 1) + ": invalid event";
                t->clip.addEvent (juce::MidiMessage (bytes, size, time));
            }
        t->clip.updateMatchedPairs();
        out.tracks.push_back (std::move (t));
    }
    return {};
}
} // namespace SongFile
