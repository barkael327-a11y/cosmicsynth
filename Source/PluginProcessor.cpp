// PluginProcessor.cpp
#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <algorithm>

//==============================================================================

CosmicsynthAudioProcessor::~CosmicsynthAudioProcessor() = default;

//==============================================================================
// APVTS layout

juce::AudioProcessorValueTreeState::ParameterLayout
CosmicsynthAudioProcessor::createLayout()
{
    using namespace juce;

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back(std::make_unique<AudioParameterBool>(
        p_gwEnable, "GW Enable", true));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwGainDepth, "GW Gain Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.6f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwPitchDepth, "GW Pitch Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.8f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwFilterDepth, "GW Filter Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.7f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwTimbreDepth, "GW Timbre Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.5f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwPanDepth, "GW Pan Depth",
        NormalisableRange<float>(0.f, 1.f, 0.001f), 0.2f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwSmoothingMs, "GW Smoothing (ms)",
        NormalisableRange<float>(1.f, 200.f, 0.1f), 30.f));

    params.push_back(std::make_unique<AudioParameterFloat>(
        p_gwTimeOffset, "GW Time Offset (s)",
        NormalisableRange<float>(-10.f, 10.f, 0.001f), 0.f));

    return { params.begin(), params.end() };
}

//==============================================================================
// Parameter IDs

const juce::String CosmicsynthAudioProcessor::p_gwEnable("gwEnable");
const juce::String CosmicsynthAudioProcessor::p_gwGainDepth("gwGainDepth");
const juce::String CosmicsynthAudioProcessor::p_gwPitchDepth("gwPitchDepth");
const juce::String CosmicsynthAudioProcessor::p_gwFilterDepth("gwFilterDepth");
const juce::String CosmicsynthAudioProcessor::p_gwTimbreDepth("gwTimbreDepth");
const juce::String CosmicsynthAudioProcessor::p_gwPanDepth("gwPanDepth");
const juce::String CosmicsynthAudioProcessor::p_gwSmoothingMs("gwSmoothingMs");
const juce::String CosmicsynthAudioProcessor::p_gwTimeOffset("gwTimeOffsetSec");

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

int  CosmicsynthAudioProcessor::getNumPrograms() { return 1; }
int  CosmicsynthAudioProcessor::getCurrentProgram() { return 0; }
void CosmicsynthAudioProcessor::setCurrentProgram(int) {}
const juce::String CosmicsynthAudioProcessor::getProgramName(int) { return {}; }
void CosmicsynthAudioProcessor::changeProgramName(int, const juce::String&) {}

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
    lastNoteOn = -1;
    currentMidiNote.store(-1, std::memory_order_relaxed);
    currentNoteHz.store(0.0f, std::memory_order_relaxed);
    lastBlockTimeSec.store(0.0, std::memory_order_relaxed);

    freeRunSeconds = 0.0;
    noteTimeSec = 0.0;

    wasAnyNoteDown = false;

    gwCyclePhase = 0.0;
    gwRatePhase = 0.0;

    synth.prepareToPlay(sampleRate);
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

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const double sr = getSampleRate();
    const int    nSamp = buffer.getNumSamples();

    //=========================
    // Read params
    //=========================
    const bool  gwEnabled = apvts.getRawParameterValue(p_gwEnable)->load() > 0.5f;
    const float depthAmp = apvts.getRawParameterValue(p_gwGainDepth)->load();
    const float depthPitch = apvts.getRawParameterValue(p_gwPitchDepth)->load();
    const float depthCut = apvts.getRawParameterValue(p_gwFilterDepth)->load();
    const float depthTimb = apvts.getRawParameterValue(p_gwTimbreDepth)->load();
    const float smoothMs = apvts.getRawParameterValue(p_gwSmoothingMs)->load();
    const float tOffset = apvts.getRawParameterValue(p_gwTimeOffset)->load();

    setSmoothMs(smoothMs);

    //=========================================
    // Time (UI only)
    //=========================================
    double tBlockStart = 0.0;
    juce::AudioPlayHead::CurrentPositionInfo pos;
    if (auto* ph = getPlayHead(); ph && ph->getCurrentPosition(pos))
    {
        tBlockStart = pos.timeInSeconds + (double)tOffset;
    }
    else
    {
        tBlockStart = freeRunSeconds + (double)tOffset;
        freeRunSeconds += (double)nSamp / sr;
    }
    lastBlockTimeSec.store(tBlockStart, std::memory_order_relaxed);

    //=========================================
    // Track noteDown[] (UI + playhead)
    //=========================================
    for (const auto md : midiMessages)
    {
        const auto msg = md.getMessage();

        if (msg.isNoteOn())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < 128)
            {
                noteDown[(size_t)n] = true;
                lastNoteOn = n;
            }
        }
        else if (msg.isNoteOff())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < 128)
                noteDown[(size_t)n] = false;
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            noteDown.fill(false);
            lastNoteOn = -1;
        }
    }

    int chosen = -1;
    if (lastNoteOn >= 0 && noteDown[(size_t)lastNoteOn])
        chosen = lastNoteOn;
    else
    {
        for (int n = 0; n < 128; ++n)
            if (noteDown[(size_t)n]) { chosen = n; break; }
    }

    currentMidiNote.store(chosen, std::memory_order_relaxed);

    const float noteHz = (chosen >= 0)
        ? (float)juce::MidiMessage::getMidiNoteInHertz(chosen)
        : 0.0f;

    currentNoteHz.store(noteHz, std::memory_order_relaxed);

    const bool anyDown = (chosen >= 0);

    if (anyDown && !wasAnyNoteDown)
    {
        noteTimeSec = 0.0;
        gwCyclePhase = 0.0;
        gwRatePhase = 0.0;
    }

    if (anyDown)
        noteTimeSec += (double)nSamp / sr;

    wasAnyNoteDown = anyDown;

    //=========================================
    // Always render with ramping (stable; no stepping)
    //=========================================
    std::array<float, 128> ratiosStart{};
    std::array<float, 128> ratiosEnd{};
    ratiosStart.fill(1.0f);
    ratiosEnd.fill(1.0f);

    float gainStart = 0.20f;
    float gainEnd = 0.20f;

    // Defaults (non-GW)
    float cutoffMid = 6000.0f;
    float timbreMid = 0.25f;

    //=========================================
    // GW mode (if enabled + loaded)
    //=========================================
    if (gwEnabled && gwLoaded.load(std::memory_order_relaxed))
    {
        const double dur = juce::jmax(1e-9, gw.getTN() - gw.getT0());

        auto wrapToLocal = [&](double tSec) -> double
            {
                double tWrapped = std::fmod(tSec, dur);
                if (tWrapped < 0.0) tWrapped += dur;
                return gw.getT0() + tWrapped;
            };

        const double t0 = wrapToLocal(noteTimeSec + (double)tOffset);
        const double t1 = wrapToLocal(noteTimeSec + (double)tOffset + (double)nSamp / sr);
        const double tMid = wrapToLocal(noteTimeSec + (double)tOffset + 0.5 * (double)nSamp / sr);

        const auto s0 = gw.sampleAt(t0);
        const auto s1 = gw.sampleAt(t1);
        const auto sm = gw.sampleAt(tMid);

        // Gain: safe multiplicative around baseGain (no dips to silence)
        constexpr float baseGain = 0.20f;
        const float a0 = smAmp.process((float)gw.normalizeAbsH(s0.h));
        const float a1 = smAmp.process((float)gw.normalizeAbsH(s1.h));

        gainStart = juce::jlimit(0.0f, 1.0f, baseGain * (1.0f + depthAmp * a0));
        gainEnd = juce::jlimit(0.0f, 1.0f, baseGain * (1.0f + depthAmp * a1));

        // Pitch ratios: same “fracture” idea, but start/end computed for ramping
        auto computeBendCents = [&](const GWFrame& s) -> float
            {
                const float energy01 = (float)gw.normalizeAbsH(s.h);
                const float energySm = smPitch.process(energy01);

                // gentle chirp feel
                const float maxCents = 60.0f * depthPitch;

                // Use |h| as intensity; keep it stable
                const float intensity = juce::jlimit(0.0f, 1.0f, energySm);
                const float motion = std::sin((float)gwRatePhase);

                return maxCents * intensity * motion;
            };

        if (anyDown)
        {
            const double dt = (double)nSamp / sr;
            // rate rises with |h|
            const float e = (float)gw.normalizeAbsH(sm.h);
            const float lfoHz = 0.4f + (8.0f - 0.4f) * e;
            gwRatePhase += 2.0 * juce::MathConstants<double>::pi * (double)lfoHz * dt;
            if (gwRatePhase > 1e6) gwRatePhase = std::fmod(gwRatePhase, 2.0 * juce::MathConstants<double>::pi);
        }

        const float bend0 = computeBendCents(s0);
        const float bend1 = computeBendCents(s1);

        // active notes
        std::array<int, 128> active{};
        int activeCount = 0;
        for (int n = 0; n < 128; ++n)
            if (noteDown[(size_t)n])
                active[(size_t)activeCount++] = n;

        std::sort(active.begin(), active.begin() + activeCount);

        auto fillRatios = [&](std::array<float, 128>& ratios, float bendCents)
            {
                for (int i = 0; i < activeCount; ++i)
                {
                    const int n = active[(size_t)i];
                    const float voiceScale = (i == 0) ? 0.20f : (1.0f + 0.50f * (float)i);
                    const float cents = juce::jlimit(-60.0f * depthPitch, +60.0f * depthPitch, bendCents * voiceScale);
                    ratios[(size_t)n] = std::pow(2.0f, cents / 1200.0f);
                }
            };

        fillRatios(ratiosStart, bend0);
        fillRatios(ratiosEnd, bend1);

        // Filter: smooth in Hz
        const float gwHz = juce::jlimit(1.0f, 12000.0f, (float)sm.fgw);
        const float rawCutoff = 200.0f + 4.0f * gwHz;
        const float target = juce::jlimit(800.0f, 18000.0f, rawCutoff);
        cutoffMid = smCut.process(juce::jmap(depthCut, 800.0f, target));

        // Timbre: continuous signed (no sign-flip jumps)
        const double denom = gw.getMaxAbsH();
        const double x = (denom > 0.0) ? (sm.h / denom) : 0.0;
        const float signedH = (float)juce::jlimit(-1.0, 1.0, x);
        timbreMid = juce::jlimit(-1.0f, 1.0f, smTimbre.process(signedH) * depthTimb);
    }

    // Apply smooth-ish params (engine also smooths cutoff base)
    synth.setFilterCutoff(cutoffMid);
    synth.setWaveshapeMix(timbreMid);

    // Render ramped always (stable)
    synth.processBlockRamped(buffer, midiMessages,
        ratiosStart, ratiosEnd,
        gainStart, gainEnd);
}

//==============================================================================
// GW helpers

bool CosmicsynthAudioProcessor::loadGWFile(const juce::File& f)
{
    const bool ok = gw.loadFromCSV(f);
    gwLoaded.store(ok, std::memory_order_relaxed);
    return ok;
}

//==============================================================================

bool CosmicsynthAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* CosmicsynthAudioProcessor::createEditor()
{
    return new CosmicsynthAudioProcessorEditor(*this);
}

//==============================================================================

void CosmicsynthAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ignoreUnused(destData);
}

void CosmicsynthAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::ignoreUnused(data, sizeInBytes);
}

//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CosmicsynthAudioProcessor();
}
