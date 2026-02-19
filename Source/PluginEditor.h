/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "InspiralView.h"

//==============================================================================

class CosmicsynthAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    CosmicsynthAudioProcessorEditor(CosmicsynthAudioProcessor&);
    ~CosmicsynthAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // Quick access to the processor that created this editor
    CosmicsynthAudioProcessor& audioProcessor;

    // === UI controls ===
    juce::TextButton   loadCsvButton{ "Load GW CSV" };
    juce::ToggleButton gwEnable{ "GW Enable" };

    juce::Slider sGain, sPitch, sSmooth, sOffset;
    juce::Label  lGain{ {}, "Gain" };
    juce::Label  lPitch{ {}, "Pitch" };
    juce::Label  lSmooth{ {}, "Smooth (ms)" };
    juce::Label  lOffset{ {}, "Offset (s)" };

    std::unique_ptr<juce::FileChooser> chooser;

    // === APVTS attachments ===
    using APVTS = CosmicsynthAudioProcessor::APVTS;
    using SliderAttachment = APVTS::SliderAttachment;
    using ButtonAttachment = APVTS::ButtonAttachment;

    std::unique_ptr<ButtonAttachment> aEnable;
    std::unique_ptr<SliderAttachment> aGain, aPitch, aSmooth, aOffset;

    // === Visual ===
    InspiralView inspiral;

    // Small helper to style a rotary knob + label
    void styleKnob(juce::Slider& s, juce::Label& label);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CosmicsynthAudioProcessorEditor)
};
