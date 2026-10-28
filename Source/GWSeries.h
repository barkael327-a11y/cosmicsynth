#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <cstdint>

// One sample of your gravitational-wave time series
struct GWFrame
{
    double t = 0.0;    // time [s]
    double h = 0.0;    // strain (or amplitude-like quantity)
    double fgw = 0.0;  // GW frequency [Hz]
    double psi = 0.0;  // phase [rad] (optional)
};

// Loads GW data from CSV and supports linear interpolation sampling.
// Also caches reference values and a light “activity” profile to pick interesting segments.
class GWSeries
{
public:
    GWSeries() = default;

    // Expected CSV columns:
    // t, h, fgw, psi
    // Extra columns ignored. Header lines starting with # are skipped.
    bool loadFromCSV(const juce::File& file)
    {
        data.clear();
        activity.clear();
        activityCDF.clear();

        t0 = tN = dt = 0.0;
        firstFgwHz = 0.0;
        maxAbsH = 0.0;
        maxFgw = 0.0;

        if (!file.existsAsFile())
            return false;

        juce::FileInputStream in(file);
        if (!in.openedOk())
            return false;

        while (!in.isExhausted())
        {
            auto line = in.readNextLine().trim();
            if (line.isEmpty() || line.startsWithChar('#'))
                continue;

            const auto first = line[0];
            if (!(juce::CharacterFunctions::isDigit(first) || first == '-' || first == '.'))
                continue;

            juce::StringArray tokens;
            tokens.addTokens(line, ",", "\"");
            tokens.trim();
            tokens.removeEmptyStrings();

            if (tokens.size() < 2)
                continue;

            const double t = tokens[0].getDoubleValue();
            const double h = tokens[1].getDoubleValue();
            const double fgw = (tokens.size() > 2) ? tokens[2].getDoubleValue() : 0.0;
            const double psi = (tokens.size() > 3) ? tokens[3].getDoubleValue() : 0.0;

            data.push_back({ t, h, fgw, psi });

            if (firstFgwHz <= 0.0 && fgw > 0.0)
                firstFgwHz = fgw;

            const double ah = std::abs(h);
            if (ah > maxAbsH)
                maxAbsH = ah;

            if (fgw > maxFgw)
                maxFgw = fgw;
        }

        return finalise();
    }

    // Load frames directly (e.g. the built-in waveform). Same post-processing
    // as a CSV load.
    bool loadFromFrames(std::vector<GWFrame> frames)
    {
        data = std::move(frames);
        activity.clear();
        activityCDF.clear();
        t0 = tN = dt = 0.0;
        firstFgwHz = 0.0;
        maxAbsH = 0.0;
        maxFgw = 0.0;
        for (const auto& f : data)
        {
            if (firstFgwHz <= 0.0 && f.fgw > 0.0) firstFgwHz = f.fgw;
            maxAbsH = juce::jmax(maxAbsH, std::abs(f.h));
            maxFgw = juce::jmax(maxFgw, f.fgw);
        }
        return finalise();
    }

    // Built-in inspiral-merger-ringdown waveform — the same model and numbers
    // as make_gw_csv.py / gw_imr_full.csv (11.5 + 11.5 Msun, from 20 Hz,
    // remnant spin 0.69), so the synth works with no file loaded.
    static std::vector<GWFrame> makeBuiltInIMR()
    {
        const double m1 = 11.5, m2 = 11.5, fStart = 20.0, fs = 4096.0;
        const double remnantFrac = 0.952, spin = 0.69;
        const double TSUN = 4.925491e-6, pi = juce::MathConstants<double>::pi;
        const double M = m1 + m2;
        const double Mc_s = std::pow(m1 * m2, 0.6) / std::pow(M, 0.2) * TSUN;

        const double Mf_s = remnantFrac * M * TSUN;
        const double omegaQnm = (1.5251 - 1.1568 * std::pow(1.0 - spin, 0.1292)) / Mf_s;
        const double Q = 0.7000 + 1.4187 * std::pow(1.0 - spin, -0.4990);
        const double fQnm = omegaQnm / (2.0 * pi);
        const double tauQnm = 2.0 * Q / omegaQnm;

        auto tauOfF = [&](double f) { return (5.0 / 256.0) * std::pow(Mc_s, -5.0 / 3.0) * std::pow(pi * f, -8.0 / 3.0); };
        auto fOfTau = [&](double tau) { return (1.0 / pi) * std::pow(5.0 / (256.0 * tau), 0.375) * std::pow(Mc_s, -0.625); };

        const double Tinsp = tauOfF(fStart);
        const double fIsco = 1.0 / (std::pow(6.0, 1.5) * pi * M * TSUN);
        const double tPeak = Tinsp - tauOfF(fIsco);
        const double Ttotal = tPeak + 12.0 * tauQnm;
        const int N = (int)std::ceil(Ttotal * fs);
        const double w = 0.25 * tauQnm;

        std::vector<double> fgw((size_t)N), amp((size_t)N);
        for (int i = 0; i < N; ++i)
        {
            const double t = i / fs;
            const double fIn = juce::jmin(fOfTau(juce::jmax(Tinsp - t, 1e-6)), fIsco * 1.15);
            const double blend = 0.5 * (1.0 + std::tanh((t - tPeak) / w));
            fgw[(size_t)i] = (1.0 - blend) * fIn + blend * fQnm;
            amp[(size_t)i] = (t < tPeak) ? std::pow(fIn / fIsco, 2.0 / 3.0)
                : std::exp(-juce::jmax(t - tPeak, 0.0) / tauQnm);
        }
        // 1 ms box smoothing at the inspiral/ringdown junction (as in the script)
        const int k = ((int)(0.001 * fs)) | 1, hk = k / 2;
        std::vector<double> ampS((size_t)N);
        for (int i = 0; i < N; ++i)
        {
            double acc = 0.0;
            for (int j = i - hk; j <= i + hk; ++j) if (j >= 0 && j < N) acc += amp[(size_t)j];
            ampS[(size_t)i] = acc / k;
        }
        std::vector<GWFrame> out((size_t)N);
        double psi = 0.0, peak = 0.0;
        for (int i = 0; i < N; ++i)
        {
            psi += 2.0 * pi * fgw[(size_t)i] / fs;
            const double ps = psi - 2.0 * pi * fgw[0] / fs;   // psi[0] = 0
            out[(size_t)i] = { i / fs, ampS[(size_t)i] * std::sin(ps), fgw[(size_t)i], ps };
            peak = juce::jmax(peak, std::abs(out[(size_t)i].h));
        }
        if (peak > 0.0) for (auto& f : out) f.h /= peak;
        return out;
    }

private:
    bool finalise()
    {
        if (data.empty())
            return false;

        t0 = data.front().t;
        tN = data.back().t;

        // NOTE: assumes roughly uniform sampling; OK for your current CSVs.
        dt = (data.size() > 1) ? (tN - t0) / (double)(data.size() - 1) : 1.0;

        if (firstFgwHz <= 0.0)
            firstFgwHz = data.front().fgw;

        if (maxAbsH <= 0.0)
            maxAbsH = 1.0;

        if (maxFgw <= 0.0)
            maxFgw = 1.0;

        buildActivityProfile();
        return true;
    }

public:
    size_t size() const noexcept { return data.size(); }
    const GWFrame& frame(size_t i) const noexcept { return data[i]; }

    // Linear interpolation between frames (fast path assumes uniform dt).
    // If t outside [t0, tN], returns first/last (clamped).
    GWFrame sampleAt(double t) const
    {
        GWFrame out;
        if (data.empty())
            return out;

        if (t <= t0) return data.front();
        if (t >= tN) return data.back();
        if (dt <= 0.0) return data.front();

        const double alpha = (t - t0) / dt;
        int i0 = (int)std::floor(alpha);
        int i1 = i0 + 1;

        if (i0 < 0) i0 = 0;
        if (i1 >= (int)data.size()) i1 = (int)data.size() - 1;

        const double w = alpha - (double)i0;

        const auto& a = data[(size_t)i0];
        const auto& b = data[(size_t)i1];

        out.t = t;
        out.h = (1.0 - w) * a.h + w * b.h;
        out.fgw = (1.0 - w) * a.fgw + w * b.fgw;
        out.psi = (1.0 - w) * a.psi + w * b.psi;

        return out;
    }

    bool   empty() const noexcept { return data.empty(); }
    double getT0() const noexcept { return t0; }
    double getTN() const noexcept { return tN; }

    double getFirstFgwHz() const noexcept { return firstFgwHz; }

    double getRefFgwHz(double fallbackHz = 30.0) const noexcept
    {
        if (firstFgwHz > 0.0)
            return firstFgwHz;
        return (fallbackHz > 0.0) ? fallbackHz : 30.0;
    }

    double getMaxAbsH() const noexcept { return maxAbsH; }
    double getMaxFgw()  const noexcept { return maxFgw; }

    double normalizeAbsH(double hVal) const noexcept
    {
        const double denom = (maxAbsH > 0.0) ? maxAbsH : 1.0;
        const double x = std::abs(hVal) / denom;
        return juce::jlimit(0.0, 1.0, x);
    }

    double normalizeFgw(double fgwVal) const noexcept
    {
        const double denom = (maxFgw > 0.0) ? maxFgw : 1.0;
        const double x = fgwVal / denom;
        return juce::jlimit(0.0, 1.0, x);
    }

    // ----------------------------
    // Smart segment picking
    // ----------------------------
    // Pick a start time for an “event” segment of duration spanSec.
    // This biases toward high activity (high |h| and/or higher fgw).
    //
    // rng01 must be in [0,1). The caller supplies randomness (audio-thread safe).
    // biasStrength: 0 = uniform, 1 = moderate bias, 2 = strong bias.
    //
    // Returns a clamped start time tStart such that tStart + spanSec <= tN.
    double pickEventStartTime(double spanSec, double rng01, double biasStrength = 1.5) const noexcept
    {
        if (data.empty())
            return 0.0;

        const double dur = juce::jmax(1e-9, tN - t0);
        const double span = juce::jlimit(0.0, dur, spanSec);

        const double latestStart = tN - span;
        if (latestStart <= t0)
            return t0;

        // If we have no activity profile, fall back to uniform
        if (activityCDF.empty() || activityCDF.size() != data.size())
        {
            return t0 + (latestStart - t0) * juce::jlimit(0.0, 1.0, rng01);
        }

        // Convert rng01 to a “biased” quantile: smaller rng values get pushed toward high-activity mass.
        // We do: q = 1 - (1 - rng)^(biasStrength)
        const double u = juce::jlimit(0.0, 0.999999999, rng01);
        const double q = 1.0 - std::pow(1.0 - u, juce::jlimit(0.0, 6.0, biasStrength));

        // Invert CDF via binary search
        const double target = q * activityCDF.back();

        size_t lo = 0, hi = activityCDF.size() - 1;
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (activityCDF[mid] < target)
                lo = mid + 1;
            else
                hi = mid;
        }

        const double tPick = data[lo].t;

        // Clamp to keep segment inside bounds
        return juce::jlimit(t0, latestStart, tPick);
    }

private:
    void buildActivityProfile()
    {
        activity.resize(data.size(), 0.0);
        activityCDF.resize(data.size(), 0.0);

        // Weighting: emphasize |h| (event-ness) but include fgw (chirp-ness)
        constexpr double wH = 0.70;
        constexpr double wF = 0.30;

        double sum = 0.0;
        for (size_t i = 0; i < data.size(); ++i)
        {
            const double hN = normalizeAbsH(data[i].h);
            const double fN = normalizeFgw(data[i].fgw);

            // perceptual lift so quiet sections still get some mass but peaks dominate
            const double a = std::pow(hN, 0.55);
            const double b = std::pow(fN, 0.85);

            const double s = juce::jlimit(0.0, 1.0, wH * a + wF * b);

            // small floor so CDF is strictly increasing (prevents dead zones)
            const double floor = 1.0e-6;
            activity[i] = floor + s;

            sum += activity[i];
            activityCDF[i] = sum;
        }
    }

    std::vector<GWFrame> data;
    std::vector<double> activity;
    std::vector<double> activityCDF;

    double t0 = 0.0;
    double tN = 0.0;
    double dt = 0.0;

    double firstFgwHz = 0.0;
    double maxAbsH = 0.0;
    double maxFgw = 0.0;
};