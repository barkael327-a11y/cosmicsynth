/*
  ==============================================================================
    GWVisuals.h  —  physics displays for the Cosmic Synth editor (header-only)

    Every display is driven by the engine's real state or by the same
    equations the audio uses:
      OrbitView    — the system making the sound (per preset)
      StrainView   — h(t) of the loaded data with read-heads, the Kepler strain
                     of the Kozai-Lidov binary, or the lens amplification |F(f)|
      Spectrogram  — live FFT of the plugin's output (own radix-2 FFT, so no
                     juce_dsp module is needed)
  ==============================================================================
*/
#pragma once
#include <JuceHeader.h>
#include <complex>
#include <functional>
#include "GWLookAndFeel.h"
#include "GWSeries.h"

//==============================================================================
// Knob + name + value, stacked. Value text comes from a formatter so every
// knob shows real units.
class LabelledKnob : public juce::Component
{
public:
    explicit LabelledKnob(const juce::String& name) : labelText(name.toUpperCase())
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        // 7 o'clock → 5 o'clock (JUCE requires positive angles: 225° … 495°)
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible(slider);
    }

    juce::Slider slider;
    std::function<juce::String(double)> formatter;

    void setActive(bool shouldBeActive)
    {
        slider.setEnabled(shouldBeActive);
        repaint();
    }

    void resized() override
    {
        auto b = getLocalBounds();
        slider.setBounds(b.removeFromTop(juce::jmin(b.getWidth(), b.getHeight() - 36)));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace GWTheme;
        auto b = getLocalBounds();
        b.removeFromTop(slider.getHeight());
        const bool on = slider.isEnabled();
        g.setFont(sans(11.0f, true));
        g.setColour(on ? textSecondary : textMuted);
        g.drawText(labelText, b.removeFromTop(17), juce::Justification::centred);
        g.setFont(mono(12.5f));
        g.setColour(on ? textPrimary.withAlpha(0.9f) : textMuted);
        g.drawText(formatter ? formatter(slider.getValue()) : juce::String(slider.getValue(), 2),
            b.removeFromTop(17), juce::Justification::centred);
    }

private:
    juce::String labelText;
};

//==============================================================================
// Four name/value rows.
class ReadoutColumn : public juce::Component
{
public:
    std::array<std::pair<juce::String, juce::String>, 4> rows;
    bool rightAligned = false;

    void set(int i, const juce::String& name, const juce::String& value)
    {
        if (rows[(size_t)i].first != name || rows[(size_t)i].second != value)
        {
            rows[(size_t)i] = { name, value };
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        using namespace GWTheme;
        const float rowH = (float)getHeight() / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            auto r = juce::Rectangle<float>(0.0f, rowH * (float)i, (float)getWidth(), rowH);
            g.setFont(sans(12.5f));
            g.setColour(textSecondary);
            g.drawText(rows[(size_t)i].first, r, juce::Justification::centredLeft);
            g.setFont(mono(13.0f));
            g.setColour(textPrimary);
            g.drawText(rows[(size_t)i].second, r, juce::Justification::centredRight);
        }
    }
};

//==============================================================================
// The system making the sound. The editor sets `state` each frame and calls
// advance(dt); positions follow the same physics as the audio.
class OrbitView : public juce::Component
{
public:
    enum class Mode { Binary, Pluck, Bell, Kozai, Lens };

    struct State
    {
        Mode  mode = Mode::Binary;
        // binary / pluck
        bool  event = false;
        float fRatio = 1.0f, split = 1.0f, orbitHz = 0.0f, postMerger = -1.0f;
        float pluckFrac = 1.0f; int pluckCount = 0;
        float remnantScale = 1.64f;   // horizon radius of remnant / each body (from mass loss + spin)
        // bell
        float bellEnv = 0.0f;
        // Kozai-Lidov
        float e = 0.1f, orbitPhase = 0.0f, tiltDeg = 57.0f;
        // lens
        float lensMass = 22607.0f, lensDelaySec = 0.45f, muPlus = 1.59f, muMinus = 0.59f;
    } state;

    void advance(float dt)
    {
        time += dt;
        const bool binaryish = (state.mode == Mode::Binary);
        if (binaryish)
        {
            const float rate = state.event && state.postMerger < 0.0f ? state.orbitHz : 0.12f;
            angle += juce::MathConstants<float>::twoPi * rate * dt;
            // gravitational waves leave at twice the orbital frequency
            const float waveHz = state.event ? juce::jmax(0.3f, 2.0f * rate) : 0.24f;
            rippleClock += dt * waveHz;
            if (state.postMerger >= 0.0f) rippleClock += dt * 3.0f * std::exp(-state.postMerger / 0.9f);
        }
        else if (state.mode == Mode::Bell)
            rippleClock += dt * 4.0f * juce::jmax(0.05f, state.bellEnv);
        else if (state.mode == Mode::Pluck)
            rippleClock += dt * (state.pluckFrac < 1.0f ? 6.0f : 0.8f);
        else
            rippleClock += dt * 0.5f;

        while (rippleClock >= 1.0f)
        {
            rippleClock -= 1.0f;
            float strength = 0.6f;
            if (state.mode == Mode::Bell) strength = state.bellEnv;
            if (state.mode == Mode::Binary) strength = state.event ? 0.5f + 0.5f * juce::jmin(1.0f, (state.fRatio - 1.0f) / 5.0f) : 0.25f;
            ripples.push_back({ time, strength });
        }
        while (!ripples.empty() && time - ripples.front().born > 2.5f) ripples.erase(ripples.begin());
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        switch (state.mode)
        {
        case Mode::Binary: paintBinary(g, b); break;
        case Mode::Pluck:  paintPluck(g, b);  break;
        case Mode::Bell:   paintBell(g, b);   break;
        case Mode::Kozai:  paintKozai(g, b);  break;
        case Mode::Lens:   paintLens(g, b);   break;
        }
    }

private:
    struct Ripple { float born, strength; };
    std::vector<Ripple> ripples;
    float time = 0.0f, angle = 0.0f, rippleClock = 0.0f;

    static void drawHole(juce::Graphics& g, juce::Point<float> p, float r, float glow = 1.0f)
    {
        using namespace GWTheme;
        for (int i = 4; i >= 1; --i)
        {
            g.setColour(purpleGlow.withAlpha(0.055f * glow));
            g.fillEllipse(juce::Rectangle<float>((r + (float)i * 3.0f) * 2.0f, (r + (float)i * 3.0f) * 2.0f).withCentre(p));
        }
        g.setColour(juce::Colour(0xff04050a));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(p));
        g.setColour(purpleBright.withAlpha(0.85f));                 // photon ring
        g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(p), 1.4f);
    }

    void drawRipples(juce::Graphics& g, juce::Point<float> c, float maxR, float squash)
    {
        const float edge = juce::jmin((float)getHeight() * 0.5f / squash, (float)getWidth() * 0.5f) - 2.0f;
        for (auto& rp : ripples)
        {
            const float age = time - rp.born;
            const float rr = 10.0f + age / 2.5f * maxR;
            const float fade = juce::jlimit(0.0f, 1.0f, (edge - rr) / (0.35f * edge));
            const float a = rp.strength * 0.35f * (1.0f - age / 2.5f) * fade;
            if (a <= 0.005f) continue;
            g.setColour(GWTheme::purple.withAlpha(a));
            g.drawEllipse(juce::Rectangle<float>(rr * 2.0f, rr * 2.0f * squash).withCentre(c), 1.2f);
        }
    }

    // Two equal-mass bodies; separation r ∝ f^(-2/3) (Kepler), fuse at merger
    void paintBinary(juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto c = b.getCentre();
        const float squash = 0.42f;                       // viewed from ~65° inclination
        const float R0 = juce::jmin(b.getWidth() * 0.36f, b.getHeight() * 0.95f);
        const float bodyR = juce::jmax(6.0f, R0 * 0.085f);
        drawRipples(g, c, b.getWidth() * 0.55f, squash);

        float r = R0 * std::pow(juce::jmax(1.0f, state.fRatio), -2.0f / 3.0f);
        if (state.postMerger >= 0.0f) r *= state.split;

        juce::Path trail;
        trail.addEllipse(c.x - r, c.y - r * squash, r * 2.0f, r * 2.0f * squash);
        g.setColour(GWTheme::purple.withAlpha(0.22f));
        g.strokePath(trail, juce::PathStrokeType(1.0f));

        if (state.postMerger >= 0.0f && state.split < 0.08f)
        {
            drawHole(g, c, bodyR * state.remnantScale, 1.5f);
            return;
        }
        const juce::Point<float> off(r * std::cos(angle), r * squash * std::sin(angle));
        drawHole(g, c + off, bodyR);
        drawHole(g, c - off, bodyR);
    }

    // Each note: a fast inspiral. Newtonian chirp: r ∝ τ^(1/4), phase ∝ τ^(5/8)
    void paintPluck(juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto c = b.getCentre();
        const float squash = 0.42f;
        const float R0 = juce::jmin(b.getWidth() * 0.34f, b.getHeight() * 0.9f);
        const float bodyR = juce::jmax(6.0f, R0 * 0.085f);
        drawRipples(g, c, b.getWidth() * 0.55f, squash);

        const float tau = 1.0f - juce::jlimit(0.0f, 1.0f, state.pluckFrac);
        if (tau <= 0.0f)
        {
            drawHole(g, c, bodyR * state.remnantScale, 1.3f);
            return;
        }
        const float r = R0 * std::pow(tau, 0.25f);
        const float th = (float)state.pluckCount * 1.3f - juce::MathConstants<float>::twoPi * 5.0f * std::pow(tau, 0.625f);
        juce::Path spiral;                                 // the path still to travel
        for (int i = 0; i <= 80; ++i)
        {
            const float tt = tau * (1.0f - (float)i / 80.0f);
            const float rr = R0 * std::pow(juce::jmax(tt, 1e-4f), 0.25f);
            const float a = (float)state.pluckCount * 1.3f - juce::MathConstants<float>::twoPi * 5.0f * std::pow(juce::jmax(tt, 1e-4f), 0.625f);
            const juce::Point<float> p(c.x + rr * std::cos(a), c.y + rr * squash * std::sin(a));
            if (i == 0) spiral.startNewSubPath(p); else spiral.lineTo(p);
        }
        g.setColour(GWTheme::purple.withAlpha(0.25f));
        g.strokePath(spiral, juce::PathStrokeType(1.0f));
        const juce::Point<float> off(r * std::cos(th), r * squash * std::sin(th));
        drawHole(g, c + off, bodyR);
        drawHole(g, c - off, bodyR);
    }

    // The remnant ringing in its l = m = 2 quasinormal mode (a rotating bar shape)
    void paintBell(juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto c = b.getCentre();
        const float R = juce::jmin(b.getWidth(), b.getHeight()) * 0.2f;
        drawRipples(g, c, b.getWidth() * 0.55f, 0.6f);
        const float A = 0.12f * juce::jlimit(0.0f, 1.0f, state.bellEnv);
        juce::Path horizon;
        for (int i = 0; i <= 96; ++i)
        {
            const float phi = juce::MathConstants<float>::twoPi * (float)i / 96.0f;
            const float rr = R * (1.0f + A * std::cos(2.0f * (phi - 2.4f * time)));
            const juce::Point<float> p(c.x + rr * std::cos(phi), c.y + rr * std::sin(phi));
            if (i == 0) horizon.startNewSubPath(p); else horizon.lineTo(p);
        }
        horizon.closeSubPath();
        for (int i = 4; i >= 1; --i)
        {
            g.setColour(GWTheme::purpleGlow.withAlpha(0.05f + 0.06f * state.bellEnv));
            g.strokePath(horizon, juce::PathStrokeType((float)i * 4.0f));
        }
        g.setColour(juce::Colour(0xff04050a));
        g.fillPath(horizon);
        g.setColour(GWTheme::purpleBright.withAlpha(0.6f + 0.4f * state.bellEnv));
        g.strokePath(horizon, juce::PathStrokeType(1.6f));
    }

    // Inner binary on a Kepler ellipse of the current e (burst at periapsis),
    // distant third body on a tilted outer orbit
    void paintKozai(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        const auto c = b.getCentre();
        const float e = juce::jlimit(0.0f, 0.95f, state.e);

        // outer orbit (projected circle, tilted by the mutual inclination)
        const float Ro = juce::jmin(b.getWidth() * 0.46f, b.getHeight() * 1.2f);
        const float tilt = std::cos(juce::degreesToRadians(state.tiltDeg));
        g.setColour(textMuted.withAlpha(0.6f));
        juce::Path outer;
        outer.addEllipse(c.x - Ro, c.y - Ro * 0.35f * juce::jmax(0.15f, tilt), Ro * 2.0f, Ro * 0.7f * juce::jmax(0.15f, tilt));
        const float dashes[] = { 3.0f, 5.0f };
        juce::Path dashed;
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, outer, dashes, 2);
        g.fillPath(dashed);
        const float ao = time * 0.35f;
        const juce::Point<float> third(c.x + Ro * std::cos(ao), c.y + Ro * 0.35f * juce::jmax(0.15f, tilt) * std::sin(ao));
        g.setColour(textSecondary);
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(third));

        // inner Kepler orbit, primary at the focus
        const float a = juce::jmin(b.getWidth() * 0.24f, b.getHeight() * 0.40f / juce::jmax(0.3f, std::sqrt(1.0f - e * e)));
        const float bb = a * std::sqrt(1.0f - e * e);
        const juce::Point<float> focus(c.x + a * e * 0.5f, c.y);
        const float cx = focus.x - a * e;
        juce::Path orbit;
        orbit.addEllipse(cx - a, c.y - bb, a * 2.0f, bb * 2.0f);
        g.setColour(purple.withAlpha(0.35f));
        g.strokePath(orbit, juce::PathStrokeType(1.2f));

        double E = juce::MathConstants<double>::twoPi * state.orbitPhase;
        const double Mean = E;
        for (int i = 0; i < 30; ++i) E -= (E - e * std::sin(E) - Mean) / (1.0 - e * std::cos(E));
        const float nu = 2.0f * std::atan2(std::sqrt(1.0f + e) * std::sin((float)E / 2.0f), std::sqrt(1.0f - e) * std::cos((float)E / 2.0f));
        const float r = a * (1.0f - e * e) / (1.0f + e * std::cos(nu));
        const juce::Point<float> comp(focus.x + r * std::cos(nu), focus.y - r * std::sin(nu));

        // burst glow ∝ strain envelope ~ 1/r²
        const float burst = juce::jlimit(0.0f, 1.0f, std::pow(a * (1.0f - e) / r, 2.0f));
        for (int i = 5; i >= 1; --i)
        {
            g.setColour(amber.withAlpha(0.06f * burst));
            g.fillEllipse(juce::Rectangle<float>((float)i * 9.0f, (float)i * 9.0f).withCentre((focus + comp) * 0.5f));
        }
        drawHole(g, focus, 11.0f);
        drawHole(g, comp, 7.0f);
    }

    // Source → lens → detector, two image paths (widths ∝ √|μ|)
    void paintLens(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        const float cy = b.getCentreY();
        const juce::Point<float> src(b.getX() + b.getWidth() * 0.10f, cy);
        const juce::Point<float> det(b.getRight() - b.getWidth() * 0.10f, cy);
        const juce::Point<float> lens(b.getCentreX() - b.getWidth() * 0.05f, cy);
        const float t01 = juce::jlimit(0.0f, 1.0f, (std::log(state.lensMass) - std::log(3.0f)) / (std::log(50000.0f) - std::log(3.0f)));
        const float RE = 10.0f + 30.0f * t01;
        const float xp = 1.2808f, xm = 0.7808f;                 // image positions for y = 0.5 (Einstein radii)
        const juce::Point<float> top(lens.x, cy - RE * xp * 1.6f), bot(lens.x, cy + RE * xm * 1.6f);

        auto ray = [&](juce::Point<float> via, float mu, float offset)
            {
                juce::Path p;
                p.startNewSubPath(src);
                p.quadraticTo(via.x, via.y + (via.y - cy) * 0.9f, det.x, det.y);
                g.setColour(purple.withAlpha(0.55f));
                g.strokePath(p, juce::PathStrokeType(0.8f + 2.2f * std::sqrt(mu)));
                // travelling pulse
                const float L = p.getLength();
                const float u = std::fmod(time * 0.45f + 1.0f - offset, 1.0f);
                const auto q = p.getPointAlongPath(u * L);
                for (int i = 3; i >= 1; --i)
                {
                    g.setColour(amber.withAlpha(0.12f));
                    g.fillEllipse(juce::Rectangle<float>((float)i * 5.0f, (float)i * 5.0f).withCentre(q));
                }
                g.setColour(amber);
                g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f).withCentre(q));
            };
        // second image arrives later: show its lag as a fraction of the loop (log-scaled)
        const float lag = juce::jlimit(0.02f, 0.45f, 0.08f * std::log10(1.0f + state.lensDelaySec * 1.0e4f));
        ray(top, state.muPlus, 0.0f);
        ray(bot, state.muMinus, lag);

        // lens: black hole + Einstein ring
        juce::Path ring;
        ring.addEllipse(lens.x - RE, cy - RE, RE * 2.0f, RE * 2.0f);
        const float dashes[] = { 2.0f, 4.0f };
        juce::Path dashed;
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, ring, dashes, 2);
        g.setColour(purpleBright.withAlpha(0.6f));
        g.fillPath(dashed);
        drawHole(g, lens, juce::jmax(4.0f, RE * 0.28f));

        // source: a tiny binary; detector: an L-shaped interferometer
        const float a = time * 3.0f;
        g.setColour(textPrimary);
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(src + juce::Point<float>(4.0f * std::cos(a), 2.0f * std::sin(a))));
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(src - juce::Point<float>(4.0f * std::cos(a), 2.0f * std::sin(a))));
        juce::Path L;
        L.startNewSubPath(det.x - 12.0f, det.y - 12.0f);
        L.lineTo(det.x, det.y);
        L.lineTo(det.x + 12.0f, det.y - 12.0f);
        g.setColour(textSecondary);
        g.strokePath(L, juce::PathStrokeType(2.0f));
        g.setFont(sans(10.5f, true));
        g.setColour(textMuted);
        g.drawText("SOURCE", juce::Rectangle<float>(src.x - 30.0f, cy + 22.0f, 60.0f, 14.0f), juce::Justification::centred);
        g.drawText("LENS", juce::Rectangle<float>(lens.x - 30.0f, cy + RE + 26.0f, 60.0f, 14.0f), juce::Justification::centred);
        g.drawText("DETECTOR", juce::Rectangle<float>(det.x - 34.0f, cy + 22.0f, 68.0f, 14.0f), juce::Justification::centred);
    }
};

//==============================================================================
// Bottom-left display. Three modes:
//   Data  — h(t) of the loaded waveform (full + zoom on merger) with read-heads
//   Kozai — strain h+(t) of the eccentric Kepler orbit, scrolling, plus e(t)
//   Lens  — amplification |F(f)| of the point-mass lens (the model the audio uses)
class StrainView : public juce::Component
{
public:
    enum class Mode { Data, Kozai, Lens };
    struct Head { double t; juce::String label; };

    Mode mode = Mode::Data;
    // data
    const GWSeries* series = nullptr;
    int dataVersion = -1;
    double tMerger = 0.0;
    std::vector<Head> heads;
    juce::String phaseLabel;
    bool ringdownGlow = false;
    // Kozai
    float e = 0.1f, orbitPhase = 0.0f, cycle = 0.0f;
    std::function<float(float)> eOfCycle;   // e(cycle position) for the current tilt
    // lens
    float lensMass = 22607.0f;
    float lensAp = 1.2613f, lensAm = 0.7687f, lensNorm = 0.6772f, lensT = 1.0103f;

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        if (mode == Mode::Data)
        {
            // the waveform never changes: draw it once, then only the moving parts
            drawCached(g, "data|" + juce::String(dataVersion) + "|" + juce::String(tMerger, 6),
                [&](juce::Graphics& lg) { paintDataStatic(lg, b); });
            paintDataDynamic(g, b);
        }
        else if (mode == Mode::Kozai) paintKozai(g, b);
        else
            drawCached(g, "lens|" + juce::String(lensMass, 3) + "|" + juce::String(lensAp, 4) + "|" + juce::String(lensAm, 4),
                [&](juce::Graphics& lg) { paintLens(lg, b); });
    }

private:
    std::vector<float> colMin, colMax;
    int cachedVersion = -2, cachedWidth = -1;
    juce::Image layer;
    juce::String layerKey;
    std::vector<float> klOrbit;       // strain over one orbit for klOrbitE (cached)
    float klOrbitE = -1.0f;

    // Draw `drawStatic` into an image once per key/size/scale, then blit it.
    template <typename Fn>
    void drawCached(juce::Graphics& g, const juce::String& key, Fn&& drawStatic)
    {
        const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const juce::String k = key + "|" + juce::String(getWidth()) + "x" + juce::String(getHeight()) + "@" + juce::String(scale, 3);
        if (k != layerKey || !layer.isValid())
        {
            layer = juce::Image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt((float)getWidth() * scale)),
                juce::jmax(1, juce::roundToInt((float)getHeight() * scale)), true);
            juce::Graphics lg(layer);
            lg.addTransform(juce::AffineTransform::scale(scale));
            drawStatic(lg);
            layerKey = k;
        }
        if (scale == 1.0f) g.drawImageAt(layer, 0, 0);                 // plain blit (fast path)
        else               g.drawImageTransformed(layer, juce::AffineTransform::scale(1.0f / scale));
    }

    struct DataGeom
    {
        juce::Rectangle<float> full, zoom, axis;
        double t0 = 0, tN = 0, tm = 0, tEnd = 0, z0 = 0, z1 = 0;
        float xFull(double t) const { return full.getX() + (float)((t - t0) / (tEnd - t0)) * full.getWidth(); }
        float xZoom(double t) const { return zoom.getX() + (float)((t - z0) / (z1 - z0)) * zoom.getWidth(); }
    };
    DataGeom dataGeom(juce::Rectangle<float> b) const
    {
        DataGeom d;
        d.t0 = series->getT0(); d.tN = series->getTN();
        d.tm = tMerger > d.t0 ? tMerger : d.tN;
        d.tEnd = juce::jmin(d.tN, d.tm + 0.02);
        d.z0 = d.tm - 0.060; d.z1 = juce::jmin(d.tN, d.tm + 0.020);
        auto area = b.reduced(8.0f, 4.0f);
        d.axis = area.removeFromBottom(16.0f);
        d.zoom = area.removeFromRight(area.getWidth() * 0.34f);
        area.removeFromRight(14.0f);
        d.full = area;
        return d;
    }

    static void axisText(juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, juce::Justification j)
    {
        g.setFont(GWTheme::mono(10.5f));
        g.setColour(GWTheme::textMuted);
        g.drawText(s, r, j);
    }

    void paintDataStatic(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        if (series == nullptr || series->empty()) return;
        const auto d = dataGeom(b);
        const auto& full = d.full; const auto& zoom = d.zoom; const auto& axis = d.axis;

        // ---- full series: min/max per pixel column (the classic growing band)
        const int W = juce::jmax(2, (int)full.getWidth());
        if (cachedVersion != dataVersion || cachedWidth != W)
        {
            colMin.assign((size_t)W, 0.0f); colMax.assign((size_t)W, 0.0f);
            const size_t N = series->size();
            for (size_t i = 0; i < N; ++i)
            {
                const auto& f = series->frame(i);
                if (f.t > d.tEnd) break;
                const int c = juce::jlimit(0, W - 1, (int)((f.t - d.t0) / (d.tEnd - d.t0) * (W - 1)));
                colMin[(size_t)c] = juce::jmin(colMin[(size_t)c], (float)f.h);
                colMax[(size_t)c] = juce::jmax(colMax[(size_t)c], (float)f.h);
            }
            cachedVersion = dataVersion; cachedWidth = W;
        }
        const float mid = full.getCentreY(), amp = full.getHeight() * 0.46f;
        juce::Path band;
        band.startNewSubPath(full.getX(), mid - colMax[0] * amp);
        for (int c = 1; c < W; ++c) band.lineTo(full.getX() + (float)c, mid - colMax[(size_t)c] * amp);
        for (int c = W - 1; c >= 0; --c) band.lineTo(full.getX() + (float)c, mid - colMin[(size_t)c] * amp);
        band.closeSubPath();
        g.setColour(purple.withAlpha(0.55f));
        g.fillPath(band);
        g.setColour(purpleBright.withAlpha(0.35f));
        g.strokePath(band, juce::PathStrokeType(1.0f));

        // time axis (seconds before merger)
        for (int sec = (int)std::floor(d.t0 - d.tm); sec <= 0; ++sec)
        {
            const double t = d.tm + sec;
            if (t < d.t0) continue;
            const float x = d.xFull(t);
            g.setColour(panelBorder);
            g.drawVerticalLine((int)x, full.getY(), full.getBottom());
            if (sec == 0) axisText(g, "0 s", { x - 40.0f, axis.getY(), 40.0f, 14.0f }, juce::Justification::centredRight);
            else axisText(g, juce::String(sec), { x - 20.0f, axis.getY(), 40.0f, 14.0f }, juce::Justification::centred);
        }

        // ---- zoom: last 60 ms + ringdown, drawn as the actual waveform
        g.setColour(panelBorder);
        g.drawRoundedRectangle(zoom.expanded(4.0f, 2.0f), 5.0f, 1.0f);
        juce::Path w;
        const int ZW = juce::jmax(2, (int)zoom.getWidth() * 2);
        for (int i = 0; i < ZW; ++i)
        {
            const double t = d.z0 + (d.z1 - d.z0) * i / (ZW - 1);
            const float y = zoom.getCentreY() - (float)series->sampleAt(t).h * zoom.getHeight() * 0.46f;
            if (i == 0) w.startNewSubPath(zoom.getX(), y); else w.lineTo(d.xZoom(t), y);
        }
        glowStroke(g, w, purple, 1.3f);
        const float xm = d.xZoom(d.tm);
        g.setColour(textMuted);
        g.drawVerticalLine((int)xm, zoom.getY(), zoom.getBottom());
        axisText(g, "-60 ms", { zoom.getX(), axis.getY(), 50.0f, 14.0f }, juce::Justification::centredLeft);
        axisText(g, "merger", { xm - 25.0f, axis.getY(), 50.0f, 14.0f }, juce::Justification::centred);
        axisText(g, "+20", { zoom.getRight() - 30.0f, axis.getY(), 30.0f, 14.0f }, juce::Justification::centredRight);
        g.setFont(sans(10.0f, true));
        g.setColour(textMuted);
        g.drawText("INSPIRAL", juce::Rectangle<float>(zoom.getX() + 4.0f, zoom.getY(), 80.0f, 12.0f), juce::Justification::centredLeft);
        g.drawText("RINGDOWN", juce::Rectangle<float>(xm + 4.0f, zoom.getY(), 70.0f, 12.0f), juce::Justification::centredLeft);
    }

    void paintDataDynamic(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        if (series == nullptr || series->empty()) return;
        const auto d = dataGeom(b);
        const auto& full = d.full; const auto& zoom = d.zoom;
        if (ringdownGlow)
        {
            const float xm = d.xZoom(d.tm);
            g.setColour(amber.withAlpha(0.12f));
            g.fillRect(juce::Rectangle<float>(xm, zoom.getY(), d.xZoom(juce::jmin(d.z1, d.tm + 0.006)) - xm, zoom.getHeight()));
        }
        // ---- read-heads (where the current preset is reading the data)
        for (auto& h : heads)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool inZoom = pass == 1;
                if (inZoom && (h.t < d.z0 || h.t > d.z1)) continue;
                const float x = inZoom ? d.xZoom(h.t) : d.xFull(juce::jmin(h.t, d.tEnd));
                const auto& box = inZoom ? zoom : full;
                g.setColour(amber.withAlpha(0.15f));
                g.fillRect(juce::Rectangle<float>(x - 3.0f, box.getY(), 6.0f, box.getHeight()));
                g.setColour(amber);
                g.drawVerticalLine((int)x, box.getY(), box.getBottom());
                if (!inZoom && h.label.isNotEmpty())
                {
                    g.setFont(mono(10.5f));
                    g.drawText(h.label, juce::Rectangle<float>(juce::jmin(x + 5.0f, full.getRight() - 120.0f), full.getY(), 120.0f, 13.0f),
                        juce::Justification::centredLeft);
                }
            }
        }
        if (phaseLabel.isNotEmpty())
        {
            g.setFont(sans(11.0f, true));
            g.setColour(amber);
            g.drawText(phaseLabel, juce::Rectangle<float>(full.getX(), full.getBottom() - 14.0f, full.getWidth(), 14.0f), juce::Justification::centredLeft);
        }
    }

    void paintKozai(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        auto area = b.reduced(10.0f, 4.0f);
        auto strip = area.removeFromBottom(46.0f);
        area.removeFromBottom(6.0f);

        // strain h+ (face-on) of a Kepler orbit, 3 orbits around "now".
        // One orbit is computed per eccentricity value and cached.
        const float eq = std::round(juce::jlimit(0.0f, 0.95f, e) * 500.0f) / 500.0f;
        if (eq != klOrbitE)
        {
            const int NO = 1024;
            const double ecc = eq, dM = juce::MathConstants<double>::twoPi / NO;
            std::vector<double> q((size_t)NO);
            for (int i = 0; i < NO; ++i)
            {
                const double Mm = dM * i;
                double E = ecc > 0.8 ? juce::MathConstants<double>::pi : Mm;
                for (int k = 0; k < 50; ++k)
                {
                    const double dE = (E - ecc * std::sin(E) - Mm) / (1.0 - ecc * std::cos(E));
                    E -= dE;
                    if (std::abs(dE) < 1e-10) break;
                }
                const double nu = 2.0 * std::atan2(std::sqrt(1.0 + ecc) * std::sin(E / 2), std::sqrt(1.0 - ecc) * std::cos(E / 2));
                const double r = (1.0 - ecc * ecc) / (1.0 + ecc * std::cos(nu));
                const double x = r * std::cos(nu), y = r * std::sin(nu);
                q[(size_t)i] = x * x - y * y;
            }
            klOrbit.assign((size_t)NO, 0.0f);
            float peak = 1e-9f;
            for (int i = 0; i < NO; ++i)
            {
                const double v = 0.5 * (q[(size_t)((i + 1) % NO)] - 2.0 * q[(size_t)i] + q[(size_t)((i + NO - 1) % NO)]);
                klOrbit[(size_t)i] = (float)v;
                peak = juce::jmax(peak, std::abs((float)v));
            }
            for (auto& v : klOrbit) v /= peak;
            klOrbitE = eq;
        }
        const int NP = juce::jmax(64, (int)area.getWidth() / 2);
        const double span = 3.0, start = orbitPhase - span * 0.5;
        const int NO = (int)klOrbit.size();
        juce::Path wave;
        for (int i = 0; i < NP; ++i)
        {
            double u = start + span * i / (NP - 1);
            u -= std::floor(u);
            const double fi = u * NO;
            const int i0 = (int)fi % NO, i1 = (i0 + 1) % NO;
            const float hv = klOrbit[(size_t)i0] + (klOrbit[(size_t)i1] - klOrbit[(size_t)i0]) * (float)(fi - std::floor(fi));
            const float x = area.getX() + area.getWidth() * (float)i / (float)(NP - 1);
            const float y = area.getCentreY() - hv * area.getHeight() * 0.45f;
            if (i == 0) wave.startNewSubPath(x, y); else wave.lineTo(x, y);
        }
        glowStroke(g, wave, purple, 1.4f);
        g.setColour(amber);
        g.drawVerticalLine((int)area.getCentreX(), area.getY(), area.getBottom());
        axisText(g, "now", { area.getCentreX() + 4.0f, area.getY(), 40.0f, 12.0f }, juce::Justification::centredLeft);
        axisText(g, "one burst per orbit (periapsis)", { area.getX(), area.getBottom() - 12.0f, 260.0f, 12.0f }, juce::Justification::centredLeft);

        // e(t) over the Kozai-Lidov cycle
        g.setColour(panelBorder);
        g.drawRoundedRectangle(strip, 5.0f, 1.0f);
        if (eOfCycle)
        {
            auto s = strip.reduced(8.0f, 6.0f);
            s.removeFromLeft(62.0f);
            juce::Path ec;
            for (int i = 0; i <= 200; ++i)
            {
                const float u = (float)i / 200.0f;
                const float y = s.getBottom() - eOfCycle(u) / 0.95f * s.getHeight();
                if (i == 0) ec.startNewSubPath(s.getX(), y); else ec.lineTo(s.getX() + u * s.getWidth(), y);
            }
            g.setColour(purple.withAlpha(0.8f));
            g.strokePath(ec, juce::PathStrokeType(1.4f));
            const juce::Point<float> dot(s.getX() + cycle * s.getWidth(), s.getBottom() - eOfCycle(cycle) / 0.95f * s.getHeight());
            g.setColour(amber);
            g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(dot));
            for (int bar = 1; bar < 8; ++bar)
            {
                g.setColour(panelBorder);
                g.drawVerticalLine((int)(s.getX() + s.getWidth() * (float)bar / 8.0f), s.getY(), s.getBottom());
            }
            axisText(g, "e(t)", { strip.getX() + 8.0f, strip.getY() + 6.0f, 60.0f, 14.0f }, juce::Justification::centredLeft);
            axisText(g, "8 bars", { strip.getX() + 8.0f, strip.getBottom() - 20.0f, 60.0f, 14.0f }, juce::Justification::centredLeft);
        }
    }

    void paintLens(juce::Graphics& g, juce::Rectangle<float> b)
    {
        using namespace GWTheme;
        auto area = b.reduced(10.0f, 4.0f);
        auto axis = area.removeFromBottom(16.0f);
        area.removeFromLeft(34.0f);
        const float fLo = 20.0f, fHi = 16000.0f, dbLo = -9.0f, dbHi = 9.0f;
        auto xOf = [&](float f) { return area.getX() + area.getWidth() * std::log(f / fLo) / std::log(fHi / fLo); };
        auto yOf = [&](float db) { return area.getBottom() - (db - dbLo) / (dbHi - dbLo) * area.getHeight(); };

        for (float db : { -6.0f, 0.0f, 6.0f })
        {
            g.setColour(db == 0.0f ? textMuted : panelBorder);
            g.drawHorizontalLine((int)yOf(db), area.getX(), area.getRight());
            axisText(g, (db > 0 ? "+" : "") + juce::String((int)db) + " dB", { b.getX() + 2.0f, yOf(db) - 7.0f, 40.0f, 14.0f }, juce::Justification::centredLeft);
        }
        for (float f : { 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f })
            axisText(g, f >= 1000.0f ? juce::String((int)(f / 1000.0f)) + "k" : juce::String((int)f), { xOf(f) - 20.0f, axis.getY(), 40.0f, 14.0f }, juce::Justification::centred);
        axisText(g, "Hz", { area.getRight() - 20.0f, axis.getY(), 20.0f, 14.0f }, juce::Justification::centredRight);

        // |F(f)| = norm · |1 + HP(f)·((√μ+ − 1) − i·√|μ−|·e^{i2πfΔt})|   (what the audio applies)
        const double TSUN = 4.925491e-6, pi = juce::MathConstants<double>::pi;
        const double dt = 4.0 * TSUN * lensMass * lensT;
        const double fc = 0.7 / (8.0 * pi * TSUN * lensMass);
        const int W = juce::jmax(2, (int)area.getWidth());
        std::vector<float> lo((size_t)W, 1e9f), hi((size_t)W, -1e9f);
        const int sub = 24;
        bool unresolved = false;
        for (int c = 0; c < W; ++c)
        {
            const double fa = fLo * std::pow((double)fHi / fLo, (double)c / W);
            const double fb = fLo * std::pow((double)fHi / fLo, (double)(c + 1) / W);
            if ((fb - fa) * dt > 0.5)
            {
                // comb teeth (spacing 1/Δt) narrower than a pixel: the two images'
                // relative phase sweeps fully inside this column, so |F| spans
                // exactly | |A| − |B| | … |A| + |B|
                unresolved = true;
                const double f = std::sqrt(fa * fb);
                const std::complex<double> hp = std::complex<double>(0.0, f / fc) / std::complex<double>(1.0, f / fc);
                const double A = std::abs(1.0 + hp * ((double)lensAp - 1.0));
                const double B = std::abs(hp) * (double)lensAm;
                lo[(size_t)c] = (float)(20.0 * std::log10(juce::jmax(1e-6, lensNorm * std::abs(A - B))));
                hi[(size_t)c] = (float)(20.0 * std::log10(juce::jmax(1e-6, lensNorm * (A + B))));
                continue;
            }
            for (int k = 0; k < sub; ++k)
            {
                const double f = fLo * std::pow((double)fHi / fLo, ((double)c + (double)k / sub) / W);
                const std::complex<double> hp = std::complex<double>(0.0, f / fc) / std::complex<double>(1.0, f / fc);
                const std::complex<double> img = std::complex<double>(0.0, -1.0) * (double)lensAm * std::exp(std::complex<double>(0.0, 2.0 * pi * f * dt));
                const double mag = lensNorm * std::abs(1.0 + hp * ((double)lensAp - 1.0 + img));
                const float db = (float)(20.0 * std::log10(juce::jmax(1e-6, mag)));
                lo[(size_t)c] = juce::jmin(lo[(size_t)c], db); hi[(size_t)c] = juce::jmax(hi[(size_t)c], db);
            }
        }
        juce::Path top, fill;
        for (int c = 0; c < W; ++c)
        {
            const float x = area.getX() + (float)c;
            if (c == 0) { top.startNewSubPath(x, yOf(hi[0])); fill.startNewSubPath(x, yOf(hi[0])); }
            else { top.lineTo(x, yOf(hi[(size_t)c])); fill.lineTo(x, yOf(hi[(size_t)c])); }
        }
        for (int c = W - 1; c >= 0; --c) fill.lineTo(area.getX() + (float)c, yOf(lo[(size_t)c]));
        fill.closeSubPath();
        g.setColour(purple.withAlpha(0.35f));
        g.fillPath(fill);
        glowStroke(g, top, purpleBright, 1.2f);

        if (unresolved)
        {
            g.setFont(mono(10.5f));
            g.setColour(textSecondary);
            const double sp = 1.0 / dt;
            g.drawText("comb teeth every " + (sp < 10.0 ? juce::String(sp, 2) : juce::String(juce::roundToInt(sp))) + u8(" Hz (too fine to draw) \xe2\x80\x94 heard as an echo"),
                juce::Rectangle<float>(area.getX(), area.getY(), area.getWidth(), 13.0f), juce::Justification::centredRight);
        }
        if (fc > fLo && fc < fHi)
        {
            const float x = xOf((float)fc);
            g.setColour(amber.withAlpha(0.8f));
            const float dashes[] = { 3.0f, 3.0f };
            juce::Path l; l.startNewSubPath(x, area.getY()); l.lineTo(x, area.getBottom());
            juce::Path d; juce::PathStrokeType(1.0f).createDashedStroke(d, l, dashes, 2);
            g.fillPath(d);
            g.setFont(mono(10.5f));
            g.drawText("diffraction", juce::Rectangle<float>(x + 4.0f, area.getY(), 90.0f, 12.0f), juce::Justification::centredLeft);
        }
    }
};

//==============================================================================
// Live spectrogram of the plugin's output. Log-frequency axis, newest at right.
// Cost control: samples are only copied into a ring buffer as they arrive; one
// FFT is computed per display frame (the 186 ms window makes more pointless),
// and the new columns are interpolated from the previous one. Silence skips
// the FFT entirely.
class SpectrogramView : public juce::Component
{
public:
    static constexpr int kOrder = 13, kN = 1 << kOrder, kHop = 512;   // 8192-pt: ~6 Hz bins resolve bass harmonics

    SpectrogramView()
    {
        window.resize(kN);
        for (int i = 0; i < kN; ++i)
            window[(size_t)i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float)i / (float)(kN - 1));
        ring.assign(kN, 0.0f);
        re.assign(kN, 0.0f); im.assign(kN, 0.0f);
        bitrev.resize(kN);
        for (int i = 0; i < kN; ++i)
        {
            int r = 0;
            for (int b = 0; b < kOrder; ++b) if (i & (1 << b)) r |= 1 << (kOrder - 1 - b);
            bitrev[(size_t)i] = r;
        }
        twr.resize(kN / 2); twi.resize(kN / 2);
        for (int k = 0; k < kN / 2; ++k)
        {
            twr[(size_t)k] = std::cos(-juce::MathConstants<float>::twoPi * (float)k / (float)kN);
            twi[(size_t)k] = std::sin(-juce::MathConstants<float>::twoPi * (float)k / (float)kN);
        }
        buildColourMap();
    }

    void setSampleRate(double sr) { if (sr > 1000.0 && sr != sampleRate) { sampleRate = sr; mapHeight = -1; } }

    // Feed new output samples (cheap: copy only)
    void push(const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t)ringPos] = x[i];
            ringPos = (ringPos + 1) & (kN - 1);
        }
        pendingSamples += n;
    }

    // Once per display frame: one FFT, then scroll in the columns that elapsed
    void update()
    {
        if (!image.isValid()) return;
        const int W = image.getWidth(), H = image.getHeight();
        int cols = pendingSamples / kHop;
        if (cols <= 0) return;
        pendingSamples -= cols * kHop;
        cols = juce::jmin(cols, W);
        if (mapHeight != H) buildRowMap(H);
        if ((int)lastCol.size() != H) lastCol.assign((size_t)H, -200.0f);

        // newest column
        std::vector<float>& col = newCol;
        col.resize((size_t)H);
        float peakIn = 0.0f;
        for (int i = 0; i < kN; ++i) peakIn = juce::jmax(peakIn, std::abs(ring[(size_t)i]));
        float colMax = -200.0f;
        if (peakIn < 1.0e-6f)
            std::fill(col.begin(), col.end(), -200.0f);
        else
        {
            for (int i = 0; i < kN; ++i)
            {
                const int j = bitrev[(size_t)i];
                re[(size_t)j] = ring[(size_t)((ringPos + i) & (kN - 1))] * window[(size_t)i];
                im[(size_t)j] = 0.0f;
            }
            fft();
            const float norm = 4.0f / (float)kN;          // Hann: full-scale sine → 0 dB
            auto mag = [&](int k) { return std::sqrt(re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]); };
            for (int r = 0; r < H; ++r)
            {
                float m = 0.0f;
                const int b0 = rowBins[(size_t)r].first, b1 = rowBins[(size_t)r].second;
                if (b1 > b0) for (int k = b0; k <= b1; ++k) m = juce::jmax(m, mag(k));
                else
                {   // row narrower than a bin (low frequencies): interpolate between bins
                    const float cb = rowCentreBin[(size_t)r];
                    const int k0 = (int)cb; const float fr = cb - (float)k0;
                    m = (1.0f - fr) * mag(k0) + fr * mag(k0 + 1);
                }
                col[(size_t)r] = 20.0f * std::log10(m * norm + 1e-9f);
                colMax = juce::jmax(colMax, col[(size_t)r]);
            }
        }
        // Auto-exposure: follow the loudest recent level (fast up, 10 dB/s down)
        // and show the 66 dB below it, so every preset reads clearly at any volume.
        refDb = juce::jmax(colMax, refDb - 10.0f * (float)(cols * kHop) / (float)sampleRate, -60.0f);

        image.moveImageSection(0, 0, cols, 0, W - cols, H);
        juce::Image::BitmapData bd(image, W - cols, 0, cols, H, juce::Image::BitmapData::writeOnly);
        for (int c = 0; c < cols; ++c)
        {
            const float t = (float)(c + 1) / (float)cols;      // ease from the previous column
            for (int r = 0; r < H; ++r)
            {
                const float db = lastCol[(size_t)r] + (col[(size_t)r] - lastCol[(size_t)r]) * t;
                const float u = juce::jlimit(0.0f, 1.0f, (db - (refDb - 66.0f)) / 66.0f);
                bd.setPixelColour(c, r, colours[(size_t)juce::jlimit(0, 255, (int)(255.0f * u * u))]);
            }
        }
        lastCol = col;
        repaint();
    }

    void resized() override
    {
        const auto plot = plotArea();
        image = juce::Image(juce::Image::RGB, juce::jmax(1, (int)plot.getWidth()), juce::jmax(1, (int)plot.getHeight()), true);
        juce::Graphics(image).fillAll(colours[0]);
        mapHeight = -1;
        lastCol.clear();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace GWTheme;
        const auto plot = plotArea();
        g.drawImageAt(image, (int)plot.getX(), (int)plot.getY());
        g.setColour(panelBorder);
        g.drawRect(plot, 1.0f);
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float y = plot.getBottom() - plot.getHeight() * yNorm(f);
            if (y < plot.getY() + 4.0f) continue;
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            g.drawHorizontalLine((int)y, plot.getX(), plot.getRight());
            g.setFont(mono(10.5f));
            g.setColour(textMuted);
            g.drawText(f >= 1000.0f ? juce::String((int)(f / 1000.0f)) + "k" : juce::String((int)f),
                juce::Rectangle<float>(0.0f, y - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        }
        g.setFont(mono(10.5f));
        g.setColour(textMuted);
        g.drawText("Hz", juce::Rectangle<float>(0.0f, plot.getBottom() + 3.0f, plot.getX() - 6.0f, 13.0f), juce::Justification::centredRight);
        const float secs = plot.getWidth() * (float)kHop / (float)sampleRate;
        g.drawText(juce::String(secs, 1) + " s ago", juce::Rectangle<float>(plot.getX(), plot.getBottom() + 3.0f, 80.0f, 13.0f), juce::Justification::centredLeft);
        g.drawText("now", juce::Rectangle<float>(plot.getRight() - 40.0f, plot.getBottom() + 3.0f, 40.0f, 13.0f), juce::Justification::centredRight);
    }

private:
    std::vector<float> window, ring, re, im, twr, twi, lastCol, newCol;
    std::vector<int> bitrev;
    int ringPos = 0, pendingSamples = 0;
    double sampleRate = 48000.0;
    juce::Image image;
    std::array<juce::Colour, 256> colours;
    std::vector<std::pair<int, int>> rowBins;   // bin range per pixel row
    std::vector<float> rowCentreBin;            // fractional bin at each row's centre
    float refDb = -30.0f;                       // auto-exposure reference (loudest recent level)
    int mapHeight = -1;
    static constexpr float fMin = 30.0f, fMax = 12000.0f;

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().withTrimmedLeft(38.0f).withTrimmedBottom(18.0f).withTrimmedTop(4.0f); }
    static float yNorm(float f) { return std::log(f / fMin) / std::log(fMax / fMin); }

    void buildColourMap()
    {
        // black → deep indigo → purple → pink → amber → white
        const juce::Colour stops[] = { juce::Colour(0xff06070d), juce::Colour(0xff1b1446), juce::Colour(0xff5b3fc4),
                                       juce::Colour(0xffa78bfa), juce::Colour(0xfff472b6), juce::Colour(0xfff5b971), juce::Colour(0xfffff7e6) };
        const float pos[] = { 0.0f, 0.22f, 0.45f, 0.62f, 0.78f, 0.9f, 1.0f };
        for (int i = 0; i < 256; ++i)
        {
            const float u = (float)i / 255.0f;
            int k = 0; while (k < 5 && u > pos[k + 1]) ++k;
            colours[(size_t)i] = stops[k].interpolatedWith(stops[k + 1], (u - pos[k]) / (pos[k + 1] - pos[k]));
        }
    }

    void buildRowMap(int H)
    {
        rowBins.assign((size_t)H, { 0, 0 });
        rowCentreBin.assign((size_t)H, 1.0f);
        const float binHz = (float)sampleRate / (float)kN;
        for (int r = 0; r < H; ++r)
        {
            const float f0 = fMin * std::pow(fMax / fMin, (float)r / (float)H);
            const float f1 = fMin * std::pow(fMax / fMin, (float)(r + 1) / (float)H);
            const int b0 = juce::jlimit(1, kN / 2 - 1, (int)std::floor(f0 / binHz));
            const int b1 = juce::jlimit(b0, kN / 2 - 1, (int)std::floor(f1 / binHz));
            rowBins[(size_t)(H - 1 - r)] = { b0, b1 };   // row 0 = top = highest frequency
            rowCentreBin[(size_t)(H - 1 - r)] = juce::jlimit(1.0f, (float)(kN / 2 - 2), std::sqrt(f0 * f1) / binHz);
        }
        mapHeight = H;
    }

    // In-place radix-2 FFT on re/im (input already in bit-reversed order)
    void fft()
    {
        float* xr = re.data(); float* xi = im.data();
        for (int len = 2, tstep = kN / 2; len <= kN; len <<= 1, tstep >>= 1)
        {
            const int half = len >> 1;
            for (int i = 0; i < kN; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const float wr = twr[(size_t)(k * tstep)], wi = twi[(size_t)(k * tstep)];
                    const int a = i + k, b = a + half;
                    const float vr = xr[b] * wr - xi[b] * wi, vi = xr[b] * wi + xi[b] * wr;
                    xr[b] = xr[a] - vr; xi[b] = xi[a] - vi;
                    xr[a] += vr;        xi[a] += vi;
                }
        }
    }
};