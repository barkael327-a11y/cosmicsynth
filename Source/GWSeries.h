#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cmath>

// One sample of your gravitational-wave time series
struct GWFrame
{
    double t = 0.0;  // time [s]
    double h = 0.0;  // strain (or amplitude-like quantity)
    double fgw = 0.0;  // GW frequency [Hz]
    double psi = 0.0;  // phase [rad] (optional)
};

// Loads GW data from CSV and supports linear interpolation sampling.
// Also caches "reference" values so your modulation mappings are stable & audible.
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
        t0 = tN = dt = 0.0;
        firstFgwHz = 0.0;
        maxAbsH = 0.0;

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

            // naive header detection: skip line if first char is non-digit and not - or .
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

            // cache reference frequency (first positive fgw encountered)
            if (firstFgwHz <= 0.0 && fgw > 0.0)
                firstFgwHz = fgw;

            // cache amplitude scale so |h| can be normalized to 0..1
            const double ah = std::abs(h);
            if (ah > maxAbsH)
                maxAbsH = ah;
        }

        if (data.empty())
            return false;

        t0 = data.front().t;
        tN = data.back().t;

        // NOTE: assumes roughly uniform sampling; OK for "first working" CSVs.
        dt = (data.size() > 1) ? (tN - t0) / (double)(data.size() - 1) : 1.0;

        // If CSV had no positive fgw, fall back to first row's fgw (even if 0)
        if (firstFgwHz <= 0.0)
            firstFgwHz = data.front().fgw;

        // Ensure nonzero maxAbsH to avoid division by zero in normalization
        if (maxAbsH <= 0.0)
            maxAbsH = 1.0;

        return true;
    }

    // Linear interpolation between frames.
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

    // Cached "neutral" frequency for pitch mapping
    double getFirstFgwHz() const noexcept { return firstFgwHz; }

    // Guaranteed-positive reference frequency
    double getRefFgwHz(double fallbackHz = 30.0) const noexcept
    {
        if (firstFgwHz > 0.0)
            return firstFgwHz;
        return (fallbackHz > 0.0) ? fallbackHz : 30.0;
    }

    // Cached amplitude scale (max |h| in file) for audible normalization
    double getMaxAbsH() const noexcept { return maxAbsH; }

    // Normalize a given h value into ~[0,1] using the file’s maxAbsH
    double normalizeAbsH(double hVal) const noexcept
    {
        const double denom = (maxAbsH > 0.0) ? maxAbsH : 1.0;
        const double x = std::abs(hVal) / denom;
        return juce::jlimit(0.0, 1.0, x);
    }

private:
    std::vector<GWFrame> data;

    double t0 = 0.0;
    double tN = 0.0;
    double dt = 0.0;

    // cached "neutral" frequency from CSV
    double firstFgwHz = 0.0;

    // cached max |h| for normalization
    double maxAbsH = 0.0;
};
