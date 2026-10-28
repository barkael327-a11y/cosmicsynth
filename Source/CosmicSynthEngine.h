// CosmicSynthEngine.h
#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <cmath>
#include "CosmicSynthOscillator.h"
#include "Smooth.h"
#include "PresetManager.h"

class CosmicSynthEngine
{
public:
    CosmicSynthEngine() = default;

    void prepareToPlay(double newSampleRate);

    void processBlock(juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midiMessages);

    void setPitchRatio(float ratio);
    void setPerNotePitchRatios(const std::array<float, 128>& ratios);
    void setGain(float newGain01);
    void setOscFrequency(float newHz);
    void setFilterCutoff(float newHz);
    void setWaveshapeMix(float newMixMinus1to1);
    void setCosmicWidth(float width01);
    void setReverbAmount(float amount01);

    // Apply all preset-specific settings at once (called on preset switch)
    void applyPresetSettings(const struct PresetSettings& ps);

    void processBlockRamped(juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midiMessages,
        const std::array<float, 128>& ratiosStart,
        const std::array<float, 128>& ratiosEnd,
        float gainStart,
        float gainEnd);

    // Binary orbit pair (Binary Pad GW events): two soft voices, one per black hole.
    // They circle each other — swapping sides in stereo and Doppler-shifting in
    // antiphase — while climbing with the chirp, then fuse into one at merger.
    // The engine ramps every value from its current state to these targets
    // across the block. baseHz <= 0 keeps the current pitch (use when fading out).
    //   level        0..1 overall level of the pair
    //   orbitHz      how fast they circle (real-time, stretched from the data)
    //   dopplerCents pitch swing of each body as it approaches/recedes
    //   split        1 = two separate bodies, 0 = merged into one
    void setOrbitTarget(float baseHz, float level, float orbitHz,
        float dopplerCents, float split);

    // Bell overtone modes (Kerr l=m=3 and l=m=4 relative to l=m=2):
    // frequency ratios, amplitudes and e-folding decay times in seconds.
    void setBellModes(float ratio1, float ratio2, float amp1, float amp2,
        float tau1Sec, float tau2Sec);

    // Kozai-Lidov Triple: inner-binary eccentricity (0..0.95), orbit (burst) rate
    // in Hz and overall level. The engine ramps to these across the block.
    void setKozaiLidov(float eccentricity, float orbitHz, float level);

    // Gravitational Lens: point-mass lens of the given (redshifted) mass in solar
    // masses. on = false bypasses the lens.
    void setLens(bool on, float massMsun);

    // Current orbital phase of the Kozai-Lidov binary (0 = periapsis burst)
    float getKozaiOrbitPhase() const noexcept { return klOrbitPh; }

private:
    static std::vector<float> generateWaveTable(int type);
    static float midiNoteNumberToFrequency(int midiNoteNumber);
    void initializeOscillators();
    void render(juce::AudioBuffer<float>& buffer, int startSample, int endSample);
    void handleMidiEvent(const juce::MidiMessage& midiEvent);
    void updateLowpassCoeff();
    static float softClipTanh(float x, float drive);

    void initReverb();
    void processReverb(float& L, float& R, float reverbAmt, float shimmerAmt);

    static inline float fastHash01(int n)
    {
        uint32_t x = (uint32_t)n * 1103515245u + 12345u;
        x ^= (x >> 16);
        return (float)(x & 0xFFFFu) / 65535.0f;
    }

    // Per-note state
    std::array<float, 128> perNotePitchRatio{};
    std::array<float, 128> baseHz{};
    std::array<juce::ADSR, 128> ampEnv{};
    juce::ADSR::Parameters padEnvParams{};

    // Per-note allpass decorrelation
    std::array<float, 128> apU{};
    std::array<float, 128> apZ{};

    // Per-note shadow oscillator (chorus detuning)
    std::array<float, 128> shadowIndex{};
    std::array<float, 128> shadowIncrement{};

    double sampleRate = 44100.0;

    // Synthesis controls
    float globalGain = 0.20f;
    float pitchRatio = 1.0f;
    float globalFrequency = 0.0f;
    float filterCutoffHz = 2800.0f;  // dark ambient baseline
    float waveshapeMix = 0.15f;    // gentle saturation for warmth
    float stereoWidth = 0.80f;    // ~20% L/R spread
    float cosmicWidth = 0.70f;
    float reverbAmount = 0.40f;    // present but not overwhelming

    // LFO phases
    float driftPhase = 0.0f;   // primary pitch drift  (0.08 Hz — slower, more ambient)
    float filterLfoPhase = 0.0f;   // filter breath        (0.018 Hz — very slow sweep)
    float cosmicPhaseL = 0.0f;   // cosmic shimmer L     (0.031 Hz)
    float cosmicPhaseR = 0.0f;   // cosmic shimmer R     (0.037 Hz, offset)
    float tremoloPhase = 0.0f;   // amplitude breath     (0.05 Hz — barely perceptible)

    int heldNotesCount = 0;

    // Lowpass
    float lpZL = 0.0f, lpZR = 0.0f;
    float lpA = 0.0f;

    // M/S width — subtle, mono-safe
    float msWidthGain = 1.25f;

    // ---- Per-preset modulation (previously hardcoded in render) ----
    // Defaults equal the old hardcoded values, so Binary Pad sounds unchanged.
    float filterBaseHz = 2800.0f;   // cutoff set by the processor; LFO moves around it
    float driftHz = 0.08f, driftCents = 6.0f;
    float filterLfoHz = 0.018f, filterLfoDepth = 900.0f;
    float cosmicHzL = 0.031f, cosmicHzR = 0.037f, cosmicCents = 14.0f;
    float tremoloHz = 0.05f, tremoloDepth = 0.06f;
    float shadowMix = 0.15f, shadowCents = 4.0f;
    int   waveTableType = 0;

    // ---- Binary orbit pair ----
    float orbBaseCur = 0.0f, orbBaseTgt = 0.0f;
    float orbLevelCur = 0.0f, orbLevelTgt = 0.0f;
    float orbHzCur = 0.0f, orbHzTgt = 0.0f;
    float orbDopCur = 0.0f, orbDopTgt = 0.0f;
    float orbSplitCur = 1.0f, orbSplitTgt = 1.0f;
    float orbPhase = 0.0f;                       // orbital angle (radians)
    std::array<float, 2> orbVoicePh{};           // oscillator phase 0..1 per body
    std::vector<float> orbitTable;               // soft, rounded voice

    // ---- Bell overtone modes ----
    bool  bellModesOn = false;
    std::array<float, 2> bellRatio{ 1.586f, 2.152f };
    std::array<float, 2> bellAmp{ 0.30f, 0.15f };
    std::array<float, 2> bellTau{ 2.3f, 2.2f };
    std::array<std::array<float, 2>, 128> bellPhase{};   // 0..1 per partial
    std::array<float, 128> noteAgeSec{};                  // since note-on
    std::array<float, 128> lastHz{};                      // pitch at last render (for release tail)
    std::array<float, 128> noteHz{};                      // note pitch, kept through release
    bool  trackReleasePitch = false;                      // keep applying pitch ratios after note-off (pluck)
    std::array<bool, 128> freshStart{};                   // voice was silent at its last note-on

    // ---- Kozai-Lidov Triple ----
    // Voice: wavetable bank of the Peters-Mathews GW harmonic spectrum vs e,
    // with band-limited mip levels. Bursts: strain envelope |h| of a Kepler orbit.
    static constexpr int KL_NE = 48, KL_MIPS = 5, KL_TAB = 2048;
    static constexpr int KL_ENV_NE = 96, KL_ENV_NM = 256;
    static constexpr float KL_EMAX = 0.95f;
    std::array<int, KL_MIPS> klMipHarm{ 48, 24, 12, 6, 3 };
    std::vector<float> klBank;                   // [mip][e][KL_TAB]
    std::vector<float> klEnvTab;                 // [e][mean anomaly]
    bool  klModeOn = false;
    float klECur = 0.1f, klETgt = 0.1f;
    float klOrbHzCur = 4.0f, klOrbHzTgt = 4.0f;
    float klLevCur = 1.0f, klLevTgt = 1.0f;
    float klOrbitPh = 0.0f;                      // mean anomaly / 2π, 0 = periapsis
    std::array<float, 128> klPhase{};            // per-note orbital phase 0..1
    void  buildKozaiTables();      // copies the shared tables (computed once per session)
    void  computeKozaiTables();
    float klVoice(int note, float hz, float e);
    float klBurst(float e, float ph01) const;

    // ---- Gravitational Lens ----
    struct Allpass2
    {
        float a2 = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        inline float process(float x)
        {
            const float y = a2 * (x + y2) - x2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }
    };
    struct HilbertPath
    {
        std::array<Allpass2, 4> st{};
        void init(const float* a) { for (int i = 0; i < 4; ++i) { st[(size_t)i] = {}; st[(size_t)i].a2 = a[i] * a[i]; } }
        inline float process(float x) { for (auto& s : st) x = s.process(x); return x; }
    };
    HilbertPath hilA_L, hilA_R, hilB;            // A paths (direct), B path (90° image)
    float hilAdelL = 0.0f, hilAdelR = 0.0f;      // one-sample delay on the A paths
    bool  lensOn = false;
    float lensMassTgt = 22607.0f;
    float lensAp = 1.26f, lensAm = 0.77f, lensNorm = 0.68f, lensT = 1.01f;
    float lensDuckDepth = 0.5f;
    std::vector<float> lensBuf; int lensMask = 0, lensW = 0;
    float lensStepCur = 0.0f, lensStepNew = 0.0f, lensXf = 1.0f; // stepped tap (samples)
    int   lensStepCount = 0;
    float lensSlide = 0.0f;                      // sliding tap (samples)
    float lensHpA = 0.0f, hpZL = 0.0f, hpXL = 0.0f, hpZR = 0.0f, hpXR = 0.0f;
    float lensEnv = 0.0f, lensRef = 1.0e-3f;
    float lensDtTgt = 0.0f;                      // target image delay (samples)
    float lensSlideK = 0.0f, lensAtt = 0.0f, lensRel = 0.0f, lensRefDec = 0.0f;
    void  resetLens();
    inline float lensRead(float delaySamples) const;
    void  processLens(float& L, float& R);

    OnePole decorrAmountSmooth;

    // FDN reverb — 4-line feedback delay network
    static constexpr int FDN_LINES = 4;
    static constexpr int FDN_MAX = 8192;

    std::array<std::array<float, FDN_MAX>, FDN_LINES> fdnBufL{};
    std::array<std::array<float, FDN_MAX>, FDN_LINES> fdnBufR{};
    std::array<int, FDN_LINES> fdnLen{};
    std::array<int, FDN_LINES> fdnPos{};
    float fdnFeedback = 0.78f;  // lush, long tail

    // Shimmer pitch-shift buffer
    static constexpr int SHIMMER_BUF = 8192;
    std::array<float, SHIMMER_BUF> shimBufL{};
    std::array<float, SHIMMER_BUF> shimBufR{};
    int   shimWritePos = 0;
    float shimReadPosL = 0.0f;
    float shimReadPosR = 0.0f;

    std::vector<CosmicSynthOscillator> oscillators;
    std::vector<float> waveTable;
};