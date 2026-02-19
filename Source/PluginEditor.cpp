/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "InspiralView.h"

//==============================================================================

CosmicsynthAudioProcessorEditor::CosmicsynthAudioProcessorEditor(CosmicsynthAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(540, 420);

    // Buttons & sliders visible
    addAndMakeVisible(loadCsvButton);
    addAndMakeVisible(gwEnable);

    addAndMakeVisible(sGain);   addAndMakeVisible(lGain);
    addAndMakeVisible(sPitch);  addAndMakeVisible(lPitch);
    addAndMakeVisible(sSmooth); addAndMakeVisible(lSmooth);
    addAndMakeVisible(sOffset); addAndMakeVisible(lOffset);

    // Visual panel
    addAndMakeVisible(inspiral);

    // Style knobs
    styleKnob(sGain, lGain);
    styleKnob(sPitch, lPitch);
    styleKnob(sSmooth, lSmooth);
    styleKnob(sOffset, lOffset);

    // APVTS attachments
    auto& vts = audioProcessor.getVTS();
    aEnable = std::make_unique<ButtonAttachment>(vts, CosmicsynthAudioProcessor::p_gwEnable, gwEnable);
    aGain = std::make_unique<SliderAttachment>(vts, CosmicsynthAudioProcessor::p_gwGainDepth, sGain);
    aPitch = std::make_unique<SliderAttachment>(vts, CosmicsynthAudioProcessor::p_gwPitchDepth, sPitch);
    aSmooth = std::make_unique<SliderAttachment>(vts, CosmicsynthAudioProcessor::p_gwSmoothingMs, sSmooth);
    aOffset = std::make_unique<SliderAttachment>(vts, CosmicsynthAudioProcessor::p_gwTimeOffset, sOffset);

    // Load button callback (calls your processor helper)
    loadCsvButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>("Select GW CSV", juce::File(), "*.csv");

            chooser->launchAsync(juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc)
                {
                    auto f = fc.getResult();
                    if (f.existsAsFile())
                    {
                        if (!audioProcessor.loadGWFile(f))
                        {
                            juce::AlertWindow::showMessageBoxAsync(
                                juce::AlertWindow::WarningIcon,
                                "Load failed",
                                "Could not parse CSV.");
                        }
                    }
                });
        };

    // Drive the visual at ~60 fps
    startTimerHz(60);
}

CosmicsynthAudioProcessorEditor::~CosmicsynthAudioProcessorEditor() = default;

//==============================================================================

void CosmicsynthAudioProcessorEditor::timerCallback()
{
    const bool loaded = audioProcessor.isGWLoaded();

    inspiral.setSeries(loaded ? &audioProcessor.getGW() : nullptr);
    inspiral.setIsActive(loaded);

    // NEW: match the visual to the actually played note
    inspiral.setNote(audioProcessor.getCurrentMidiNote(),
        audioProcessor.getCurrentNoteHz());

    // Use processor's block time (transport/free-run)
    inspiral.setTime(audioProcessor.getLastBlockTime());
}

void CosmicsynthAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void CosmicsynthAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced(10);

    // Row 1: button + toggle
    auto top = r.removeFromTop(28);
    loadCsvButton.setBounds(top.removeFromLeft(140));
    top.removeFromLeft(10);
    gwEnable.setBounds(top.removeFromLeft(110));

    r.removeFromTop(10);

    // Row 2: four knobs
    auto knobRow = r.removeFromTop(120);
    const int w = knobRow.getWidth() / 4;

    sGain.setBounds(knobRow.removeFromLeft(w).reduced(6));
    sPitch.setBounds(knobRow.removeFromLeft(w).reduced(6));
    sSmooth.setBounds(knobRow.removeFromLeft(w).reduced(6));
    sOffset.setBounds(knobRow.removeFromLeft(w).reduced(6));

    r.removeFromTop(10);

    // Remaining space: inspiral view
    inspiral.setBounds(r);
}

//==============================================================================

void CosmicsynthAudioProcessorEditor::styleKnob(juce::Slider& s, juce::Label& label)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);

    label.setJustificationType(juce::Justification::centred);
    label.attachToComponent(&s, false);
}
