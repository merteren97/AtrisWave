using System.ComponentModel;
using System.Windows;
using System.Windows.Input;
using AtrisWave.Core.Dsp;
using AtrisWave.Core.Models;
using SkiaSharp;
using SkiaSharp.Views.Desktop;
using SkiaSharp.Views.WPF;

namespace AtrisWave.UI.Controls;

/// <summary>
/// Interactive 20 Hz - 20 kHz 5-band Parametric EQ curve visualizer & node editor (<see cref="SKElement"/>).
/// Pre-allocates all <see cref="SKPaint"/> and <see cref="SKPath"/> objects (zero per-frame allocations).
/// </summary>
public sealed class EqCurveControl : SKElement
{
    private const double MinFreqHz = 20.0;
    private const double MaxFreqHz = 20000.0;
    private const double MinDb = -18.0;
    private const double MaxDb = 18.0;
    private const int CurveSamples = 160;

    public static readonly DependencyProperty SettingsProperty = DependencyProperty.Register(
        nameof(Settings),
        typeof(MicDspSettings),
        typeof(EqCurveControl),
        new PropertyMetadata(null, OnSettingsChanged));

    public static readonly DependencyProperty SpectrumDbProperty = DependencyProperty.Register(
        nameof(SpectrumDb),
        typeof(float[]),
        typeof(EqCurveControl),
        new PropertyMetadata(null, OnSpectrumChanged));

    public MicDspSettings? Settings
    {
        get => (MicDspSettings?)GetValue(SettingsProperty);
        set => SetValue(SettingsProperty, value);
    }

    public float[]? SpectrumDb
    {
        get => (float[]?)GetValue(SpectrumDbProperty);
        set => SetValue(SpectrumDbProperty, value);
    }

    public event EventHandler? CurveEdited;

    private readonly SKPaint _bgPaint = new() { Color = new SKColor(0x12, 0x16, 0x20), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _gridPaint = new() { Color = new SKColor(0x28, 0x30, 0x42), IsAntialias = false, StrokeWidth = 1f };
    private readonly SKPaint _zeroLinePaint = new() { Color = new SKColor(0x47, 0x55, 0x69), IsAntialias = false, StrokeWidth = 1.5f };
    private readonly SKPaint _curveStrokePaint = new() { Color = new SKColor(0x38, 0xBD, 0xF8), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeWidth = 2.5f };
    private readonly SKPaint _curveFillPaint = new() { Color = new SKColor(0x38, 0xBD, 0xF8, 0x38), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _nodeFillPaint = new() { Color = new SKColor(0x0F, 0x17, 0x2A), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _nodeStrokePaint = new() { Color = new SKColor(0x22, 0xD3, 0xEE), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeWidth = 2f };
    private readonly SKPaint _nodeActivePaint = new() { Color = new SKColor(0x38, 0xBD, 0xF8), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _spectrumFillPaint = new() { Color = new SKColor(0xA8, 0x55, 0xF7, 0x35), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _spectrumStrokePaint = new() { Color = new SKColor(0xA8, 0x55, 0xF7, 0x95), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeWidth = 1.5f };
    private readonly SKPaint _labelPaint = new() { Color = new SKColor(0x94, 0xA3, 0xB8), IsAntialias = true };
    private readonly SKFont _labelFont = new() { Size = 11f };

    private readonly SKPath _strokePath = new();
    private readonly SKPath _fillPath = new();
    private readonly SKPath _spectrumStrokePath = new();
    private readonly SKPath _spectrumFillPath = new();

    private int _draggedNode = -1;
    private float _lastWidth;
    private float _lastHeight;
    private float _dpiScale = 1.0f;

    public EqCurveControl()
    {
        IgnorePixelScaling = false;
        MouseLeftButtonDown += OnMouseLeftButtonDown;
        MouseMove += OnMouseMove;
        MouseLeftButtonUp += OnMouseLeftButtonUp;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (Settings is not null)
        {
            Settings.PropertyChanged -= OnSettingsPropertyChanged;
            Settings.PropertyChanged += OnSettingsPropertyChanged;
            InvalidateVisual();
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        if (Settings is not null)
        {
            Settings.PropertyChanged -= OnSettingsPropertyChanged;
        }
    }

    private static void OnSettingsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not EqCurveControl ctrl)
        {
            return;
        }

        if (e.OldValue is MicDspSettings oldSettings)
        {
            oldSettings.PropertyChanged -= ctrl.OnSettingsPropertyChanged;
        }

        if (e.NewValue is MicDspSettings newSettings)
        {
            newSettings.PropertyChanged += ctrl.OnSettingsPropertyChanged;
        }

        ctrl.InvalidateVisual();
    }

    private static void OnSpectrumChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        => (d as EqCurveControl)?.InvalidateVisual();

    public void RefreshSpectrum() => InvalidateVisual();

    private void OnSettingsPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        InvalidateVisual();
    }

    protected override void OnPaintSurface(SKPaintSurfaceEventArgs e)
    {
        base.OnPaintSurface(e);

        SKCanvas canvas = e.Surface.Canvas;
        int width = e.Info.Width;
        int height = e.Info.Height;
        canvas.Clear(SKColors.Transparent);

        if (width < 40 || height < 40)
        {
            return;
        }

        _lastWidth = width;
        _lastHeight = height;
        _dpiScale = ActualWidth > 0 ? width / (float)ActualWidth : 1.0f;

        float pad = 12f * _dpiScale;
        float plotW = width - (pad * 2f);
        float plotH = height - (pad * 2f);

        canvas.DrawRoundRect(0, 0, width, height, 8f * _dpiScale, 8f * _dpiScale, _bgPaint);

        DrawGrid(canvas, pad, plotW, plotH);

        if (SpectrumDb is { Length: > 1 } spectrum)
        {
            DrawSpectrum(canvas, spectrum, pad, plotW, plotH);
        }

        MicDspSettings? settings = Settings;
        if (settings is null)
        {
            return;
        }

        DrawCurve(canvas, settings, pad, plotW, plotH);
        DrawNodes(canvas, settings, pad, plotW, plotH);
    }

    private void DrawGrid(SKCanvas canvas, float pad, float plotW, float plotH)
    {
        _gridPaint.StrokeWidth = MathF.Max(1f, _dpiScale);
        _zeroLinePaint.StrokeWidth = MathF.Max(1.2f, 1.2f * _dpiScale);
        _labelFont.Size = 10f * _dpiScale;

        double[] freqLines = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
        string[] freqLabels = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };

        for (int i = 0; i < freqLines.Length; i++)
        {
            float x = FreqToX(freqLines[i], pad, plotW);
            canvas.DrawLine(x, pad, x, pad + plotH, _gridPaint);
            canvas.DrawText(freqLabels[i], x + (3f * _dpiScale), pad + plotH - (4f * _dpiScale), SKTextAlign.Left, _labelFont, _labelPaint);
        }

        double[] dbLines = { -12, -6, 0, 6, 12 };
        foreach (double db in dbLines)
        {
            float y = DbToY(db, pad, plotH);
            canvas.DrawLine(pad, y, pad + plotW, y, Math.Abs(db) < 0.01 ? _zeroLinePaint : _gridPaint);
        }
    }

    private void DrawCurve(SKCanvas canvas, MicDspSettings settings, float pad, float plotW, float plotH)
    {
        _strokePath.Reset();
        _fillPath.Reset();

        float zeroY = DbToY(0.0, pad, plotH);
        _curveStrokePaint.StrokeWidth = 2.2f * _dpiScale;

        for (int i = 0; i < CurveSamples; i++)
        {
            double t = i / (double)(CurveSamples - 1);
            double freq = MinFreqHz * Math.Pow(MaxFreqHz / MinFreqHz, t);
            double db = Math.Clamp(EqCurveCalculator.EvaluateMagnitudeDb(settings, freq), MinDb, MaxDb);

            float x = pad + (float)(t * plotW);
            float y = DbToY(db, pad, plotH);

            if (i == 0)
            {
                _strokePath.MoveTo(x, y);
                _fillPath.MoveTo(x, zeroY);
                _fillPath.LineTo(x, y);
            }
            else
            {
                _strokePath.LineTo(x, y);
                _fillPath.LineTo(x, y);
            }
        }

        _fillPath.LineTo(pad + plotW, zeroY);
        _fillPath.Close();

        canvas.DrawPath(_fillPath, _curveFillPaint);
        canvas.DrawPath(_strokePath, _curveStrokePaint);
    }

    private void DrawSpectrum(SKCanvas canvas, float[] spectrumDb, float pad, float plotW, float plotH)
    {
        _spectrumStrokePath.Reset();
        _spectrumFillPath.Reset();
        _spectrumStrokePaint.StrokeWidth = 1.4f * _dpiScale;

        float bottomY = pad + plotH;
        int n = spectrumDb.Length;
        for (int i = 0; i < n; i++)
        {
            double t = i / (double)(n - 1);
            float x = pad + (float)(t * plotW);
            double norm = Math.Clamp((spectrumDb[i] - (-90.0)) / 90.0, 0.0, 1.0);
            float y = pad + (float)((1.0 - norm) * plotH);

            if (i == 0)
            {
                _spectrumStrokePath.MoveTo(x, y);
                _spectrumFillPath.MoveTo(x, bottomY);
                _spectrumFillPath.LineTo(x, y);
            }
            else
            {
                _spectrumStrokePath.LineTo(x, y);
                _spectrumFillPath.LineTo(x, y);
            }
        }

        _spectrumFillPath.LineTo(pad + plotW, bottomY);
        _spectrumFillPath.Close();
        canvas.DrawPath(_spectrumFillPath, _spectrumFillPaint);
        canvas.DrawPath(_spectrumStrokePath, _spectrumStrokePaint);
    }

    private void DrawNodes(SKCanvas canvas, MicDspSettings settings, float pad, float plotW, float plotH)
    {
        float radius = 6f * _dpiScale;
        _nodeStrokePaint.StrokeWidth = 2f * _dpiScale;

        for (int i = 0; i < 5; i++)
        {
            SKPoint pt = GetNodePoint(settings, i, pad, plotW, plotH);
            canvas.DrawCircle(pt, radius, _draggedNode == i ? _nodeActivePaint : _nodeFillPaint);
            canvas.DrawCircle(pt, radius, _nodeStrokePaint);
        }
    }

    private static SKPoint GetNodePoint(MicDspSettings s, int index, float pad, float plotW, float plotH) => index switch
    {
        0 => new SKPoint(FreqToX(s.EqHpfHz, pad, plotW), DbToY(0.0, pad, plotH)),
        1 => new SKPoint(FreqToX(120.0, pad, plotW), DbToY(s.EqLowDb, pad, plotH)),
        2 => new SKPoint(FreqToX(s.EqMid1Hz, pad, plotW), DbToY(s.EqMid1Db, pad, plotH)),
        3 => new SKPoint(FreqToX(s.EqMid2Hz, pad, plotW), DbToY(s.EqMid2Db, pad, plotH)),
        _ => new SKPoint(FreqToX(8000.0, pad, plotW), DbToY(s.EqHighDb, pad, plotH)),
    };

    private void OnMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (Settings is null || _lastWidth <= 0 || _lastHeight <= 0)
        {
            return;
        }

        Point pos = e.GetPosition(this);
        float px = (float)pos.X * _dpiScale;
        float py = (float)pos.Y * _dpiScale;
        float pad = 12f * _dpiScale;
        float plotW = _lastWidth - (pad * 2f);
        float plotH = _lastHeight - (pad * 2f);
        float hitRadiusSq = MathF.Pow(14f * _dpiScale, 2f);

        for (int i = 0; i < 5; i++)
        {
            SKPoint nodePt = GetNodePoint(Settings, i, pad, plotW, plotH);
            float dx = px - nodePt.X;
            float dy = py - nodePt.Y;
            if ((dx * dx) + (dy * dy) <= hitRadiusSq)
            {
                _draggedNode = i;
                CaptureMouse();
                InvalidateVisual();
                break;
            }
        }
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        if (_draggedNode < 0 || Settings is null || _lastWidth <= 0 || _lastHeight <= 0)
        {
            return;
        }

        Point pos = e.GetPosition(this);
        float px = (float)pos.X * _dpiScale;
        float py = (float)pos.Y * _dpiScale;
        float pad = 12f * _dpiScale;
        float plotW = Math.Max(1f, _lastWidth - (pad * 2f));
        float plotH = Math.Max(1f, _lastHeight - (pad * 2f));

        double freq = XToFreq(px, pad, plotW);
        double db = Math.Clamp(YToDb(py, pad, plotH), -15.0, 15.0);

        switch (_draggedNode)
        {
            case 0:
                Settings.EqHpfHz = (float)Math.Clamp(freq, 20.0, 300.0);
                break;
            case 1:
                Settings.EqLowDb = (float)db;
                break;
            case 2:
                Settings.EqMid1Hz = (float)Math.Clamp(freq, 150.0, 2000.0);
                Settings.EqMid1Db = (float)db;
                break;
            case 3:
                Settings.EqMid2Hz = (float)Math.Clamp(freq, 1000.0, 8000.0);
                Settings.EqMid2Db = (float)db;
                break;
            case 4:
                Settings.EqHighDb = (float)db;
                break;
        }

        CurveEdited?.Invoke(this, EventArgs.Empty);
    }

    private void OnMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_draggedNode >= 0)
        {
            _draggedNode = -1;
            ReleaseMouseCapture();
            InvalidateVisual();
            CurveEdited?.Invoke(this, EventArgs.Empty);
        }
    }

    private static float FreqToX(double freqHz, float pad, float plotW)
    {
        double clamped = Math.Clamp(freqHz, MinFreqHz, MaxFreqHz);
        double norm = Math.Log(clamped / MinFreqHz) / Math.Log(MaxFreqHz / MinFreqHz);
        return pad + (float)(norm * plotW);
    }

    private static double XToFreq(float x, float pad, float plotW)
    {
        double norm = Math.Clamp((x - pad) / plotW, 0.0, 1.0);
        return MinFreqHz * Math.Pow(MaxFreqHz / MinFreqHz, norm);
    }

    private static float DbToY(double db, float pad, float plotH)
    {
        double norm = (db - MinDb) / (MaxDb - MinDb);
        return pad + (float)((1.0 - norm) * plotH);
    }

    private static double YToDb(float y, float pad, float plotH)
    {
        double norm = 1.0 - Math.Clamp((y - pad) / plotH, 0.0, 1.0);
        return MinDb + (norm * (MaxDb - MinDb));
    }
}
