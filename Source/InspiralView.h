#pragma once
#include <JuceHeader.h>
#include "GWSeries.h"

class InspiralView : public juce::Component,
    private juce::Timer
{
public:
    void setSeries(const GWSeries* s) { series = s; }
    void setIsActive(bool active) { isActive = active; }
    void setTime(double tSec) { t = tSec; }
    void setNote(int midi, float hz)
    {
        midiNote = midi;
        noteHz = hz;
    }


    InspiralView() { startTimerHz(60); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::transparentBlack);

        auto b = getLocalBounds().toFloat().reduced(10.0f);
        auto c = b.getCentre();

        // background halo
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillEllipse(b);

        if (!isActive || series == nullptr || series->empty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawFittedText("Load a GW CSV to animate", getLocalBounds(), juce::Justification::centred, 1);
            return;
        }

        auto f = series->sampleAt(t);

        // Map: frequency -> shrink radius, phase -> angle
        // You can refine mapping later; this is a good “first working” mapping.
        const float fHz = (float)juce::jlimit(20.0, 800.0, f.fgw);
        const float phase = (float)f.psi;

        // radius goes from ~45% panel to ~10% as f increases
        const float rMax = 0.45f * juce::jmin(b.getWidth(), b.getHeight());
        const float rMin = 0.10f * juce::jmin(b.getWidth(), b.getHeight());
        const float x = (fHz - 20.0f) / (800.0f - 20.0f); // 0..1
        const float sep = rMax + (rMin - rMax) * x;

        juce::Point<float> p1{ c.x + sep * std::cos(phase), c.y + sep * std::sin(phase) };
        juce::Point<float> p2{ c.x - sep * std::cos(phase), c.y - sep * std::sin(phase) };

        // orbit trail ring
        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawEllipse(c.x - sep, c.y - sep, 2 * sep, 2 * sep, 2.0f);

        // bodies
        const float m = 8.0f;
        g.setColour(juce::Colours::deepskyblue.withAlpha(0.9f));
        g.fillEllipse(p1.x - m, p1.y - m, 2 * m, 2 * m);

        g.setColour(juce::Colours::orange.withAlpha(0.9f));
        g.fillEllipse(p2.x - m, p2.y - m, 2 * m, 2 * m);

        // small readout
        g.setColour(juce::Colours::white.withAlpha(0.65f));
        g.setFont(12.0f);
        g.drawText("fgw: " + juce::String(fHz, 1) + " Hz", 8, 8, getWidth() - 16, 16, juce::Justification::left);
    }

private:
    void timerCallback() override { repaint(); }
    int midiNote = -1;
    float noteHz = 0.0f;


    const GWSeries* series = nullptr;
    bool isActive = false;
    double t = 0.0;
};
