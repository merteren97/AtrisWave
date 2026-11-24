using AtrisWave.Core.Dsp;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class EqCurveCalculatorTests
{
    [Fact]
    public void EvaluateMagnitudeDb_ReturnsFlatZero_WhenEqDisabled()
    {
        var settings = new MicDspSettings
        {
            EqEnabled = false,
            EqMid1Db = 6.0f,
            EqLowDb = -6.0f,
        };

        Assert.Equal(0.0, EqCurveCalculator.EvaluateMagnitudeDb(settings, 100.0), precision: 6);
        Assert.Equal(0.0, EqCurveCalculator.EvaluateMagnitudeDb(settings, 1000.0), precision: 6);
        Assert.Equal(0.0, EqCurveCalculator.EvaluateMagnitudeDb(settings, 10000.0), precision: 6);
    }

    [Fact]
    public void EvaluateMagnitudeDb_ReflectsPlus6DbPeakAtMid1()
    {
        var settings = new MicDspSettings
        {
            EqEnabled = true,
            EqHpfHz = 20.0f,
            EqMid1Hz = 1000.0f,
            EqMid1Db = 6.0f,
        };

        double magAt1k = EqCurveCalculator.EvaluateMagnitudeDb(settings, 1000.0);
        Assert.InRange(magAt1k, 5.8, 6.2);
    }

    [Fact]
    public void EvaluateMagnitudeDb_AttenuatesBelowHpfCutoff()
    {
        var settings = new MicDspSettings
        {
            EqEnabled = true,
            EqHpfHz = 120.0f,
        };

        double magAt30Hz = EqCurveCalculator.EvaluateMagnitudeDb(settings, 30.0);
        double magAt1kHz = EqCurveCalculator.EvaluateMagnitudeDb(settings, 1000.0);

        Assert.True(magAt30Hz < -18.0, $"Expected >18 dB attenuation at 30 Hz for 120 Hz HPF, got {magAt30Hz:F2} dB");
        Assert.InRange(magAt1kHz, -0.5, 0.5);
    }
}
