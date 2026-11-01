#pragma once
#include "Synth.h"

// Preset file: {"format":"LocalMIDIStudio.preset","version":1,"name":"...","params":{"cutoff":2500,...}}.
// Files may be hand-edited, so loading validates: unknown keys ignored, missing keys keep their current value,
// values clamped to range, other formats and newer versions refused (we can't know what they mean).
namespace Presets
{
constexpr int version = 1;

inline juce::String toJson (SynthParams& p, const juce::String& name)
{
    auto* params = new juce::DynamicObject();
    p.forEach ([&] (const char* key, auto& v, auto, auto) { params->setProperty (key, v.load()); });
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "LocalMIDIStudio.preset");
    root->setProperty ("version", version);
    root->setProperty ("name", name);
    root->setProperty ("params", juce::var (params));
    return juce::JSON::toString (juce::var (root));
}

// Returns an error message (empty on success). On error `p` is left untouched.
inline juce::String fromJson (const juce::String& text, SynthParams& p, juce::String& name)
{
    const auto root = juce::JSON::parse (text);
    if (root["format"].toString() != "LocalMIDIStudio.preset") return "not a Local MIDI Studio preset";
    const int v = root["version"];
    if (v < 1 || v > version) return "preset version " + root["version"].toString() + " is not supported";
    const auto params = root["params"];
    if (! params.isObject()) return "preset has no parameters";
    p.forEach ([&] (const char* key, auto& value, auto lo, auto hi)
    {
        const auto x = params[key];
        if (x.isInt() || x.isInt64() || x.isDouble())
            value = juce::jlimit (lo, hi, static_cast<decltype (lo)> ((double) x));
    });
    name = root["name"].toString();
    return {};
}

// Built-in sounds, all plain synthesis: names say so where a real instrument is only imitated.
struct Factory { const char* name; void (*set) (SynthParams&); };
inline const std::vector<Factory>& factory()
{
    static const std::vector<Factory> list {
        { "Init",                    [] (SynthParams&) {} },
        { "Analog Synth",            [] (SynthParams& p) { p.wave = 1; p.cutoff = 2500; p.resonance = 2; p.attack = 0.01f; p.decay = 0.3f; p.sustain = 0.6f; p.release = 0.4f; } },
        { "Synth Bass",              [] (SynthParams& p) { p.wave = 2; p.cutoff = 600; p.resonance = 4; p.attack = 0.003f; p.decay = 0.25f; p.sustain = 0.3f; p.release = 0.1f; } },
        { "Ambient Pad",             [] (SynthParams& p) { p.wave = 1; p.cutoff = 1500; p.attack = 1.2f; p.decay = 1.0f; p.sustain = 0.8f; p.release = 2.5f; p.lfoTarget = 1; p.lfoDepth = 0.3f; p.lfoRate = 0.2f; p.chorusMix = 0.4f; p.reverbMix = 0.5f; p.reverbSize = 0.85f; } },
        { "Electronic Lead",         [] (SynthParams& p) { p.wave = 5; p.pulseWidth = 0.3f; p.cutoff = 5000; p.resonance = 2; p.lfoRate = 5.5f; p.delayMix = 0.25f; p.delayTime = 0.3f; p.delayFeedback = 0.35f; } },
        { "Electric Piano (synth)",  [] (SynthParams& p) { p.wave = 3; p.cutoff = 4000; p.attack = 0.003f; p.decay = 1.2f; p.sustain = 0.3f; p.release = 0.4f; p.lfoTarget = 2; p.lfoDepth = 0.25f; p.lfoRate = 4.5f; p.chorusMix = 0.3f; } },
        { "Organ (synth)",           [] (SynthParams& p) { p.wave = 3; p.cutoff = 6000; p.attack = 0.005f; p.decay = 0.01f; p.sustain = 1.0f; p.release = 0.05f; p.lfoDepth = 0.15f; p.lfoRate = 6.0f; } },
        { "Pluck",                   [] (SynthParams& p) { p.wave = 3; p.cutoff = 3000; p.attack = 0.002f; p.decay = 0.25f; p.sustain = 0.0f; p.release = 0.2f; } },
        { "PWM Strings",             [] (SynthParams& p) { p.wave = 5; p.cutoff = 4000; p.attack = 0.4f; p.decay = 0.5f; p.sustain = 0.8f; p.release = 1.2f; p.lfoTarget = 3; p.lfoDepth = 0.6f; p.lfoRate = 0.8f; p.chorusMix = 0.5f; p.reverbMix = 0.35f; } },
    };
    return list;
}

inline juce::String factoryJson (const Factory& f)
{
    SynthParams p;
    f.set (p);
    return toJson (p, f.name);
}
} // namespace Presets
