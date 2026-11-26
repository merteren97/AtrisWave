using System.Diagnostics;
using System.Windows;
using System.Windows.Media;
using SkiaSharp;
using SkiaSharp.Views.Desktop;
using SkiaSharp.Views.WPF;

namespace AtrisWave.UI.Controls;

/// <summary>
/// 60 FPS SkiaSharp stereo VU meter control (<see cref="SKElement"/>, never SKGLControl).
/// Pre-allocates all <see cref="SKPaint"/> instances and auto-unsubscribes when unloaded, hidden, or minimized.
/// </summary>
public sealed class VuMeterControl : SKElement
{
    private const double TargetFrameIntervalMs = 1000.0 / 60.0;
    private const double PeakHoldDurationMs = 750.0;
    private const float PeakHoldDecayPerSec = 0.45f;
    private const float BallisticDecayPerSec = 1.8f;
    private const float MinDb = -60.0f;

    public static readonly DependencyProperty PeakLeftProperty = DependencyProperty.Register(
        nameof(PeakLeft), typeof(float), typeof(VuMeterControl), new PropertyMetadata(0.0f, OnMeterInputChanged));

    public static readonly DependencyProperty PeakRightProperty = DependencyProperty.Register(
        nameof(PeakRight), typeof(float), typeof(VuMeterControl), new PropertyMetadata(0.0f, OnMeterInputChanged));

    public static readonly DependencyProperty RmsLeftProperty = DependencyProperty.Register(
        nameof(RmsLeft), typeof(float), typeof(VuMeterControl), new PropertyMetadata(0.0f, OnMeterInputChanged));

    public static readonly DependencyProperty RmsRightProperty = DependencyProperty.Register(
        nameof(RmsRight), typeof(float), typeof(VuMeterControl), new PropertyMetadata(0.0f, OnMeterInputChanged));

    public static readonly DependencyProperty IsMutedProperty = DependencyProperty.Register(
        nameof(IsMuted), typeof(bool), typeof(VuMeterControl), new PropertyMetadata(false, OnMeterInputChanged));

    public static readonly DependencyProperty IsClippingProperty = DependencyProperty.Register(
        nameof(IsClipping), typeof(bool), typeof(VuMeterControl), new PropertyMetadata(false, OnMeterInputChanged));

    public float PeakLeft
    {
        get => (float)GetValue(PeakLeftProperty);
        set => SetValue(PeakLeftProperty, value);
    }

    public float PeakRight
    {
        get => (float)GetValue(PeakRightProperty);
        set => SetValue(PeakRightProperty, value);
    }

    public float RmsLeft
    {
        get => (float)GetValue(RmsLeftProperty);
        set => SetValue(RmsLeftProperty, value);
    }

    public float RmsRight
    {
        get => (float)GetValue(RmsRightProperty);
        set => SetValue(RmsRightProperty, value);
    }

    public bool IsMuted
    {
        get => (bool)GetValue(IsMutedProperty);
        set => SetValue(IsMutedProperty, value);
    }

    public bool IsClipping
    {
        get => (bool)GetValue(IsClippingProperty);
        set => SetValue(IsClippingProperty, value);
    }

    private readonly Stopwatch _frameStopwatch = new();
    private readonly SKPaint _bgPaint = new() { Color = new SKColor(0x16, 0x18, 0x1E), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _trackPaint = new() { Color = new SKColor(0x22, 0x26, 0x30), IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _rmsPaint = new() { IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _rmsMutedPaint = new() { Color = new SKColor(0x55, 0x5E, 0x70, 0xB0), IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _peakPaint = new() { IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _peakHoldPaint = new() { Color = new SKColor(0xF8, 0xFA, 0xFC), IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _clipOffPaint = new() { Color = new SKColor(0x2C, 0x20, 0x24), IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _clipOnPaint = new() { Color = new SKColor(0xEF, 0x44, 0x44), IsAntialias = false, Style = SKPaintStyle.Fill };
    private readonly SKPaint _gridPaint = new() { Color = new SKColor(0x36, 0x3C, 0x4A, 0x80), IsAntialias = false, StrokeWidth = 1f };

    private SKShader? _rmsShader;
    private SKShader? _peakShader;
    private int _cachedShaderTop;
    private int _cachedShaderBottom;

    private Window? _hostWindow;
    private bool _isRenderingSubscribed;
    private long _lastFrameTicks;
    private float _dispRmsL;
    private float _dispRmsR;
    private float _dispPeakL;
    private float _dispPeakR;
    private float _holdPeakL;
    private float _holdPeakR;
    private double _holdTimerLMs;
    private double _holdTimerRMs;

    public VuMeterControl()
    {
        IgnorePixelScaling = false;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        IsVisibleChanged += OnVisibilityChanged;
    }

    private bool _isSettledAtZero = true;

    private static void OnMeterInputChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VuMeterControl ctrl)
        {
            ctrl._isSettledAtZero = false;
            if (ctrl._isRenderingSubscribed)
            {
                return;
            }
            ctrl.UpdateRenderingSubscription();
            ctrl.InvalidateVisual();
        }
    }

    private bool HasActiveInput()
        => PeakLeft > 0.0005f || PeakRight > 0.0005f || RmsLeft > 0.0005f || RmsRight > 0.0005f || IsClipping;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        AttachHostWindow();
        UpdateRenderingSubscription();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        UnsubscribeRendering();
        DetachHostWindow();
        DisposeShaders();
    }

    private void OnVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        UpdateRenderingSubscription();
    }

    private void AttachHostWindow()
    {
        DetachHostWindow();
        _hostWindow = Window.GetWindow(this);
        if (_hostWindow is not null)
        {
            _hostWindow.StateChanged += OnHostWindowStateOrVisibilityChanged;
            _hostWindow.IsVisibleChanged += OnHostWindowVisibilityChanged;
        }
    }

    private void DetachHostWindow()
    {
        if (_hostWindow is not null)
        {
            _hostWindow.StateChanged -= OnHostWindowStateOrVisibilityChanged;
            _hostWindow.IsVisibleChanged -= OnHostWindowVisibilityChanged;
            _hostWindow = null;
        }
    }

    private void OnHostWindowStateOrVisibilityChanged(object? sender, EventArgs e)
    {
        UpdateRenderingSubscription();
    }

    private void OnHostWindowVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        UpdateRenderingSubscription();
    }

    private void UpdateRenderingSubscription()
    {
        bool windowVisible = _hostWindow is null || (_hostWindow.IsVisible && _hostWindow.WindowState != WindowState.Minimized);
        bool shouldRender = IsLoaded && IsVisible && windowVisible && (!_isSettledAtZero || HasActiveInput());

        if (shouldRender && !_isRenderingSubscribed)
        {
            _frameStopwatch.Restart();
            _lastFrameTicks = _frameStopwatch.ElapsedTicks;
            CompositionTarget.Rendering += OnCompositionTargetRendering;
            _isRenderingSubscribed = true;
        }
        else if (!shouldRender && _isRenderingSubscribed)
        {
            UnsubscribeRendering();
        }
    }

    private void UnsubscribeRendering()
    {
        if (_isRenderingSubscribed)
        {
            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _frameStopwatch.Stop();
            _isRenderingSubscribed = false;
        }
    }

    private void OnCompositionTargetRendering(object? sender, EventArgs e)
    {
        long nowTicks = _frameStopwatch.ElapsedTicks;
        double elapsedMs = (nowTicks - _lastFrameTicks) * 1000.0 / Stopwatch.Frequency;

        if (elapsedMs < TargetFrameIntervalMs)
        {
            return;
        }

        _lastFrameTicks = nowTicks;
        float dtSec = (float)Math.Clamp(elapsedMs / 1000.0, 0.001, 0.1);

        UpdateBallistics(dtSec, elapsedMs);
        InvalidateVisual();

        if (!HasActiveInput() &&
            _dispRmsL < 0.0005f && _dispRmsR < 0.0005f &&
            _dispPeakL < 0.0005f && _dispPeakR < 0.0005f &&
            _holdPeakL < 0.005f && _holdPeakR < 0.005f)
        {
            _dispRmsL = _dispRmsR = _dispPeakL = _dispPeakR = _holdPeakL = _holdPeakR = 0f;
            _isSettledAtZero = true;
            UnsubscribeRendering();
        }
    }

    private void UpdateBallistics(float dtSec, double elapsedMs)
    {
        float targetRmsL = LinearToNormalized(RmsLeft);
        float targetRmsR = LinearToNormalized(RmsRight);
        float targetPeakL = LinearToNormalized(PeakLeft);
        float targetPeakR = LinearToNormalized(PeakRight);

        float decay = BallisticDecayPerSec * dtSec;
        _dispRmsL = targetRmsL >= _dispRmsL ? targetRmsL : Math.Max(targetRmsL, _dispRmsL - decay);
        _dispRmsR = targetRmsR >= _dispRmsR ? targetRmsR : Math.Max(targetRmsR, _dispRmsR - decay);
        _dispPeakL = targetPeakL >= _dispPeakL ? targetPeakL : Math.Max(targetPeakL, _dispPeakL - decay);
        _dispPeakR = targetPeakR >= _dispPeakR ? targetPeakR : Math.Max(targetPeakR, _dispPeakR - decay);

        UpdatePeakHold(targetPeakL, ref _holdPeakL, ref _holdTimerLMs, dtSec, elapsedMs);
        UpdatePeakHold(targetPeakR, ref _holdPeakR, ref _holdTimerRMs, dtSec, elapsedMs);
    }

    private static void UpdatePeakHold(float currentPeak, ref float holdPeak, ref double holdTimerMs, float dtSec, double elapsedMs)
    {
        if (currentPeak >= holdPeak)
        {
            holdPeak = currentPeak;
            holdTimerMs = PeakHoldDurationMs;
        }
        else if (holdTimerMs > 0)
        {
            holdTimerMs -= elapsedMs;
        }
        else
        {
            holdPeak = Math.Max(currentPeak, holdPeak - (PeakHoldDecayPerSec * dtSec));
        }
    }

    private static float LinearToNormalized(float linear)
    {
        if (linear <= 0.0001f)
        {
            return 0.0f;
        }

        float db = 20.0f * MathF.Log10(linear);
        return Math.Clamp((db - MinDb) / -MinDb, 0.0f, 1.0f);
    }

    protected override void OnPaintSurface(SKPaintSurfaceEventArgs e)
    {
        base.OnPaintSurface(e);

        SKCanvas canvas = e.Surface.Canvas;
        int width = e.Info.Width;
        int height = e.Info.Height;
        canvas.Clear(SKColors.Transparent);

        if (width < 8 || height < 16)
        {
            return;
        }

        float dpiScale = ActualWidth > 0 ? width / (float)ActualWidth : 1.0f;
        float pad = MathF.Round(3f * dpiScale);
        float gap = MathF.Round(3f * dpiScale);
        float clipHeight = MathF.Round(6f * dpiScale);
        float cornerRadius = MathF.Round(4f * dpiScale);

        canvas.DrawRoundRect(0, 0, width, height, cornerRadius, cornerRadius, _bgPaint);

        float meterTop = pad + clipHeight + pad;
        float meterBottom = height - pad;
        float meterHeight = Math.Max(4f, meterBottom - meterTop);
        float barWidth = Math.Max(2f, (width - (pad * 2f) - gap) / 2f);
        float leftX = pad;
        float rightX = pad + barWidth + gap;

        EnsureShaders((int)meterTop, (int)meterBottom);

        // Clipping LEDs at top of L/R bars
        SKPaint clipPaint = (IsClipping || PeakLeft >= 0.999f || PeakRight >= 0.999f) && !IsMuted
            ? _clipOnPaint
            : _clipOffPaint;
        canvas.DrawRect(leftX, pad, barWidth, clipHeight, clipPaint);
        canvas.DrawRect(rightX, pad, barWidth, clipHeight, clipPaint);

        // Meter tracks
        canvas.DrawRect(leftX, meterTop, barWidth, meterHeight, _trackPaint);
        canvas.DrawRect(rightX, meterTop, barWidth, meterHeight, _trackPaint);

        // Draw L and R bars
        DrawChannelBar(canvas, leftX, barWidth, meterTop, meterHeight, _dispPeakL, _dispRmsL, _holdPeakL, dpiScale);
        DrawChannelBar(canvas, rightX, barWidth, meterTop, meterHeight, _dispPeakR, _dispRmsR, _holdPeakR, dpiScale);

        // Subtle dB grid ticks (-6, -12, -24, -48 dB)
        _gridPaint.StrokeWidth = MathF.Max(1f, dpiScale);
        Span<float> tickDbs = stackalloc float[] { -6f, -12f, -24f, -48f };
        foreach (float db in tickDbs)
        {
            float norm = (db - MinDb) / -MinDb;
            float y = meterBottom - (norm * meterHeight);
            canvas.DrawLine(leftX, y, rightX + barWidth, y, _gridPaint);
        }
    }

    private void DrawChannelBar(
        SKCanvas canvas,
        float x,
        float width,
        float top,
        float height,
        float peakNorm,
        float rmsNorm,
        float holdNorm,
        float dpiScale)
    {
        float bottom = top + height;

        if (peakNorm > 0.001f)
        {
            float peakH = peakNorm * height;
            canvas.DrawRect(x, bottom - peakH, width, peakH, IsMuted ? _rmsMutedPaint : _peakPaint);
        }

        if (rmsNorm > 0.001f)
        {
            float rmsH = rmsNorm * height;
            canvas.DrawRect(x, bottom - rmsH, width, rmsH, IsMuted ? _rmsMutedPaint : _rmsPaint);
        }

        if (holdNorm > 0.01f)
        {
            float tickH = MathF.Max(2f, 2f * dpiScale);
            float tickY = Math.Clamp(bottom - (holdNorm * height), top, bottom - tickH);
            canvas.DrawRect(x, tickY, width, tickH, _peakHoldPaint);
        }
    }

    private void EnsureShaders(int top, int bottom)
    {
        if (_rmsShader is not null && _cachedShaderTop == top && _cachedShaderBottom == bottom)
        {
            return;
        }

        DisposeShaders();

        SKColor[] colors =
        {
            new(0xEF, 0x44, 0x44), // Red (0 dB .. -3 dB)
            new(0xF5, 0x9E, 0x0B), // Amber (-6 dB)
            new(0x10, 0xB9, 0x81), // Emerald (-18 dB)
            new(0x06, 0xB6, 0xD4), // Cyan (-60 dB)
        };
        float[] positions = { 0.0f, 0.14f, 0.45f, 1.0f };

        _rmsShader = SKShader.CreateLinearGradient(
            new SKPoint(0, top),
            new SKPoint(0, bottom),
            colors,
            positions,
            SKShaderTileMode.Clamp);

        SKColor[] peakColors =
        {
            new(0xEF, 0x44, 0x44, 0x90),
            new(0xF5, 0x9E, 0x0B, 0x80),
            new(0x10, 0xB9, 0x81, 0x70),
            new(0x06, 0xB6, 0xD4, 0x60),
        };
        _peakShader = SKShader.CreateLinearGradient(
            new SKPoint(0, top),
            new SKPoint(0, bottom),
            peakColors,
            positions,
            SKShaderTileMode.Clamp);

        _rmsPaint.Shader = _rmsShader;
        _peakPaint.Shader = _peakShader;
        _cachedShaderTop = top;
        _cachedShaderBottom = bottom;
    }

    private void DisposeShaders()
    {
        _rmsPaint.Shader = null;
        _peakPaint.Shader = null;
        _rmsShader?.Dispose();
        _rmsShader = null;
        _peakShader?.Dispose();
        _peakShader = null;
    }
}
