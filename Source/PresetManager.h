// PresetManager.h
#pragma once
#include <JuceHeader.h>

//==============================================================================
// Preset IDs — add new entries here as more presets are created
//==============================================================================
enum class PresetId : int
{
    BinaryPad = 0,
    InspiralPluck = 1,
    RingdownBell = 2,
    KozaiLidov = 3,
    GravLens = 4,
    // Future presets go here...
};

constexpr int kNumPresets = 5;

inline juce::String presetName(PresetId id)
{
    switch (id)
    {
    case PresetId::BinaryPad:     return "Binary Pad";
    case PresetId::InspiralPluck: return "Inspiral Pluck";
    case PresetId::RingdownBell:  return "Ringdown Bell";
    case PresetId::KozaiLidov:    return "Kozai-Lidov Triple";
    case PresetId::GravLens:      return "Gravitational Lens";
    default:                      return "Unknown";
    }
}

//==============================================================================
// PresetSettings — all tunable values that differ between presets.
// Add a new field here whenever a parameter needs to vary per preset.
//==============================================================================
struct PresetSettings
{
    // ADSR
    float adsrAttack = 1.40f;
    float adsrDecay = 2.00f;
    float adsrSustain = 0.80f;
    float adsrRelease = 5.50f;

    // Oscillator
    float waveshapeMix = 0.15f;   // gentle saturation
    float freqSlewMs = 35.0f;   // pitch slew time

    // Filter baseline and LFO
    float filterBaseline = 2800.0f;
    float filterLfoDepth = 900.0f;
    float filterLfoHz = 0.018f;

    // LFO rates
    float driftHz = 0.08f;
    float driftCents = 6.0f;
    float cosmicHzL = 0.031f;
    float cosmicHzR = 0.037f;
    float cosmicCents = 14.0f;
    float tremoloHz = 0.05f;
    float tremoloDepth = 0.06f;  // fraction of amplitude

    // Stereo
    float stereoWidth = 0.80f;
    float msWidthGain = 1.25f;
    float cosmicWidth = 0.70f;
    float shadowMix = 0.15f;  // chorus detuning blend
    float shadowCents = 4.0f;   // ±cents for shadow osc

    // Reverb
    float reverbBase = 0.38f;
    float reverbPeak = 0.68f; // bloom at GW merger
    float reverbFeedback = 0.78f;

    // GW event timing
    float gwEventDurMin = 4.0f;
    float gwEventDurMax = 8.0f;
    float gwWaitMin = 3.0f;
    float gwWaitMax = 9.0f;   // sqrt-randomised in processor
    float gwCsvSpanMin = 2.0f;
    float gwCsvSpanMax = 5.0f;

    // GW modulation depths (within preset, independent of APVTS params)
    float gwPitchMaxCents = 80.0f;  // pitch ceiling — visible on spectrogram
    float gwVibHzMin = 0.30f;  // target Hz early inspiral — ~1 cycle per 3s
    float gwVibHzMax = 2.50f;  // target Hz near merger — fast visible ripple

    // Passive drift (between events)
    float passiveDriftHz = 0.022f;
    float passiveDriftCents = 4.0f;

    // Pluck-specific: chirp pitch envelope
    // When chirpEnabled, each note-on samples the CSV and sets chirp depth
    // dynamically from fgw — so the chirp character varies with the GW data
    bool  chirpEnabled = false;
    float chirpCentsMin = 0.0f;   // chirp depth when fgw is at its minimum
    float chirpCentsMax = 0.0f;   // chirp depth when fgw is at its maximum (near merger)
    float chirpDecayMs = 0.0f;   // how fast the chirp collapses back to 0

    // Pluck transient: waveshape spike at note-on, decays quickly
    // Depth set by |h| at the CSV sample moment — louder near merger
    float transientDepthMax = 0.0f;   // max waveshapeMix spike (0..1)
    float transientDecayMs = 0.0f;   // how fast the transient decays

    // CSV walk: how far the read-head advances per note-on
    float gwCsvWalkStepSec = 0.0f;

    // Real-time CSV walk during note lifetime (replaces static chirp decay)
    // fgwWalkSpeedScale: how many seconds of CSV data per second of note time
    // 1.0 = real-time, 8.0 = 8x compressed (traverses more of the inspiral per note)
    float fgwWalkSpeedScale = 0.0f;

    // Merger cutoff: when normalized fgw exceeds this, ringdown begins
    // 0.0 = never triggers, 1.0 = only at absolute max fgw
    float fgwMergerThreshold = 1.1f;  // >1 means disabled by default

    // Ringdown: amplitude decay after merger cutoff
    float ringdownDecayMs = 0.0f;

    // h-column amplitude modulation — uses signed h to create GW waveform tremolo
    // hModDepth: fraction of amplitude modulated (0 = off, 0.3 = 30% AM)
    // hModSpeedScale: how fast we walk through h values (separate from fgw walk)
    float hModDepth = 0.0f;
    float hModSpeedScale = 0.0f;

    // Global gain ceiling
    float gainCeiling = 0.45f;

    // Ringdown Bell specific
    // qnmDecayMs: ? in e^(-t/?) — the QNM exponential decay time
    // Driven dynamically by how fast |h| drops in the CSV ringdown region
    float qnmDecayMs = 0.0f;   // base decay time (ms)
    float qnmDetuneCents = 0.0f;   // second layer detuning for mode splitting
    float qnmDetuneGain = 0.0f;   // second layer gain (0..1)
    // ringdownSampleStart: normalized CSV position (0..1) to begin sampling
    // Set to the ringdown tail region so |h| decay rate is read from there
    float ringdownSampleStart = 0.80f; // start sampling at 80% through CSV

    // ---- Voice character (engine) ----
    // 0 = warm pad table, 1 = soft pluck table, 2 = pure sine (QNM bell)
    int   waveTableType = 0;

    // ---- Binary orbit pair (Binary Pad) ----
    // Two voices circling each other through each GW event, climbing with fgw
    // and fusing at merger — the rising GW track on the spectrogram.
    float orbitLevel = 0.0f;     // 0 = off
    float orbitOctaves = 1.5849625f; // climb from event start to the RINGDOWN frequency

    // ---- Pluck chirp-into-note ----
    // Each note starts below pitch and rises INTO the note along the real chirp
    // curve, arriving (in tune) at merger after chirpTimeMs.
    float chirpTimeMs = 140.0f;

    // ---- Bell overtones ----
    // Adds the Kerr l=m=3 and l=m=4 quasinormal modes as inharmonic partials.
    // Their amplitudes are set by the binary's mass ratio q = m1/m2 >= 1
    // (Kamaretsos et al. 2012). Equal masses (q = 1, like gw_imr_full.csv):
    // the l=m=3 mode vanishes by symmetry and l=m=4 is ~4% — an almost pure
    // bell. Raise q for an unequal-mass merger: the l=m=3 partial (≈ a minor
    // sixth above) grows in, which is exactly what the physics predicts.
    bool  bellModes = false;
    float bellMassRatio = 1.0f;

    // ---- Kozai-Lidov Triple ----
    // The voice is the gravitational-wave harmonic spectrum of an eccentric orbit
    // (Peters & Mathews 1963), and it pulses once per orbit with the strain
    // envelope |h| of a Kepler orbit. Eccentricity follows the secular
    // Kozai-Lidov cycle for the tilt set on the "KL Tilt" knob.
    bool  klMode = false;
    float klOrbitBeats = 0.5f;     // one orbit (one burst) per eighth note
    float klCycleBeats = 32.0f;    // one Kozai-Lidov cycle per 8 bars of 4/4
    float klLevelExp = 0.12f;      // loudness ∝ f(e)^klLevelExp (Peters-Mathews enhancement, compressed)

    // ---- Gravitational Lens ----
    // Point-mass lens applied to the voice: two images (magnifications, delay,
    // 90° Morse phase on the second image), diffraction below c^3/(8πGM).
    bool  lensMode = false;
    float lensAlignment = 0.5f;    // source position y in Einstein radii
    float lensDuckDb = 6.0f;       // echo dip while playing (echo range only; stylization)
};

//==============================================================================
// Factory — returns canonical settings for each preset
//==============================================================================
inline PresetSettings makePreset(PresetId id)
{
    PresetSettings s; // defaults = Binary Pad

    if (id == PresetId::InspiralPluck)
    {
        // === Inspiral Pluck ===
        // Short, punchy, upward chirp on attack

        s.adsrAttack = 0.004f;  // 4 ms — soft enough not to click, still a pluck
        s.adsrDecay = 0.90f;   // every note rings the same length, set only by the envelope
        s.adsrSustain = 0.00f;   // no sustain — true pluck
        s.adsrRelease = 0.35f;  // natural tail when the key is released early

        s.waveshapeMix = 0.18f;   // a little bite, less harsh than before
        s.freqSlewMs = 22.0f;   // rounds off the last whip of the chirp

        s.filterBaseline = 4200.0f;  // a touch darker — rounder pluck
        s.filterLfoDepth = 900.0f;   // gentle movement (now actually applied by the engine)
        s.filterLfoHz = 0.08f;

        s.driftHz = 0.18f;  // faster drift for energy
        s.driftCents = 5.0f;
        s.cosmicHzL = 0.05f;
        s.cosmicHzR = 0.06f;
        s.cosmicCents = 6.0f;   // less shimmer, tighter
        s.tremoloHz = 0.0f;   // no tremolo — pluck is percussive
        s.tremoloDepth = 0.0f;

        s.stereoWidth = 0.70f;
        s.msWidthGain = 1.15f;
        s.cosmicWidth = 0.40f;
        s.shadowMix = 0.10f;  // minimal chorus
        s.shadowCents = 3.0f;

        // Short room reverb — tight, rhythmic stays clean
        s.reverbBase = 0.20f;
        s.reverbPeak = 0.35f;
        s.reverbFeedback = 0.42f; // short room tail

        // GW events shorter and more frequent for rhythmic feel
        s.gwEventDurMin = 0.30f;
        s.gwEventDurMax = 0.80f;
        s.gwWaitMin = 0.5f;
        s.gwWaitMax = 3.0f;
        s.gwCsvSpanMin = 0.3f;
        s.gwCsvSpanMax = 1.2f;

        s.gwPitchMaxCents = 25.0f;  // inter-note pitch modulation range
        s.gwVibHzMin = 0.5f;
        s.gwVibHzMax = 6.0f;

        s.passiveDriftHz = 0.0f;  // no passive drift — pluck is rhythmic
        s.passiveDriftCents = 0.0f;

        // Chirp into the note: each note starts below pitch and rises along the
        // real chirp curve, landing exactly on the note at "merger".
        // Depth depends on where in the inspiral the note was struck:
        // early = deep, slow-then-sudden swoop; late = short flick.
        s.chirpEnabled = true;
        s.chirpCentsMin = 0.0f;
        s.chirpCentsMax = 500.0f;  // deepest swoop ≈ a fourth below the note
        s.chirpDecayMs = 180.0f;   // (unused)
        s.chirpTimeMs = 140.0f;    // every note arrives on pitch after 140 ms

        // Merger "ding": a short brightness spike when the note arrives on pitch
        s.transientDepthMax = 0.45f;
        s.transientDecayMs = 25.0f;

        s.waveTableType = 1;        // soft pluck table

        // CSV walk: moderate step so notes land at varied CSV positions
        // without jumping so far that the walk wraps unexpectedly
        s.gwCsvWalkStepSec = 0.30f;  // bigger jumps — more variation between notes

        // Real-time CSV walk: CSV-seconds per real second (× 0.4–1.6 by velocity).
        // At 8×, a ~0.2 s pluck reads ~0.6–2.5 s of inspiral: notes struck in the
        // last second or two reach merger mid-note; earlier notes only bend a little.
        // (At 25× every note raced to merger within a few ms, so every note sounded
        // the same.)
        s.fgwWalkSpeedScale = 8.0f;

        // Merger is now detected from the data (peak |h|) — this threshold is
        // no longer used by the pluck.
        s.fgwMergerThreshold = 0.60f;

        // Ringdown: no longer chops the volume — the envelope alone sets note length
        s.ringdownDecayMs = 0.0f;

        // h-column AM: signed h drives a slow waveform tremolo
        // Depth 0.22 = gentle organic fluctuation from the actual GW waveform shape
        // Speed 4.0 = walk through h values 4x faster than real time
        s.hModDepth = 0.0f;   // AM removed — chirp+ringdown carry GW identity
        s.hModSpeedScale = 0.0f;

        s.gainCeiling = 0.55f;  // slightly louder than pad
    }

    if (id == PresetId::RingdownBell)
    {
        // === Ringdown Bell (Kerr QNM Bell) ===
        // After merger, the black hole rings in quasinormal modes: e^(-t/?) sin(?t)
        // Each keypress is one bell tone. Pitch = MIDI note. Decay = ? from CSV ringdown.
        // A detuned second layer mimics the two GW polarizations (h+, hx) beating.

        s.adsrAttack = 0.001f;  // instant attack — bell strikes immediately
        s.adsrDecay = 2.50f;   // long decay envelope (QNM rings slowly)
        s.adsrSustain = 0.00f;   // no sustain — bell rings and dies
        s.adsrRelease = 0.80f;   // gentle release tail

        s.waveshapeMix = 0.05f;   // near-pure sine — QNM is a clean sinusoid
        s.freqSlewMs = 2.0f;    // very snappy pitch — bell doesn't glide

        s.filterBaseline = 18000.0f; // wide open — let the sine breathe
        s.filterLfoDepth = 0.0f;     // no filter LFO — bell is pure
        s.filterLfoHz = 0.0f;

        s.driftHz = 0.0f;   // no pitch drift — bell is stable
        s.driftCents = 0.0f;
        s.cosmicHzL = 0.025f;
        s.cosmicHzR = 0.031f;
        s.cosmicCents = 5.0f;   // subtle spatial shimmer
        s.tremoloHz = 0.0f;
        s.tremoloDepth = 0.0f;

        s.stereoWidth = 0.90f;  // wide — bell resonates in space
        s.msWidthGain = 1.30f;
        s.cosmicWidth = 0.50f;
        s.shadowMix = 0.35f;  // detuned layer prominent — mode splitting
        s.shadowCents = 6.0f;   // ±6 cents = h+/hx polarization beat

        // Long cathedral reverb — bell in space
        s.reverbBase = 0.60f;
        s.reverbPeak = 0.85f;
        s.reverbFeedback = 0.88f;

        // No GW events — bell doesn't have an inspiral event arc
        // Instead the CSV is sampled continuously for ? modulation
        s.gwEventDurMin = 999.0f; // effectively disabled
        s.gwEventDurMax = 999.0f;
        s.gwWaitMin = 999.0f;
        s.gwWaitMax = 999.0f;

        s.gwPitchMaxCents = 0.0f;  // no pitch modulation from GW events
        s.gwVibHzMin = 0.0f;
        s.gwVibHzMax = 0.0f;

        s.passiveDriftHz = 0.0f;
        s.passiveDriftCents = 0.0f;

        s.chirpEnabled = false; // bell has no chirp — it's post-merger

        s.gainCeiling = 0.50f;

        // Bell-specific QNM parameters
        // qnmDecayMs is the BASE decay — modulated by CSV |h| rate of decay
        // Faster |h| drop in CSV = shorter bell decay (high-spin Kerr = faster ringdown)
        s.qnmDecayMs = 1800.0f; // ~1.8s base decay — a large bell
        s.qnmDetuneCents = 6.5f;    // 6.5 cents sharp for mode-2 layer
        s.qnmDetuneGain = 0.55f;   // second mode at 55% amplitude
        s.ringdownSampleStart = 0.82f;  // (unused — merger/ringdown are detected from the data)

        // Softer, more resonant preset adjustments
        s.adsrDecay = 4.00f;   // long QNM ring
        s.adsrRelease = 1.50f;   // gentle release tail
        s.waveshapeMix = 0.02f;   // near-pure sine
        s.reverbFeedback = 0.92f; // cathedral wash
        s.shadowCents = 4.0f;    // softer mode split
        s.shadowMix = 0.10f;   // light chorus only — the "h+/hx detune" isn't a real
        // QNM effect (both polarizations ring at the same frequency)
        s.qnmDecayMs = 2400.0f; // longer base decay

        s.waveTableType = 2;    // pure sine — a QNM is a clean damped sinusoid
        s.bellModes = true;     // add the l=m=3 / l=m=4 overtones (inharmonic → bell-like)
    }

    if (id == PresetId::BinaryPad)
    {
        s.orbitLevel = 0.72f;        // the two black holes
        // One uniform log-frequency map for the WHOLE signal (inspiral, merger
        // and ringdown), anchored so the ringdown frequency lands exactly a
        // twelfth (3×, octave + fifth) above the pair's starting note — a
        // perfectly consonant landing. The merger point falls wherever the
        // physics puts it on the way up.
        s.orbitOctaves = 1.5849625f; // log2(3)
        s.gwPitchMaxCents = 130.0f;  // wobble ceiling for the widest chord voice
    }

    if (id == PresetId::KozaiLidov)
    {
        // === Kozai-Lidov Triple ===
        // A third body pumps the inner binary's eccentricity up and down; an
        // eccentric orbit radiates in bursts at closest approach. Rhythm = the
        // orbit, timbre = its GW harmonic spectrum, 8-bar breathing = the KL cycle.
        s.klMode = true;

        s.adsrAttack = 0.020f;
        s.adsrDecay = 0.50f;
        s.adsrSustain = 1.00f;     // the orbit keeps pulsing while you hold the note
        s.adsrRelease = 0.35f;

        s.waveshapeMix = 0.0f;     // timbre comes from the physics, no extra saturation
        s.freqSlewMs = 4.0f;
        s.filterBaseline = 9000.0f;
        s.filterLfoDepth = 0.0f;
        s.filterLfoHz = 0.0f;

        s.driftHz = 0.0f;  s.driftCents = 0.0f;
        s.cosmicHzL = 0.03f; s.cosmicHzR = 0.037f; s.cosmicCents = 3.0f;
        s.tremoloHz = 0.0f; s.tremoloDepth = 0.0f;

        s.stereoWidth = 0.55f; s.msWidthGain = 1.10f; s.cosmicWidth = 0.15f;
        s.shadowMix = 0.0f;        // chorus would smear the harmonic series

        s.reverbBase = 0.30f; s.reverbPeak = 0.30f; s.reverbFeedback = 0.65f;  // matches the prototype's burst depth

        s.gwEventDurMin = s.gwEventDurMax = 999.0f;   // no GW event scheduler
        s.gwWaitMin = s.gwWaitMax = 999.0f;
        s.gwPitchMaxCents = 0.0f; s.passiveDriftHz = 0.0f; s.passiveDriftCents = 0.0f;
        s.gainCeiling = 0.50f;
    }

    if (id == PresetId::GravLens)
    {
        // === Gravitational Lens ===
        // Soft keys through a point-mass lens. Heavy lens (knob right): a clean
        // second arrival (echo). Light lens (knob left): the images interfere
        // (comb filter / flanger), and diffraction confines it to the treble.
        s.lensMode = true;
        s.waveTableType = 1;       // soft keys table

        s.adsrAttack = 0.004f;
        s.adsrDecay = 1.20f;
        s.adsrSustain = 0.35f;
        s.adsrRelease = 0.50f;

        s.waveshapeMix = 0.0f;
        s.freqSlewMs = 0.1f;       // effectively none: a glide up from 0 Hz at note-on ticks under a 4 ms attack
        s.filterBaseline = 7000.0f;
        s.filterLfoDepth = 0.0f;
        s.filterLfoHz = 0.0f;

        s.driftHz = 0.0f;  s.driftCents = 0.0f;
        s.cosmicHzL = 0.03f; s.cosmicHzR = 0.037f; s.cosmicCents = 2.0f;
        s.tremoloHz = 0.0f; s.tremoloDepth = 0.0f;

        s.stereoWidth = 0.45f; s.msWidthGain = 1.05f;
        s.cosmicWidth = 0.0f;      // no octave shimmer — it clouds the echoes
        s.shadowMix = 0.0f;

        s.reverbBase = 0.12f; s.reverbPeak = 0.12f; s.reverbFeedback = 0.50f; // short, light room

        s.gwEventDurMin = s.gwEventDurMax = 999.0f;
        s.gwWaitMin = s.gwWaitMax = 999.0f;
        s.gwPitchMaxCents = 0.0f; s.passiveDriftHz = 0.0f; s.passiveDriftCents = 0.0f;
        s.gainCeiling = 0.50f;
    }

    return s;
}