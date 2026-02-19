// PluginProcessor.h
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>

#include "CosmicSynthEngine.h"
#include "GWSeries.h"
#include "Smooth.h"

//==============================================================================

class CosmicsynthAudioProcessor : public juce::AudioProcessor
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    CosmicsynthAudioProcessor();
    ~CosmicsynthAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // APVTS
    APVTS& getVTS() noexcept { return apvts; }
    const APVTS& getVTS() const noexcept { return apvts; }

    // GW CSV loading
    bool loadGWFile(const juce::File& f);

    // UI read-only hooks
    const GWSeries& getGW() const noexcept { return gw; }
    bool isGWLoaded() const noexcept { return gwLoaded.load(std::memory_order_relaxed); }

    double getLastBlockTime() const noexcept
    {
        return lastBlockTimeSec.load(std::memory_order_relaxed);
    }

    int getCurrentMidiNote() const noexcept
    {
        return currentMidiNote.load(std::memory_order_relaxed);
    }

    float getCurrentNoteHz() const noexcept
    {
        return currentNoteHz.load(std::memory_order_relaxed);
    }

    //==============================================================================
    // Parameter IDs
    static const juce::String p_gwEnable;
    static const juce::String p_gwGainDepth;
    static const juce::String p_gwPitchDepth;
    static const juce::String p_gwFilterDepth;
    static const juce::String p_gwTimbreDepth;
    static const juce::String p_gwPanDepth;
    static const juce::String p_gwSmoothingMs;
    static const juce::String p_gwTimeOffset;

private:
    //==============================================================================
    // APVTS
    APVTS apvts;
    static APVTS::ParameterLayout createLayout();

    //==============================================================================
    // GW data + smoothing
    GWSeries gw;
    std::atomic<bool> gwLoaded{ false };

    OnePole smAmp, smPitch, smCut, smTimbre, smPan;
    void setSmoothMs(double ms);

    //==============================================================================
    // Timebase (UI + GW sampling)
    double freeRunSeconds = 0.0;
    std::atomic<double> lastBlockTimeSec{ 0.0 };

    //==============================================================================
    // Note tracking (audio thread writes; UI reads atomics)
    std::array<bool, 128> noteDown{};   // audio-thread only
    int lastNoteOn = -1;                // audio-thread only

    std::atomic<int>   currentMidiNote{ -1 };
    std::atomic<float> currentNoteHz{ 0.0f };

    //==============================================================================
    // Note-tied playhead
    double noteTimeSec = 0.0;           // audio-thread only
    bool   wasAnyNoteDown = false;      // audio-thread only

    //==============================================================================
    // GW instrument phases
    double gwCyclePhase = 0.0;          // envelope phase (slow->peak->reset)
    double gwRatePhase = 0.0;          // pitch LFO phase

    //==============================================================================
    // Synth engine
    CosmicSynthEngine synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CosmicsynthAudioProcessor)
};
