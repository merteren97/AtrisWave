using System.Numerics;
using AtrisWave.Core.Models;

namespace AtrisWave.Core.Dsp;

/// <summary>
/// Analytical 5-band Simper SVF magnitude-in-dB calculator matching <c>ParametricEq5Band</c> in C++.
/// </summary>
public static class EqCurveCalculator
{
    private const double DefaultQ = 0.7071067811865475;
    private const double PeakQ = 1.2;
    private const double LowShelfFreqHz = 120.0;
    private const double HighShelfFreqHz = 8000.0;

    /// <summary>
    /// Computes the combined magnitude response (in dB) at <paramref name="freqHz"/>.
    /// </summary>
    public static double EvaluateMagnitudeDb(MicDspSettings settings, double freqHz, double sampleRate = 48000.0)
    {
        ArgumentNullException.ThrowIfNull(settings);

        if (!settings.EqEnabled || freqHz <= 0.0 || sampleRate <= 0.0)
        {
            return 0.0;
        }

        double totalDb = 0.0;
        totalDb += HighPassDb(freqHz, Math.Clamp(settings.EqHpfHz, 20.0, 500.0), DefaultQ, sampleRate);
        totalDb += LowShelfDb(freqHz, LowShelfFreqHz, settings.EqLowDb, DefaultQ, sampleRate);
        totalDb += PeakDb(freqHz, Math.Clamp(settings.EqMid1Hz, 100.0, 4000.0), settings.EqMid1Db, PeakQ, sampleRate);
        totalDb += PeakDb(freqHz, Math.Clamp(settings.EqMid2Hz, 500.0, 12000.0), settings.EqMid2Db, PeakQ, sampleRate);
        totalDb += HighShelfDb(freqHz, HighShelfFreqHz, settings.EqHighDb, DefaultQ, sampleRate);
        return totalDb;
    }

    private static double HighPassDb(double freqHz, double cutoffHz, double q, double sampleRate)
    {
        double w0 = 2.0 * Math.PI * cutoffHz / sampleRate;
        double cosW0 = Math.Cos(w0);
        double sinW0 = Math.Sin(w0);
        double alpha = sinW0 / (2.0 * q);

        double b0 = (1.0 + cosW0) * 0.5;
        double b1 = -(1.0 + cosW0);
        double b2 = (1.0 + cosW0) * 0.5;
        double a0 = 1.0 + alpha;
        double a1 = -2.0 * cosW0;
        double a2 = 1.0 - alpha;

        return BiquadMagnitudeDb(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, freqHz, sampleRate);
    }

    private static double PeakDb(double freqHz, double centerHz, double gainDb, double q, double sampleRate)
    {
        if (Math.Abs(gainDb) < 0.001)
        {
            return 0.0;
        }

        double a = Math.Pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * Math.PI * centerHz / sampleRate;
        double cosW0 = Math.Cos(w0);
        double sinW0 = Math.Sin(w0);
        double alpha = sinW0 / (2.0 * q);

        double b0 = 1.0 + (alpha * a);
        double b1 = -2.0 * cosW0;
        double b2 = 1.0 - (alpha * a);
        double a0 = 1.0 + (alpha / a);
        double a1 = -2.0 * cosW0;
        double a2 = 1.0 - (alpha / a);

        return BiquadMagnitudeDb(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, freqHz, sampleRate);
    }

    private static double LowShelfDb(double freqHz, double centerHz, double gainDb, double q, double sampleRate)
    {
        if (Math.Abs(gainDb) < 0.001)
        {
            return 0.0;
        }

        double a = Math.Pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * Math.PI * centerHz / sampleRate;
        double cosW0 = Math.Cos(w0);
        double sinW0 = Math.Sin(w0);
        double alpha = sinW0 / (2.0 * q);
        double twoSqrtAAlpha = 2.0 * Math.Sqrt(a) * alpha;

        double b0 = a * ((a + 1.0) - ((a - 1.0) * cosW0) + twoSqrtAAlpha);
        double b1 = 2.0 * a * ((a - 1.0) - ((a + 1.0) * cosW0));
        double b2 = a * ((a + 1.0) - ((a - 1.0) * cosW0) - twoSqrtAAlpha);
        double a0 = (a + 1.0) + ((a - 1.0) * cosW0) + twoSqrtAAlpha;
        double a1 = -2.0 * ((a - 1.0) + ((a + 1.0) * cosW0));
        double a2 = (a + 1.0) + ((a - 1.0) * cosW0) - twoSqrtAAlpha;

        return BiquadMagnitudeDb(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, freqHz, sampleRate);
    }

    private static double HighShelfDb(double freqHz, double centerHz, double gainDb, double q, double sampleRate)
    {
        if (Math.Abs(gainDb) < 0.001)
        {
            return 0.0;
        }

        double a = Math.Pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * Math.PI * centerHz / sampleRate;
        double cosW0 = Math.Cos(w0);
        double sinW0 = Math.Sin(w0);
        double alpha = sinW0 / (2.0 * q);
        double twoSqrtAAlpha = 2.0 * Math.Sqrt(a) * alpha;

        double b0 = a * ((a + 1.0) + ((a - 1.0) * cosW0) + twoSqrtAAlpha);
        double b1 = -2.0 * a * ((a - 1.0) + ((a + 1.0) * cosW0));
        double b2 = a * ((a + 1.0) + ((a - 1.0) * cosW0) - twoSqrtAAlpha);
        double a0 = (a + 1.0) - ((a - 1.0) * cosW0) + twoSqrtAAlpha;
        double a1 = 2.0 * ((a - 1.0) - ((a + 1.0) * cosW0));
        double a2 = (a + 1.0) - ((a - 1.0) * cosW0) - twoSqrtAAlpha;

        return BiquadMagnitudeDb(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, freqHz, sampleRate);
    }

    private static double BiquadMagnitudeDb(double b0, double b1, double b2, double a1, double a2, double freqHz, double sampleRate)
    {
        double w = 2.0 * Math.PI * freqHz / sampleRate;
        var z1 = Complex.FromPolarCoordinates(1.0, -w);
        var z2 = Complex.FromPolarCoordinates(1.0, -2.0 * w);

        Complex num = b0 + (b1 * z1) + (b2 * z2);
        Complex den = 1.0 + (a1 * z1) + (a2 * z2);
        double mag = Math.Max(1e-6, Complex.Abs(num / den));
        return 20.0 * Math.Log10(mag);
    }
}
