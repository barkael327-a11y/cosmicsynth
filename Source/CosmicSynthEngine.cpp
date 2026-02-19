// CosmicSynthEngine.cpp
#include "CosmicSynthEngine.h"
#include <cmath>
#include <algorithm>

// ================= Utility ======================

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float clampHz(float x) { return x < 20.f ? 20.f : (x > 20000.f ? 20000.f : x); }

// ================= Table + DSP helpers ======================

std::vector<float> CosmicSynthEngine::generateWaveTable()
{
    constexpr int WAVETABLE_LENGTH = 2048;
    std::vector<float> wt((size_t)WAVETABLE_LENGTH);

    constexpr float twoPi = 6.2831853071795864769f;

    float maxAbs = 0.0f;

    for (int i = 0; i < WAVETABLE_LENGTH; ++i)
    {
        const float phase = twoPi * (float)i / (float)WAVETABLE_LENGTH;

        // Gentle phase skew for slight asymmetry (adds life)
        const float skew = 0.02f * std::sinf(phase);

        const float ph = phase + skew;

        // Harmonic structure:
        // Strong fundamental
        // Warm lower harmonics
        // Light upper shimmer (very small amplitude)
        float x =
            1.00f * std::sinf(ph) +           // fundamental
            0.32f * std::sinf(2.0f * ph) +    // warmth
            0.20f * std::sinf(3.0f * ph) +    // body
            0.12f * std::sinf(4.0f * ph) +
            0.08f * std::sinf(5.0f * ph) +
            0.05f * std::sinf(7.0f * ph) +    // shimmer
            0.03f * std::sinf(9.0f * ph);     // air

        wt[(size_t)i] = x;

        maxAbs = std::max(maxAbs, std::abs(x));
    }

    // Proper normalization (cleaner than fixed scale)
    if (maxAbs > 0.0f)
    {
        const float norm = 0.95f / maxAbs;
        for (auto& s : wt)
            s *= norm;
    }

    return wt;
}

float CosmicSynthEngine::softClipTanh(float x, float drive)
{
    // drive >= 1
    const float d = juce::jmax(1.0f, drive);
    const float y = std::tanh(d * x);
    const float n = std::tanh(d); // normalize so max ~1
    return (n > 1.0e-6f) ? (y / n) : y;
}

void CosmicSynthEngine::updateLowpassCoeff()
{
    const float fc = clampHz(filterCutoffHz <= 0.0f ? 12000.0f : filterCutoffHz);

    if (sampleRate <= 0.0)
    {
        lpA = 0.0f;
        return;
    }

    // one-pole coefficient: a = exp(-2*pi*fc/sr)
    const float twoPi = 6.2831853071795864769f;
    lpA = std::exp(-twoPi * fc / (float)sampleRate);
}

void CosmicSynthEngine::initializeOscillators()
{
    constexpr int OSC_COUNT = 128;

    const auto waveTable = generateWaveTable();

    oscillators.clear();
    oscillators.reserve(OSC_COUNT);

    for (int i = 0; i < OSC_COUNT; ++i)
        oscillators.emplace_back(waveTable, sampleRate);
}

// ================= Lifecycle ======================

void CosmicSynthEngine::prepareToPlay(double newSampleRate)
{
    sampleRate = (newSampleRate > 1.0 ? newSampleRate : 44100.0);

    // ------------------------
    // Core defaults (no magic “statics” buried elsewhere)
    // ------------------------
    globalGain = 0.20f;

    // “GW” controls start neutral
    pitchRatio = 1.0f;
    globalFrequency = 0.0f;

    // ------------------------
    // Pad defaults (these are the *initial* state; UI/GW can override later)
    // ------------------------
    filterCutoffHz = 6000.0f;   // base brightness
    waveshapeMix = 0.25f;     // gentle warmth
    stereoWidth = 0.60f;     // stable width

    // ------------------------
    // Per-note state
    // ------------------------
    perNotePitchRatio.fill(1.0f);
    baseHz.fill(0.0f);

    // ------------------------
    // Voices
    // ------------------------
    initializeOscillators();

    // ------------------------
    // Pad ADSR defaults
    // ------------------------
    padEnvParams.attack = 0.60f;
    padEnvParams.decay = 1.80f;
    padEnvParams.sustain = 0.85f;
    padEnvParams.release = 2.50f;

    for (int n = 0; n < 128; ++n)
    {
        ampEnv[(size_t)n].setSampleRate(sampleRate);
        ampEnv[(size_t)n].setParameters(padEnvParams);
        ampEnv[(size_t)n].reset();
    }

    // Smooth frequency changes (pad-friendly)
    constexpr float defaultFreqSlewMs = 20.0f;
    for (auto& osc : oscillators)
        osc.setFreqSlewMs(defaultFreqSlewMs);

    // ------------------------
    // Filter + modulation state
    // ------------------------
    lpZL = 0.0f;
    lpZR = 0.0f;
    updateLowpassCoeff();

    // LFO phases reset
    driftPhase = 0.0f;
    filterLfoPhase = 0.0f;
}

void CosmicSynthEngine::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    int currentSample = 0;

    for (const auto meta : midiMessages)
    {
        const auto msg = meta.getMessage();
        const int eventSample = juce::jlimit(0, numSamples, (int)meta.samplePosition);

        render(buffer, currentSample, eventSample);
        handleMidiEvent(msg);

        currentSample = eventSample;
    }

    render(buffer, currentSample, numSamples);
}

// ================= Rendering ======================

void CosmicSynthEngine::render(juce::AudioBuffer<float>& buffer, int startSample, int endSample)
{
    if (endSample <= startSample)
        return;

    const int nCh = buffer.getNumChannels();
    auto* L = buffer.getWritePointer(0);
    auto* R = (nCh > 1) ? buffer.getWritePointer(1) : nullptr;

    // Deterministic spread: stable pan per MIDI note
    auto notePan = [&](int note) -> float
        {
            const float x = std::sin(0.17f * (float)note);
            return clampf(x * stereoWidth, -1.0f, 1.0f);
        };

    // waveshape amount
    const float mix = clampf(std::abs(waveshapeMix), 0.0f, 1.0f);
    const float drive = 1.0f + 4.0f * mix; // 1..5

    // Drift parameters (global pad drift)
    const float driftHz = 0.12f;
    const float driftDepthCents = 6.0f;
    const float driftPhaseInc = (float)(juce::MathConstants<float>::twoPi * driftHz / (float)sampleRate);

    // Slow filter motion cosmic “breathing”
    const float filterLfoHz = 0.03f;
    const float filterDepthHz = 1500.0f;
    const float filterPhaseInc = (float)(juce::MathConstants<float>::twoPi * filterLfoHz / (float)sampleRate);

    // Treat the current cutoff as the “base” brightness (so UI still matters)
    const float baseCutoffHz = (filterCutoffHz > 0.0f ? filterCutoffHz : 6000.0f);

    for (int i = startSample; i < endSample; ++i)
    {
        //========================
        // 1) Drift (per sample)
        //========================
        driftPhase += driftPhaseInc;
        if (driftPhase >= juce::MathConstants<float>::twoPi)
            driftPhase -= juce::MathConstants<float>::twoPi;

        const float cents = driftDepthCents * std::sin(driftPhase);
        const float driftRatio = std::pow(2.0f, cents / 1200.0f);

        //========================
        // 2) Filter breathing (update coeff occasionally)
        //========================
        filterLfoPhase += filterPhaseInc;
        if (filterLfoPhase >= juce::MathConstants<float>::twoPi)
            filterLfoPhase -= juce::MathConstants<float>::twoPi;

        // Update lowpass coeff every 32 samples (cheap + smooth)
        if ((i & 31) == 0)
        {
            const float filterMod = std::sin(filterLfoPhase); // [-1, +1]
            filterCutoffHz = clampHz(baseCutoffHz + filterDepthHz * filterMod);
            updateLowpassCoeff();
        }

        float sumL = 0.0f;
        float sumR = 0.0f;

        for (int n = 0; n < (int)oscillators.size(); ++n)
        {
            auto& osc = oscillators[(size_t)n];
            if (!osc.isPlaying())
                continue;

            // Update frequency while held (includes drift)
            if (baseHz[(size_t)n] > 0.0f)
            {
                const float hz = clampf(
                    baseHz[(size_t)n] * perNotePitchRatio[(size_t)n] * driftRatio,
                    20.0f, 20000.0f);

                osc.setFrequency(hz);
            }

            const float env = ampEnv[(size_t)n].getNextSample();

            // If released and envelope finished, hard stop voice
            if (env <= 1.0e-5f && baseHz[(size_t)n] <= 0.0f)
            {
                osc.stop();
                continue;
            }

            const float s = osc.getSample() * env;

            const float pan = notePan(n);
            const float gL = std::sqrt(0.5f * (1.0f - pan));
            const float gR = std::sqrt(0.5f * (1.0f + pan));

            sumL += s * gL;
            sumR += s * gR;
        }

        float outL = sumL * globalGain;
        float outR = sumR * globalGain;

        // gentle saturation
        if (mix > 0.0f)
        {
            outL = (1.0f - mix) * outL + mix * softClipTanh(outL, drive);
            outR = (1.0f - mix) * outR + mix * softClipTanh(outR, drive);
        }

        // one-pole lowpass (lpA updated above)
        lpZL = (1.0f - lpA) * outL + lpA * lpZL;
        outL = lpZL;

        if (R)
        {
            lpZR = (1.0f - lpA) * outR + lpA * lpZR;
            outR = lpZR;
        }

        L[i] += outL;
        if (R) R[i] += outR;
    }

    // If more than 2 channels, copy L to others (simple)
    for (int ch = 2; ch < nCh; ++ch)
    {
        auto* dst = buffer.getWritePointer(ch);
        std::copy(L + startSample, L + endSample, dst + startSample);
    }
}


// ================= MIDI handling ======================

void CosmicSynthEngine::handleMidiEvent(const juce::MidiMessage& midiEvent)
{
    if (midiEvent.isNoteOn())
    {
        const int n = midiEvent.getNoteNumber();
        if (n < 0 || n >= (int)oscillators.size())
            return;

        const float hz0 = midiNoteNumberToFrequency(n);
        baseHz[(size_t)n] = hz0;

        // Start at current ratio (drift is applied during render)
        const float hz = clampf(hz0 * perNotePitchRatio[(size_t)n], 20.0f, 20000.0f);
        oscillators[(size_t)n].setFrequency(hz);

        ampEnv[(size_t)n].noteOn();
    }
    else if (midiEvent.isNoteOff())
    {
        const int n = midiEvent.getNoteNumber();
        if (n < 0 || n >= (int)oscillators.size())
            return;

        // Release tail (do not hard stop)
        baseHz[(size_t)n] = 0.0f;
        ampEnv[(size_t)n].noteOff();
    }
    else if (midiEvent.isAllNotesOff() || midiEvent.isAllSoundOff())
    {
        for (int n = 0; n < (int)oscillators.size(); ++n)
        {
            baseHz[(size_t)n] = 0.0f;
            ampEnv[(size_t)n].noteOff();
        }
    }
}

float CosmicSynthEngine::midiNoteNumberToFrequency(int midiNoteNumber)
{
    constexpr float A4 = 440.0f;
    constexpr float A4_NOTE = 69.0f;

    const float semitones = (float)midiNoteNumber - A4_NOTE;
    return A4 * std::powf(2.0f, semitones / 12.0f);
}

// ================= GW control wrappers ======================

void CosmicSynthEngine::setPitchRatio(float ratio)
{
    pitchRatio = clampf(ratio, 0.25f, 4.0f);

    // legacy behavior: uniform ratios
    for (auto& r : perNotePitchRatio)
        r = pitchRatio;
}

void CosmicSynthEngine::setPerNotePitchRatios(const std::array<float, 128>& ratios)
{
    perNotePitchRatio = ratios;

    for (auto& r : perNotePitchRatio)
        r = clampf(r, 0.25f, 4.0f);
}

void CosmicSynthEngine::setGain(float newGain01)
{
    globalGain = clamp01(newGain01);
}

void CosmicSynthEngine::setOscFrequency(float newHz)
{
    globalFrequency = clampf(newHz, 20.0f, 20000.0f);

    // optional override mode intentionally disabled
}

void CosmicSynthEngine::setFilterCutoff(float newHz)
{
    filterCutoffHz = clampf(newHz, 20.0f, 20000.0f);
    updateLowpassCoeff();
}

void CosmicSynthEngine::setWaveshapeMix(float newMixMinus1to1)
{
    waveshapeMix = clampf(newMixMinus1to1, -1.0f, 1.0f);
}

// ================= Ramped Rendering (log-space pitch + stereo + env + DSP + drift) ======================

void CosmicSynthEngine::processBlockRamped(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midiMessages,
    const std::array<float, 128>& ratiosStart,
    const std::array<float, 128>& ratiosEnd,
    float gainStart,
    float gainEnd)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    const int nCh = buffer.getNumChannels();
    auto* L = buffer.getWritePointer(0);
    auto* R = (nCh > 1) ? buffer.getWritePointer(1) : nullptr;

    const float denom = (numSamples > 1) ? (float)(numSamples - 1) : 1.0f;
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

    // Precompute log ratios ONCE per block
    std::array<float, 128> logR0{};
    std::array<float, 128> logR1{};
    for (int n = 0; n < 128; ++n)
    {
        const float r0 = clampf(ratiosStart[(size_t)n], 0.25f, 4.0f);
        const float r1 = clampf(ratiosEnd[(size_t)n], 0.25f, 4.0f);
        logR0[(size_t)n] = std::log(r0);
        logR1[(size_t)n] = std::log(r1);
    }

    auto ratioAt = [&](int sampleIndex, int note) -> float
        {
            const float t = (float)sampleIndex / denom;
            const float logRt = lerp(logR0[(size_t)note], logR1[(size_t)note], t);
            return clampf(std::exp(logRt), 0.25f, 4.0f);
        };

    auto gainAt = [&](int sampleIndex) -> float
        {
            const float t = (float)sampleIndex / denom;
            return clamp01(lerp(gainStart, gainEnd, t));
        };

    auto notePan = [&](int note) -> float
        {
            const float x = std::sin(0.17f * (float)note);
            return clampf(x * stereoWidth, -1.0f, 1.0f);
        };

    const float mix = clampf(std::abs(waveshapeMix), 0.0f, 1.0f);
    const float drive = 1.0f + 4.0f * mix;

    // Drift parameters (same as render)
    const float driftHz = 0.07f;
    const float driftDepthCents = 4.0f;
    const float driftPhaseInc = (float)(juce::MathConstants<float>::twoPi * driftHz / (float)sampleRate);

    auto renderSegment = [&](int start, int end)
        {
            start = juce::jlimit(0, numSamples, start);
            end = juce::jlimit(0, numSamples, end);
            if (end <= start) return;

            for (int i = start; i < end; ++i)
            {
                // Advance drift PER SAMPLE
                driftPhase += driftPhaseInc;
                if (driftPhase >= juce::MathConstants<float>::twoPi)
                    driftPhase -= juce::MathConstants<float>::twoPi;

                const float cents = driftDepthCents * std::sin(driftPhase);
                const float driftRatio = std::pow(2.0f, cents / 1200.0f);

                const float g = gainAt(i);

                float sumL = 0.0f;
                float sumR = 0.0f;

                for (int n = 0; n < (int)oscillators.size(); ++n)
                {
                    auto& osc = oscillators[(size_t)n];
                    if (!osc.isPlaying())
                        continue;

                    // apply ramped ratio only to held notes (+ drift)
                    if (baseHz[(size_t)n] > 0.0f)
                    {
                        const float r = ratioAt(i, n);
                        const float hz = clampf(baseHz[(size_t)n] * r * driftRatio, 20.0f, 20000.0f);
                        osc.setFrequency(hz);
                    }

                    const float env = ampEnv[(size_t)n].getNextSample();

                    if (env <= 1.0e-5f && baseHz[(size_t)n] <= 0.0f)
                    {
                        osc.stop();
                        continue;
                    }

                    const float s = osc.getSample() * env;

                    const float pan = notePan(n);
                    const float gL = std::sqrt(0.5f * (1.0f - pan));
                    const float gR = std::sqrt(0.5f * (1.0f + pan));

                    sumL += s * gL;
                    sumR += s * gR;
                }

                float outL = sumL * g;
                float outR = sumR * g;

                if (mix > 0.0f)
                {
                    outL = (1.0f - mix) * outL + mix * softClipTanh(outL, drive);
                    outR = (1.0f - mix) * outR + mix * softClipTanh(outR, drive);
                }

                lpZL = (1.0f - lpA) * outL + lpA * lpZL;
                outL = lpZL;

                if (R)
                {
                    lpZR = (1.0f - lpA) * outR + lpA * lpZR;
                    outR = lpZR;
                }

                L[i] = outL;
                if (R) R[i] = outR;
            }
        };

    int currentSample = 0;

    for (const auto meta : midiMessages)
    {
        const auto msg = meta.getMessage();
        const int eventSample = juce::jlimit(0, numSamples, (int)meta.samplePosition);

        renderSegment(currentSample, eventSample);
        handleMidiEvent(msg);

        // If this was a note-on, force it to start at correct ramped ratio immediately
        if (msg.isNoteOn())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < (int)oscillators.size())
            {
                const float r = ratioAt(eventSample, n);
                const float hz = clampf(baseHz[(size_t)n] * r, 20.0f, 20000.0f);
                oscillators[(size_t)n].setFrequency(hz);
            }
        }

        currentSample = eventSample;
    }

    renderSegment(currentSample, numSamples);

    // If more than 2 channels, copy L to others
    for (int ch = 2; ch < nCh; ++ch)
    {
        auto* dst = buffer.getWritePointer(ch);
        std::copy(L, L + numSamples, dst);
    }
}
