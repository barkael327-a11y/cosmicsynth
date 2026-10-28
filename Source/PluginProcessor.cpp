// PluginProcessor.cpp
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

//==============================================================================

CosmicsynthAudioProcessor::~CosmicsynthAudioProcessor() = default;

//==============================================================================
// RNG

uint32_t CosmicsynthAudioProcessor::xorshift32() noexcept
{
    uint32_t x = rngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rngState = x;
    return x;
}

float CosmicsynthAudioProcessor::rand01() noexcept
{
    return (float)(xorshift32() & 0x00FFFFFFu) / (float)0x01000000u;
}

float CosmicsynthAudioProcessor::randRange(float lo, float hi) noexcept
{
    return lo + (hi - lo) * rand01();
}

//==============================================================================
// Preset system

void CosmicsynthAudioProcessor::setPreset(PresetId id)
{
    const juce::ScopedLock sl(presetLock);
    klCyclePh = 0.0;
    currentPreset.store(static_cast<int>(id), std::memory_order_relaxed);
    ps = makePreset(id);
    applyPresetToEngine();

    // Reset GW scheduler so the new preset starts fresh
    gwEventActive = false;
    gwWaitSec = (double)ps.gwWaitMin;
    gwEventPhase01 = 0.0;
    gwVibPhase = 0.0;
    prevGainEnd = 0.20f;
    passiveDriftPhase = 0.0;
    chirpPhase.fill(0.0);
    noteJustOn.fill(false);
    chirpCentsPerNote.fill(ps.chirpCentsMin);
    noteOnH.fill(0.0f);
    noteOnFgw.fill(0.0f);
    noteOnPsi.fill(0.0f);
    noteOnVelocity.fill(0.5f);
    noteAge.fill(0.0);
    bellDecaySec.fill(1.8);
    bellAge.fill(0.0);
    bellDetuneGain.fill(0.55f);
    bellSpinAlpha.fill(0.5f);
    gwMemoryDrift = (double)ps.reverbBase;  // start at preset base reverb
    gwMemoryTarget = (double)ps.reverbBase;
    transientPhase.fill(1.0e3);
    fgwWalkPos.fill(gw.getT0());
    hModWalkPos.fill(gw.getT0());
    noteInRingdown.fill(true);    // idle notes count as 'finished chirping'
    lastChirpCents.fill(0.0f);
    ringdownPhase.fill(0.0);
    gwCsvWalkPos = gw.getT0();
    padHWalkPos = gw.getT0();
    gwSinceMergerSec = 1.0e9;
    prevWobCents = 8.0f;
    padHWalkPosDrift = gw.getT0();

    smAmp.reset(1.0f);
    smCut.reset(1.0f);
    smTimbre.reset(0.0f);
    smPitch.reset(0.0f);
}

void CosmicsynthAudioProcessor::applyPresetToEngine()
{
    synth.setCosmicWidth(ps.cosmicWidth);
    synth.setReverbAmount(ps.reverbBase);
    synth.setFilterCutoff(ps.filterBaseline);
    synth.setWaveshapeMix(ps.waveshapeMix);
    // Gain, stereoWidth, msWidthGain, shadow, ADSR, slewMs, LFO rates
    // are pushed into the engine via the preset struct fields exposed through
    // the new applyPresetSettings() method:
    synth.applyPresetSettings(ps);
    pushBellModes();
}

//==============================================================================
// Kerr overtone modes for the Kerr Quasinormal Modes preset.
//
// A black hole rings in several quasinormal modes at once. Relative to the
// dominant l=m=2 mode, the l=m=3 and l=m=4 modes sit at NON-integer frequency
// ratios that depend on the spin — exactly the inharmonic partials that make a
// real bell sound metallic. Fits from Berti, Cardoso & Will (2006):
//     M·ω = f1 + f2 (1 - a)^f3        Q = q1 + q2 (1 - a)^q3
// Each mode's decay time τ = 2Q/ω, scaled to the musical decay of l=m=2.
// Amplitudes relative to l=m=2 follow Kamaretsos, Hannam & Sathyaprakash
// (2012) fits in the symmetric mass ratio η = q / (1+q)²:
//     A33 ≈ 0.44 (1 - 4η)^0.45        A44 ≈ 5.4 (η - 0.22)² + 0.04
// Equal masses (η = 1/4): A33 = 0 exactly, A44 ≈ 0.045.

void CosmicsynthAudioProcessor::pushBellModes()
{
    const double a = juce::jlimit(0.0, 0.99, gwRemnantSpin);
    const double x = 1.0 - a;

    auto mw = [x](double f1, double f2, double f3) { return f1 + f2 * std::pow(x, f3); };
    auto qf = [x](double q1, double q2, double q3) { return q1 + q2 * std::pow(x, q3); };

    const double w22 = mw(1.5251, -1.1568, 0.1292), Q22 = qf(0.7000, 1.4187, -0.4990);
    const double w33 = mw(1.8956, -1.3043, 0.1818), Q33 = qf(0.9000, 2.3430, -0.4810);
    const double w44 = mw(2.3000, -1.5056, 0.2244), Q44 = qf(1.1929, 3.1191, -0.4825);

    // Musical decay of the fundamental (same mapping as processGW_RingdownBell)
    const double qScale = juce::jlimit(0.5, 2.5, gwQnmQ / 3.0);
    const double tau22 = juce::jlimit(0.25, 6.0, (ps.qnmDecayMs / 1000.0) * qScale);

    // τ_lm / τ_22 = (Q_lm / ω_lm) / (Q_22 / ω_22)
    const double tau33 = tau22 * (Q33 / w33) / (Q22 / w22);
    const double tau44 = tau22 * (Q44 / w44) / (Q22 / w22);

    const double q = juce::jmax(1.0, (double)ps.bellMassRatio);
    const double eta = q / ((1.0 + q) * (1.0 + q));
    const double A33 = 0.44 * std::pow(juce::jmax(0.0, 1.0 - 4.0 * eta), 0.45);
    const double A44 = 5.4 * (eta - 0.22) * (eta - 0.22) + 0.04;

    bellModeInfo = { (float)(w33 / w22), (float)(w44 / w22), (float)A33, (float)A44, (float)tau22 };
    synth.setBellModes((float)(w33 / w22), (float)(w44 / w22),
        (float)A33, (float)A44, (float)tau33, (float)tau44);
}

//==============================================================================
// GW event scheduler

void CosmicsynthAudioProcessor::startNewGWEvent(double /*tOffsetSec*/)
{
    gwEventActive = true;
    gwSinceMergerSec = 1.0e9;
    gwEventPhase01 = 0.0;
    gwVibPhase = 0.0;

    gwEventDurSec = (double)randRange(ps.gwEventDurMin, ps.gwEventDurMax);
    gwPeakScale = randRange(0.85f, 1.20f);
    gwWaitSec = (double)(ps.gwWaitMin + (ps.gwWaitMax - ps.gwWaitMin) * std::sqrt(rand01()));

    const double gwT0 = gw.getT0();
    const double gwTN = gw.getTN();
    gwCsvSpanSec = juce::jlimit(0.1, juce::jmax(0.1, gwTN - gwT0),
        (double)randRange(ps.gwCsvSpanMin, ps.gwCsvSpanMax));
    gwCsvStartT = gw.pickEventStartTime(gwCsvSpanSec, (double)rand01(), 1.5);

    // With a real merger in the file, every event ENDS at merger, so each one
    // plays the complete inspiral → merger → ringdown story (the GW signature),
    // instead of a random slice that may never reach the chirp.
    if (gwHasRingdown)
    {
        gwCsvSpanSec = juce::jlimit(0.05, juce::jmax(0.05, gwTMerger - gwT0), gwCsvSpanSec);
        gwCsvStartT = gwTMerger - gwCsvSpanSec;
    }
}

//==============================================================================
// APVTS layout

//==============================================================================
// Lens-mass knob taper. The knob reads out in solar masses; its travel is split
// so the two musical regimes sit at opposite ends and the muddy in-between
// (slapback, ~10–240 ms delays) takes only 10% of the travel:
//   0.00–0.45   3 → 500 Msun        flanger / diffraction (0.06–10 ms)
//   0.45–0.55   500 → 12,000 Msun   quick pass through the slapback zone
//   0.55–1.00   12,000 → 50,000     echo (0.24–1.0 s)
namespace LensTaper
{
    static const float kx[4] = { 0.0f, 0.45f, 0.55f, 1.0f };
    static const float km[4] = { 3.0f, 500.0f, 12000.0f, 50000.0f };
    inline float from01(float, float, float x)
    {
        x = juce::jlimit(0.0f, 1.0f, x);
        int k = (x < kx[1]) ? 0 : (x < kx[2] ? 1 : 2);
        const float f = (x - kx[k]) / (kx[k + 1] - kx[k]);
        return std::exp(std::log(km[k]) + f * (std::log(km[k + 1]) - std::log(km[k])));
    }
    inline float to01(float, float, float m)
    {
        m = juce::jlimit(km[0], km[3], m);
        int k = (m < km[1]) ? 0 : (m < km[2] ? 1 : 2);
        const float f = (std::log(m) - std::log(km[k])) / (std::log(km[k + 1]) - std::log(km[k]));
        return kx[k] + f * (kx[k + 1] - kx[k]);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
CosmicsynthAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back(std::make_unique<AudioParameterBool>(
        p_gwEnable, "GW Enable", true));
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwGainDepth, "GW Gain Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.55f));
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwPitchDepth, "GW Pitch Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.80f));
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwFilterDepth, "GW Filter Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.70f));
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwTimbreDepth, "GW Timbre Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.50f));
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwSmoothingMs, "GW Smoothing (ms)",
        NormalisableRange<float>(1.f, 200.f, 0.1f), 30.f));

    // Kozai-Lidov: initial mutual inclination of the inner and outer orbits.
    // Sets the peak eccentricity, e_max = sqrt(1 - 5/3 cos^2 i) (39.2° = no KL).
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_klTilt, "KL Tilt",
        NormalisableRange<float>(40.f, 75.f, 0.1f), 57.f,
        AudioParameterFloatAttributes().withLabel("deg")
        .withStringFromValueFunction([](float v, int) { return String(v, 1) + " deg"; })));

    // Gravitational Lens: lens mass in solar masses (tapered, see LensTaper)
    params.push_back(std::make_unique<AudioParameterFloat>(
        p_lensMass, "Lens Mass",
        NormalisableRange<float>(3.f, 50000.f, LensTaper::from01, LensTaper::to01), 22607.f,
        AudioParameterFloatAttributes().withLabel("Msun")
        .withStringFromValueFunction([](float v, int)
            {
                return (v >= 1000.f ? String(v / 1000.f, 1) + "k" : (v < 10.f ? String(v, 1) : String(roundToInt(v)))) + " Msun";
            })));

    return { params.begin(), params.end() };
}

//==============================================================================
// Parameter IDs

const juce::String CosmicsynthAudioProcessor::p_gwEnable("gwEnable");
const juce::String CosmicsynthAudioProcessor::p_gwGainDepth("gwGainDepth");
const juce::String CosmicsynthAudioProcessor::p_gwPitchDepth("gwPitchDepth");
const juce::String CosmicsynthAudioProcessor::p_gwFilterDepth("gwFilterDepth");
const juce::String CosmicsynthAudioProcessor::p_gwTimbreDepth("gwTimbreDepth");
const juce::String CosmicsynthAudioProcessor::p_gwSmoothingMs("gwSmoothingMs");
const juce::String CosmicsynthAudioProcessor::p_klTilt("klTiltDeg");
const juce::String CosmicsynthAudioProcessor::p_lensMass("lensMassMsun");

//==============================================================================
// Ctor

CosmicsynthAudioProcessor::CosmicsynthAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
#else
    : AudioProcessor()
#endif
    , apvts(*this, nullptr, "Params", createLayout())
{
    ps = makePreset(PresetId::BinaryPad);
    buildKozaiCycleTable();
    loadBuiltInWaveform();   // works out of the box — no CSV needed on camera
}

void CosmicsynthAudioProcessor::loadBuiltInWaveform()
{
    suspendProcessing(true);
    gwLoaded.store(false, std::memory_order_relaxed);
    const bool ok = gw.loadFromFrames(GWSeries::makeBuiltInIMR());
    if (ok) { analyseGW(); pushBellModes(); resetWalkPositions(); }
    dataName = "Built-in: 11.5 + 11.5 Msun merger";
    dataPath.clear();
    ++dataVersion;
    gwLoaded.store(ok, std::memory_order_relaxed);
    suspendProcessing(false);
}

int CosmicsynthAudioProcessor::pullScopeSamples(float* dest, int maxSamples)
{
    const auto scope = scopeFifo.read(juce::jmin(maxSamples, scopeFifo.getNumReady()));
    if (scope.blockSize1 > 0) std::copy_n(scopeBuf.data() + scope.startIndex1, scope.blockSize1, dest);
    if (scope.blockSize2 > 0) std::copy_n(scopeBuf.data() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);
    return scope.blockSize1 + scope.blockSize2;
}

//==============================================================================
// Kozai-Lidov eccentricity over one cycle, for each tilt on the knob.
// Secular quadrupole equations, test-particle limit (e.g. Naoz 2016 review):
//   de/dτ = (15/8) e √(1−e²) sin²i sin2ω
//   dω/dτ = (3/(4√(1−e²))) [2(1−e²) + 5 sin²ω (e² − sin²i)],   √(1−e²) cos i conserved
// Integrated with RK4 from e = 0.1 (a cycle minimum) until the next minimum,
// then resampled to kKlPhases points over the cycle.

void CosmicsynthAudioProcessor::buildKozaiCycleTable()
{
    // Same for every instance: compute once per session
    static const std::vector<float> shared = []
        {
            std::vector<float> table;
            computeKozaiCycleTable(table);
            return table;
        }();
    klETable = shared;
}

void CosmicsynthAudioProcessor::computeKozaiCycleTable(std::vector<float>& table)
{
    table.assign((size_t)kKlTilts * kKlPhases, 0.1f);
    for (int it = 0; it < kKlTilts; ++it)
    {
        const double inc = juce::degreesToRadians(kKlTiltMin + (kKlTiltMax - kKlTiltMin) * it / (kKlTilts - 1));
        const double e0 = 0.1;
        const double Th = std::sqrt(1.0 - e0 * e0) * std::cos(inc);
        auto rhs = [Th](double e, double w, double& de, double& dw)
            {
                const double j = std::sqrt(juce::jmax(1e-9, 1.0 - e * e));
                const double ci = juce::jlimit(-1.0, 1.0, Th / j);
                const double si2 = 1.0 - ci * ci;
                de = 15.0 / 8.0 * e * j * si2 * std::sin(2.0 * w);
                dw = 3.0 / (4.0 * j) * (2.0 * j * j + 5.0 * std::sin(w) * std::sin(w) * (e * e - si2));
            };
        std::vector<double> ev; ev.reserve(20000);
        double e = e0, w = 0.0; const double h = 0.002;
        bool rose = false; ev.push_back(e);
        for (int step = 0; step < 40000; ++step)
        {
            double k1e, k1w, k2e, k2w, k3e, k3w, k4e, k4w;
            rhs(e, w, k1e, k1w);
            rhs(e + 0.5 * h * k1e, w + 0.5 * h * k1w, k2e, k2w);
            rhs(e + 0.5 * h * k2e, w + 0.5 * h * k2w, k3e, k3w);
            rhs(e + h * k3e, w + h * k3w, k4e, k4w);
            const double en = juce::jlimit(1e-6, 0.999, e + h / 6.0 * (k1e + 2 * k2e + 2 * k3e + k4e));
            w += h / 6.0 * (k1w + 2 * k2w + 2 * k3w + k4w);
            if (en > e + 1e-12) rose = true;
            if (rose && en > e && ev.size() > 10 && e < ev[ev.size() - 2]) break;   // passed the next minimum
            e = en; ev.push_back(e);
        }
        const size_t N = ev.size();
        for (int k = 0; k < kKlPhases; ++k)
        {
            const double x = (double)k / kKlPhases * (double)(N - 1);
            const size_t i0 = (size_t)x; const size_t i1 = juce::jmin(N - 1, i0 + 1);
            table[(size_t)it * kKlPhases + (size_t)k] = (float)(ev[i0] + (ev[i1] - ev[i0]) * (x - (double)i0));
        }
    }
}

float CosmicsynthAudioProcessor::klEccentricityAt(float tiltDeg, double phase01) const
{
    const float ti = juce::jlimit(0.0f, (float)(kKlTilts - 1),
        (tiltDeg - (float)kKlTiltMin) / (float)(kKlTiltMax - kKlTiltMin) * (float)(kKlTilts - 1));
    const int t0 = juce::jmin((int)ti, kKlTilts - 2);
    const float tf = ti - (float)t0;
    const double p = (phase01 - std::floor(phase01)) * kKlPhases;
    const int p0 = (int)p % kKlPhases, p1 = (p0 + 1) % kKlPhases;
    const float pf = (float)(p - std::floor(p));
    auto at = [&](int t, int k) { return klETable[(size_t)t * kKlPhases + (size_t)k]; };
    const float a = at(t0, p0) + (at(t0, p1) - at(t0, p0)) * pf;
    const float b = at(t0 + 1, p0) + (at(t0 + 1, p1) - at(t0 + 1, p0)) * pf;
    return a + (b - a) * tf;
}

//==============================================================================

const juce::String CosmicsynthAudioProcessor::getName() const { return JucePlugin_Name; }

bool CosmicsynthAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool CosmicsynthAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool CosmicsynthAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double CosmicsynthAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int    CosmicsynthAudioProcessor::getNumPrograms() { return kNumPresets; }
int    CosmicsynthAudioProcessor::getCurrentProgram()
{
    return static_cast<int>(getCurrentPreset());
}
void   CosmicsynthAudioProcessor::setCurrentProgram(int index)
{
    setPreset(static_cast<PresetId>(juce::jlimit(0, kNumPresets - 1, index)));
}
const  juce::String CosmicsynthAudioProcessor::getProgramName(int index)
{
    return presetName(static_cast<PresetId>(index));
}
void   CosmicsynthAudioProcessor::changeProgramName(int, const juce::String&) {}

//==============================================================================

void CosmicsynthAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    smAmp.prepare(sampleRate);
    smPitch.prepare(sampleRate);
    smCut.prepare(sampleRate);
    smTimbre.prepare(sampleRate);
    smPan.prepare(sampleRate);
    setSmoothMs(30.0);

    noteDown.fill(false);
    chirpPhase.fill(0.0);
    noteJustOn.fill(false);
    chirpCentsPerNote.fill(ps.chirpCentsMin);
    noteOnH.fill(0.0f);
    noteOnFgw.fill(0.0f);
    noteOnPsi.fill(0.0f);
    noteOnVelocity.fill(0.5f);
    noteAge.fill(0.0);
    bellDecaySec.fill(1.8);
    bellAge.fill(0.0);
    bellDetuneGain.fill(0.55f);
    bellSpinAlpha.fill(0.5f);
    gwMemoryDrift = (double)ps.reverbBase;  // start at preset base reverb
    gwMemoryTarget = (double)ps.reverbBase;
    transientPhase.fill(1.0e3);
    fgwWalkPos.fill(gw.getT0());
    hModWalkPos.fill(gw.getT0());
    noteInRingdown.fill(true);    // idle notes count as 'finished chirping'
    lastChirpCents.fill(0.0f);
    ringdownPhase.fill(0.0);
    gwCsvWalkPos = gw.getT0();
    padHWalkPos = gw.getT0();
    gwSinceMergerSec = 1.0e9;
    prevWobCents = 8.0f;
    padHWalkPosDrift = gw.getT0();
    lastNoteOn = -1;

    currentMidiNote.store(-1, std::memory_order_relaxed);
    currentNoteHz.store(0.0f, std::memory_order_relaxed);
    lastBlockTimeSec.store(0.0, std::memory_order_relaxed);

    freeRunSeconds = 0.0;
    noteTimeSec = 0.0;
    wasAnyNoteDown = false;

    rngState = (uint32_t)juce::Time::getMillisecondCounter() ^ 0xA53A9D1Bu;

    gwEventActive = false;
    gwWaitSec = (double)ps.gwWaitMin;
    gwEventPhase01 = 0.0;
    gwEventDurSec = (double)ps.gwEventDurMin;
    gwVibPhase = 0.0;
    gwPeakScale = 1.0f;
    gwCsvStartT = 0.0;
    gwCsvSpanSec = (double)ps.gwCsvSpanMin;

    prevGainEnd = 0.20f;
    passiveDriftPhase = 0.0;

    smAmp.reset(1.0f);
    smCut.reset(1.0f);
    smTimbre.reset(0.0f);
    smPitch.reset(0.0f);

    synth.prepareToPlay(sampleRate);
    applyPresetToEngine();
}

void CosmicsynthAudioProcessor::setSmoothMs(double ms)
{
    smAmp.setTimeMs(ms);
    smPitch.setTimeMs(ms);
    smCut.setTimeMs(ms);
    smTimbre.setTimeMs(ms);
    smPan.setTimeMs(ms);
}

void CosmicsynthAudioProcessor::releaseResources() {}

#ifndef JucePlugin_PreferredChannelConfigurations
bool CosmicsynthAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif
    return true;
#endif
}
#endif

//==============================================================================
// MAIN AUDIO CALLBACK

void CosmicsynthAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // setPreset() runs on the UI thread and rewrites `ps` and all per-note state.
    // Hold the same lock here so a preset switch can never land mid-block.
    // (Uncontended almost always, so this costs next to nothing.)
    const juce::ScopedLock presetGuard(presetLock);
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const double sr = getSampleRate();
    const int    nSamp = buffer.getNumSamples();
    const double dt = (sr > 0.0) ? ((double)nSamp / sr) : 0.0;

    //=========================
    // Params
    //=========================
    const bool  gwEnabled = apvts.getRawParameterValue(p_gwEnable)->load() > 0.5f;
    const float depthAmp = apvts.getRawParameterValue(p_gwGainDepth)->load();
    const float depthPitch = apvts.getRawParameterValue(p_gwPitchDepth)->load();
    const float depthCut = apvts.getRawParameterValue(p_gwFilterDepth)->load();
    const float depthTimb = apvts.getRawParameterValue(p_gwTimbreDepth)->load();
    const float smoothMs = apvts.getRawParameterValue(p_gwSmoothingMs)->load();

    setSmoothMs(smoothMs);
    // These smoothers step once per buffer, not once per sample: tell them the
    // buffer size so the Smoothing knob's milliseconds are real milliseconds.
    for (auto* s : { &smAmp, &smPitch, &smCut, &smTimbre, &smPan })
        s->setSamplesPerCall(nSamp);

    // Orbit pair fades out unless Binary Pad drives it this block
    synth.setOrbitTarget(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    //=========================
    // Timebase
    //=========================
    double tBlockStart = 0.0;
    juce::AudioPlayHead::CurrentPositionInfo pos;
    if (auto* ph = getPlayHead(); ph && ph->getCurrentPosition(pos))
    {
        tBlockStart = pos.timeInSeconds;
        if (pos.bpm > 1.0) hostBpm = pos.bpm;
    }
    else
    {
        tBlockStart = freeRunSeconds;
        freeRunSeconds += dt;
    }
    lastBlockTimeSec.store(tBlockStart, std::memory_order_relaxed);

    //=========================
    // Note tracking + chirp phase
    //=========================
    noteJustOn.fill(false);

    for (const auto md : midiMessages)
    {
        const auto msg = md.getMessage();
        if (msg.isNoteOn())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < 128)
            {
                noteDown[(size_t)n] = true;
                noteJustOn[(size_t)n] = true;
                chirpPhase[(size_t)n] = 0.0;
                transientPhase[(size_t)n] = 1.0e3;   // no spike at onset — it fires at merger
                noteInRingdown[(size_t)n] = false;
                lastChirpCents[(size_t)n] = 0.0f;
                ringdownPhase[(size_t)n] = 0.0;
                lastNoteOn = n;

                if (gwEnabled && ps.chirpEnabled && gwLoaded.load(std::memory_order_relaxed)
                    && getCurrentPreset() != PresetId::RingdownBell)
                {
                    const double t0 = gw.getT0();
                    const double tN = gw.getTN();
                    const double tMerge = gwHasRingdown ? gwTMerger : tN;
                    gwCsvWalkPos = juce::jlimit(t0, tN, gwCsvWalkPos);
                    // Every note needs some inspiral left to chirp through
                    if (gwCsvWalkPos > tMerge - 0.03) gwCsvWalkPos = t0;

                    // Set this note's real-time walk start position
                    fgwWalkPos[(size_t)n] = gwCsvWalkPos;
                    hModWalkPos[(size_t)n] = gwCsvWalkPos;

                    const auto frame = gw.sampleAt(gwCsvWalkPos);
                    noteOnFgw[(size_t)n] = (float)frame.fgw;

                    // Walk speed chosen so EVERY note reaches merger after
                    // chirpTimeMs — same timing for every note, whatever part of
                    // the inspiral it started in. The segment is squeezed linearly
                    // in time, so the pitch still follows the real f(t) shape.
                    const double chirpSec = juce::jmax(0.02, (double)ps.chirpTimeMs / 1000.0);
                    noteWalkSpeed[(size_t)n] = juce::jmax(0.0, tMerge - gwCsvWalkPos) / chirpSec;

                    // Depth: notes struck early in the inspiral travel further in
                    // frequency, so they start further below the note.
                    const double fullOct = juce::jmax(0.1, std::log2(gwFgwMerger / juce::jmax(1.0, gwFgwMin)));
                    const double octHere = std::log2(gwFgwMerger / juce::jmax(1.0, frame.fgw));
                    const float  octFrac = (float)juce::jlimit(0.0, 1.0, octHere / fullOct);
                    const float  vel = msg.getFloatVelocity();
                    noteChirpDepth[(size_t)n] = (ps.chirpCentsMax - ps.chirpCentsMin)
                        * (0.35f + 0.65f * octFrac) * (0.4f + 0.6f * vel) + ps.chirpCentsMin;
                    lastChirpCents[(size_t)n] = -noteChirpDepth[(size_t)n];

                    // h → merger "ding" intensity (local wave amplitude)
                    noteOnH[(size_t)n] = (float)juce::jlimit(0.0, 1.0,
                        hEnvelopeAt(gwCsvWalkPos) / gwPeakEnv);

                    noteOnPsi[(size_t)n] = (float)frame.psi;
                    noteOnVelocity[(size_t)n] = vel;
                    noteAge[(size_t)n] = 0.0;

                    viz.pluckNoteCount.fetch_add(1, std::memory_order_relaxed);

                    // Advance global walk — next note starts further in the inspiral
                    gwCsvWalkPos += (double)ps.gwCsvWalkStepSec;
                    if (gwCsvWalkPos > tN) gwCsvWalkPos = t0;
                }
                else
                {
                    // GW LIVE off (or a preset without chirps): this note does not
                    // chirp, and won't start a stale chirp if GW LIVE comes back on.
                    noteInRingdown[(size_t)n] = true;
                }
            }
        }
        else if (msg.isNoteOff())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < 128) noteDown[(size_t)n] = false;
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            noteDown.fill(false);
            noteJustOn.fill(false);
            lastNoteOn = -1;
        }
    }

    // chirpPhase kept for reference but real-time walk handles timing now
    // transientPhase advanced inside the chirp block below

    int chosen = -1;
    if (lastNoteOn >= 0 && noteDown[(size_t)lastNoteOn])
        chosen = lastNoteOn;
    else
        for (int n = 0; n < 128; ++n)
            if (noteDown[(size_t)n]) { chosen = n; break; }

    currentMidiNote.store(chosen, std::memory_order_relaxed);
    currentNoteHz.store(
        (chosen >= 0) ? (float)juce::MidiMessage::getMidiNoteInHertz(chosen) : 0.0f,
        std::memory_order_relaxed);

    const bool anyDown = (chosen >= 0);

    if (anyDown && !wasAnyNoteDown)
    {
        noteTimeSec = 0.0;
        gwEventActive = false;
        gwWaitSec = (double)randRange(ps.gwWaitMin * 0.2f, ps.gwWaitMin);
        gwEventPhase01 = 0.0;
        gwVibPhase = 0.0;
        prevGainEnd = 0.20f;
        passiveDriftPhase = 0.0;

        smAmp.reset(1.0f);
        smCut.reset(1.0f);
        smTimbre.reset(0.0f);
        smPitch.reset(0.0f);
    }

    if (anyDown) noteTimeSec += dt;
    // A new phrase after a real pause (> 2 s) restarts the Kozai-Lidov cycle at
    // its calm end; short gaps between notes don't, so sequenced parts evolve.
    if (anyDown && !wasAnyNoteDown && silenceSec > 2.0) klCyclePh = 0.0;
    silenceSec = anyDown ? 0.0 : silenceSec + dt;
    wasAnyNoteDown = anyDown;

    //=========================
    // Per-block defaults
    //=========================
    std::array<float, 128> ratiosStart{}, ratiosEnd{};
    ratiosStart.fill(1.0f);
    ratiosEnd.fill(1.0f);

    constexpr float baseGain = 0.20f;
    const     float baseCutoff = ps.filterBaseline;
    const     float baseTimbre = ps.waveshapeMix;

    float gainStart = prevGainEnd;
    float gainEnd = baseGain;
    float cutoffMid = baseCutoff;
    float timbreMid = baseTimbre;

    //=========================
    // Preset-specific GW modulation
    //=========================
    const PresetId pid = getCurrentPreset();

    // New presets: physics that doesn't need the CSV (a triple system, a lens)
    synth.setLens(pid == PresetId::GravLens && gwEnabled,
        apvts.getRawParameterValue(p_lensMass)->load());

    if (pid == PresetId::KozaiLidov)
    {
        const float tilt = apvts.getRawParameterValue(p_klTilt)->load();
        const double cycleSec = (double)ps.klCycleBeats * 60.0 / hostBpm;
        klCyclePh += dt / juce::jmax(0.5, cycleSec);
        klCyclePh -= std::floor(klCyclePh);

        // GW Enable off = no third body: circular orbit, steady pure tone
        const float e = gwEnabled ? klEccentricityAt(tilt, klCyclePh) : 0.0f;
        // Radiated power grows as the Peters-Mathews enhancement f(e); heard
        // compressed so the peak is louder but never overwhelming.
        auto fPM = [](double x) { return (1.0 + 73.0 / 24.0 * x * x + 37.0 / 96.0 * x * x * x * x)
            / std::pow(1.0 - x * x, 3.5); };
        const float level = (float)std::pow(fPM(e) / fPM(0.1), (double)ps.klLevelExp);
        const float orbitHz = (float)(hostBpm / 60.0 / juce::jmax(0.125f, ps.klOrbitBeats));
        synth.setKozaiLidov(e, gwEnabled ? orbitHz : 0.0f, level);
        klCurrentE.store(e, std::memory_order_relaxed);

        gainEnd = baseGain * 1.0f;   // peaks near the other presets (~0.5)
        cutoffMid = baseCutoff;
        timbreMid = baseTimbre;
    }
    else if (pid == PresetId::GravLens)
    {
        gainEnd = baseGain * 0.75f;  // six-note chords stay below ~0.55
        cutoffMid = baseCutoff;
        timbreMid = baseTimbre;
    }
    else if (gwEnabled && gwLoaded.load(std::memory_order_relaxed) && anyDown)
    {
        if (pid == PresetId::BinaryPad)
            processGW_BinaryPad(dt, depthAmp, depthPitch, depthCut, depthTimb,
                anyDown, ratiosStart, ratiosEnd, gainEnd, cutoffMid, timbreMid);
        else if (pid == PresetId::RingdownBell)
            processGW_RingdownBell(dt, depthAmp, depthPitch, depthCut, depthTimb,
                anyDown, ratiosStart, ratiosEnd, gainEnd, cutoffMid, timbreMid);
        else
            processGW_InspiralPluck(dt, depthAmp, depthPitch, depthCut, depthTimb,
                anyDown, ratiosStart, ratiosEnd, gainEnd, cutoffMid, timbreMid);
    }
    else
    {
        if (!anyDown)
        {
            gwEventActive = false;
            gwWaitSec = (double)ps.gwWaitMin;
            gwEventPhase01 = 0.0;
            gwVibPhase = 0.0;
        }

        smAmp.process(1.0f);
        smCut.process(1.0f);
        smTimbre.process(0.0f);
        smPitch.process(0.0f);

        gainEnd = baseGain;
        cutoffMid = baseCutoff;
        timbreMid = baseTimbre;
    }

    // =========================================================
    // Real-time GW chirp — applies per note, pluck preset only
    // =========================================================
    // Pluck — "chirp into the note"
    //
    // Each note starts BELOW its pitch and rises into it along the real chirp
    // f(t) from the CSV, arriving exactly on pitch at merger, chirpTimeMs after
    // note-on. Every note has the same timing and the same envelope length; what
    // varies is the swoop depth and shape, set by where in the inspiral it began.
    //
    //   - merger does not chop the volume; the note rings on in tune
    //   - notes END in tune
    //   - the brightness spike fires at merger ("ding"), not on the attack
    //   - notes keep chirping after key release, so staccato notes still land in tune
    //   - GW LIVE off = no chirp at all
    if (gwEnabled && ps.chirpEnabled && gwLoaded.load(std::memory_order_relaxed))
    {
        const double tN = gw.getTN();
        const double tMergeHere = gwHasRingdown ? gwTMerger : tN;
        const double transientDecaySec = juce::jmax(0.001,
            static_cast<double>(ps.transientDecayMs) / 1000.0);

        for (int n = 0; n < 128; ++n)
        {
            const bool chirping = !noteInRingdown[(size_t)n] && noteAge[(size_t)n] < 2.0;
            if (!noteDown[(size_t)n] && !noteJustOn[(size_t)n] && !chirping) continue;

            noteAge[(size_t)n] += dt;

            // Pitch at the start of this block = where the last block ended
            // (a new note: its starting depth below the note, set at note-on)
            const float centsAtBlockStart = lastChirpCents[(size_t)n];

            if (!noteInRingdown[(size_t)n])
            {
                fgwWalkPos[(size_t)n] = juce::jmin(tMergeHere,
                    fgwWalkPos[(size_t)n] + dt * noteWalkSpeed[(size_t)n]);

                if (fgwWalkPos[(size_t)n] >= tMergeHere)
                {
                    noteInRingdown[(size_t)n] = true;   // arrived: merger
                    ringdownPhase[(size_t)n] = 0.0;
                    transientPhase[(size_t)n] = 0.0;    // fire the merger "ding"
                    lastChirpCents[(size_t)n] = 0.0f;
                }
                else
                {
                    // Fraction of this note's own frequency journey completed,
                    // in log-frequency (= musical pitch)
                    const double fOn = juce::jmax(1.0, (double)noteOnFgw[(size_t)n]);
                    const double fTop = juce::jmax(fOn * 1.0001, gwFgwMerger);
                    const double fNow = juce::jlimit(fOn, fTop, gw.sampleAt(fgwWalkPos[(size_t)n]).fgw);
                    const float  frac = (float)(std::log(fNow / fOn) / std::log(fTop / fOn));
                    lastChirpCents[(size_t)n] = -noteChirpDepth[(size_t)n] * (1.0f - frac);
                }
            }
            else
            {
                ringdownPhase[(size_t)n] += dt;
            }
            if (n == lastNoteOn)
            {
                viz.pluckFrac.store(noteInRingdown[(size_t)n] ? 1.0f
                    : (float)juce::jlimit(0.0, 1.0, noteAge[(size_t)n] / juce::jmax(0.02, (double)ps.chirpTimeMs / 1000.0)),
                    std::memory_order_relaxed);
                viz.pluckPos.store(fgwWalkPos[(size_t)n], std::memory_order_relaxed);
            }

            // Glide from the block-start pitch to the block-end pitch sample by
            // sample. (Setting both ends to the new pitch made the chirp climb in
            // one step per audio buffer, ~100 steps a second: a zipper crackle.)
            ratiosStart[(size_t)n] *= std::pow(2.0f, centsAtBlockStart / 1200.0f);
            ratiosEnd[(size_t)n] *= std::pow(2.0f, lastChirpCents[(size_t)n] / 1200.0f);

            // --- Merger "ding": short brightness spike when the note lands ---
            if (ps.transientDepthMax > 0.0f)
            {
                const double tTransient = transientPhase[(size_t)n];
                if (tTransient < transientDecaySec * 4.0)
                {
                    const float tEnv = (float)std::exp(-4.0 * tTransient / transientDecaySec);
                    const float hNorm = 0.4f + 0.6f * noteOnH[(size_t)n];
                    const float spikeMix = ps.transientDepthMax * hNorm * tEnv;
                    timbreMid = juce::jmax(timbreMid,
                        baseTimbre + spikeMix * (0.8f - baseTimbre));
                }
                transientPhase[(size_t)n] += dt;
            }
        }
    }

    prevGainEnd = gainEnd;

    synth.setFilterCutoff(cutoffMid);
    synth.setWaveshapeMix(timbreMid);

    // GW-reactive reverb bloom (event-driven presets only).
    // The Kerr Quasinormal Modes preset owns the reverb through its memory
    // bloom, so this block skips it (otherwise it would reset the reverb to
    // reverbBase every block and the bloom would never be heard).
    if (pid == PresetId::RingdownBell)
    {
        // Keep the memory settling even with no keys held (processGW_RingdownBell
        // only runs while a note is down).
        if (!(gwEnabled && gwLoaded.load(std::memory_order_relaxed) && anyDown))
            advanceGravMemory(dt);
    }
    else
    {
        const float ph = (float)juce::jlimit(0.0, 1.0, gwEventPhase01);
        float reverbEnv = 0.0f;
        if (gwEventActive || ph > 0.001f)
        {
            if (ph < 0.75f) reverbEnv = std::pow(ph / 0.75f, 1.5f);
            else if (ph < 0.80f) reverbEnv = 1.0f;
            else                 reverbEnv = std::exp(-2.5f * ((ph - 0.80f) / 0.20f));
        }
        reverbEnv = juce::jlimit(0.0f, 1.0f, reverbEnv);
        synth.setReverbAmount(ps.reverbBase + (ps.reverbPeak - ps.reverbBase) * reverbEnv);
    }

    synth.processBlockRamped(buffer, midiMessages,
        ratiosStart, ratiosEnd, gainStart, gainEnd);

    // ---- UI feeds ----
    viz.klOrbitPhase.store(synth.getKozaiOrbitPhase(), std::memory_order_relaxed);
    viz.klCycle.store((float)klCyclePh, std::memory_order_relaxed);
    viz.bpm.store((float)hostBpm, std::memory_order_relaxed);
    if (pid != PresetId::BinaryPad || !anyDown) viz.padEvent.store(false, std::memory_order_relaxed);
    if (pid != PresetId::RingdownBell || !anyDown) viz.bellEnv.store(0.0f, std::memory_order_relaxed);

    // Mono tap for the spectrogram. If the editor is closed the FIFO fills and
    // the extra samples are simply dropped.
    {
        const int n = buffer.getNumSamples();
        const auto scope = scopeFifo.write(juce::jmin(n, scopeFifo.getFreeSpace()));
        const int nch = juce::jmin(2, buffer.getNumChannels());
        auto mono = [&](int i)
            {
                float v = 0.0f;
                for (int c = 0; c < nch; ++c) v += buffer.getSample(c, i);
                return nch > 0 ? v / (float)nch : 0.0f;
            };
        for (int i = 0; i < scope.blockSize1; ++i) scopeBuf[(size_t)(scope.startIndex1 + i)] = mono(i);
        for (int i = 0; i < scope.blockSize2; ++i) scopeBuf[(size_t)(scope.startIndex2 + i)] = mono(scope.blockSize1 + i);
    }
}

//==============================================================================
// Binary Pad GW modulation
//
// Pitch is driven directly by the signed h column of the CSV —
// the actual gravitational wave strain waveform. No synthetic LFO.
// During events, padHWalkPos advances through the CSV at a speed
// proportional to fgw (faster as the orbit tightens), so the pitch
// oscillation rate naturally accelerates toward merger.
// Between events, padHWalkPosDrift advances slowly for ambient movement.

void CosmicsynthAudioProcessor::processGW_BinaryPad(
    double dt, float depthAmp, float depthPitch, float depthCut, float depthTimb,
    bool /*anyDown*/,
    std::array<float, 128>& ratiosStart, std::array<float, 128>& ratiosEnd,
    float& gainEnd, float& cutoffMid, float& timbreMid)
{
    constexpr float baseGain = 0.20f;
    const     float baseCutoff = ps.filterBaseline;
    const     float baseTimbre = ps.waveshapeMix;
    const     double t0 = gw.getT0();
    const     double tN = gw.getTN();
    constexpr double kMergerPhase = 0.80;   // event phase at which the CSV reaches merger

    // --- Event scheduler ---
    if (!gwEventActive)
    {
        gwWaitSec -= dt;
        if (gwWaitSec <= 0.0)
        {
            startNewGWEvent(0.0);
            // Sync padHWalkPos to event CSV start — walk and event are now locked together
            padHWalkPos = gwCsvStartT;
        }
    }

    float env = 0.0f;

    if (gwEventActive)
    {
        gwEventPhase01 += dt / juce::jmax(1e-6, gwEventDurSec);
        if (gwEventPhase01 >= 1.0)
        {
            gwEventActive = false;
            gwEventPhase01 = 1.0;
            gwWaitSec = (double)(ps.gwWaitMin
                + (ps.gwWaitMax - ps.gwWaitMin) * std::sqrt(rand01()));
        }

        const float ph = (float)juce::jlimit(0.0, 1.0, gwEventPhase01);

        // Event arc: inspiral growth → peak AT merger (phase 0.8) → ringdown decay.
        if (ph < 0.75f)      env = std::pow(ph / 0.75f, 1.5f);
        else if (ph < 0.80f) env = 1.0f;
        else                 env = std::exp(-5.0f * ((ph - 0.80f) / 0.20f));
        env = juce::jlimit(0.0f, 1.0f, env);
    }

    // -----------------------------------------------------------------------
    // Where are we in the GW story?
    //
    // eventPos walks the event's CSV segment and reaches merger at phase 0.8.
    // chirpProg (0..1) = how far fgw has risen, in log-frequency, from the
    // event's start to merger — the chirp curve, straight from the data.
    // gwSinceMergerSec keeps running after the event ends so the ringdown
    // (wobble settling, orbit fading) finishes naturally.
    // -----------------------------------------------------------------------
    const double eventPos = gwEventActive
        ? juce::jlimit(t0, tN, gwCsvStartT
            + gwCsvSpanSec * juce::jmin(1.0, gwEventPhase01 / kMergerPhase))
        : juce::jlimit(t0, tN, padHWalkPos);

    if (gwEventActive && gwEventPhase01 >= kMergerPhase && gwSinceMergerSec > 1.0e8)
        gwSinceMergerSec = (gwEventPhase01 - kMergerPhase) * gwEventDurSec;   // just merged
    else if (gwSinceMergerSec < 1.0e8)
        gwSinceMergerSec += dt;
    const bool   postMerger = gwSinceMergerSec < 1.0e8;
    const double tPost = postMerger ? gwSinceMergerSec : 0.0;

    const bool   inspiral = gwEventActive && !postMerger;
    const double fStart = juce::jmax(1.0, gw.sampleAt(gwCsvStartT).fgw);
    const double fTop = juce::jmax(fStart * 1.01, gwFgwMerger);
    const double fNow = juce::jlimit(fStart, fTop, gw.sampleAt(eventPos).fgw);
    const float  chirpProg = postMerger ? 1.0f
        : (inspiral ? (float)(std::log(fNow / fStart) / std::log(fTop / fStart)) : 0.0f);

    const double denomEnv = juce::jmax(1e-30, gwPeakEnv);
    const float  envNorm = inspiral
        ? (float)juce::jlimit(0.0, 1.0, hEnvelopeAt(eventPos) / denomEnv) : 0.0f;

    // -----------------------------------------------------------------------
    // WOBBLE — the pad's pitch traces the real h waveform, time-stretched.
    //   inspiral: rate 0.35 → ~4.5 Hz and depth growing with |h| — the orbit
    //             speeding up and the wave getting stronger
    //   ringdown: fast wobble that dies away over ~1 s (the remnant settling)
    //   idle:     slow 8-cent drift
    // -----------------------------------------------------------------------
    // Rate ∝ fgw (h oscillates at the GW frequency): one stretch constant,
    // 0.6 Hz at the start of the event → ~4.3 Hz at merger for this file.
    const float fRatio = (float)(fNow / fStart);   // 1 at event start, fTop/fStart at merger
    float targetHz = 0.35f;
    float wobCents = 8.0f;
    if (inspiral)
    {
        targetHz = juce::jmin(5.0f, 0.6f * fRatio);
        wobCents = 8.0f + 55.0f * envNorm * (0.3f + 0.7f * env);
    }
    else if (postMerger && tPost < 8.0)
    {
        targetHz = 0.35f + 4.5f * (float)std::exp(-tPost / 1.2);
        wobCents = 8.0f + 55.0f * (float)std::exp(-tPost / 0.9);
    }
    wobCents *= depthPitch / 0.8f;        // Pitch knob (0.8 default = as designed)

    // Stretch the local GW cycles (at padHWalkPos) down to targetHz
    const auto   walkFrame = gw.sampleAt(juce::jlimit(t0, tN, padHWalkPos));
    const float  fgwHz = (float)juce::jlimit(1.0, 2000.0, walkFrame.fgw);
    const double walkSpeed = juce::jlimit(1e-5, 10.0, (double)targetHz / (double)fgwHz);

    // Unit-amplitude waveform shape: h / local envelope → -1..1
    auto shapeAt = [&](double t)
        {
            const double tt = juce::jlimit(t0, tN, t);
            const double e = hEnvelopeAt(tt);
            return (float)juce::jlimit(-1.0, 1.0, (e > 1e-30) ? gw.sampleAt(tt).h / e : 0.0);
        };

    const float motion0 = shapeAt(padHWalkPos);
    padHWalkPos += dt * walkSpeed;
    const double loopEnd = gwHasRingdown ? juce::jmax(t0 + 0.01, gwTMerger) : tN;
    if (padHWalkPos > loopEnd) padHWalkPos = t0;
    const float motion1 = shapeAt(padHWalkPos);

    // Depth ramps across the block too, so growth/decay never steps
    const float bend0 = prevWobCents * motion0;
    const float bend1 = wobCents * motion1;
    prevWobCents = wobCents;

    // --- Apply pitch to all active notes, higher voices wobbling wider ---
    const float maxCents = ps.gwPitchMaxCents * depthPitch / 0.8f;
    std::array<int, 128> active{};
    int cnt = 0;
    for (int n = 0; n < 128; ++n) if (noteDown[(size_t)n]) active[(size_t)cnt++] = n;
    std::sort(active.begin(), active.begin() + cnt);

    auto fillRatios = [&](std::array<float, 128>& ratios, float bendCents)
        {
            for (int i = 0; i < cnt; ++i)
            {
                const int   n = active[(size_t)i];
                const float voiceScale = (i == 0) ? 0.28f : 0.8f * (1.0f + 0.5f * (float)i);
                const float cents = juce::jlimit(-maxCents, maxCents, bendCents * voiceScale);
                ratios[(size_t)n] = std::pow(2.0f, cents / 1200.0f);
            }
        };
    fillRatios(ratiosStart, bend0);
    fillRatios(ratiosEnd, bend1);

    // --- Gain ---
    float e = std::pow(envNorm, 0.40f);
    e = smAmp.process(e);
    const float swell = depthAmp * (0.25f * e + 0.60f * env);
    gainEnd = juce::jlimit(baseGain, ps.gainCeiling, baseGain * (1.0f + swell));

    // --- Filter: opens with the chirp, blooms at merger ---
    const float dataCutoff = baseCutoff + (18000.0f - baseCutoff) * 0.12f * chirpProg;
    const float filterBoost = (9000.0f - baseCutoff) * env * depthCut;
    cutoffMid = juce::jmax(baseCutoff, smCut.process(
        juce::jlimit(baseCutoff, 18000.0f, dataCutoff + filterBoost)));

    // --- Timbre: |h| drives waveshape, event envelope adds brightness ---
    const float timbreEvent = 0.20f * std::sqrt(env) * depthTimb;
    timbreMid = juce::jlimit(baseTimbre, 0.70f,
        baseTimbre + smTimbre.process(envNorm) * depthTimb * 0.5f + timbreEvent);

    // -----------------------------------------------------------------------
    // BINARY ORBIT PAIR — the two black holes
    //
    // Every curve below is a physical scaling law with ONE musical constant:
    //   pitch    one uniform log-frequency map of the real fgw(t) for the whole
    //            signal — inspiral, merger AND ringdown — anchored so the
    //            ringdown frequency f_QNM lands a twelfth (3×) above the start.
    //            The merger→ringdown frequency jump (≈4.5× in this file) is
    //            therefore reproduced, compressed, and lands consonantly.
    //   orbit    orbital frequency = fgw / 2 → rate ∝ fgw           (0.4 Hz × f/f0)
    //   Doppler  orbital speed v ∝ (M f)^(1/3) → swing ∝ fgw^(1/3)  (35 cents × (f/f0)^⅓)
    //   level    |h| envelope ∝ f^(2/3), perceptually compressed (^0.6)
    //   merger   the two bodies fuse (split → 0 in ~0.15 s) and the remnant
    //            rings at f_QNM, decaying exponentially
    // -----------------------------------------------------------------------
    if (ps.orbitLevel > 0.0f)
    {
        int lowest = -1;
        for (int n = 0; n < 128; ++n) if (noteDown[(size_t)n]) { lowest = n; break; }

        if (lowest >= 0 && (inspiral || (postMerger && tPost < 4.0)))
        {
            const double anchorHz = 2.0 * juce::MidiMessage::getMidiNoteInHertz(lowest);
            const double fQnm = gwHasRingdown ? juce::jmax(fTop * 1.01, gwQnmFreq) : fTop * 1.5;
            // octaves of pitch per octave of GW frequency (same for the whole signal)
            const double kMap = (double)ps.orbitOctaves / std::log2(fQnm / fStart);
            const double octMerger = kMap * std::log2(fTop / fStart);

            double oct;
            float level, orbHz, split;
            if (inspiral)
            {
                oct = kMap * std::log2(fNow / fStart);
                level = ps.orbitLevel * std::pow(envNorm, 0.6f)
                    * (float)juce::jmin(1.0, gwEventPhase01 / 0.08);
                orbHz = 0.4f * fRatio;
                split = 1.0f;
            }
            else
            {
                // Merger → ringdown: frequency rises to f_QNM (fast, ~40 ms)
                oct = octMerger + ((double)ps.orbitOctaves - octMerger) * (1.0 - std::exp(-tPost / 0.04));
                level = ps.orbitLevel * (float)std::exp(-tPost / 0.9);
                orbHz = 0.0f;
                split = (float)std::exp(-tPost / 0.15);
            }
            const float doppler = 35.0f * std::cbrt(juce::jmin(fRatio, (float)(fTop / fStart)));
            synth.setOrbitTarget((float)(anchorHz * std::pow(2.0, oct)), level, orbHz, doppler, split);
        }
    }

    // Read-head position and orbit state for the display
    vizCsvPos.store(eventPos, std::memory_order_relaxed);
    viz.padEvent.store(inspiral || (postMerger && tPost < 4.0), std::memory_order_relaxed);
    viz.padFRatio.store(postMerger ? (float)(fTop / fStart) : fRatio, std::memory_order_relaxed);
    viz.padSplit.store(postMerger ? (float)std::exp(-tPost / 0.15) : 1.0f, std::memory_order_relaxed);
    viz.padOrbitHz.store(inspiral ? 0.4f * fRatio : 0.0f, std::memory_order_relaxed);
    viz.padPostMerger.store(postMerger ? (float)tPost : -1.0f, std::memory_order_relaxed);
    vizOrbitHz.store(inspiral || (postMerger && tPost < 4.0) ? 1.0f : 0.0f, std::memory_order_relaxed);
}

//==============================================================================
// Inspiral Pluck GW modulation
//
// The pluck's chirp is per note: each note-on picks a moment in the inspiral,
// starts below its pitch, rises along the data's f(t) and lands in tune at
// merger, chirpTimeMs later, with a short brightness "ding" (see the chirp
// block in processBlock). This function adds the global gain, filter and
// timbre modulation on top.

void CosmicsynthAudioProcessor::processGW_InspiralPluck(
    double dt, float depthAmp, float depthPitch, float depthCut, float depthTimb,
    bool /*anyDown*/,
    std::array<float, 128>& ratiosStart, std::array<float, 128>& ratiosEnd,
    float& gainEnd, float& cutoffMid, float& timbreMid)
{
    // -----------------------------------------------------------------------
    // Inspiral Pluck:
    //
    // Every keypress starts at a later moment in the inspiral. Each note carries
    // its own fgwWalkPos, walked so that every note reaches merger after the same
    // chirpTimeMs; the pitch follows the data's f(t) on the way up and lands
    // exactly on the note at merger, where the merger "ding" fires.
    //
    // This function handles:
    //   1. Global gain context from |h| at the running CSV walk position
    //   2. Brightness tracking: fgw drives filter cutoff — tighter orbit = brighter
    //   3. Timbre from fgw and the orbital phase psi at note-on
    //
    // The per-note chirp ratios are written into ratiosStart/ratiosEnd by the
    // chirp block in processBlock. This function only adds global modulation.
    // -----------------------------------------------------------------------

    constexpr float baseGain = 0.20f;
    const     float baseCutoff = ps.filterBaseline;
    const     float baseTimbre = ps.waveshapeMix;
    const     double t0 = gw.getT0();
    const     double tN = gw.getTN();

    // --- Advance global CSV walk for filter/gain context ---
    // gwCsvWalkPos advances steadily, giving the global sound a sense of
    // progressing through the inspiral arc even between keystrokes
    gwCsvWalkPos += dt * (double)ps.fgwWalkSpeedScale * 0.5;
    if (gwCsvWalkPos > tN) gwCsvWalkPos = t0;

    const auto globalFrame = gw.sampleAt(juce::jlimit(t0, tN, gwCsvWalkPos));
    const float globalFgw = (float)juce::jmax(1.0, globalFrame.fgw);
    const float globalFgwNorm = fgwLogNorm(globalFrame.fgw);
    const float globalHNorm = (float)juce::jlimit(0.0, 1.0,
        hEnvelopeAt(gwCsvWalkPos) / juce::jmax(1e-30, gwPeakEnv));

    // Energy proxy for gain — smoothed |h| from global walk
    float e = std::pow(globalHNorm, 0.40f);
    e = smAmp.process(e);

    // --- Gain ---
    // h amplitude from global walk drives gentle swell — louder as inspiral tightens
    const float swell = depthAmp * 0.50f * e;
    gainEnd = juce::jlimit(baseGain, ps.gainCeiling, baseGain * (1.0f + swell));

    // --- Filter: fgw directly drives brightness ---
    // Higher orbital frequency = brighter timbre. Early inspiral (low fgw)
    // sounds warm/dark. Near merger (high fgw) snaps bright.
    float avgNoteFgw = globalFgw; // fallback to global walk
    int activeCnt = 0;
    for (int n = 0; n < 128; ++n)
    {
        if (!noteDown[(size_t)n]) continue;
        // Use each note's own fgwWalkPos for per-note brightness
        const auto nf = gw.sampleAt(juce::jlimit(t0, tN, fgwWalkPos[(size_t)n]));
        avgNoteFgw += (float)nf.fgw;
        ++activeCnt;
    }
    if (activeCnt > 0) avgNoteFgw /= (float)(activeCnt + 1); // blend with global

    const float fgwBright = fgwLogNorm(avgNoteFgw) * depthCut;
    const float targetCutoff = baseCutoff
        + (16000.0f - baseCutoff) * std::pow(fgwBright, 1.5f);
    cutoffMid = juce::jmax(baseCutoff, smCut.process(targetCutoff));

    // --- Timbre ---
    // psi (orbital phase angle) at note-on gives each note a slightly different
    // waveshape character (a stylization)
    float psiMod = 0.0f;
    int psiCnt = 0;
    for (int n = 0; n < 128; ++n)
    {
        if (!noteDown[(size_t)n]) continue;
        psiMod += std::abs(std::sin(noteOnPsi[(size_t)n] * 0.5f));
        ++psiCnt;
    }
    if (psiCnt > 0) psiMod /= (float)psiCnt;

    // fgwNorm also brightens timbre as orbit tightens
    const float timbreSnap = depthTimb * (0.30f * globalFgwNorm + 0.10f * psiMod);
    timbreMid = juce::jlimit(baseTimbre, 0.80f, baseTimbre + timbreSnap);

    // Per-note pitch ratios are already applied by the chirp block in
    // processBlock — nothing more needed here.
    juce::ignoreUnused(depthPitch, ratiosStart, ratiosEnd);
}

//==============================================================================
// Kerr Quasinormal Modes GW modulation
//
// Physics: after merger the black hole rings in its quasinormal modes.
// Each mode is a damped sinusoid, h(t) ~ e^(-t/τ) sin(ωt).
//   l=m=2 (fundamental): the note pitch, pure sine
//   l=m=3, l=m=4:        inharmonic overtones at spin-dependent ratios
//                        (pushBellModes, Berti-Cardoso-Will fits)
//
// From the loaded waveform (analyseGW):
//   - τ of l=m=2 is scaled by the measured quality factor Q = π f τ
//     (dimensionless, so it transfers honestly to any note pitch)
//   - the remnant spin is inverted from Q and sets the overtone ratios
//   - the strike strength comes from |h| early in the ringdown
//
// Pitch is locked: quasinormal frequencies don't drift, so the spectrogram
// shows clean straight lines. No event scheduler — purely note-on triggered.

void CosmicsynthAudioProcessor::processGW_RingdownBell(
    double dt, float depthAmp, float /*depthPitch*/, float depthCut, float depthTimb,
    bool /*anyDown*/,
    std::array<float, 128>& ratiosStart, std::array<float, 128>& ratiosEnd,
    float& gainEnd, float& cutoffMid, float& timbreMid)
{
    // -----------------------------------------------------------------------
    // The data drives three things that DON'T move pitch:
    //
    // 1. DECAY TIME τ — from the measured Q (same for every note; higher spin
    //    → higher Q → longer ring).
    //
    // 2. STRIKE CHARACTER (per note, from |h| at a random point 0–2 e-folds
    //    into the ringdown): near the peak = hard bright strike, further in =
    //    softer.
    //
    // 3. REVERB BLOOM — a stylization inspired by the Christodoulou memory
    //    effect (the real effect is a permanent strain offset, not reverb).
    //    Each strike adds to a reverb bloom that settles over ~12 s.
    //
    // A gentle spin-rate gain shimmer is also added (a stylization).
    // -----------------------------------------------------------------------

    constexpr float baseGain = 0.20f;
    const     float baseCutoff = ps.filterBaseline;
    const     float baseTimbre = ps.waveshapeMix;
    const     double t0 = gw.getT0();
    const     double tN = gw.getTN();

    // -----------------------------------------------------------------------
    // Note-on: strike with the black hole measured by analyseGW()
    //
    //   τ (decay)  — from the ringdown's quality factor Q = π f τ.
    //                Q ≈ 3 is a typical merger remnant.
    //   spin α     — inverted from Q with the Berti–Cardoso–Will fit.
    //   strike     — |h| envelope at a random point in the early ringdown.
    // -----------------------------------------------------------------------
    for (int n = 0; n < 128; ++n)
    {
        if (!noteJustOn[(size_t)n]) continue;

        bellAge[(size_t)n] = 0.0;

        const double qScale = juce::jlimit(0.5, 2.5, gwQnmQ / 3.0);
        bellDecaySec[(size_t)n] = juce::jlimit(0.25, 6.0, (ps.qnmDecayMs / 1000.0) * qScale);

        const float spinAlpha = (float)gwRemnantSpin;
        bellSpinAlpha[(size_t)n] = spinAlpha;

        // Strike point: 0–2 e-folds into the ringdown
        const double tStrike = juce::jlimit(t0, tN,
            gwTMerger + (gwHasRingdown ? gwQnmTau : 0.0) * 2.0 * (double)rand01());
        const float hNorm = (float)juce::jlimit(0.0, 1.0,
            hEnvelopeAt(tStrike) / juce::jmax(1e-30, gwPeakEnv));

        // --- Shimmer gain (stylization): strike energy + spin ---
        bellDetuneGain[(size_t)n] = ps.qnmDetuneGain
            * juce::jlimit(0.15f, 1.0f, 0.15f + 0.55f * hNorm + 0.30f * spinAlpha);

        // --- Reverb bloom (stylization): each strike adds to it ---
        const double bloomStep = 0.06 * (double)hNorm * (1.0 + 0.4 * (double)spinAlpha);
        gwMemoryTarget = juce::jlimit(0.0, 0.95, gwMemoryTarget + bloomStep);
    }

    // Reverb bloom — advance toward target, then decay; push to reverb
    advanceGravMemory(dt);

    // -----------------------------------------------------------------------
    // Per-note: l=m=2 exponential envelope, strike character, shimmer
    // Pitch ratios are NOT modified — pitch is locked at strike
    // -----------------------------------------------------------------------
    float bellEnvMax = 0.0f;
    float attackBright = 0.0f; // brightest active note's attack contribution
    bool  anyBell = false;

    for (int n = 0; n < 128; ++n)
    {
        if (!noteDown[(size_t)n] && !noteJustOn[(size_t)n]) continue;

        bellAge[(size_t)n] += dt;
        const double age = bellAge[(size_t)n];
        const double tau = juce::jmax(0.001, bellDecaySec[(size_t)n]);

        // l=m=2 exponential envelope — the whole amplitude arc
        const float envFund = (float)std::exp(-age / tau);
        bellEnvMax = juce::jmax(bellEnvMax, envFund);
        anyBell = true;

        const float spin = bellSpinAlpha[(size_t)n];
        const float hNorm = bellDetuneGain[(size_t)n] / juce::jmax(0.001f, ps.qnmDetuneGain);

        // --- Strike character from |h| ---
        // High |h| at the strike point = sharp bright strike, settling to a
        // pure sine in ~40 ms. Attack character only, not the sustain.
        constexpr double kAttackDecaySec = 0.040;
        const float attackEnv = (float)std::exp(-age / kAttackDecaySec);
        const float strikeBite = hNorm * attackEnv;
        attackBright = juce::jmax(attackBright, strikeBite);

        // --- Shimmer (stylization): gentle gain oscillation, no pitch ---
        // Rate set by the spin: low spin ~0.5 Hz, high spin ~2 Hz.
        const float beatHz = 0.5f + spin * 1.5f;
        const float beatEnv = bellDetuneGain[(size_t)n] * envFund;
        const float beat = 1.0f + 0.08f * beatEnv
            * std::sin((float)(age * (double)beatHz
                * juce::MathConstants<double>::twoPi));

        // gainEnd is global, so the shimmer is folded into bellEnvMax
        bellEnvMax = juce::jmax(bellEnvMax, envFund * beat);
    }

    juce::ignoreUnused(ratiosStart, ratiosEnd);   // pitch locked

    // -----------------------------------------------------------------------
    // Gain, Filter, Timbre — all driven by the l=m=2 envelope + strike character
    // -----------------------------------------------------------------------
    if (anyBell)
    {
        // Gain: envelope × depthAmp
        const float swell = depthAmp * 0.65f * bellEnvMax;
        gainEnd = juce::jlimit(baseGain * 0.4f, ps.gainCeiling,
            baseGain + (ps.gainCeiling - baseGain) * swell);

        // Filter: two components
        //   1. envelope opens the filter as it rings → closes as it decays
        //   2. strike brightness from |h|, decaying in ~40 ms
        const float attackFilterBoost = attackBright * depthCut * 8000.0f;
        const float envFilterOpen = bellEnvMax * depthCut * 6000.0f;
        const float targetCutoff = baseCutoff + envFilterOpen + attackFilterBoost;
        cutoffMid = juce::jmax(baseCutoff, smCut.process(
            juce::jlimit(baseCutoff, 18000.0f, targetCutoff)));

        // Timbre: pure sine on sustain; strike adds waveshape briefly.
        const float timbreBite = depthTimb * 0.20f * attackBright;
        timbreMid = juce::jlimit(baseTimbre, 0.40f, baseTimbre + timbreBite);
    }
    else
    {
        gainEnd = baseGain;
        cutoffMid = smCut.process(baseCutoff);
        timbreMid = baseTimbre;
    }

    smAmp.process(bellEnvMax);
    viz.bellEnv.store(bellEnvMax, std::memory_order_relaxed);
    smPitch.process(0.0f);
}

//==============================================================================
// Reverb bloom (stylization inspired by the Christodoulou memory effect):
// rises fast at each strike, then settles back to base reverb over ~12 s.
// Called every block for the Kerr Quasinormal Modes preset.

void CosmicsynthAudioProcessor::advanceGravMemory(double dt)
{
    const double attackRate = 1.0 - std::exp(-dt / gwMemoryAttackSec);
    gwMemoryDrift += (gwMemoryTarget - gwMemoryDrift) * attackRate;

    const double decayRate = std::exp(-dt / gwMemoryDecaySec);
    gwMemoryTarget = ps.reverbBase + (gwMemoryTarget - ps.reverbBase) * decayRate;

    synth.setReverbAmount(juce::jlimit(ps.reverbBase, 0.95f, (float)gwMemoryDrift));
}


bool CosmicsynthAudioProcessor::loadGWFile(const juce::File& f)
{
    // The audio thread reads `gw` every block. Loading replaces its data, so
    // pause the audio callback for the duration of the load — otherwise the
    // audio thread can read a half-built series (crash or garbage values).
    suspendProcessing(true);
    gwLoaded.store(false, std::memory_order_relaxed);

    const bool ok = gw.loadFromCSV(f);
    if (ok)
    {
        analyseGW();
        pushBellModes();   // overtone tuning follows the measured spin
        // All walk positions were set before this file existed (usually to 0).
        // Re-anchor them to the new file's time axis.
        resetWalkPositions();
    }

    if (ok)
    {
        dataName = f.getFileName();
        dataPath = f.getFullPathName();
        ++dataVersion;
    }
    gwLoaded.store(ok, std::memory_order_relaxed);
    suspendProcessing(false);
    return ok;
}

void CosmicsynthAudioProcessor::resetWalkPositions()
{
    const double t0 = gw.getT0();
    fgwWalkPos.fill(t0);
    hModWalkPos.fill(t0);
    gwCsvWalkPos = t0;
    padHWalkPos = t0;
    padHWalkPosDrift = t0;
    gwCsvStartT = t0;
    noteInRingdown.fill(true);    // idle notes count as 'finished chirping'
    lastChirpCents.fill(0.0f);
    gwEventActive = false;
    gwEventPhase01 = 0.0;
}

//==============================================================================
// Local |h| amplitude: h oscillates through zero twice per GW cycle, so a single
// sample of |h| is NOT the wave's amplitude. Take the max over one cycle.

double CosmicsynthAudioProcessor::hEnvelopeAt(double t) const
{
    const double t0 = gw.getT0();
    const double tN = gw.getTN();
    const double f = juce::jmax(1.0, gw.sampleAt(juce::jlimit(t0, tN, t)).fgw);
    const double period = 1.0 / f;

    double m = 0.0;
    constexpr int kPts = 8;
    for (int k = 0; k < kPts; ++k)
    {
        const double tk = juce::jlimit(t0, tN, t - 0.5 * period + period * (double)k / (double)kPts);
        m = juce::jmax(m, std::abs(gw.sampleAt(tk).h));
    }
    return m;
}

// Musical pitch is logarithmic, and so is the chirp: most of the inspiral
// sits at low fgw, then it shoots up in the last fraction of a second.
// A linear 0..1 normalisation over 20–800 Hz leaves everything near 0 until
// the final milliseconds; log normalisation spreads the arc out evenly.
float CosmicsynthAudioProcessor::fgwLogNorm(double f) const
{
    const double lo = juce::jmax(1.0, gwFgwMin);
    const double hi = juce::jmax(lo * 1.001, gwFgwMerger);
    const double x = std::log(juce::jmax(lo, f) / lo) / std::log(hi / lo);
    return (float)juce::jlimit(0.0, 1.0, x);
}

//==============================================================================
// One-time analysis of the loaded waveform (message thread, audio suspended).
// Finds merger, ringdown frequency, damping time, Q and the implied remnant spin.

void CosmicsynthAudioProcessor::analyseGW()
{
    const double t0 = gw.getT0();
    const double tN = gw.getTN();
    const double span = juce::jmax(1e-6, tN - t0);

    // --- Pass 1: merger = time of peak |h|; fgw range
    constexpr int kScan = 16384;
    double peakAbsH = -1.0;
    gwTMerger = tN;
    gwFgwMin = 1e9;
    for (int i = 0; i <= kScan; ++i)
    {
        const double t = t0 + span * (double)i / (double)kScan;
        const auto fr = gw.sampleAt(t);
        if (std::abs(fr.h) > peakAbsH) { peakAbsH = std::abs(fr.h); gwTMerger = t; }
        if (fr.fgw > 0.0) gwFgwMin = juce::jmin(gwFgwMin, fr.fgw);
    }
    if (gwFgwMin > 1e8) gwFgwMin = 20.0;

    gwPeakEnv = juce::jmax(1e-30, hEnvelopeAt(gwTMerger));
    // Frequency just before merger = top of the chirp (avoid the QNM jump)
    gwFgwMerger = juce::jmax(gwFgwMin * 1.5,
        gw.sampleAt(juce::jmax(t0, gwTMerger - 0.002)).fgw);

    // --- Pass 2: ringdown. Fit ln(envelope) vs t after the peak (least squares)
    //     env(t) = A e^{-(t - t_m)/tau}  ->  slope = -1/tau
    //     Accept only a genuine decay: good exponential fit (R² > 0.9) that
    //     falls below 10% of the peak. Files without a ringdown fall back.
    gwHasRingdown = false;
    const double tailLen = tN - gwTMerger;
    if (tailLen > 0.0)
    {
        constexpr int kTail = 256;
        double xs[kTail], ys[kTail];
        double sx = 0, sy = 0, sxx = 0, sxy = 0, minE = 1.0; int n = 0;
        for (int i = 1; i <= kTail; ++i)
        {
            const double t = gwTMerger + tailLen * (double)i / (double)kTail;
            const double e = hEnvelopeAt(t) / gwPeakEnv;
            if (e < 0.7 && e > 0.01)         // skip the peak itself and the numerical floor
            {
                const double x = t - gwTMerger, y = std::log(e);
                xs[n] = x; ys[n] = y;
                sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
                minE = juce::jmin(minE, e);
            }
        }
        if (n >= 4)
        {
            const double denom = n * sxx - sx * sx;
            const double slope = (denom != 0.0) ? (n * sxy - sx * sy) / denom : 0.0;
            const double icpt = (sy - slope * sx) / n;
            double ssRes = 0.0, ssTot = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double r = ys[i] - (icpt + slope * xs[i]);
                const double m = ys[i] - sy / n;
                ssRes += r * r; ssTot += m * m;
            }
            const double r2 = (ssTot > 0.0) ? 1.0 - ssRes / ssTot : 0.0;

            if (slope < 0.0 && r2 > 0.9 && minE < 0.1)
            {
                gwQnmTau = -1.0 / slope;
                // read the frequency a few e-folds in, after the merger transition
                gwQnmFreq = gw.sampleAt(juce::jmin(tN, gwTMerger + 3.0 * gwQnmTau)).fgw;
                gwQnmQ = juce::MathConstants<double>::pi * gwQnmFreq * gwQnmTau;
                gwHasRingdown = true;
            }
        }
    }

    // --- Spin from Q: invert Berti, Cardoso & Will (2006) fit for the l=m=2 mode
    //     Q = 0.7 + 1.4187 (1 - a)^(-0.499)
    //     Higher spin -> higher Q -> the hole rings for MORE cycles.
    if (gwHasRingdown && gwQnmQ > 0.75)
    {
        const double oneMinusA = std::pow((gwQnmQ - 0.7) / 1.4187, -1.0 / 0.499);
        gwRemnantSpin = juce::jlimit(0.0, 0.998, 1.0 - oneMinusA);
    }
    else
    {
        gwRemnantSpin = 0.5;
        gwQnmQ = 3.0;
    }
}

bool CosmicsynthAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* CosmicsynthAudioProcessor::createEditor()
{
    return new CosmicsynthAudioProcessorEditor(*this);
}

// Saved with the DAW project: every knob, the preset, and the waveform file
// (empty path = built-in). Reopening a Reaper project restores the exact setup.
void CosmicsynthAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty("preset", static_cast<int>(getCurrentPreset()), nullptr);
    state.setProperty("dataPath", dataPath, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void CosmicsynthAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;
    auto state = juce::ValueTree::fromXml(*xml);
    const int preset = juce::jlimit(0, kNumPresets - 1, (int)state.getProperty("preset", 0));
    const juce::String path = state.getProperty("dataPath", juce::String()).toString();
    apvts.replaceState(state);
    setPreset(static_cast<PresetId>(preset));

    const juce::File f(path);
    if (path.isNotEmpty() && f.existsAsFile())
        loadGWFile(f);
    else if (dataPath.isNotEmpty())
        loadBuiltInWaveform();   // saved file is gone — fall back to the built-in data
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CosmicsynthAudioProcessor();
}