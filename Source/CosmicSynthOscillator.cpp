#include <cmath>
#include "CosmicSynthOscillator.h"

CosmicSynthOscillator::CosmicSynthOscillator(std::vector<float> wt, double sr)
    : waveTable{ std::move(wt) }, sampleRate{ (sr > 1.0 ? sr : 44100.0) }
{
    updateSlewCoeff();
}

void CosmicSynthOscillator::setFreqSlewMs(float ms)
{
    // keep it sane; 0.1ms is basically "none", 200ms is super slow
    if (ms < 0.1f) ms = 0.1f;
    if (ms > 200.0f) ms = 200.0f;

    slewMs = ms;
    updateSlewCoeff();
}

void CosmicSynthOscillator::updateSlewCoeff()
{
    // One-pole: y[n] = a*y[n-1] + (1-a)*x[n]
    // tau = slewMs/1000, a = exp(-1/(tau*sr))
    const double tau = (double)slewMs / 1000.0;
    if (tau <= 0.0 || sampleRate <= 0.0)
        slewA = 0.0f;
    else
        slewA = (float)std::exp(-1.0 / (tau * sampleRate));
}

void CosmicSynthOscillator::setFrequency(float frequency)
{
    // frequency <= 0 means "off"
    if (frequency <= 0.0f || waveTable.empty())
    {
        targetIncrement = 0.0f;
        active = false;
        return;
    }

    const float tableSize = (float)waveTable.size();
    targetIncrement = (frequency * tableSize) / (float)sampleRate;

    // mark active as soon as we have a nonzero target
    active = true;
}

void CosmicSynthOscillator::startAt(float frequency)
{
    setFrequency(frequency);
    // A stopped voice has indexIncrement = 0, so without this the slew would
    // sweep the pitch up from 0 Hz over the first 10-30 ms of every note:
    // heard as a click/crackle on presets with a fast attack.
    indexIncrement = targetIncrement;
    index = 0.f;   // every wavetable is a sum of sines: 0 at index 0
}

float CosmicSynthOscillator::getSample()
{
    if (!active)
        return 0.0f;

    const float sample = interpolateLinearly();

    // Smooth the increment to avoid “step” clicks at block boundaries
    indexIncrement = slewA * indexIncrement + (1.0f - slewA) * targetIncrement;

    index += indexIncrement;

    const float size = (float)waveTable.size();
    // Faster/cleaner wrap than fmod for positive index
    while (index >= size) index -= size;
    while (index < 0.0f)  index += size;

    // If we slewed basically to zero, treat as stopped
    if (std::abs(indexIncrement) < 1.0e-8f && std::abs(targetIncrement) < 1.0e-8f)
        active = false;

    return sample;
}

float CosmicSynthOscillator::interpolateLinearly() const
{
    const int size = (int)waveTable.size();
    if (size <= 1)
        return (size == 1 ? waveTable[0] : 0.0f);

    const int i0 = (int)index;
    const int i1 = (i0 + 1) % size;

    const float frac = index - (float)i0;
    return (1.0f - frac) * waveTable[(size_t)i0] + frac * waveTable[(size_t)i1];
}

void CosmicSynthOscillator::stop()
{
    index = 0.f;
    indexIncrement = 0.f;
    targetIncrement = 0.f;
    active = false;
}

bool CosmicSynthOscillator::isPlaying() const
{
    return active;
}