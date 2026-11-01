#pragma once
#include <juce_core/juce_core.h>

// Controller knob (MIDI channel + CC number) -> parameter key, learned from the hardware.
// One CC drives one parameter and one parameter has one CC: binding again replaces both old links.
struct MidiMap
{
    struct Binding { int channel, cc; juce::String param; };
    std::vector<Binding> bindings;

    void bind (int channel, int cc, const juce::String& param)
    {
        forget (param);
        std::erase_if (bindings, [&] (const Binding& b) { return b.channel == channel && b.cc == cc; });
        bindings.push_back ({ channel, cc, param });
    }

    void forget (const juce::String& param) { std::erase_if (bindings, [&] (const Binding& b) { return b.param == param; }); }

    juce::String find (int channel, int cc) const
    {
        for (auto& b : bindings) if (b.channel == channel && b.cc == cc) return b.param;
        return {};
    }

    const Binding* forParam (const juce::String& param) const
    {
        for (auto& b : bindings) if (b.param == param) return &b;
        return nullptr;
    }

    juce::String toJson() const
    {
        juce::Array<juce::var> list;
        for (auto& b : bindings)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("channel", b.channel);
            o->setProperty ("cc", b.cc);
            o->setProperty ("param", b.param);
            list.add (juce::var (o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("format", "LocalMIDIStudio.midimap");
        root->setProperty ("version", 1);
        root->setProperty ("bindings", list);
        return juce::JSON::toString (juce::var (root));
    }

    // The file can be hand-edited: entries out of range or malformed are skipped, anything else gives an empty map.
    static MidiMap fromJson (const juce::String& text)
    {
        MidiMap m;
        const auto root = juce::JSON::parse (text);
        if (root["format"].toString() != "LocalMIDIStudio.midimap" || (int) root["version"] != 1) return m;
        if (auto* list = root["bindings"].getArray())
            for (auto& b : *list)
            {
                const int ch = b["channel"], cc = b["cc"];
                const auto param = b["param"].toString();
                if (ch >= 1 && ch <= 16 && cc >= 0 && cc <= 127 && param.isNotEmpty()) m.bind (ch, cc, param);
            }
        return m;
    }
};
