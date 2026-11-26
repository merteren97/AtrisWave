using System.Windows;
using System.Windows.Input;
using SkiaSharp;
using SkiaSharp.Views.Desktop;
using SkiaSharp.Views.WPF;

namespace AtrisWave.UI.Controls;

/// <summary>
/// 60 FPS SkiaSharp vertical studio console fader (<see cref="SKElement"/>, never SKGLControl).
/// Draws dB scale ticks (+3, 0, -6, -12, -24, -48), glowing track, and beveled fader cap.
/// Supports mouse drag, wheel, and double-click to reset to <see cref="DefaultValue"/> (1.0 = 0.0 dB).
/// </summary>
public sealed class StudioFaderControl : SKElement
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(StudioFaderControl),
        new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnVisualPropChanged));

    public static readonly DependencyProperty DefaultValueProperty = DependencyProperty.Register(
        nameof(DefaultValue), typeof(double), typeof(StudioFaderControl), new PropertyMetadata(1.0));

    public static readonly DependencyProperty AccentColorProperty = DependencyProperty.Register(
        nameof(AccentColor), typeof(string), typeof(StudioFaderControl), new PropertyMetadata("#38BDF8", OnAccentChanged));

    public static readonly DependencyProperty IsMutedProperty = DependencyProperty.Register(
        nameof(IsMuted), typeof(bool), typeof(StudioFaderControl), new PropertyMetadata(false, OnVisualPropChanged));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public double DefaultValue { get => (double)GetValue(DefaultValueProperty); set => SetValue(DefaultValueProperty, value); }
    public string AccentColor { get => (string)GetValue(AccentColorProperty); set => SetValue(AccentColorProperty, value); }
    public bool IsMuted { get => (bool)GetValue(IsMutedProperty); set => SetValue(IsMutedProperty, value); }

    private readonly SKPaint _trackBgPaint = new() { Color = new SKColor(0x13, 0x18, 0x24), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _fillPaint = new() { Color = new SKColor(0x38, 0xBD, 0xF8), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _mutedFillPaint = new() { Color = new SKColor(0x64, 0x74, 0x8B), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _tickPaint = new() { Color = new SKColor(0x47, 0x55, 0x69), IsAntialias = false, StrokeWidth = 1f };
    private readonly SKPaint _capBodyPaint = new() { Color = new SKColor(0x33, 0x41, 0x55), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _capBorderPaint = new() { Color = new SKColor(0x64, 0x74, 0x8B), IsAntialias = true, Style = SKPaintStyle.Stroke };
    private readonly SKPaint _capLinePaint = new() { Color = new SKColor(0xF8, 0xFA, 0xFC), IsAntialias = false };
    private readonly SKPaint _labelPaint = new() { Color = new SKColor(0x94, 0xA3, 0xB8), IsAntialias = true };
    private readonly SKFont _tickFont = new() { Size = 8.5f };

    private bool _isDragging;

    public StudioFaderControl()
    {
        IgnorePixelScaling = false;
        MinWidth = 44;
        Cursor = Cursors.Hand;
        MouseLeftButtonDown += OnMouseDown;
        MouseMove += OnMouseMove;
        MouseLeftButtonUp += OnMouseUp;
        MouseWheel += OnMouseWheel;
    }

    private static void OnVisualPropChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        => (d as StudioFaderControl)?.InvalidateVisual();

    private static void OnAccentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is StudioFaderControl ctrl && e.NewValue is string hex && SKColor.TryParse(hex, out SKColor col))
        {
            ctrl._fillPaint.Color = col;
            ctrl.InvalidateVisual();
        }
    }

    private void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            Value = Math.Clamp(DefaultValue, 0.0, 1.5);
            return;
        }

        _isDragging = true;
        CaptureMouse();
        UpdateValueFromY(e.GetPosition(this).Y);
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        if (_isDragging)
        {
            UpdateValueFromY(e.GetPosition(this).Y);
        }
    }

    private void OnMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (_isDragging)
        {
            _isDragging = false;
            ReleaseMouseCapture();
        }
    }

    private void OnMouseWheel(object sender, MouseWheelEventArgs e)
    {
        double step = Keyboard.IsKeyDown(Key.LeftShift) || Keyboard.IsKeyDown(Key.RightShift) ? 0.01 : 0.05;
        Value = Math.Clamp(Value + (e.Delta > 0 ? step : -step), 0.0, 1.5);
        e.Handled = true;
    }

    private static readonly (double lin, string lbl)[] DbTicks =
    [
        (1.412, "+3"),
        (1.000, "0"),
        (0.501, "-6"),
        (0.251, "-12"),
        (0.063, "-24"),
        (0.004, "-48"),
    ];

    private const double TaperExponent = 0.42;

    private static double LinearToTaper(double linear)
        => Math.Pow(Math.Clamp(linear / 1.5, 0.0, 1.0), TaperExponent);

    private static double TaperToLinear(double norm)
        => Math.Clamp(1.5 * Math.Pow(Math.Clamp(norm, 0.0, 1.0), 1.0 / TaperExponent), 0.0, 1.5);

    private void UpdateValueFromY(double yWpf)
    {
        double pad = 12.0;
        double h = Math.Max(1.0, ActualHeight - (pad * 2.0));
        double norm = 1.0 - Math.Clamp((yWpf - pad) / h, 0.0, 1.0);
        Value = TaperToLinear(norm);
    }

    protected override void OnPaintSurface(SKPaintSurfaceEventArgs e)
    {
        base.OnPaintSurface(e);
        SKCanvas canvas = e.Surface.Canvas;
        int w = e.Info.Width;
        int h = e.Info.Height;
        canvas.Clear(SKColors.Transparent);
        if (w < 20 || h < 40) return;

        float dpi = ActualWidth > 0 ? w / (float)ActualWidth : 1f;
        float pad = 12f * dpi;
        float trackH = Math.Max(10f, h - (pad * 2f));
        float trackW = 6f * dpi;
        float trackX = (w * 0.56f) - (trackW * 0.5f);

        _tickPaint.StrokeWidth = MathF.Max(1f, dpi);
        _tickFont.Size = 8f * dpi;

        foreach ((double lin, string lbl) in DbTicks)
        {
            float norm = (float)LinearToTaper(lin);
            float ty = pad + ((1f - norm) * trackH);
            canvas.DrawLine(trackX - (8f * dpi), ty, trackX - (2f * dpi), ty, _tickPaint);
            canvas.DrawText(lbl, trackX - (10f * dpi), ty + (3f * dpi), SKTextAlign.Right, _tickFont, _labelPaint);
        }

        // Track background + active fill
        canvas.DrawRoundRect(trackX, pad, trackW, trackH, 3f * dpi, 3f * dpi, _trackBgPaint);

        float valNorm = (float)LinearToTaper(Value);
        float fillH = valNorm * trackH;
        float capY = pad + ((1f - valNorm) * trackH);

        if (fillH > 1f)
        {
            canvas.DrawRoundRect(trackX, capY, trackW, fillH, 3f * dpi, 3f * dpi, IsMuted ? _mutedFillPaint : _fillPaint);
        }

        // Metallic beveled fader cap
        float capW = 24f * dpi;
        float capH = 14f * dpi;
        float capX = (w * 0.56f) - (capW * 0.5f);
        float capTop = capY - (capH * 0.5f);
        _capBorderPaint.StrokeWidth = MathF.Max(1f, dpi);
        _capLinePaint.StrokeWidth = MathF.Max(1.5f, 1.5f * dpi);

        canvas.DrawRoundRect(capX, capTop, capW, capH, 3f * dpi, 3f * dpi, _capBodyPaint);
        canvas.DrawRoundRect(capX, capTop, capW, capH, 3f * dpi, 3f * dpi, _capBorderPaint);
        canvas.DrawLine(capX + (4f * dpi), capY, capX + capW - (4f * dpi), capY, _capLinePaint);
    }
}
