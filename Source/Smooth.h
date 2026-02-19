// Smooth.h
#pragma once
#include <JuceHeader.h>
#include <cmath>

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
        timeMs = juce::jlimit(0.1, 5000.0, ms);
        updateCoeff();
    }

    void reset(float value = 0.0f)
    {
        z = value;
    }

    float process(float x)
    {
        // y[n] = a*y[n-1] + (1-a)*x[n]
        z = a * z + (1.0f - a) * x;
        return z;
    }

    float get() const { return z; }

private:
    void updateCoeff()
    {
        // a = exp(-1/(tau*sr)), tau = timeMs/1000
        const double tau = timeMs / 1000.0;
        if (sampleRate > 0.0 && tau > 0.0)
            a = (float)std::exp(-1.0 / (tau * sampleRate));
        else
            a = 0.0f;
    }

    double sampleRate = 44100.0;
    double timeMs = 20.0;
    float  a = 0.0f;
    float  z = 0.0f;
};
