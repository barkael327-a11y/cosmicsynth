#pragma once
#include <vector>

class CosmicSynthOscillator
{
public:
    CosmicSynthOscillator(std::vector<float> waveTable, double sampleRate);

    // Sets the *target* frequency (Hz). The oscillator slews to it smoothly.
    // frequency <= 0 means "off" (hard stop).
    void setFrequency(float frequency);

    // Starts a silent voice exactly at this frequency: no glide up from 0 Hz,
    // and the waveform begins at a zero crossing. Use only on a voice that is
    // not playing (on a sounding voice, use setFrequency).
    void startAt(float frequency);

    // Optional: adjust smoothing amount (ms). Smaller = snappier, larger = smoother.
    void setFreqSlewMs(float ms);

    // Pull next sample
    float getSample();

    // Hard stop (resets phase and slews)
    void stop();

    // True if oscillator is currently active
    bool isPlaying() const;

private:
    float interpolateLinearly() const;
    void updateSlewCoeff();

    std::vector<float> waveTable;
    double sampleRate = 44100.0;

    float index = 0.f;

    // Smoothed increment
    float indexIncrement = 0.f;        // current
    float targetIncrement = 0.f;       // target
    float slewA = 0.0f;                // one-pole coeff

    float slewMs = 5.0f;               // default smoothing time
    bool  active = false;
};