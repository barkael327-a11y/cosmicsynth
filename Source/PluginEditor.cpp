/*
  ==============================================================================
    PluginEditor.cpp  -  Cosmic Synth UI

    Layout (design size 1280 x 800, scaled to the window):
      header      preset browser, data source, GW LIVE
      left panel  the gravitational-wave system: readouts, orbit, strain / physics view
      right panel live spectrogram of the output
      bottom      only the controls that reach the audio; each dims when the
                  current preset doesn't use it
  ==============================================================================
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace GWTheme;

namespace
{
    // NB: juce::String(double, 0) means "full precision", so whole numbers use roundToInt
    juce::String whole(double v) { return juce::String(juce::roundToInt(v)); }
    juce::String hz(double v)
    {
        if (v >= 1000.0) return juce::String(v / 1000.0, 2) + " kHz";
        return (v < 100.0 ? juce::String(v, 1) : whole(v)) + " Hz";
    }
    juce::String secs(double s)
    {
        if (s >= 1.0) return juce::String(s, 2) + " s";
        if (s >= 1.0e-3) return juce::String(s * 1.0e3, s < 0.01 ? 2 : 1) + " ms";
        return whole(s * 1.0e6) + u8(" \xc2\xb5s");
    }
    juce::String msun(double m)
    {
        return (m >= 1000.0 ? juce::String(m / 1000.0, 1) + "k" : (m < 10.0 ? juce::String(m, 1) : whole(m))) + " Msun";
    }

    // Lens geometry for alignment y (Einstein radii) - same formulas as the engine
    struct LensNumbers { double muPlus, muMinus, T; };
    LensNumbers lensNumbers(double y)
    {
        const double s = std::sqrt(y * y + 4.0);
        return { 0.5 + (y * y + 2.0) / (2.0 * y * s), 0.5 - (y * y + 2.0) / (2.0 * y * s),
                 y * s / 2.0 + std::log((s + y) / (s - y)) };
    }
}

//==============================================================================
CosmicMainView::CosmicMainView(CosmicsynthAudioProcessor& p) : proc(p)
{
    // ---------------- header ----------------
    for (int i = 0; i < kNumPresets; ++i)
        presetBox.addItem(presetName(static_cast<PresetId>(i)), i + 1);
    presetBox.setWantsKeyboardFocus(true);
    presetBox.onChange = [this]
        {
            const int id = presetBox.getSelectedId();
            if (id > 0 && id - 1 != (int)proc.getCurrentPreset())
                proc.setPreset(static_cast<PresetId>(id - 1));
        };
    prevButton.onClick = [this] { presetBox.setSelectedId(juce::jmax(1, presetBox.getSelectedId() - 1)); };
    nextButton.onClick = [this] { presetBox.setSelectedId(juce::jmin(kNumPresets, presetBox.getSelectedId() + 1)); };

    loadButton.setTooltip("Load a gravitational-wave CSV (columns t, h, fgw, psi)");
    loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>("Select GW CSV", juce::File(), "*.csv");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (f.existsAsFile() && !proc.loadGWFile(f))
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Load failed",
                            "Could not parse that CSV. Expected columns: t, h, fgw, psi.");
                });
        };
    builtInButton.setTooltip("Use the built-in 11.5 + 11.5 Msun merger waveform");
    builtInButton.onClick = [this] { proc.loadBuiltInWaveform(); };

    for (auto* c : std::initializer_list<juce::Component*>{ &presetBox, &prevButton, &nextButton, &loadButton, &builtInButton, &liveToggle })
        addAndMakeVisible(c);
    liveAttachment = std::make_unique<APVTS::ButtonAttachment>(proc.getVTS(), CosmicsynthAudioProcessor::p_gwEnable, liveToggle);

    // ---------------- panels ----------------
    rightReadout.rightAligned = true;
    for (auto* c : std::initializer_list<juce::Component*>{ &leftReadout, &rightReadout, &orbit, &strain, &spectrogram })
        addAndMakeVisible(c);
    strain.series = &proc.getGW();
    strain.eOfCycle = [this](float u)
        {
            const float tilt = proc.getVTS().getRawParameterValue(CosmicsynthAudioProcessor::p_klTilt)->load();
            return proc.klEccentricityAt(tilt, (double)u);
        };

    // ---------------- knobs ----------------
    auto attach = [this](LabelledKnob& k, const juce::String& id)
        {
            addAndMakeVisible(k);
            sliderAttachments.push_back(std::make_unique<APVTS::SliderAttachment>(proc.getVTS(), id, k.slider));
        };
    auto pct = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    kAmp.formatter = pct; kPitch.formatter = pct; kFilter.formatter = pct; kTimbre.formatter = pct;
    kSmooth.formatter = [](double v) { return whole(v) + " ms"; };
    kTilt.formatter = [](double v) { return juce::String(v, 1) + u8("\xc2\xb0"); };
    kLens.formatter = [](double v) { return msun(v); };
    attach(kAmp, CosmicsynthAudioProcessor::p_gwGainDepth);
    attach(kPitch, CosmicsynthAudioProcessor::p_gwPitchDepth);
    attach(kFilter, CosmicsynthAudioProcessor::p_gwFilterDepth);
    attach(kTimbre, CosmicsynthAudioProcessor::p_gwTimbreDepth);
    attach(kSmooth, CosmicsynthAudioProcessor::p_gwSmoothingMs);
    attach(kTilt, CosmicsynthAudioProcessor::p_klTilt);
    attach(kLens, CosmicsynthAudioProcessor::p_lensMass);

    setSize(kW, kH);
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    timerCallback();
    startTimerHz(30);
}

CosmicMainView::~CosmicMainView() { stopTimer(); }

//==============================================================================
juce::String CosmicMainView::presetDescription(PresetId pid)
{
    switch (pid)
    {
    case PresetId::BinaryPad:
        return u8("Binary orbit \xe2\x80\x94 the chord wobbles with the data's h(t); two bodies spiral in, chirp and fuse at merger");
    case PresetId::InspiralPluck:
        return u8("Chirp into the note \xe2\x80\x94 every note rises along the data's f(t) curve and lands in tune at merger");
    case PresetId::RingdownBell:
        return u8("Kerr quasinormal modes \xe2\x80\x94 the remnant rings in its l=m=2, 3 and 4 modes, tuned by its measured spin");
    case PresetId::KozaiLidov:
        return u8("Kozai-Lidov triple \xe2\x80\x94 a third body pumps the eccentricity; one GW burst per orbit, 8-bar cycle");
    case PresetId::GravLens:
        return u8("Gravitational lens \xe2\x80\x94 heavy lens: a second arrival (echo). Light lens: the images interfere (flanger)");
    default: return {};
    }
}

void CosmicMainView::presetChanged(PresetId pid)
{
    shownPreset = (int)pid;
    presetBox.setSelectedId((int)pid + 1, juce::dontSendNotification);
    description = presetDescription(pid);

    const bool csvPreset = pid == PresetId::BinaryPad || pid == PresetId::InspiralPluck || pid == PresetId::RingdownBell;
    kAmp.setActive(csvPreset);
    kPitch.setActive(pid == PresetId::BinaryPad);
    kFilter.setActive(csvPreset);
    kTimbre.setActive(csvPreset);
    kSmooth.setActive(csvPreset);
    kTilt.setActive(pid == PresetId::KozaiLidov);
    kLens.setActive(pid == PresetId::GravLens);

    auto setMode = [this](OrbitView::Mode om, StrainView::Mode sm, const char* sub, const char* stitle)
        {
            orbit.state.mode = om; strain.mode = sm;
            gwSubtitle = u8(sub); strainTitle = u8(stitle);
        };
    switch (pid)
    {
    case PresetId::BinaryPad:     setMode(OrbitView::Mode::Binary, StrainView::Mode::Data, "SOURCE  \xc2\xb7  BINARY BLACK HOLE", "STRAIN h(t)  \xc2\xb7  DATA"); break;
    case PresetId::InspiralPluck: setMode(OrbitView::Mode::Pluck, StrainView::Mode::Data, "SOURCE  \xc2\xb7  ONE INSPIRAL PER NOTE", "STRAIN h(t)  \xc2\xb7  DATA"); break;
    case PresetId::RingdownBell:  setMode(OrbitView::Mode::Bell, StrainView::Mode::Data, "SOURCE  \xc2\xb7  REMNANT BLACK HOLE, RINGING", "STRAIN h(t)  \xc2\xb7  DATA"); break;
    case PresetId::KozaiLidov:    setMode(OrbitView::Mode::Kozai, StrainView::Mode::Kozai, "SOURCE  \xc2\xb7  HIERARCHICAL TRIPLE", "STRAIN h(t)  \xc2\xb7  ECCENTRIC ORBIT (FACE-ON)"); break;
    case PresetId::GravLens:      setMode(OrbitView::Mode::Lens, StrainView::Mode::Lens, "SOURCE  \xc2\xb7  POINT-MASS LENS", "LENS AMPLIFICATION  |F(f)|  \xc2\xb7  WHAT THE AUDIO APPLIES"); break;
    default: break;
    }
    repaint();
}

//==============================================================================
void CosmicMainView::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float dt = (float)juce::jlimit(0.0, 0.1, (nowMs - lastTickMs) / 1000.0);
    lastTickMs = nowMs;

    const PresetId pid = proc.getCurrentPreset();
    if ((int)pid != shownPreset) presetChanged(pid);
    if (dataName != proc.getDataName()) { dataName = proc.getDataName(); repaint(); }

    // ---- spectrogram: drain the audio tap
    spectrogram.setSampleRate(proc.getSampleRate());
    for (int guard = 0; guard < 8; ++guard)            // copying only; analysis happens once below
    {
        const int n = proc.pullScopeSamples(scopeScratch.data(), (int)scopeScratch.size());
        if (n <= 0) break;
        spectrogram.push(scopeScratch.data(), n);
    }
    spectrogram.update();

    // ---- system state for the visuals
    auto& v = proc.viz;
    const auto an = proc.getAnalysis();
    auto& os = orbit.state;
    os.event = v.padEvent.load();
    os.fRatio = v.padFRatio.load();
    os.split = v.padSplit.load();
    os.orbitHz = v.padOrbitHz.load();
    os.postMerger = v.padPostMerger.load();
    os.pluckFrac = v.pluckFrac.load();
    os.pluckCount = v.pluckNoteCount.load();
    os.bellEnv = v.bellEnv.load();
    os.e = proc.getKozaiEccentricity();
    os.orbitPhase = v.klOrbitPhase.load();
    os.tiltDeg = proc.getVTS().getRawParameterValue(CosmicsynthAudioProcessor::p_klTilt)->load();
    os.lensMass = proc.getVTS().getRawParameterValue(CosmicsynthAudioProcessor::p_lensMass)->load();
    {
        const auto ln = lensNumbers(makePreset(PresetId::GravLens).lensAlignment);
        os.muPlus = (float)ln.muPlus; os.muMinus = (float)std::abs(ln.muMinus);
        os.lensDelaySec = (float)(4.0 * 4.925491e-6 * os.lensMass * ln.T);
        strain.lensMass = os.lensMass;
        strain.lensAp = (float)std::sqrt(ln.muPlus);
        strain.lensAm = (float)std::sqrt(std::abs(ln.muMinus));
        strain.lensNorm = (float)(1.0 / std::sqrt(ln.muPlus + std::abs(ln.muMinus)));
        strain.lensT = (float)ln.T;
    }
    // remnant horizon radius vs each (non-spinning) body: mass 0.952 M, r+ = M(1 + sqrt(1 - a^2))
    os.remnantScale = (float)(0.952 * (1.0 + std::sqrt(juce::jmax(0.0, 1.0 - an.spin * an.spin))));

    // ---- strain view
    strain.dataVersion = proc.getDataVersion();
    strain.tMerger = an.tMerger;
    strain.heads.clear();
    strain.phaseLabel.clear();
    strain.ringdownGlow = false;
    if (pid == PresetId::BinaryPad)
    {
        if (os.event)
        {
            strain.heads.push_back({ proc.getCsvReadHead(), "reading" });
            strain.phaseLabel = os.postMerger < 0.0f ? u8("INSPIRAL \xe2\x80\x94 orbit tightening") : u8("MERGER \xe2\x86\x92 RINGDOWN");
            strain.ringdownGlow = os.postMerger >= 0.0f && os.postMerger < 1.0f;
        }
        else strain.phaseLabel = "WAITING FOR THE NEXT EVENT";
    }
    else if (pid == PresetId::InspiralPluck)
    {
        if (os.pluckFrac < 1.0f) { strain.heads.push_back({ v.pluckPos.load(), "note chirping" }); strain.phaseLabel = u8("CHIRP \xe2\x86\x92 MERGER"); }
        else if (os.pluckCount > 0) strain.phaseLabel = "ARRIVED AT MERGER";
    }
    else if (pid == PresetId::RingdownBell)
    {
        strain.ringdownGlow = os.bellEnv > 0.02f;
        strain.phaseLabel = u8("RINGDOWN \xe2\x80\x94 the modes are tuned from this decay");
    }
    strain.e = os.e;
    strain.orbitPhase = os.orbitPhase;
    strain.cycle = v.klCycle.load();
    strain.repaint();

    orbit.advance(dt);
    updateReadouts(pid);
}

void CosmicMainView::updateReadouts(PresetId pid)
{
    const auto an = proc.getAnalysis();
    auto& L = leftReadout;
    auto& R = rightReadout;
    auto& v = proc.viz;
    const juce::String dash = u8("\xe2\x80\x94");

    auto dataColumn = [&]()
        {
            R.set(0, "f at merger", hz(an.fMerger));
            R.set(1, "f ringdown", an.hasRingdown ? hz(an.fQnm) : dash);
            R.set(2, u8("\xcf\x84 ringdown"), an.hasRingdown ? secs(an.tauQnm) : dash);
            R.set(3, "remnant spin", an.hasRingdown ? juce::String(an.spin, 2) : dash);
        };

    switch (pid)
    {
    case PresetId::BinaryPad:
    {
        const bool ev = v.padEvent.load();
        const float post = v.padPostMerger.load();
        L.set(0, "f_GW now", ev ? hz(proc.getGW().sampleAt(proc.getCsvReadHead()).fgw) : dash);
        L.set(1, "f / f_start", ev ? u8("\xc3\x97") + juce::String(v.padFRatio.load(), 2) : dash);
        L.set(2, "orbit rate", ev && post < 0.0f ? hz(v.padOrbitHz.load()) : dash);
        L.set(3, "phase", !ev ? "waiting" : (post < 0.0f ? "inspiral" : (post < 0.3f ? "merger" : "ringdown")));
        dataColumn();
        break;
    }
    case PresetId::InspiralPluck:
    {
        const float frac = v.pluckFrac.load();
        const bool on = frac < 1.0f;
        const double chirp = makePreset(PresetId::InspiralPluck).chirpTimeMs;
        L.set(0, "f_GW now", on ? hz(proc.getGW().sampleAt(v.pluckPos.load()).fgw) : dash);
        L.set(1, "to merger", on ? whole((1.0 - frac) * chirp) + " ms" : "arrived");
        L.set(2, "chirp time", whole(chirp) + " ms");
        L.set(3, "notes played", juce::String(v.pluckNoteCount.load()));
        dataColumn();
        break;
    }
    case PresetId::RingdownBell:
    {
        const auto bm = proc.getBellModes();
        L.set(0, "f ringdown", an.hasRingdown ? hz(an.fQnm) : dash);
        L.set(1, u8("\xcf\x84 ringdown"), an.hasRingdown ? secs(an.tauQnm) : dash);
        L.set(2, "quality Q", juce::String(an.Q, 2));
        L.set(3, "remnant spin", juce::String(an.spin, 2));
        R.set(0, "l=m=3 mode", u8("\xc3\x97") + juce::String(bm[0], 3) + "  " + juce::String(bm[2] * 100.0f, 1) + "%");
        R.set(1, "l=m=4 mode", u8("\xc3\x97") + juce::String(bm[1], 3) + "  " + juce::String(bm[3] * 100.0f, 1) + "%");
        R.set(2, "l=m=2 decay", secs(bm[4]));
        R.set(3, "mass ratio q", juce::String(makePreset(PresetId::RingdownBell).bellMassRatio, 1));
        break;
    }
    case PresetId::KozaiLidov:
    {
        const float tilt = proc.getVTS().getRawParameterValue(CosmicsynthAudioProcessor::p_klTilt)->load();
        const double e = proc.getKozaiEccentricity();
        double emax = 0.0;                                  // from the same table the audio uses
        for (int k = 0; k < 64; ++k) emax = juce::jmax(emax, (double)proc.klEccentricityAt(tilt, k / 64.0));
        const double fPM = (1.0 + 73.0 / 24.0 * e * e + 37.0 / 96.0 * e * e * e * e) / std::pow(1.0 - e * e, 3.5);
        const double nPeak = 2.0 * std::pow(1.0 + e, 1.1954) / std::pow(1.0 - e * e, 1.5);
        L.set(0, "tilt i", juce::String(tilt, 1) + u8("\xc2\xb0"));
        L.set(1, "e now", juce::String(e, 3));
        L.set(2, "e max", juce::String(emax, 3));
        L.set(3, "cycle", "bar " + juce::String(juce::jlimit(1, 8, 1 + (int)(v.klCycle.load() * 8.0f))) + " of 8");
        R.set(0, "bursts", "1/8 @ " + whole(v.bpm.load()) + " BPM");
        R.set(1, "peak harmonic", u8("n \xe2\x89\x88 ") + juce::String(nPeak, 1));
        R.set(2, "GW power", u8("\xc3\x97") + juce::String(fPM, fPM < 10.0 ? 2 : 1));
        R.set(3, "closest approach", juce::String(1.0 - e, 2) + " a");
        break;
    }
    case PresetId::GravLens:
    {
        const double M = proc.getVTS().getRawParameterValue(CosmicsynthAudioProcessor::p_lensMass)->load();
        const double y = makePreset(PresetId::GravLens).lensAlignment;
        const auto ln = lensNumbers(y);
        const double dts = 4.0 * 4.925491e-6 * M * ln.T;
        const double fc = 0.7 / (8.0 * juce::MathConstants<double>::pi * 4.925491e-6 * M);
        L.set(0, "lens mass", msun(M));
        L.set(1, "alignment y", juce::String(y, 2) + " R_E");
        L.set(2, "image delay", secs(dts));
        L.set(3, "regime", dts >= 0.06 ? "echo" : (dts >= 0.01 ? "slapback" : "interference"));
        R.set(0, u8("\xce\xbc+  image 1"), juce::String(ln.muPlus, 2));
        R.set(1, u8("\xce\xbc\xe2\x88\x92  image 2"), juce::String(ln.muMinus, 2));
        R.set(2, "image 2 phase", u8("90\xc2\xb0 (Morse)"));
        R.set(3, "diffraction <", hz(fc));
        break;
    }
    default: break;
    }
}

//==============================================================================
void CosmicMainView::paint(juce::Graphics& g)
{
    g.fillAll(bg);

    // header: logo + subtitle
    g.setFont(sans(26.0f, true));
    g.setColour(textPrimary);
    g.drawText("COSMIC", 28, 14, 110, 34, juce::Justification::centredLeft);
    g.setColour(purple);
    g.drawText("SYNTH", 136, 14, 100, 34, juce::Justification::centredLeft);
    g.setFont(sans(11.5f));
    g.setColour(textMuted);
    g.drawText(u8("GRAVITATIONAL WAVES \xe2\x86\x92 SOUND"), 240, 14, 240, 34, juce::Justification::centredLeft);

    g.setFont(sans(13.0f));
    g.setColour(textSecondary);
    g.drawText(description, 28, 62, 900, 22, juce::Justification::centredLeft);
    g.setFont(mono(11.5f));
    g.setColour(textMuted);
    g.drawText("DATA  " + dataName, 900, 62, 352, 22, juce::Justification::centredRight);

    // panels
    drawPanel(g, { 20.0f, 96.0f, 780.0f, 456.0f });
    drawPanel(g, { 816.0f, 96.0f, 444.0f, 456.0f });
    drawPanel(g, { 20.0f, 568.0f, 1240.0f, 212.0f });

    auto title = [&](const juce::String& t, const juce::String& sub, int x, int y, int subX)
        {
            g.setFont(sans(14.0f, true));
            g.setColour(textPrimary);
            g.drawText(t, x, y, 400, 20, juce::Justification::centredLeft);
            g.setFont(sans(11.0f));
            g.setColour(textMuted);
            g.drawText(sub, subX, y, 460, 20, juce::Justification::centredLeft);
        };
    title("GRAVITATIONAL WAVE", gwSubtitle, 40, 108, 222);
    title("SPECTROGRAM", "LIVE OUTPUT", 836, 108, 952);
    title("GW MODULATION", "MAP GRAVITATIONAL-WAVE FEATURES TO SOUND", 40, 580, 176);

    g.setFont(sans(11.5f, true));
    g.setColour(textSecondary);
    g.drawText(strainTitle, 40, 300, 500, 16, juce::Justification::centredLeft);

    // separator between GW-depth knobs and physics knobs
    g.setColour(panelBorder);
    g.drawVerticalLine(872, 606.0f, 764.0f);
    g.setFont(sans(11.0f, true));
    g.setColour(textMuted);
    g.drawText("PHYSICS", 892, 580, 200, 20, juce::Justification::centredLeft);
}

void CosmicMainView::resized()
{
    // header
    prevButton.setBounds(470, 16, 30, 32);
    presetBox.setBounds(506, 16, 268, 32);
    nextButton.setBounds(780, 16, 30, 32);
    liveToggle.setBounds(930, 17, 112, 30);
    loadButton.setBounds(1054, 17, 96, 30);
    builtInButton.setBounds(1158, 17, 94, 30);

    // gravitational-wave panel
    leftReadout.setBounds(40, 138, 186, 144);
    rightReadout.setBounds(582, 138, 198, 144);
    orbit.setBounds(236, 132, 336, 160);
    strain.setBounds(30, 318, 760, 226);

    // spectrogram
    spectrogram.setBounds(828, 136, 422, 408);

    // knobs
    const int y = 608, w = 118, h = 160;
    LabelledKnob* depth[] = { &kAmp, &kPitch, &kFilter, &kTimbre, &kSmooth };
    for (int i = 0; i < 5; ++i) depth[i]->setBounds(56 + i * 162, y, w, h);
    kTilt.setBounds(930, y, w, h);
    kLens.setBounds(1100, y, w, h);
}

//==============================================================================
CosmicsynthAudioProcessorEditor::CosmicsynthAudioProcessorEditor(CosmicsynthAudioProcessor& p)
    : AudioProcessorEditor(&p), view(p)
{
    setLookAndFeel(&lookAndFeel);
    addAndMakeVisible(view);
    setResizable(true, true);
    setResizeLimits(CosmicMainView::kW * 5 / 8, CosmicMainView::kH * 5 / 8, CosmicMainView::kW * 3 / 2, CosmicMainView::kH * 3 / 2);
    getConstrainer()->setFixedAspectRatio((double)CosmicMainView::kW / (double)CosmicMainView::kH);
    setSize(CosmicMainView::kW, CosmicMainView::kH);
}

CosmicsynthAudioProcessorEditor::~CosmicsynthAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void CosmicsynthAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(GWTheme::bg);
}

void CosmicsynthAudioProcessorEditor::resized()
{
    const float s = (float)getWidth() / (float)CosmicMainView::kW;
    view.setBounds(0, 0, CosmicMainView::kW, CosmicMainView::kH);
    view.setTransform(juce::AffineTransform::scale(s));
}