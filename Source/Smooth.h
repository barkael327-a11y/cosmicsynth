// Smooth.h
#pragma once
#include <JuceHeader.h>
#include <cmath>

// One-pole smoother: y[n] = a*y[n-1] + (1-a)*x[n]
//
// By default each process() call is one audio sample. When it is called once
// per audio buffer instead (as the processor's GW smoothers are), call
// setSamplesPerCall(bufferSize) every buffer so that "timeMs" still means
// milliseconds. Without it, a 30 ms setting behaves like 30 ms x buffer size
// (about 14 s for a 480-sample buffer).
class OnePole
{
public:
    OnePole() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = (newSampleRate > 1.0 ? newSampleRate : 44100.0);
        updateCoeff();
    }

    void setTimeMs(double ms)
    {
        const double t = juce::jlimit(0.1, 5000.0, ms);
        if (t != timeMs) { timeMs = t; updateCoeff(); }
    }

    // How many audio samples each process() call stands for (1 = per sample)
    void setSamplesPerCall(int n)
    {
        n = juce::jmax(1, n);
        if (n != samplesPerCall) { samplesPerCall = n; updateCoeff(); }
    }

    void reset(float value = 0.0f)
    {
        z = value;
    }

    float process(float x)
    {
        z = a * z + (1.0f - a) * x;
        return z;
    }

    float get() const { return z; }

private:
    void updateCoeff()
    {
        // a = exp(-n/(tau*sr)), tau = timeMs/1000, n = samples per call
        const double tau = timeMs / 1000.0;
        if (sampleRate > 0.0 && tau > 0.0)
            a = (float)std::exp(-(double)samplesPerCall / (tau * sampleRate));
        else
            a = 0.0f;
    }

    double sampleRate = 44100.0;
    double timeMs = 20.0;
    int    samplesPerCall = 1;
    float  a = 0.0f;
    float  z = 0.0f;
};