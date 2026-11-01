#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// MIDI from any thread to the audio thread, without the audio thread ever waiting.
// Producers (MIDI driver threads, GUI) serialise among themselves; the consumer takes no lock.
class MidiQueue
{
public:
    static double now() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    void push (const juce::MidiMessage& m, double timeSec)
    {
        const int size = m.getRawDataSize();
        if (size > 3) return; // sysex: the synth doesn't use it
        const juce::SpinLock::ScopedLockType lock (producerLock);
        const auto w = fifo.write (1);
        if (w.blockSize1 + w.blockSize2 == 0) { ++dropped; return; }
        auto& e = events[(size_t) (w.blockSize1 > 0 ? w.startIndex1 : w.startIndex2)];
        e.time = timeSec;
        e.size = size;
        std::memcpy (e.data, m.getRawData(), (size_t) size);
    }

    // Audio thread. Events keep their relative timing: an event that arrived one block ago lands
    // at sample 0, one that arrived just now at the last sample (= one block of fixed latency).
    void popInto (juce::MidiBuffer& out, int numSamples, double sampleRate, double nowSec)
    {
        // MidiBuffer::addEvent is O(n) per insert, so cap the work; a flood spills into the next block.
        const auto r = fifo.read (juce::jmin (fifo.getNumReady(), maxPerBlock));
        r.forEach ([&] (int i)
        {
            const auto& e = events[(size_t) i];
            const int age = juce::roundToInt ((nowSec - e.time) * sampleRate);
            out.addEvent (e.data, e.size, juce::jlimit (0, numSamples - 1, numSamples - 1 - age));
        });
    }

    static constexpr int capacity = 1024, maxPerBlock = 256;
    std::atomic<int> dropped { 0 };

private:
    struct Event { double time; int size; juce::uint8 data[3]; };
    juce::AbstractFifo fifo { capacity };
    std::array<Event, capacity> events {};
    juce::SpinLock producerLock;
};
