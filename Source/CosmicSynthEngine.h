// CosmicSynthEngine.h
#pragma once

#include <JuceHeader.h>

#include <array>
#include <vector>
#include <cmath>

#include "CosmicSynthOscillator.h"

class CosmicSynthEngine
{
public:
    CosmicSynthEngine() = default;

    // Lifecycle
    void prepareToPlay(double newSampleRate);

    // Standard block processing (no ramping)
    void processBlock(juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midiMessages);

    // GW-driven control hooks (called from PluginProcessor)
    void setPitchRatio(float ratio);                 // ratio applied on top of played MIDI note
    void setPerNotePitchRatios(const std::array<float, 128>& ratios); // per-note ratios (poly)
    void setGain(float newGain01);                   // 0..1
    void setOscFrequency(float newHz);               // optional "force frequency" tool (stored)
    void setFilterCutoff(float newHz);               // stored + applied (1-pole LP)
    void setWaveshapeMix(float newMixMinus1to1);     // used for gentle saturation

    // Ramped rendering across a block (prevents block-step discontinuities)
    void processBlockRamped(juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midiMessages,
        const std::array<float, 128>& ratiosStart,
        const std::array<float, 128>& ratiosEnd,
        float gainStart,
        float gainEnd);

private:
    // Helpers
    static std::vector<float> generateWaveTable(); // richer-than-sine pad table
    static float midiNoteNumberToFrequency(int midiNoteNumber);

    void initializeOscillators();
    void render(juce::AudioBuffer<float>& buffer, int startSample, int endSample);
    void handleMidiEvent(const juce::MidiMessage& midiEvent);

    // One-pole lowpass: y = (1-a)*x + a*yPrev
    void updateLowpassCoeff();
    static float softClipTanh(float x, float drive);

    // Per-note pitch control (polyphonic)
    std::array<float, 128> perNotePitchRatio{};
    std::array<float, 128> baseHz{}; // base pitch (Hz) for each MIDI note while held; set to 0 on release

    // Per-note amplitude envelope (pad)
    std::array<juce::ADSR, 128> ampEnv{};
    juce::ADSR::Parameters padEnvParams{};

    // State
    double sampleRate = 44100.0;

    // Synthesis controls
    float globalGain = 0.2f;
    float pitchRatio = 1.0f;
    float globalFrequency = 0.0f;   // stored; not applied unless you decide to override MIDI
    float filterCutoffHz = 12000.0f;
    float waveshapeMix = 0.0f;      // [-1,1] -> saturation amount

    // Stereo width (0..1)
    float stereoWidth = 0.6f;
    float driftPhase = 0.0f;
	float filterLfoPhase = 0.0f;


    // Lowpass state
    float lpZL = 0.0f, lpZR = 0.0f;
    float lpA = 0.0f; // feedback coeff

    // One oscillator per MIDI note
    std::vector<CosmicSynthOscillator> oscillators;
};
