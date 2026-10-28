/*
    GWLookAndFeel.h  —  dark / purple "GW Synth" visual language
    Header-only (no Projucer changes needed). Ported from the GWSynth mock-up
    and updated for JUCE 8 (FontOptions, no deprecated Font constructors).
*/
#pragma once
#include <JuceHeader.h>

namespace GWTheme
{
    inline const juce::Colour bg{ 0xff0a0d16 };
    inline const juce::Colour panelBg{ 0xff0e1220 };
    inline const juce::Colour panelBorder{ 0xff1c2333 };
    inline const juce::Colour purple{ 0xff8b7cf6 };
    inline const juce::Colour purpleBright{ 0xffa78bfa };
    inline const juce::Colour purpleGlow{ 0xff7c5cff };
    inline const juce::Colour amber{ 0xfff5b971 };   // "now" / read-head accent
    inline const juce::Colour textPrimary{ 0xfff1f2f6 };
    inline const juce::Colour textSecondary{ 0xff7b8194 };
    inline const juce::Colour textMuted{ 0xff4b5266 };
    inline const juce::Colour knobBody{ 0xff141827 };
    inline const juce::Colour knobTrack{ 0xff232a3d };

    inline juce::Font sans(float height, bool bold = false)
    {
        return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
    }
    inline juce::Font mono(float height)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
    }
    // UTF-8 literal helper (keeps symbols correct whatever the compiler code page)
    inline juce::String u8(const char* text) { return juce::String(juce::CharPointer_UTF8(text)); }

    inline void glowStroke(juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.6f)
    {
        // one soft halo + the line (each extra pass costs a full path stroke)
        g.setColour(c.withAlpha(0.14f));
        g.strokePath(p, juce::PathStrokeType(width + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    inline void drawPanel(juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour(panelBg);
        g.fillRoundedRectangle(r, 10.0f);
        g.setColour(panelBorder);
        g.drawRoundedRectangle(r.reduced(0.5f), 10.0f, 1.0f);
    }
}

//==============================================================================
class GWLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GWLookAndFeel()
    {
        using namespace GWTheme;
        setColour(juce::ResizableWindow::backgroundColourId, bg);
        setColour(juce::Label::textColourId, textSecondary);
        setColour(juce::ComboBox::backgroundColourId, knobBody);
        setColour(juce::ComboBox::outlineColourId, panelBorder);
        setColour(juce::ComboBox::textColourId, textPrimary);
        setColour(juce::ComboBox::arrowColourId, textSecondary);
        setColour(juce::PopupMenu::backgroundColourId, panelBg);
        setColour(juce::PopupMenu::textColourId, textPrimary);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, purpleGlow.withAlpha(0.35f));
        setColour(juce::PopupMenu::highlightedTextColourId, textPrimary);
        setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour(juce::TextButton::textColourOffId, textSecondary);
        setColour(juce::TextButton::textColourOnId, textPrimary);
        setColour(juce::AlertWindow::backgroundColourId, panelBg);
        setColour(juce::AlertWindow::textColourId, textPrimary);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
        float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override
    {
        using namespace GWTheme;
        auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(4.0f);
        const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const auto centre = bounds.getCentre();
        const float radius = diameter * 0.5f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const bool enabled = slider.isEnabled();

        const float track = juce::jmax(2.5f, radius * 0.14f);
        const float arcR = radius - track * 0.5f;
        juce::Path bgArc;
        bgArc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(knobTrack);
        g.strokePath(bgArc, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (sliderPos > 0.0001f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, angle, true);
            if (enabled)
                for (int i = 3; i >= 1; --i)
                {
                    g.setColour(purpleGlow.withAlpha(0.10f));
                    g.strokePath(valueArc, juce::PathStrokeType(track + (float)i * 2.5f,
                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
            g.setColour(enabled ? purple : textMuted.withAlpha(0.7f));
            g.strokePath(valueArc, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float bodyR = radius - track - 3.0f;
        juce::ColourGradient grad(knobBody.brighter(0.15f), centre.x, centre.y - bodyR,
            knobBody.darker(0.4f), centre.x, centre.y + bodyR, false);
        g.setGradientFill(grad);
        g.fillEllipse(juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(centre));
        g.setColour(panelBorder);
        g.drawEllipse(juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(centre), 1.0f);

        juce::Path pointer;
        const float len = bodyR * 0.68f, start = bodyR * 0.22f;
        pointer.addRoundedRectangle(-1.3f, -len, 2.6f, len - start, 1.3f);
        g.setColour(enabled ? textPrimary : textMuted);
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre));
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&) override
    {
        using namespace GWTheme;
        auto b = juce::Rectangle<float>(0, 0, (float)width, (float)height).reduced(0.5f);
        g.setColour(knobBody);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(panelBorder);
        g.drawRoundedRectangle(b, 6.0f, 1.0f);
        auto chev = b.removeFromRight(26.0f).reduced(9.0f, (float)height * 0.38f);
        juce::Path p;
        p.startNewSubPath(chev.getX(), chev.getY());
        p.lineTo(chev.getCentreX(), chev.getBottom());
        p.lineTo(chev.getRight(), chev.getY());
        g.setColour(textSecondary);
        g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return GWTheme::sans(14.0f, true); }
    juce::Font getPopupMenuFont() override { return GWTheme::sans(14.0f); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return GWTheme::sans(12.5f, true); }
    juce::Font getLabelFont(juce::Label& l) override { return l.getFont(); }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(10, 1, box.getWidth() - 36, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
        label.setJustificationType(juce::Justification::centred);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        using namespace GWTheme;
        auto b = button.getLocalBounds().toFloat().reduced(0.5f);
        const bool on = button.getToggleState();
        g.setColour(on ? purpleGlow.withAlpha(0.20f) : (highlighted ? panelBorder.withAlpha(0.6f) : knobBody));
        g.fillRoundedRectangle(b, b.getHeight() * 0.5f);
        g.setColour(on ? purple.withAlpha(0.95f) : panelBorder);
        g.drawRoundedRectangle(b.reduced(0.5f), b.getHeight() * 0.5f, 1.2f);

        // status dot
        auto dot = juce::Rectangle<float>(7.0f, 7.0f).withCentre({ b.getX() + 16.0f, b.getCentreY() });
        g.setColour(on ? purpleBright : textMuted);
        g.fillEllipse(dot);

        g.setColour(on ? textPrimary : textSecondary);
        g.setFont(sans(12.5f, true));
        g.drawText(button.getButtonText(), b.withTrimmedLeft(18.0f), juce::Justification::centred);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
        bool highlighted, bool down) override
    {
        using namespace GWTheme;
        auto b = button.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(down ? purpleGlow.withAlpha(0.25f) : (highlighted ? panelBorder : knobBody));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(highlighted ? purple.withAlpha(0.7f) : panelBorder);
        g.drawRoundedRectangle(b, 6.0f, 1.0f);
    }
};