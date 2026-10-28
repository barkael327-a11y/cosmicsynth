// PluginProcessor.h
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cstdint>

#include "CosmicSynthEngine.h"
#include "GWSeries.h"
#include "Smooth.h"
#include "PresetManager.h"

//==============================================================================

class CosmicsynthAudioProcessor : public juce::AudioProcessor
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    CosmicsynthAudioProcessor();
    ~CosmicsynthAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi()  const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    APVTS& getVTS() noexcept { return apvts; }
    const APVTS& getVTS() const noexcept { return apvts; }

    bool loadGWFile(const juce::File& f);

    const GWSeries& getGW()  const noexcept { return gw; }
    bool isGWLoaded()        const noexcept { return gwLoaded.load(std::memory_order_relaxed); }

    double getLastBlockTime() const noexcept
    {
        return lastBlockTimeSec.load(std::memory_order_relaxed);
    }

    int getCurrentMidiNote() const noexcept
    {
        return currentMidiNote.load(std::memory_order_relaxed);
    }

    // Where the active preset is reading in the CSV (seconds) — for InspiralView
    double getCsvReadHead() const noexcept { return vizCsvPos.load(std::memory_order_relaxed); }
    bool   isGWEventSounding() const noexcept { return vizOrbitHz.load(std::memory_order_relaxed) > 0.5f; }

    float getCurrentNoteHz() const noexcept
    {
        return currentNoteHz.load(std::memory_order_relaxed);
    }

    //==========================================================================
    // Preset system
    //==========================================================================
    void          setPreset(PresetId id);
    PresetId      getCurrentPreset() const noexcept
    {
        return static_cast<PresetId>(currentPreset.load(std::memory_order_relaxed));
    }

    // Parameter IDs
    static const juce::String p_gwEnable;
    static const juce::String p_gwGainDepth;
    static const juce::String p_gwPitchDepth;
    static const juce::String p_gwFilterDepth;
    static const juce::String p_gwTimbreDepth;
    static const juce::String p_gwSmoothingMs;
    static const juce::String p_klTilt;
    static const juce::String p_lensMass;

    // Current inner-binary eccentricity (Kozai-Lidov preset) — for the UI
    float getKozaiEccentricity() const noexcept { return klCurrentE.load(std::memory_order_relaxed); }

    //==========================================================================
    // UI support (message thread reads; audio thread writes atomics)
    //==========================================================================
    struct Viz
    {
        // Binary Pad
        std::atomic<bool>  padEvent{ false };      // an inspiral/ringdown is sounding
        std::atomic<float> padFRatio{ 1.0f };      // f_GW now / f_GW at event start
        std::atomic<float> padSplit{ 1.0f };       // 1 = two bodies, 0 = merged
        std::atomic<float> padOrbitHz{ 0.0f };     // orbit rate the audio is using
        std::atomic<float> padPostMerger{ -1.0f }; // seconds since merger (-1 = before)
        // Inspiral Pluck (most recent note)
        std::atomic<float> pluckFrac{ 1.0f };      // 0 = note-on, 1 = arrived at merger
        std::atomic<double> pluckPos{ 0.0 };       // CSV time being read
        std::atomic<int>   pluckNoteCount{ 0 };
        // Kerr Quasinormal Modes
        std::atomic<float> bellEnv{ 0.0f };        // l=m=2 envelope of the loudest note
        // Kozai-Lidov
        std::atomic<float> klOrbitPhase{ 0.0f };   // mean anomaly / 2π (0 = periapsis burst)
        std::atomic<float> klCycle{ 0.0f };        // position in the 8-bar KL cycle (0..1)
        std::atomic<float> bpm{ 120.0f };
    };
    Viz viz;

    // Mono tap of the plugin's output for the spectrogram (lock-free FIFO)
    int  pullScopeSamples(float* dest, int maxSamples);

    // Physics measured from the loaded waveform (see analyseGW)
    struct Analysis { double tMerger, fMin, fMerger, fQnm, tauQnm, Q, spin; bool hasRingdown; };
    Analysis getAnalysis() const noexcept
    {
        return { gwTMerger, gwFgwMin, gwFgwMerger, gwQnmFreq, gwQnmTau, gwQnmQ, gwRemnantSpin, gwHasRingdown };
    }
    // Kerr overtones actually in use: {ratio33, ratio44, amp33, amp44, tau22 (s)}
    std::array<float, 5> getBellModes() const noexcept { return bellModeInfo; }
    juce::String getDataName() const { return dataName; }
    int  getDataVersion() const noexcept { return dataVersion.load(std::memory_order_relaxed); }
    void loadBuiltInWaveform();

    // Kozai-Lidov eccentricity at a point in the cycle, for a given tilt
    // (the same table the audio uses — the UI draws e(t) from it)
    float klEccentricityAt(float tiltDeg, double phase01) const;

private:
    APVTS apvts;
    static APVTS::ParameterLayout createLayout();

    GWSeries gw;
    std::atomic<bool> gwLoaded{ false };

    OnePole smAmp, smPitch, smCut, smTimbre, smPan;
    void setSmoothMs(double ms);

    double freeRunSeconds = 0.0;
    std::atomic<double> lastBlockTimeSec{ 0.0 };

    std::array<bool, 128> noteDown{};
    int lastNoteOn = -1;

    std::atomic<double> vizCsvPos{ 0.0 };

    juce::AbstractFifo scopeFifo{ 1 << 15 };
    std::vector<float> scopeBuf = std::vector<float>(1 << 15, 0.0f);
    juce::String dataName{ "none" };
    juce::String dataPath;                 // empty = built-in
    std::atomic<int> dataVersion{ 0 };
    std::array<float, 5> bellModeInfo{ 1.586f, 2.152f, 0.0f, 0.045f, 2.4f };
    std::atomic<float>  vizOrbitHz{ 0.0f };
    std::atomic<int>   currentMidiNote{ -1 };
    std::atomic<float> currentNoteHz{ 0.0f };

    double noteTimeSec = 0.0;
    bool   wasAnyNoteDown = false;

    // Inter-block gain continuity
    float prevGainEnd = 0.20f;

    // Passive pitch drift between GW events
    double passiveDriftPhase = 0.0;

    // Per-note chirp envelope state (pluck preset)
    std::array<double, 128> chirpPhase{};       // seconds since note-on
    std::array<bool, 128> noteJustOn{};        // true on the block a note fired

    // Per-note CSV snapshot taken at note-on
    std::array<float, 128> chirpCentsPerNote{}; // fgw-derived depth (kept for fallback)
    std::array<float, 128> noteOnH{};           // |h| at note-on → merger "ding" intensity
    std::array<float, 128> noteOnPsi{};         // psi at note-on → per-note timbre variation
    std::array<double, 128> transientPhase{};    // seconds since the merger "ding" fired

    // Per-note real-time CSV walk state
    std::array<double, 128> fgwWalkPos{};        // current CSV time being read for this note
    std::array<double, 128> hModWalkPos{};       // separate walk position for h AM modulation
    std::array<float, 128> noteOnFgw{};         // fgw value at note-on (reference for delta)
    std::array<float, 128> noteOnVelocity{};    // MIDI velocity 0..1 → scales chirp depth
    std::array<double, 128> noteAge{};           // seconds since note-on
    std::array<bool, 128> noteInRingdown{};    // true once this note's walk passes the merger
    std::array<float, 128> lastChirpCents{};    // pitch held when the note hits merger
    std::array<double, 128> noteWalkSpeed{};    // CSV-sec per sec so the note reaches merger in chirpTimeMs
    std::array<float, 128> noteChirpDepth{};    // cents below the note at note-on

    // Kerr overtone modes computed from the measured remnant spin
    void pushBellModes();

    // ---- CSV analysis (computed once in loadGWFile, read-only on the audio thread)
    // These let the presets find the *physical* landmarks of the waveform instead
    // of guessing from fractions of the file length.
    double gwTMerger = 0.0;   // time of peak |h| (merger)
    double gwFgwMin = 20.0;  // lowest fgw in the file (start of inspiral)
    double gwFgwMerger = 200.0; // fgw at merger — top of the chirp
    double gwQnmFreq = 250.0; // ringdown frequency measured from the tail
    double gwQnmTau = 0.004; // ringdown e-folding time measured from |h| envelope
    double gwQnmQ = 3.0;   // quality factor Q = pi * f * tau
    double gwRemnantSpin = 0.5;   // Kerr spin inverted from Q (Berti et al. fit)
    double gwPeakEnv = 1.0;   // peak |h| envelope
    bool   gwHasRingdown = false; // false for files with no detectable ringdown

    void   analyseGW();
    void   resetWalkPositions();
    double hEnvelopeAt(double t) const;         // local |h| amplitude (max over one GW cycle)
    float  fgwLogNorm(double f) const;         // 0 at fgwMin, 1 at merger, log-frequency
    void   advanceGravMemory(double dt);

    // Kerr Quasinormal Modes per-note state
    std::array<double, 128> bellDecaySec{};      // l=m=2 decay time, from the measured Q
    std::array<double, 128> bellAge{};           // seconds since note-on
    std::array<float, 128> bellDetuneGain{};    // shimmer gain (stylization), from strike energy + spin
    std::array<float, 128> bellSpinAlpha{};     // measured remnant spin 0..1

    // "Gravitational memory" reverb bloom (a stylization inspired by the
    // Christodoulou memory effect; the real effect is a permanent strain offset).
    // gwMemoryDrift tracks current reverb amount (0..1), gwMemoryTarget is the bloom goal.
    // Bloom attacks fast and decays over ~12 s.
    double gwMemoryDrift = 0.0;   // current reverb bloom level (0..1)
    double gwMemoryTarget = 0.0;   // target reverb — accumulates on each note-on
    double gwMemoryDecaySec = 12.0;  // bloom decay time constant
    double gwMemoryAttackSec = 0.08;  // bloom attack time constant
    std::array<double, 128> ringdownPhase{};     // seconds since ringdown triggered

    // CSV walk position — advances per note-on through the inspiral timeline
    double gwCsvWalkPos = 0.0;

    // Pad h-column walk — continuous position through CSV for h-driven pitch
    // Advances in real time during events; slow drift between events
    double padHWalkPos = 0.0;
    double padHWalkPosDrift = 0.0; // separate slower walk for between-event drift

    // Kozai-Lidov cycle: e(phase) per tilt, tempo-locked cycle clock
    static constexpr int    kKlTilts = 71, kKlPhases = 512;
    static constexpr double kKlTiltMin = 40.0, kKlTiltMax = 75.0;
    std::vector<float> klETable;
    double klCyclePh = 0.0;
    double silenceSec = 1.0e9;
    double hostBpm = 120.0;
    std::atomic<float> klCurrentE{ 0.0f };
    void  buildKozaiCycleTable();
    static void computeKozaiCycleTable(std::vector<float>& table);

    // GW event scheduler
    uint32_t rngState = 0x12345678u;

    bool   gwEventActive = false;
    double gwWaitSec = 2.5;
    double gwEventPhase01 = 0.0;
    double gwEventDurSec = 6.0;
    double gwVibPhase = 0.0;

    float  gwPeakScale = 1.0f;
    double gwCsvStartT = 0.0;
    double gwCsvSpanSec = 3.0;
    double gwSinceMergerSec = 1.0e9;   // seconds since last merger (1e9 = none)
    float  prevWobCents = 8.0f;        // wobble depth at end of previous block

    // Active preset
    std::atomic<int> currentPreset{ static_cast<int>(PresetId::BinaryPad) };
    PresetSettings   ps;   // copy of current preset settings, updated on setPreset()
    juce::CriticalSection presetLock;

    CosmicSynthEngine synth;

    uint32_t xorshift32() noexcept;
    float    rand01()     noexcept;
    float    randRange(float lo, float hi) noexcept;
    void     startNewGWEvent(double tOffsetSec);
    void     processGW_RingdownBell(double dt, float depthAmp, float depthPitch,
        float depthCut, float depthTimb, bool anyDown,
        std::array<float, 128>& ratiosStart, std::array<float, 128>& ratiosEnd,
        float& gainEnd, float& cutoffMid, float& timbreMid);
    void     applyPresetToEngine();

    // Per-preset processBlock helpers
    void processGW_BinaryPad(double dt, float depthAmp, float depthPitch,
        float depthCut, float depthTimb,
        bool anyDown,
        std::array<float, 128>& ratiosStart,
        std::array<float, 128>& ratiosEnd,
        float& gainEnd, float& cutoffMid,
        float& timbreMid);

    void processGW_InspiralPluck(double dt, float depthAmp, float depthPitch,
        float depthCut, float depthTimb,
        bool anyDown,
        std::array<float, 128>& ratiosStart,
        std::array<float, 128>& ratiosEnd,
        float& gainEnd, float& cutoffMid,
        float& timbreMid);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CosmicsynthAudioProcessor)
};