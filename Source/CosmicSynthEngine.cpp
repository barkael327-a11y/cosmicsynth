// CosmicSynthEngine.cpp
#include "CosmicSynthEngine.h"
#include <algorithm>
#include <cmath>

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float clampHz(float x) { return x < 20.f ? 20.f : (x > 20000.f ? 20000.f : x); }

// G·Msun/c^3 in seconds — converts a lens mass in solar masses to a time scale
static constexpr double kTsun = 4.925491e-6;

// Integer-order Bessel function J_k(x), valid for negative k too
static double besselJ(int k, double x)
{
    if (k < 0) return ((-k) & 1 ? -1.0 : 1.0) * std::cyl_bessel_j((double)(-k), x);
    return std::cyl_bessel_j((double)k, x);
}

// Peters & Mathews (1963): fraction of GW power radiated in harmonic n of the
// orbital frequency, for orbital eccentricity e.
static double petersMathewsG(int n, double e)
{
    const double ne = n * e;
    auto J = [&](int k) { return besselJ(k, ne); };
    const double a = J(n - 2) - 2.0 * e * J(n - 1) + 2.0 / n * J(n) + 2.0 * e * J(n + 1) - J(n + 2);
    const double b = J(n - 2) - 2.0 * J(n) + J(n + 2);
    const double c = J(n);
    return std::pow((double)n, 4.0) / 32.0 * (a * a + (1.0 - e * e) * b * b + 4.0 / (3.0 * n * n) * c * c);
}

// Hilbert allpass pair (O. Niemitalo): path A delayed one sample vs path B
// gives 90 degrees (±0.7°) from 30 Hz to 16 kHz.
static const float kHilA[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
static const float kHilB[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };

// ================= Wavetable ======================

std::vector<float> CosmicSynthEngine::generateWaveTable(int type)
{
    constexpr int N = 2048;
    std::vector<float> wt((size_t)N);
    const float twoPi = juce::MathConstants<float>::twoPi;
    float maxAbs = 0.0f;

    for (int i = 0; i < N; ++i)
    {
        const float phase = twoPi * (float)i / (float)N;
        float x = 0.0f;

        if (type == 2)
        {
            // Pure sine — a quasinormal mode is a clean damped sinusoid.
            // The bell's character comes from the inharmonic overtone modes instead.
            x = std::sin(phase);
        }
        else if (type == 1)
        {
            // Soft pluck: strong low harmonics rolling off quickly (kalimba/marimba-ish)
            x = 1.00f * std::sin(phase) +
                0.42f * std::sin(2.0f * phase) +
                0.20f * std::sin(3.0f * phase) +
                0.10f * std::sin(4.0f * phase) +
                0.05f * std::sin(5.0f * phase) +
                0.025f * std::sin(6.0f * phase);
        }
        else
        {
            const float ph = phase + 0.02f * std::sin(phase);

            // Slightly reduced upper harmonics vs before for a warmer, darker timbre
            x = 1.00f * std::sin(ph) +
                0.30f * std::sin(2.0f * ph) +
                0.18f * std::sin(3.0f * ph) +
                0.10f * std::sin(4.0f * ph) +
                0.06f * std::sin(5.0f * ph) +
                0.03f * std::sin(7.0f * ph) +
                0.015f * std::sin(9.0f * ph);
        }

        wt[(size_t)i] = x;
        maxAbs = std::max(maxAbs, std::abs(x));
    }
    if (maxAbs > 0.0f) { const float norm = 0.95f / maxAbs; for (auto& s : wt) s *= norm; }
    return wt;
}

float CosmicSynthEngine::softClipTanh(float x, float drive)
{
    const float d = juce::jmax(1.0f, drive);
    const float y = std::tanh(d * x);
    const float n = std::tanh(d);
    return (n > 1.0e-6f) ? (y / n) : y;
}

void CosmicSynthEngine::updateLowpassCoeff()
{
    const float fc = clampHz(filterCutoffHz > 0.0f ? filterCutoffHz : 2800.0f);
    lpA = (sampleRate > 0.0)
        ? std::exp(-juce::MathConstants<float>::twoPi * fc / (float)sampleRate)
        : 0.0f;
}

// ================= FDN Reverb ======================

void CosmicSynthEngine::initReverb()
{
    const float scale = (float)sampleRate / 44100.0f;

    // Prime-ish delay lengths — coprime to prevent flutter
    const int baseLens[FDN_LINES] = { 1481, 1867, 2251, 2837 };

    for (int i = 0; i < FDN_LINES; ++i)
    {
        fdnLen[(size_t)i] = juce::jlimit(256, FDN_MAX - 1, (int)((float)baseLens[i] * scale));
        fdnPos[(size_t)i] = 0;
        fdnBufL[(size_t)i].fill(0.0f);
        fdnBufR[(size_t)i].fill(0.0f);
    }

    shimBufL.fill(0.0f);
    shimBufR.fill(0.0f);
    shimWritePos = 0;
    shimReadPosL = 0.0f;
    shimReadPosR = 0.0f;
}

void CosmicSynthEngine::processReverb(float& inL, float& inR,
    float reverbAmt, float shimmerAmt)
{
    if (reverbAmt < 0.001f) return;

    // Wet/dry: partial wet so the direct signal stays present
    const float wet = reverbAmt;
    const float dry = 1.0f - wet * 0.30f;

    // --- Shimmer: octave-up pitch-shifted tap fed into reverb ---
    shimBufL[(size_t)shimWritePos] = inL;
    shimBufR[(size_t)shimWritePos] = inR;

    auto readShimmer = [&](std::array<float, SHIMMER_BUF>& buf, float& readPos) -> float
        {
            const int   i0 = (int)readPos % SHIMMER_BUF;
            const int   i1 = (i0 + 1) % SHIMMER_BUF;
            const float frac = readPos - std::floor(readPos);
            readPos += 2.0f;  // octave up
            if (readPos >= (float)SHIMMER_BUF) readPos -= (float)SHIMMER_BUF;
            return (1.0f - frac) * buf[(size_t)i0] + frac * buf[(size_t)i1];
        };

    // Shimmer is subtle — only noticeable during GW events when cosmicWidth is high
    const float shimLevel = shimmerAmt * 0.14f;
    const float shimL = readShimmer(shimBufL, shimReadPosL) * shimLevel;
    const float shimR = readShimmer(shimBufR, shimReadPosR) * shimLevel;

    ++shimWritePos;
    if (shimWritePos >= SHIMMER_BUF) shimWritePos = 0;

    // --- FDN: 4-line Hadamard feedback network ---
    float tapL[FDN_LINES], tapR[FDN_LINES];
    for (int i = 0; i < FDN_LINES; ++i)
    {
        const int rp = (fdnPos[(size_t)i] - fdnLen[(size_t)i] + FDN_MAX) % FDN_MAX;
        tapL[(size_t)i] = fdnBufL[(size_t)i][(size_t)rp];
        tapR[(size_t)i] = fdnBufR[(size_t)i][(size_t)rp];
    }

    // Simplified Hadamard mix matrix
    const float norm = 0.25f;
    const float mL[FDN_LINES] = {
        (tapL[0] + tapL[1] + tapL[2] + tapL[3]) * norm,
        (tapL[0] + tapL[1] - tapL[2] - tapL[3]) * norm,
        (tapL[0] - tapL[1] + tapL[2] - tapL[3]) * norm,
        (tapL[0] - tapL[1] - tapL[2] + tapL[3]) * norm
    };
    const float mR[FDN_LINES] = {
        (tapR[0] + tapR[1] + tapR[2] + tapR[3]) * norm,
        (tapR[0] + tapR[1] - tapR[2] - tapR[3]) * norm,
        (tapR[0] - tapR[1] + tapR[2] - tapR[3]) * norm,
        (tapR[0] - tapR[1] - tapR[2] + tapR[3]) * norm
    };

    for (int i = 0; i < FDN_LINES; ++i)
    {
        fdnBufL[(size_t)i][(size_t)fdnPos[(size_t)i]] = inL + mL[(size_t)i] * fdnFeedback + shimL;
        fdnBufR[(size_t)i][(size_t)fdnPos[(size_t)i]] = inR + mR[(size_t)i] * fdnFeedback + shimR;
        fdnPos[(size_t)i] = (fdnPos[(size_t)i] + 1) % FDN_MAX;
    }

    const float rvbL = (tapL[0] + tapL[1] + tapL[2] + tapL[3]) * 0.25f;
    const float rvbR = (tapR[0] + tapR[1] + tapR[2] + tapR[3]) * 0.25f;

    inL = dry * inL + wet * rvbL;
    inR = dry * inR + wet * rvbR;
}

// ================= Oscillator setup ======================

void CosmicSynthEngine::initializeOscillators()
{
    waveTable = generateWaveTable(waveTableType);
    oscillators.clear();
    oscillators.reserve(128);
    for (int i = 0; i < 128; ++i)
        oscillators.emplace_back(waveTable, sampleRate);

    shadowIndex.fill(0.0f);
    shadowIncrement.fill(0.0f);
    for (auto& p : bellPhase) p.fill(0.0f);
    noteAgeSec.fill(0.0f);
    lastHz.fill(0.0f);
    orbVoicePh = { 0.0f, 0.5f };

    // Orbit voices: a rounded tone (sine + a little 2nd/3rd) so the pair sits
    // on top of the pad without adding a bright harmonic stack.
    constexpr int ON = 2048;
    orbitTable.assign((size_t)ON, 0.0f);
    float mx = 0.0f;
    for (int i = 0; i < ON; ++i)
    {
        const float ph = juce::MathConstants<float>::twoPi * (float)i / (float)ON;
        const float v = std::sin(ph) + 0.25f * std::sin(2.0f * ph) + 0.08f * std::sin(3.0f * ph);
        orbitTable[(size_t)i] = v; mx = juce::jmax(mx, std::abs(v));
    }
    for (auto& v : orbitTable) v /= mx;
}

// ================= Lifecycle ======================

void CosmicSynthEngine::prepareToPlay(double newSampleRate)
{
    sampleRate = (newSampleRate > 1.0 ? newSampleRate : 44100.0);

    globalGain = 0.20f;
    pitchRatio = 1.0f;
    globalFrequency = 0.0f;
    filterCutoffHz = 2800.0f;   // dark ambient baseline
    filterBaseHz = 2800.0f;
    waveshapeMix = 0.15f;     // gentle warmth
    stereoWidth = 0.80f;    // ~20% L/R spread
    cosmicWidth = 0.70f;
    reverbAmount = 0.40f;
    msWidthGain = 1.25f;    // ~20% L/R spread

    perNotePitchRatio.fill(1.0f);
    baseHz.fill(0.0f);
    heldNotesCount = 0;

    apZ.fill(0.0f);
    for (int n = 0; n < 128; ++n)
        apU[(size_t)n] = fastHash01(n);

    initializeOscillators();

    // Ambient ADSR — slow bloom, long fade into reverb tail
    padEnvParams.attack = 1.40f;   // slow attack for ambient bloom
    padEnvParams.decay = 2.00f;
    padEnvParams.sustain = 0.80f;
    padEnvParams.release = 5.50f;   // long release trails into reverb

    for (int n = 0; n < 128; ++n)
    {
        ampEnv[(size_t)n].setSampleRate(sampleRate);
        ampEnv[(size_t)n].setParameters(padEnvParams);
        ampEnv[(size_t)n].reset();
    }

    // Slower pitch slew = smoother, more ambient transitions
    for (auto& osc : oscillators)
        osc.setFreqSlewMs(35.0f);

    lpZL = lpZR = 0.0f;
    updateLowpassCoeff();

    driftPhase = 0.0f;
    filterLfoPhase = 0.0f;
    cosmicPhaseL = 0.0f;
    cosmicPhaseR = juce::MathConstants<float>::pi * 0.7f;  // R offset for independence
    tremoloPhase = 0.0f;

    decorrAmountSmooth.prepare(sampleRate);
    decorrAmountSmooth.setTimeMs(40.0);  // slower decorr smoothing = fewer clicks
    decorrAmountSmooth.reset(0.0f);

    initReverb();

    // Kozai-Lidov tables (sample-rate independent — built once)
    if (klBank.empty()) buildKozaiTables();
    klPhase.fill(0.0f);
    klOrbitPh = 0.0f;

    // Lens: up to ~1.2 s of delay (a 50,000 solar-mass lens gives ~1 s)
    int sz = 1;
    while (sz < (int)(1.2 * sampleRate) + 8) sz <<= 1;
    lensBuf.assign((size_t)sz, 0.0f);
    lensMask = sz - 1;
    lensSlideK = 1.0f - std::exp(-1.0f / (0.080f * (float)sampleRate));
    lensAtt = std::exp(-1.0f / (0.005f * (float)sampleRate));
    lensRel = std::exp(-1.0f / (0.150f * (float)sampleRate));
    lensRefDec = std::exp(-1.0f / (2.0f * (float)sampleRate));
    resetLens();
}

// ================= Kozai-Lidov Triple ======================

// The tables don't depend on sample rate or settings, so they are computed once
// per session (first instance) and copied into every engine.
void CosmicSynthEngine::buildKozaiTables()
{
    struct Shared { std::vector<float> bank, env; };
    static const Shared shared = []
        {
            auto tmp = std::make_unique<CosmicSynthEngine>();   // heap: the engine holds large reverb buffers
            tmp->computeKozaiTables();
            return Shared{ std::move(tmp->klBank), std::move(tmp->klEnvTab) };
        }();
    klBank = shared.bank;
    klEnvTab = shared.env;
}

void CosmicSynthEngine::computeKozaiTables()
{
    // --- Voice bank: Peters-Mathews harmonic spectrum per eccentricity ---
    // Strain in harmonic n ∝ sqrt(g(n,e)) / n. Partials sit at n × orbital
    // frequency; the played note is n = 2 (the circular-orbit GW frequency).
    // Each mip keeps the first H harmonics (band limiting for high notes) and
    // is normalized to the same RMS, so eccentricity changes timbre, not level.
    const int Hmax = klMipHarm[0];
    klBank.assign((size_t)KL_MIPS * KL_NE * KL_TAB, 0.0f);
    std::vector<double> amp((size_t)Hmax + 1, 0.0);
    std::vector<double> acc((size_t)KL_TAB, 0.0);
    const double twoPi = juce::MathConstants<double>::twoPi;
    std::vector<double> sinTab((size_t)KL_TAB), cosTab((size_t)KL_TAB);
    for (int i = 0; i < KL_TAB; ++i) { sinTab[(size_t)i] = std::sin(twoPi * i / KL_TAB); cosTab[(size_t)i] = std::cos(twoPi * i / KL_TAB); }

    for (int ie = 0; ie < KL_NE; ++ie)
    {
        const double e = (double)KL_EMAX * ie / (KL_NE - 1);
        for (int n = 1; n <= Hmax; ++n)
            amp[(size_t)n] = std::sqrt(juce::jmax(0.0, petersMathewsG(n, e))) / n;

        std::fill(acc.begin(), acc.end(), 0.0);
        double pow2 = 0.0;
        int nextMip = KL_MIPS - 1;                       // mips are stored 48,24,12,6,3
        for (int n = 1; n <= Hmax; ++n)
        {
            // sin(2π n i/N + φ) = sin(2π n i/N) cos φ + cos(2π n i/N) sin φ, with table lookups
            const double cphi = std::cos(0.37 * n), sphi = std::sin(0.37 * n), an = amp[(size_t)n];
            for (int i = 0; i < KL_TAB; ++i)
            {
                const size_t k = (size_t)(((long long)n * i) % KL_TAB);
                acc[(size_t)i] += an * (sinTab[k] * cphi + cosTab[k] * sphi);
            }
            pow2 += amp[(size_t)n] * amp[(size_t)n];
            while (nextMip >= 0 && n == klMipHarm[(size_t)nextMip])
            {
                const double norm = (pow2 > 0.0) ? 0.70 / std::sqrt(pow2) : 0.0;
                float* dst = &klBank[((size_t)nextMip * KL_NE + (size_t)ie) * KL_TAB];
                for (int i = 0; i < KL_TAB; ++i) dst[i] = (float)(acc[(size_t)i] * norm);
                --nextMip;
            }
        }
    }

    // --- Burst envelope: strain |h| of a Kepler orbit over one orbit ---
    // Face-on quadrupole strain h+ = (Q''xx - Q''yy)/2, hx = Q''xy with Q = r r.
    // Computed on a fine grid, normalized to 1 at periapsis.
    constexpr int FINE = KL_ENV_NM * 4;
    klEnvTab.assign((size_t)KL_ENV_NE * KL_ENV_NM, 1.0f);
    std::vector<double> X(FINE), Y(FINE), A(FINE);
    for (int ie = 0; ie < KL_ENV_NE; ++ie)
    {
        const double e = (double)KL_EMAX * ie / (KL_ENV_NE - 1);
        for (int m = 0; m < FINE; ++m)
        {
            const double M = twoPi * m / FINE;
            double E = M;
            if (e > 0.8) E = juce::MathConstants<double>::pi;   // robust start for high e
            for (int it = 0; it < 50; ++it)
            {
                const double dE = (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
                E -= dE;
                if (std::abs(dE) < 1.0e-12) break;
            }
            const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E / 2), std::sqrt(1.0 - e) * std::cos(E / 2));
            const double r = (1.0 - e * e) / (1.0 + e * std::cos(nu));
            X[(size_t)m] = r * std::cos(nu); Y[(size_t)m] = r * std::sin(nu);
        }
        double mx = 0.0;
        for (int m = 0; m < FINE; ++m)
        {
            const size_t a = (size_t)((m + FINE - 1) % FINE), b = (size_t)m, c = (size_t)((m + 1) % FINE);
            auto d2 = [&](double qa, double qb, double qc) { return qa - 2.0 * qb + qc; };
            const double xx = d2(X[a] * X[a], X[b] * X[b], X[c] * X[c]);
            const double yy = d2(Y[a] * Y[a], Y[b] * Y[b], Y[c] * Y[c]);
            const double xy = d2(X[a] * Y[a], X[b] * Y[b], X[c] * Y[c]);
            A[b] = std::sqrt(0.25 * (xx - yy) * (xx - yy) + xy * xy);
            mx = juce::jmax(mx, A[b]);
        }
        for (int m = 0; m < KL_ENV_NM; ++m)
            klEnvTab[(size_t)ie * KL_ENV_NM + (size_t)m] = (float)(mx > 0.0 ? A[(size_t)m * 4] / mx : 1.0);
    }
}

float CosmicSynthEngine::klVoice(int n, float hz, float e)
{
    const float forb = 0.5f * hz;                         // note = n=2 harmonic
    float& ph = klPhase[(size_t)n];
    ph += forb / (float)sampleRate;
    if (ph >= 1.0f) ph -= std::floor(ph);

    int mip = KL_MIPS - 1;
    for (int m = 0; m < KL_MIPS; ++m)
        if ((float)klMipHarm[(size_t)m] * forb < 0.45f * (float)sampleRate) { mip = m; break; }

    const float ei = clampf(e / KL_EMAX, 0.0f, 1.0f) * (float)(KL_NE - 1);
    const int   e0 = juce::jmin((int)ei, KL_NE - 2);
    const float ef = ei - (float)e0;
    const float idx = ph * (float)KL_TAB;
    const int   i0 = ((int)idx) & (KL_TAB - 1), i1 = (i0 + 1) & (KL_TAB - 1);
    const float fr = idx - std::floor(idx);
    const float* t0 = &klBank[((size_t)mip * KL_NE + (size_t)e0) * KL_TAB];
    const float* t1 = t0 + KL_TAB;
    const float a = t0[i0] + (t0[i1] - t0[i0]) * fr;
    const float b = t1[i0] + (t1[i1] - t1[i0]) * fr;
    return a + (b - a) * ef;
}

float CosmicSynthEngine::klBurst(float e, float ph01) const
{
    const float ei = clampf(e / KL_EMAX, 0.0f, 1.0f) * (float)(KL_ENV_NE - 1);
    const int   e0 = juce::jmin((int)ei, KL_ENV_NE - 2);
    const float ef = ei - (float)e0;
    const float mi = ph01 * (float)KL_ENV_NM;
    const int   m0 = ((int)mi) % KL_ENV_NM, m1 = (m0 + 1) % KL_ENV_NM;
    const float mf = mi - std::floor(mi);
    const float* r0 = &klEnvTab[(size_t)e0 * KL_ENV_NM];
    const float* r1 = r0 + KL_ENV_NM;
    const float a = r0[m0] + (r0[m1] - r0[m0]) * mf;
    const float b = r1[m0] + (r1[m1] - r1[m0]) * mf;
    return a + (b - a) * ef;
}

void CosmicSynthEngine::setKozaiLidov(float eccentricity, float orbitHz, float level)
{
    klETgt = clampf(eccentricity, 0.0f, KL_EMAX);
    klOrbHzTgt = clampf(orbitHz, 0.0f, 40.0f);
    klLevTgt = clampf(level, 0.0f, 4.0f);
}

// ================= Gravitational Lens ======================

void CosmicSynthEngine::setLens(bool on, float massMsun)
{
    if (on && !lensOn) { lensOn = true; resetLens(); }
    lensOn = on;
    lensMassTgt = clampf(massMsun, 1.0f, 100000.0f);
}

void CosmicSynthEngine::resetLens()
{
    std::fill(lensBuf.begin(), lensBuf.end(), 0.0f);
    lensW = 0;
    hilA_L.init(kHilA); hilA_R.init(kHilA); hilB.init(kHilB);
    hilAdelL = hilAdelR = 0.0f;
    const float d = (float)(4.0 * kTsun * lensMassTgt * lensT * sampleRate);
    lensDtTgt = d;
    lensStepCur = lensStepNew = lensSlide = d;
    lensXf = 1.0f; lensStepCount = 0;
    hpZL = hpXL = hpZR = hpXR = 0.0f;
    lensEnv = 0.0f; lensRef = 1.0e-3f;
}

inline float CosmicSynthEngine::lensRead(float d) const
{
    d = clampf(d, 0.0f, (float)lensMask - 2.0f);
    const float pos = (float)lensW - d;
    const float fl = std::floor(pos);
    const int   i0 = ((int)fl) & lensMask, i1 = (i0 + 1) & lensMask;
    const float fr = pos - fl;
    return lensBuf[(size_t)i0] + (lensBuf[(size_t)i1] - lensBuf[(size_t)i0]) * fr;
}

// Point-mass lens: y = A(x) + HP[(√μ+ − 1)·A(x) + √|μ−|·B(x)(t − Δt)]
//   A/B: Hilbert pair, so the second (type-II) image carries the 90° Morse phase
//   HP: diffraction — lensing fades below f_c = c³/(8πGM) (1st order at 0.7 f_c,
//       matches exact wave optics to ~0.5 dB median)
// Delay read-out avoids false pitch glides: long delays (echo) change only in
// crossfaded steps; short delays (flanger) slide smoothly.
void CosmicSynthEngine::processLens(float& L, float& R)
{
    const float aL = hilAdelL; hilAdelL = hilA_L.process(L);
    const float aR = hilAdelR; hilAdelR = hilA_R.process(R);
    const float mono = 0.5f * (L + R);
    lensBuf[(size_t)lensW] = hilB.process(mono);

    lensSlide += (lensDtTgt - lensSlide) * lensSlideK;

    if (--lensStepCount <= 0)
    {
        lensStepCount = (int)(0.040 * sampleRate);
        if (lensXf >= 1.0f && std::abs(lensDtTgt - lensStepCur) > 0.01f * juce::jmax(lensDtTgt, 1.0f))
        {
            lensStepNew = lensDtTgt;
            lensXf = 0.0f;
        }
    }
    float stepped;
    if (lensXf < 1.0f)
    {
        const float g = std::sin(0.5f * juce::MathConstants<float>::pi * lensXf);
        stepped = std::sqrt(juce::jmax(0.0f, 1.0f - g * g)) * lensRead(lensStepCur) + g * lensRead(lensStepNew);
        lensXf += 1.0f / (0.015f * (float)sampleRate);
        if (lensXf >= 1.0f) { lensXf = 1.0f; lensStepCur = lensStepNew; }
    }
    else stepped = lensRead(lensStepCur);

    const float dSec = lensSlide / (float)sampleRate;
    const float wSlide = clampf((0.030f - dSec) / 0.020f, 0.0f, 1.0f);   // 30 ms → 10 ms
    float img2 = wSlide * lensRead(lensSlide) + (1.0f - wSlide) * stepped;

    // Echo ducking (echo range only; stylization): dip the second image while
    // the direct sound is loud, so echoes don't pile onto new notes.
    const float am = std::abs(mono);
    const float c = (am > lensEnv) ? lensAtt : lensRel;
    lensEnv = c * lensEnv + (1.0f - c) * am;
    lensRef = juce::jmax(lensRef * lensRefDec, lensEnv, 1.0e-4f);
    const float wEcho = clampf((dSec - 0.030f) / 0.030f, 0.0f, 1.0f);
    img2 *= 1.0f - lensDuckDepth * wEcho * clampf(lensEnv / lensRef, 0.0f, 1.0f);

    // Lensing term; the second image sits a little right (stylization — real
    // image separations are far too small to hear as direction)
    const float tL = (lensAp - 1.0f) * aL + lensAm * img2 * 0.775f;
    const float tR = (lensAp - 1.0f) * aR + lensAm * img2 * 1.183f;
    hpZL = lensHpA * (hpZL + tL - hpXL); hpXL = tL;
    hpZR = lensHpA * (hpZR + tR - hpXR); hpXR = tR;

    L = (aL + hpZL) * lensNorm;
    R = (aR + hpZR) * lensNorm;
    lensW = (lensW + 1) & lensMask;
}

// ================= Control wrappers ======================

void CosmicSynthEngine::setPitchRatio(float ratio)
{
    pitchRatio = clampf(ratio, 0.25f, 4.0f);
    for (auto& r : perNotePitchRatio) r = pitchRatio;
}

void CosmicSynthEngine::setPerNotePitchRatios(const std::array<float, 128>& ratios)
{
    perNotePitchRatio = ratios;
    for (auto& r : perNotePitchRatio) r = clampf(r, 0.25f, 4.0f);
}

void CosmicSynthEngine::setGain(float g) { globalGain = clamp01(g); }
void CosmicSynthEngine::setOscFrequency(float hz) { globalFrequency = clampHz(hz); }

void CosmicSynthEngine::setFilterCutoff(float hz)
{
    // This is the BASE cutoff; the filter LFO moves around it inside render().
    // (Before, the LFO wrote back into the same variable, so the base crept
    //  a little every time a block was split by a MIDI event.)
    filterBaseHz = clampHz(hz);
    filterCutoffHz = filterBaseHz;
    updateLowpassCoeff();
}

void CosmicSynthEngine::setOrbitTarget(float pairHz, float level, float orbitHz,
    float dopplerCents, float split)
{
    if (pairHz > 0.0f) orbBaseTgt = clampHz(pairHz);
    orbLevelTgt = clamp01(level);
    orbHzTgt = clampf(orbitHz, 0.0f, 20.0f);
    orbDopTgt = clampf(dopplerCents, 0.0f, 300.0f);
    orbSplitTgt = clamp01(split);
}

void CosmicSynthEngine::setBellModes(float ratio1, float ratio2, float amp1, float amp2,
    float tau1Sec, float tau2Sec)
{
    bellRatio = { clampf(ratio1, 0.5f, 8.0f), clampf(ratio2, 0.5f, 8.0f) };
    bellAmp = { clamp01(amp1), clamp01(amp2) };
    bellTau = { juce::jmax(0.02f, tau1Sec), juce::jmax(0.02f, tau2Sec) };
}

void CosmicSynthEngine::setWaveshapeMix(float m) { waveshapeMix = clampf(m, -1.0f, 1.0f); }
void CosmicSynthEngine::setCosmicWidth(float w) { cosmicWidth = clamp01(w); }
void CosmicSynthEngine::setReverbAmount(float a) { reverbAmount = clamp01(a); }

void CosmicSynthEngine::applyPresetSettings(const PresetSettings& ps)
{
    // Synthesis
    waveshapeMix = clampf(ps.waveshapeMix, -1.0f, 1.0f);
    stereoWidth = clamp01(ps.stereoWidth);
    cosmicWidth = clamp01(ps.cosmicWidth);
    reverbAmount = clamp01(ps.reverbBase);
    msWidthGain = juce::jmax(0.5f, ps.msWidthGain);
    fdnFeedback = clampf(ps.reverbFeedback, 0.0f, 0.98f);

    // Filter
    filterBaseHz = clampHz(ps.filterBaseline);
    filterCutoffHz = filterBaseHz;
    updateLowpassCoeff();

    // Modulation — these preset fields existed but the engine ignored them,
    // which is a big reason every preset sounded alike.
    driftHz = juce::jmax(0.0f, ps.driftHz);
    driftCents = juce::jmax(0.0f, ps.driftCents);
    filterLfoHz = juce::jmax(0.0f, ps.filterLfoHz);
    filterLfoDepth = juce::jmax(0.0f, ps.filterLfoDepth);
    cosmicHzL = juce::jmax(0.0f, ps.cosmicHzL);
    cosmicHzR = juce::jmax(0.0f, ps.cosmicHzR);
    cosmicCents = juce::jmax(0.0f, ps.cosmicCents);
    tremoloHz = juce::jmax(0.0f, ps.tremoloHz);
    tremoloDepth = clamp01(ps.tremoloDepth);
    shadowMix = clamp01(ps.shadowMix);
    shadowCents = juce::jmax(0.0f, ps.shadowCents);
    bellModesOn = ps.bellModes;
    trackReleasePitch = ps.chirpEnabled;
    klModeOn = ps.klMode;

    // Lens images for alignment y (Einstein radii): magnifications and delay factor
    {
        const double y = juce::jlimit(0.05, 3.0, (double)ps.lensAlignment);
        const double sq = std::sqrt(y * y + 4.0);
        const double mup = 0.5 + (y * y + 2.0) / (2.0 * y * sq);
        const double mum = 0.5 - (y * y + 2.0) / (2.0 * y * sq);
        lensAp = (float)std::sqrt(mup);
        lensAm = (float)std::sqrt(std::abs(mum));
        lensNorm = (float)(1.0 / std::sqrt(mup + std::abs(mum)));   // lensing makes it louder; keep level steady
        lensT = (float)(y * sq / 2.0 + std::log((sq + y) / (sq - y)));
        lensDuckDepth = 1.0f - std::pow(10.0f, -juce::jmax(0.0f, ps.lensDuckDb) / 20.0f);
    }
    if (!ps.lensMode) lensOn = false;

    // Wavetable per preset (rebuild voices only when it changes)
    if (ps.waveTableType != waveTableType || oscillators.empty())
    {
        waveTableType = ps.waveTableType;
        initializeOscillators();
        for (int n = 0; n < 128; ++n) { ampEnv[(size_t)n].reset(); baseHz[(size_t)n] = 0.0f; }
        heldNotesCount = 0;
    }
    orbLevelCur = orbLevelTgt = 0.0f;

    // ADSR
    padEnvParams.attack = juce::jmax(0.001f, ps.adsrAttack);
    padEnvParams.decay = juce::jmax(0.001f, ps.adsrDecay);
    padEnvParams.sustain = clamp01(ps.adsrSustain);
    padEnvParams.release = juce::jmax(0.001f, ps.adsrRelease);
    for (int n = 0; n < 128; ++n)
        ampEnv[(size_t)n].setParameters(padEnvParams);

    // Oscillator pitch slew
    for (auto& osc : oscillators)
        osc.setFreqSlewMs(ps.freqSlewMs);

    // Reset reverb buffers when switching presets to avoid stale tail
    initReverb();
}

// ================= processBlock ======================

void CosmicSynthEngine::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midiMessages)
{
    buffer.clear();
    const int numSamples = buffer.getNumSamples();
    int       currentSample = 0;

    for (const auto meta : midiMessages)
    {
        const auto msg = meta.getMessage();
        const int  evSample = juce::jlimit(0, numSamples, (int)meta.samplePosition);
        render(buffer, currentSample, evSample);
        handleMidiEvent(msg);
        currentSample = evSample;
    }
    render(buffer, currentSample, numSamples);
}

// ================= Shared render core ======================
// Used by both processBlock and processBlockRamped via lambda

namespace
{
    // Linear interpolation helper
    inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
}

void CosmicSynthEngine::render(juce::AudioBuffer<float>& buffer, int startSample, int endSample)
{
    if (endSample <= startSample) return;

    const int  nCh = buffer.getNumChannels();
    auto* L = buffer.getWritePointer(0);
    auto* R = (nCh > 1) ? buffer.getWritePointer(1) : nullptr;
    const bool hasStereo = (R != nullptr);

    auto notePan = [&](int note) -> float
        {
            return clampf(std::sin(0.17f * (float)note) * stereoWidth, -1.0f, 1.0f);
        };

    auto computePanOffset = [&]() -> float
        {
            if (heldNotesCount < 2) return 0.0f;
            float sum = 0.0f; int cnt = 0;
            for (int n = 0; n < 128; ++n)
                if (baseHz[(size_t)n] > 0.0f) { sum += notePan(n); ++cnt; }
            return (cnt >= 2) ? sum / (float)cnt : 0.0f;
        };
    const float panOffset = computePanOffset();

    const float mix = clampf(std::abs(waveshapeMix), 0.0f, 1.0f);
    const float drive = 1.0f + 2.5f * mix;  // gentle saturation ceiling

    // Ambient LFO rates — all slower than before
    const float driftInc = juce::MathConstants<float>::twoPi * driftHz / (float)sampleRate;
    const float filterInc = juce::MathConstants<float>::twoPi * filterLfoHz / (float)sampleRate;
    const float cosmicIncL = juce::MathConstants<float>::twoPi * cosmicHzL / (float)sampleRate;
    const float cosmicIncR = juce::MathConstants<float>::twoPi * cosmicHzR / (float)sampleRate;
    const float tremoloInc = juce::MathConstants<float>::twoPi * tremoloHz / (float)sampleRate;

    const float baseCutoff = filterBaseHz;
    const float filterDepth = filterLfoDepth;

    // Shadow oscillator: +4 cents L, -4 cents R — subtle chorus width
    const float shadowRatioL = std::pow(2.0f, shadowCents / 1200.0f);
    const float tableSize = (float)waveTable.size();

    for (int i = startSample; i < endSample; ++i)
    {
        // Primary drift
        driftPhase += driftInc;
        if (driftPhase >= juce::MathConstants<float>::twoPi) driftPhase -= juce::MathConstants<float>::twoPi;
        const float driftRatio = std::pow(2.0f, driftCents * std::sin(driftPhase) / 1200.0f);

        // Filter LFO — update every 64 samples (filter moves very slowly)
        if ((i & 63) == 0)
        {
            filterLfoPhase += filterInc * 64.0f;
            if (filterLfoPhase >= juce::MathConstants<float>::twoPi) filterLfoPhase -= juce::MathConstants<float>::twoPi;
            filterCutoffHz = clampHz(baseCutoff + filterDepth * std::sin(filterLfoPhase));
            updateLowpassCoeff();
        }

        // Cosmic shimmer LFOs
        cosmicPhaseL += cosmicIncL;
        if (cosmicPhaseL >= juce::MathConstants<float>::twoPi) cosmicPhaseL -= juce::MathConstants<float>::twoPi;
        cosmicPhaseR += cosmicIncR;
        if (cosmicPhaseR >= juce::MathConstants<float>::twoPi) cosmicPhaseR -= juce::MathConstants<float>::twoPi;
        const float shimmerRatioL = std::pow(2.0f, cosmicCents * cosmicWidth * std::sin(cosmicPhaseL) / 1200.0f);
        const float shimmerRatioR = std::pow(2.0f, cosmicCents * cosmicWidth * std::sin(cosmicPhaseR) / 1200.0f);

        // Tremolo — barely perceptible amplitude breath
        tremoloPhase += tremoloInc;
        if (tremoloPhase >= juce::MathConstants<float>::twoPi) tremoloPhase -= juce::MathConstants<float>::twoPi;
        const float tremolo = 1.0f - tremoloDepth * (0.5f + 0.5f * std::sin(tremoloPhase));

        const float decorrTarget = (hasStereo && heldNotesCount > 1 && stereoWidth > 0.001f)
            ? clamp01(stereoWidth) : 0.0f;
        const float decorrAmt = decorrAmountSmooth.process(decorrTarget);

        float sumL = 0.0f, sumR = 0.0f;

        for (int n = 0; n < (int)oscillators.size(); ++n)
        {
            auto& osc = oscillators[(size_t)n];
            if (!osc.isPlaying()) continue;

            if (baseHz[(size_t)n] > 0.0f)
            {
                const float hz = clampf(
                    baseHz[(size_t)n] * perNotePitchRatio[(size_t)n] * driftRatio,
                    20.0f, 20000.0f);
                osc.setFrequency(hz);
                if (tableSize > 0.0f)
                    shadowIncrement[(size_t)n] = (hz * shadowRatioL * tableSize) / (float)sampleRate;
            }

            const float env = ampEnv[(size_t)n].getNextSample();
            if (env <= 1.0e-5f && baseHz[(size_t)n] <= 0.0f) { osc.stop(); continue; }

            const float v = osc.getSample() * env;

            // Shadow oscillator read
            float shadowSampleL = 0.0f, shadowSampleR = 0.0f;
            if (!waveTable.empty() && tableSize > 0.0f)
            {
                shadowIndex[(size_t)n] += shadowIncrement[(size_t)n];
                while (shadowIndex[(size_t)n] >= tableSize) shadowIndex[(size_t)n] -= tableSize;
                while (shadowIndex[(size_t)n] < 0.0f)      shadowIndex[(size_t)n] += tableSize;

                const int   si0 = (int)shadowIndex[(size_t)n];
                const int   si1 = (si0 + 1) % (int)tableSize;
                const float sfrac = shadowIndex[(size_t)n] - (float)si0;
                const float sv = ((1.0f - sfrac) * waveTable[(size_t)si0]
                    + sfrac * waveTable[(size_t)si1]) * env;
                shadowSampleL = sv;
                shadowSampleR = sv * 0.88f; // slight asymmetry for natural width
            }

            // Allpass decorrelation
            float vR = v;
            if (hasStereo)
            {
                const float aBase = 0.10f + 0.45f * apU[(size_t)n];
                const float a = clampf(aBase * stereoWidth, 0.0f, 0.70f);
                const float y = (-a * v) + apZ[(size_t)n];
                apZ[(size_t)n] = v + (a * y);
                vR = (1.0f - decorrAmt) * v + decorrAmt * y;
            }

            // Pan with center correction
            float pan = notePan(n) - panOffset;
            pan = clampf(pan, -1.0f, 1.0f);
            const float gL = std::sqrt(0.5f * (1.0f - pan));
            const float gR = std::sqrt(0.5f * (1.0f + pan));

            // Blend shadow underneath main oscillator
            const float outVL = lerpf(v, shadowSampleL, shadowMix);
            const float outVR = lerpf(vR, shadowSampleR, shadowMix);

            sumL += outVL * gL * shimmerRatioL;
            sumR += outVR * gR * shimmerRatioR;
        }

        float outL = sumL * globalGain * tremolo;
        float outR = sumR * globalGain * tremolo;

        if (mix > 0.0f)
        {
            outL = lerpf(outL, softClipTanh(outL, drive), mix);
            outR = lerpf(outR, softClipTanh(outR, drive), mix);
        }

        lpZL = (1.0f - lpA) * outL + lpA * lpZL; outL = lpZL;
        if (hasStereo) { lpZR = (1.0f - lpA) * outR + lpA * lpZR; outR = lpZR; }

        // FDN reverb + shimmer
        if (hasStereo)
            processReverb(outL, outR, reverbAmount, cosmicWidth);

        // M/S width — subtle side boost, mono-safe
        if (hasStereo)
        {
            const float mid = (outL + outR) * 0.5f;
            const float side = (outL - outR) * 0.5f * msWidthGain;
            outL = mid + side;
            outR = mid - side;
        }

        L[i] += outL;
        if (R) R[i] += outR;
    }

    for (int ch = 2; ch < nCh; ++ch)
    {
        auto* dst = buffer.getWritePointer(ch);
        std::copy(L + startSample, L + endSample, dst + startSample);
    }
}

// ================= MIDI ======================

void CosmicSynthEngine::handleMidiEvent(const juce::MidiMessage& msg)
{
    if (msg.isNoteOn())
    {
        const int n = msg.getNoteNumber();
        if (n < 0 || n >= (int)oscillators.size()) return;
        // First note after real silence lands on a periapsis burst. Not on legato
        // note changes (a released voice is still sounding): a phase jump there
        // would make the burst envelope jump, i.e. click.
        if (klModeOn)
        {
            bool sounding = false;
            for (auto& o : oscillators) if (o.isPlaying()) { sounding = true; break; }
            if (!sounding) klOrbitPh = 0.0f;
        }
        if (baseHz[(size_t)n] <= 0.0f) heldNotesCount = juce::jmin(128, heldNotesCount + 1);
        klPhase[(size_t)n] = 0.0f;
        const float hz0 = midiNoteNumberToFrequency(n);
        baseHz[(size_t)n] = hz0;
        noteHz[(size_t)n] = hz0;
        const float hz = clampf(hz0 * perNotePitchRatio[(size_t)n], 20.0f, 20000.0f);
        // A silent voice starts right at its pitch. A voice still ringing (key
        // struck again during its release) glides, so its waveform never jumps.
        auto& osc = oscillators[(size_t)n];
        freshStart[(size_t)n] = !osc.isPlaying();
        if (freshStart[(size_t)n]) osc.startAt(hz);
        else                       osc.setFrequency(hz);
        lastHz[(size_t)n] = hz;
        noteAgeSec[(size_t)n] = 0.0f;
        bellPhase[(size_t)n].fill(0.0f);
        ampEnv[(size_t)n].noteOn();
    }
    else if (msg.isNoteOff())
    {
        const int n = msg.getNoteNumber();
        if (n < 0 || n >= (int)oscillators.size()) return;
        if (baseHz[(size_t)n] > 0.0f) heldNotesCount = juce::jmax(0, heldNotesCount - 1);
        baseHz[(size_t)n] = 0.0f;
        ampEnv[(size_t)n].noteOff();
    }
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        heldNotesCount = 0;
        for (int n = 0; n < (int)oscillators.size(); ++n)
        {
            baseHz[(size_t)n] = 0.0f;
            ampEnv[(size_t)n].noteOff();
            oscillators[(size_t)n].stop();
            apZ[(size_t)n] = 0.0f;
        }
        decorrAmountSmooth.reset(0.0f);
    }
}

float CosmicSynthEngine::midiNoteNumberToFrequency(int n)
{
    return 440.0f * std::pow(2.0f, ((float)n - 69.0f) / 12.0f);
}

// ================= processBlockRamped ======================

void CosmicSynthEngine::processBlockRamped(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midiMessages,
    const std::array<float, 128>& ratiosStart,
    const std::array<float, 128>& ratiosEnd,
    float gainStart,
    float gainEnd)
{
    buffer.clear();
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    const int  nCh = buffer.getNumChannels();
    auto* L = buffer.getWritePointer(0);
    auto* R = (nCh > 1) ? buffer.getWritePointer(1) : nullptr;
    const bool hasStereo = (R != nullptr);

    const float denom = (numSamples > 1) ? (float)(numSamples - 1) : 1.0f;
    const float twoPi = juce::MathConstants<float>::twoPi;
    const float invSr = 1.0f / (float)sampleRate;

    std::array<float, 128> logR0{}, logR1{};
    for (int n = 0; n < 128; ++n)
    {
        logR0[(size_t)n] = std::log(clampf(ratiosStart[(size_t)n], 0.25f, 4.0f));
        logR1[(size_t)n] = std::log(clampf(ratiosEnd[(size_t)n], 0.25f, 4.0f));
    }

    auto ratioAt = [&](int i, int note) -> float
        {
            return clampf(std::exp(lerpf(logR0[(size_t)note], logR1[(size_t)note],
                (float)i / denom)), 0.25f, 4.0f);
        };

    auto gainAt = [&](int i) -> float
        {
            return clamp01(lerpf(gainStart, gainEnd, (float)i / denom));
        };

    auto notePan = [&](int note) -> float
        {
            return clampf(std::sin(0.17f * (float)note) * stereoWidth, -1.0f, 1.0f);
        };

    auto computePanOffset = [&]() -> float
        {
            if (heldNotesCount < 2) return 0.0f;
            float sum = 0.0f; int cnt = 0;
            for (int n = 0; n < 128; ++n)
                if (baseHz[(size_t)n] > 0.0f) { sum += notePan(n); ++cnt; }
            return (cnt >= 2) ? sum / (float)cnt : 0.0f;
        };

    const float mix = clampf(std::abs(waveshapeMix), 0.0f, 1.0f);
    const float drive = 1.0f + 2.5f * mix;

    // Per-preset LFO rates (were hardcoded)
    const float driftInc = twoPi * driftHz * invSr;
    const float filterInc = twoPi * filterLfoHz * invSr;
    const float cosmicIncL = twoPi * cosmicHzL * invSr;
    const float cosmicIncR = twoPi * cosmicHzR * invSr;
    const float tremoloInc = twoPi * tremoloHz * invSr;

    const float shadowRatio = std::pow(2.0f, shadowCents / 1200.0f);
    const float tableSize = (float)waveTable.size();

    // Orbit pair ramps for this block
    if (orbBaseCur <= 0.0f) orbBaseCur = (orbBaseTgt > 0.0f ? orbBaseTgt : 261.6f);
    if (orbBaseTgt <= 0.0f) orbBaseTgt = orbBaseCur;
    if (orbLevelCur < 1.0e-4f) { orbBaseCur = orbBaseTgt; orbSplitCur = orbSplitTgt; } // silent: jump, don't glide
    const float oLog0 = std::log(orbBaseCur), oLog1 = std::log(orbBaseTgt);
    const float oLev0 = orbLevelCur, oLev1 = orbLevelTgt;
    const float oHz0 = orbHzCur, oHz1 = orbHzTgt;
    const float oDop0 = orbDopCur, oDop1 = orbDopTgt;
    const float oSpl0 = orbSplitCur, oSpl1 = orbSplitTgt;
    const bool  orbitActive = (oLev0 > 1.0e-5f || oLev1 > 1.0e-5f) && !orbitTable.empty();

    // Kozai-Lidov ramps
    const float kE0 = klECur, kE1 = klETgt, kH0 = klOrbHzCur, kH1 = klOrbHzTgt, kL0 = klLevCur, kL1 = klLevTgt;
    const bool  klActive = klModeOn && !klBank.empty();

    // Lens per-block values
    if (lensOn)
    {
        lensDtTgt = clampf((float)(4.0 * kTsun * lensMassTgt * lensT * sampleRate), 0.0f, (float)lensMask - 4.0f);
        const double fc = 0.7 / (8.0 * juce::MathConstants<double>::pi * kTsun * lensMassTgt);
        lensHpA = (float)std::exp(-juce::MathConstants<double>::twoPi * juce::jmin(fc, 0.45 * sampleRate) / sampleRate);
    }
    const float orbTableSize = (float)orbitTable.size();

    auto renderSegment = [&](int start, int end)
        {
            start = juce::jlimit(0, numSamples, start);
            end = juce::jlimit(0, numSamples, end);
            if (end <= start) return;

            const float panOffset = computePanOffset();

            for (int i = start; i < end; ++i)
            {
                driftPhase += driftInc;
                if (driftPhase >= twoPi) driftPhase -= twoPi;
                const float driftRatio = std::pow(2.0f, driftCents * std::sin(driftPhase) / 1200.0f);

                if ((i & 63) == 0)
                {
                    filterLfoPhase += filterInc * 64.0f;
                    if (filterLfoPhase >= twoPi) filterLfoPhase -= twoPi;
                    filterCutoffHz = clampHz(filterBaseHz + filterLfoDepth * std::sin(filterLfoPhase));
                    updateLowpassCoeff();
                }

                cosmicPhaseL += cosmicIncL;
                if (cosmicPhaseL >= twoPi) cosmicPhaseL -= twoPi;
                cosmicPhaseR += cosmicIncR;
                if (cosmicPhaseR >= twoPi) cosmicPhaseR -= twoPi;
                const float shimmerRatioL = std::pow(2.0f, cosmicCents * cosmicWidth * std::sin(cosmicPhaseL) / 1200.0f);
                const float shimmerRatioR = std::pow(2.0f, cosmicCents * cosmicWidth * std::sin(cosmicPhaseR) / 1200.0f);

                tremoloPhase += tremoloInc;
                if (tremoloPhase >= twoPi) tremoloPhase -= twoPi;
                const float tremolo = 1.0f - tremoloDepth * (0.5f + 0.5f * std::sin(tremoloPhase));

                const float decorrTarget = (hasStereo && heldNotesCount > 1 && stereoWidth > 0.001f)
                    ? clamp01(stereoWidth) : 0.0f;
                const float decorrAmt = decorrAmountSmooth.process(decorrTarget);

                const float g = gainAt(i);
                float sumL = 0.0f, sumR = 0.0f;

                for (int n = 0; n < (int)oscillators.size(); ++n)
                {
                    auto& osc = oscillators[(size_t)n];
                    if (!osc.isPlaying()) continue;

                    // Released notes keep following their pitch ratio when the
                    // preset asks for it (pluck chirp still landing in tune)
                    const float bhz = (baseHz[(size_t)n] > 0.0f) ? baseHz[(size_t)n]
                        : (trackReleasePitch ? noteHz[(size_t)n] : 0.0f);
                    if (bhz > 0.0f)
                    {
                        const float hz = clampf(
                            bhz * ratioAt(i, n) * driftRatio,
                            20.0f, 20000.0f);
                        osc.setFrequency(hz);
                        lastHz[(size_t)n] = hz;
                        if (tableSize > 0.0f)
                            shadowIncrement[(size_t)n] = (hz * shadowRatio * tableSize) / (float)sampleRate;
                    }

                    const float env = ampEnv[(size_t)n].getNextSample();
                    if (env <= 1.0e-5f && baseHz[(size_t)n] <= 0.0f) { osc.stop(); continue; }

                    float v = (klActive ? klVoice(n, lastHz[(size_t)n], lerpf(kE0, kE1, (float)i / denom))
                        : osc.getSample()) * env;

                    // Bell: Kerr overtone modes, each with its own damped sine
                    if (bellModesOn)
                    {
                        const float hz = lastHz[(size_t)n];
                        float partials = 0.0f;
                        for (int k = 0; k < 2; ++k)
                        {
                            float& ph = bellPhase[(size_t)n][(size_t)k];
                            ph += hz * bellRatio[(size_t)k] * invSr;
                            if (ph >= 1.0f) ph -= std::floor(ph);
                            const float decay = std::exp(-noteAgeSec[(size_t)n] / bellTau[(size_t)k]);
                            partials += bellAmp[(size_t)k] * decay * std::sin(twoPi * ph);
                        }
                        v += partials * env;
                        noteAgeSec[(size_t)n] += invSr;
                    }

                    float shadowSampleL = 0.0f, shadowSampleR = 0.0f;
                    if (!waveTable.empty() && tableSize > 0.0f)
                    {
                        shadowIndex[(size_t)n] += shadowIncrement[(size_t)n];
                        while (shadowIndex[(size_t)n] >= tableSize) shadowIndex[(size_t)n] -= tableSize;
                        while (shadowIndex[(size_t)n] < 0.0f)      shadowIndex[(size_t)n] += tableSize;

                        const int   si0 = (int)shadowIndex[(size_t)n];
                        const int   si1 = (si0 + 1) % (int)tableSize;
                        const float sfrac = shadowIndex[(size_t)n] - (float)si0;
                        const float sv = ((1.0f - sfrac) * waveTable[(size_t)si0]
                            + sfrac * waveTable[(size_t)si1]) * env;
                        shadowSampleL = sv;
                        shadowSampleR = sv * 0.88f;
                    }

                    float vR = v;
                    if (hasStereo)
                    {
                        const float aBase = 0.10f + 0.45f * apU[(size_t)n];
                        const float a = clampf(aBase * stereoWidth, 0.0f, 0.70f);
                        const float y = (-a * v) + apZ[(size_t)n];
                        apZ[(size_t)n] = v + (a * y);
                        vR = (1.0f - decorrAmt) * v + decorrAmt * y;
                    }

                    float pan = notePan(n) - panOffset;
                    pan = clampf(pan, -1.0f, 1.0f);
                    const float gL = std::sqrt(0.5f * (1.0f - pan));
                    const float gR = std::sqrt(0.5f * (1.0f + pan));

                    const float outVL = lerpf(v, shadowSampleL, shadowMix);
                    const float outVR = lerpf(vR, shadowSampleR, shadowMix);

                    sumL += outVL * gL * shimmerRatioL;
                    sumR += outVR * gR * shimmerRatioR;
                }

                // Kozai-Lidov: one strain burst per orbit, shaped by the current e
                if (klActive)
                {
                    const float tt = (float)i / denom;
                    klOrbitPh += lerpf(kH0, kH1, tt) * invSr;
                    if (klOrbitPh >= 1.0f) klOrbitPh -= std::floor(klOrbitPh);
                    const float b = klBurst(lerpf(kE0, kE1, tt), klOrbitPh) * lerpf(kL0, kL1, tt);
                    sumL *= b; sumR *= b;
                }

                // Binary orbit pair — two bodies circling, fusing at merger
                float orbitDuck = 0.0f;
                if (orbitActive)
                {
                    const float t = (float)i / denom;
                    const float base = std::exp(lerpf(oLog0, oLog1, t));
                    const float lev = lerpf(oLev0, oLev1, t);
                    const float oHz = lerpf(oHz0, oHz1, t);
                    const float dop = lerpf(oDop0, oDop1, t);
                    const float spl = lerpf(oSpl0, oSpl1, t);

                    orbPhase += twoPi * oHz * invSr;
                    if (orbPhase >= twoPi) orbPhase -= twoPi;
                    const float so = std::sin(orbPhase);

                    for (int b = 0; b < 2; ++b)
                    {
                        const float sgn = (b == 0) ? 1.0f : -1.0f;
                        // Doppler: approaching body sharp, receding body flat
                        const float hz = base * std::pow(2.0f, sgn * dop * so * spl / 1200.0f);
                        float& vp = orbVoicePh[(size_t)b];
                        vp += hz * invSr;
                        if (vp >= 1.0f) vp -= std::floor(vp);
                        const float idx = vp * orbTableSize;
                        const int   o0 = (int)idx % (int)orbTableSize;
                        const int   o1 = (o0 + 1) % (int)orbTableSize;
                        const float of = idx - std::floor(idx);
                        const float v = ((1.0f - of) * orbitTable[(size_t)o0]
                            + of * orbitTable[(size_t)o1]) * lev * 0.5f;
                        // Bodies swap sides as they orbit; centred once merged
                        const float pan = clampf(sgn * so * 0.9f * spl, -1.0f, 1.0f);
                        sumL += v * std::sqrt(0.5f * (1.0f - pan));
                        sumR += v * std::sqrt(0.5f * (1.0f + pan));
                    }
                    orbitDuck = clamp01(lev * 2.0f);
                }

                float outL = sumL * g * tremolo;
                float outR = sumR * g * tremolo;

                if (mix > 0.0f)
                {
                    outL = lerpf(outL, softClipTanh(outL, drive), mix);
                    outR = lerpf(outR, softClipTanh(outR, drive), mix);
                }

                lpZL = (1.0f - lpA) * outL + lpA * lpZL; outL = lpZL;
                if (hasStereo) { lpZR = (1.0f - lpA) * outR + lpA * lpZR; outR = lpZR; }

                if (lensOn && hasStereo && !lensBuf.empty())
                    processLens(outL, outR);

                // The reverb's octave-up shimmer on a rising voice creates a
                // Shepard-tone illusion, so duck the shimmer while the orbit plays.
                if (hasStereo)
                    processReverb(outL, outR, reverbAmount, cosmicWidth * (1.0f - 0.85f * orbitDuck));

                if (hasStereo)
                {
                    const float mid = (outL + outR) * 0.5f;
                    const float side = (outL - outR) * 0.5f * msWidthGain;
                    outL = mid + side;
                    outR = mid - side;
                }

                L[i] = outL;
                if (R) R[i] = outR;
            }
        };

    int currentSample = 0;
    for (const auto meta : midiMessages)
    {
        const auto msg = meta.getMessage();
        const int  evSample = juce::jlimit(0, numSamples, (int)meta.samplePosition);
        renderSegment(currentSample, evSample);
        handleMidiEvent(msg);
        if (msg.isNoteOn())
        {
            const int n = msg.getNoteNumber();
            if (0 <= n && n < (int)oscillators.size())
            {
                // Re-aim at the note's actual starting pitch (the Pluck starts
                // below the note). A fresh voice jumps there; a ringing one glides.
                const float hz = clampf(baseHz[(size_t)n] * ratioAt(evSample, n), 20.0f, 20000.0f);
                if (freshStart[(size_t)n]) oscillators[(size_t)n].startAt(hz);
                else                       oscillators[(size_t)n].setFrequency(hz);
                freshStart[(size_t)n] = false;
                lastHz[(size_t)n] = hz;
            }
        }
        currentSample = evSample;
    }
    renderSegment(currentSample, numSamples);

    // Orbit pair now sits at its targets
    klECur = klETgt; klOrbHzCur = klOrbHzTgt; klLevCur = klLevTgt;
    orbBaseCur = orbBaseTgt; orbLevelCur = orbLevelTgt; orbHzCur = orbHzTgt;
    orbDopCur = orbDopTgt;   orbSplitCur = orbSplitTgt;

    for (int ch = 2; ch < nCh; ++ch)
    {
        auto* dst = buffer.getWritePointer(ch);
        std::copy(L, L + numSamples, dst);
    }
}