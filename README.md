# CosmicSynth

**A synthesizer plugin that turns gravitational-wave physics into playable instruments.**

Each preset is a different gravitational-wave source: an inspiraling black-hole binary, the ringing black hole left after a merger, a Kozai-Lidov hierarchical triple, and a gravitationally lensed signal. The waveform data and the equations behind each source shape the pitch, timbre, rhythm and space of the sound, and a live spectrogram shows the physics as you play.

CosmicSynth is a VST3 plugin written in C++ with [JUCE](https://juce.com). It runs in any VST3 host and was developed and tested in REAPER on Windows.

**Demo video:** [VIDEO LINK]

---

## Presets

| Preset | Source | What you hear |
| --- | --- | --- |
| **Binary Pad** | Two black holes spiraling together | Held chords wobble with the waveform's strain h(t). Every few seconds a merger plays through the chord: two voices circle each other, speeding up and climbing in pitch until they fuse into one. |
| **Inspiral Pluck** | One inspiral per note | Every note starts below its pitch and chirps up along the merger's frequency curve, landing in tune exactly at merger, 140 ms after the key is struck. |
| **Ringdown Bell** (Kerr quasinormal modes) | The remnant black hole after merger | The new black hole rings like a struck bell, in its l=m=2, 3 and 4 quasinormal modes, tuned by the spin measured from the waveform. |
| **Kozai-Lidov Triple** | A binary with a distant third body | The third body slowly pumps the inner orbit's eccentricity. As the orbit stretches, the tone gets buzzier and pulses once per orbit, at closest approach. |
| **Gravitational Lens** | A signal bent by a massive object | Two arrivals of the same signal. A heavy lens gives an echo up to about 1 s later; a light lens makes the two copies interfere, heard as a flanger. |

---

## The physics behind each preset

### Waveform data and analysis

The plugin ships with a built-in simulated waveform of two 11.5-solar-mass black holes merging, so it works without any extra files. Any waveform can be loaded as a CSV with columns `t, h, fgw, psi` (time, strain, gravitational-wave frequency, orbital phase).

When a waveform loads, the plugin measures it:

- **Merger time**: the peak of |h|.
- **Ringdown frequency and damping time τ**: a least-squares fit of ln|h| against time after the peak, accepted only if it is a clean exponential decay (R² > 0.9).
- **Quality factor** Q = π f τ.
- **Remnant spin**: inverted from Q using the Berti, Cardoso & Will (2006) fit for the l=m=2 mode.

### Binary Pad

- The chord's pitch traces the signed strain h(t), time-stretched so its wobble rate rises with the gravitational-wave frequency, as the real signal does approaching merger.
- Two orbit voices represent the black holes. Their orbit rate follows f_GW / 2, and they Doppler-shift in opposite directions with a swing ∝ f^(1/3), which tracks orbital speed.
- Pitch maps the whole signal (inspiral, merger and ringdown) onto one log-frequency scale. The ringdown frequency lands a twelfth (3×) above the starting note, so the merger's frequency jump is reproduced and lands on a consonant interval.

### Inspiral Pluck

- Each note reads a different stretch of the inspiral and walks through it so that every note reaches merger in the same 140 ms. The pitch follows the data's f(t) on the way up.
- Notes struck early in the inspiral, and harder notes, start deeper below pitch, up to about a fourth.
- A short brightness spike (the "ding") fires when the note arrives at merger, scaled by the wave's strength.

### Ringdown Bell (Kerr quasinormal modes)

- A quasinormal mode is a damped sinusoid, so the fundamental (l=m=2) is a pure sine whose decay time scales with the measured Q.
- The l=m=3 and l=m=4 modes are added as overtones. Their frequency ratios and decay times come from the Berti, Cardoso & Will (2006) fits for the measured spin. They sit at non-integer ratios, which is what makes a real bell sound metallic.
- Overtone amplitudes depend on the mass ratio q, following Kamaretsos, Hannam & Sathyaprakash (2012). For equal masses the l=m=3 mode vanishes by symmetry.

### Kozai-Lidov Triple

- The eccentricity cycle comes from integrating the secular quadrupole Kozai-Lidov equations (test-particle limit) with RK4, once for every tilt on the KL Tilt knob (40°–75°). Below about 39.2° there are no Kozai-Lidov oscillations; at 75° the inner orbit reaches e ≈ 0.94.
- The voice is the gravitational-wave harmonic spectrum of an eccentric orbit (Peters & Mathews 1963), precomputed as band-limited wavetables across eccentricity.
- Each orbit produces one burst, shaped by the quadrupole strain |h| of a Kepler orbit over one period.
- Loudness follows the Peters-Mathews power enhancement f(e), compressed for listening.

### Gravitational Lens

- A point-mass lens produces two images with magnifications μ± and a time delay Δt = (4GM/c³)·T(y), at source alignment y = 0.5 Einstein radii.
- The second image carries a 90° Morse phase shift, applied with a Hilbert-transform allpass pair.
- Lensing fades below the diffraction frequency, about c³/(8πGM), applied as a high-pass on the lensed part of the signal.
- The Lens Mass knob runs from 3 to 50,000 solar masses. Its travel is split so the interference (flanger) regime and the echo regime each get a wide range, passing quickly through the in-between "slapback" delays.

### What is stylized, not physical

To keep the instruments musical, a few things are artistic choices rather than physics:

- the reverb "bloom" on the Ringdown Bell, inspired by (but not modeling) the gravitational-wave memory effect;
- the octave-up shimmer in the reverb and the slow pitch drift on some presets;
- the Ringdown Bell's slight amplitude shimmer;
- the Lens echo ducking and its stereo offset (real image separations are far too small to hear as direction);
- time scales. The Kozai-Lidov cycle takes years for real systems, but here it is locked to the host tempo (one cycle per 8 bars, one burst per eighth note), and the Pluck's inspirals are compressed to 140 ms.

---

## Controls

Each control dims on presets that don't use it.

| Control | Presets | What it does |
| --- | --- | --- |
| **Amp Depth** | Pad, Pluck, Bell | How much the wave's strength swells the volume. |
| **Pitch Depth** | Pad | How wide the chord's pitch wobbles as it traces the waveform. |
| **Filter Depth** | Pad, Pluck, Bell | How far the filter opens with the chirp or the bell strike. |
| **Timbre Depth** | Pad, Pluck, Bell | How much the wave's strength adds grit and harmonics. |
| **Smoothing** | Pad, Pluck, Bell | How quickly volume, filter and timbre follow the data (1–200 ms). |
| **KL Tilt** | Kozai-Lidov | Inclination between the inner and outer orbits (40°–75°); sets how eccentric the inner orbit gets. |
| **Lens Mass** | Lens | Lens mass in solar masses: light for interference, heavy for an echo. |
| **GW LIVE** | All | Physics on/off: a plain synth, a circular orbit, or no lens. |
| **LOAD CSV / BUILT-IN** | Pad, Pluck, Bell | Load your own waveform, or return to the built-in merger. |

The display shows the source system, the strain or physics view, live readouts (frequencies, spin, eccentricity, lens delay) and a spectrogram of the output computed with a built-in 8192-point FFT.

---

## Install

1. Download `cosmicsynth.vst3.zip` from [Releases](../../releases) and unzip it.
2. Copy `cosmicsynth.vst3` to `C:\Program Files\Common Files\VST3\`.
3. Rescan plugins in your DAW and load **cosmicsynth** on an instrument track.

## Build from source

Requirements: Windows, Visual Studio 2022, JUCE [JUCE VERSION].

1. Open `cosmicsynth.jucer` in the Projucer and check the JUCE modules path under **Exporters**.
2. Save the project to regenerate the Visual Studio files, or open `Builds/VisualStudio2022/cosmicsynth.sln` directly.
3. Build the **Release | x64** configuration of `cosmicsynth_VST3`.
4. The plugin is written to `Builds/VisualStudio2022/x64/Release/VST3/cosmicsynth.vst3`.

## Code layout

| File | Contents |
| --- | --- |
| `Source/PluginProcessor.*` | Parameters, waveform analysis, per-preset physics, Kozai-Lidov cycle integration |
| `Source/CosmicSynthEngine.*` | Voices, filter, reverb, orbit pair, Kerr overtones, Kozai-Lidov wavetables, lens |
| `Source/CosmicSynthOscillator.*` | Wavetable oscillator with pitch smoothing |
| `Source/PresetManager.h` | Settings for each preset |
| `Source/GWSeries.h` | Waveform loading and the built-in merger waveform |
| `Source/PluginEditor.*`, `GWVisuals.h`, `GWLookAndFeel.h` | Interface, physics displays and spectrogram |
| `Source/Smooth.h` | One-pole smoother |

## Changes since the demo video

After filming, I fixed a bug that made the GW smoothing run hundreds of times too slowly, so the filter, timbre and amplitude modulation on Binary Pad, Inspiral Pluck and Ringdown Bell is now stronger than in the video.

## References

- E. Berti, V. Cardoso & C. M. Will (2006), *On gravitational-wave spectroscopy of massive black holes with the space interferometer LISA*, Phys. Rev. D 73, 064030.
- I. Kamaretsos, M. Hannam & B. S. Sathyaprakash (2012), *Is black-hole ringdown a memory of its progenitor?*, Phys. Rev. Lett. 109, 141102.
- P. C. Peters & J. Mathews (1963), *Gravitational radiation from point masses in a Keplerian orbit*, Phys. Rev. 131, 435.
- S. Naoz (2016), *The Eccentric Kozai-Lidov Effect and Its Applications*, Annu. Rev. Astron. Astrophys. 54, 441.
- R. Takahashi & T. Nakamura (2003), *Wave effects in the gravitational lensing of gravitational waves from chirping binaries*, ApJ 595, 1039.

---

Kael Barba
