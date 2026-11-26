using System.Windows;
using System.Windows.Input;
using SkiaSharp;
using SkiaSharp.Views.Desktop;
using SkiaSharp.Views.WPF;

namespace AtrisWave.UI.Controls;

/// <summary>
/// 60 FPS SkiaSharp studio rotary knob (<see cref="SKElement"/>, never SKGLControl).
/// Supports vertical drag, Shift fine-control, mouse wheel, and double-click to reset to <see cref="DefaultValue"/>.
/// </summary>
public sealed class RotaryKnobControl : SKElement
{
    private const float StartAngleDeg = 135f;
    private const float SweepAngleDeg = 270f;

    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(RotaryKnobControl),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnVisualPropChanged));

    public static readonly DependencyProperty MinimumProperty = DependencyProperty.Register(
        nameof(Minimum), typeof(double), typeof(RotaryKnobControl), new PropertyMetadata(0.0, OnVisualPropChanged));

    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(
        nameof(Maximum), typeof(double), typeof(RotaryKnobControl), new PropertyMetadata(1.0, OnVisualPropChanged));

    public static readonly DependencyProperty DefaultValueProperty = DependencyProperty.Register(
        nameof(DefaultValue), typeof(double), typeof(RotaryKnobControl), new PropertyMetadata(0.0));

    public static readonly DependencyProperty UnitProperty = DependencyProperty.Register(
        nameof(Unit), typeof(string), typeof(RotaryKnobControl), new PropertyMetadata(string.Empty, OnVisualPropChanged));

    public static readonly DependencyProperty LabelProperty = DependencyProperty.Register(
        nameof(Label), typeof(string), typeof(RotaryKnobControl), new PropertyMetadata(string.Empty, OnVisualPropChanged));

    public static readonly DependencyProperty AccentColorProperty = DependencyProperty.Register(
        nameof(AccentColor), typeof(string), typeof(RotaryKnobControl), new PropertyMetadata("#38BDF8", OnAccentChanged));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public double Minimum { get => (double)GetValue(MinimumProperty); set => SetValue(MinimumProperty, value); }
    public double Maximum { get => (double)GetValue(MaximumProperty); set => SetValue(MaximumProperty, value); }
    public double DefaultValue { get => (double)GetValue(DefaultValueProperty); set => SetValue(DefaultValueProperty, value); }
    public string Unit { get => (string)GetValue(UnitProperty); set => SetValue(UnitProperty, value); }
    public string Label { get => (string)GetValue(LabelProperty); set => SetValue(LabelProperty, value); }
    public string AccentColor { get => (string)GetValue(AccentColorProperty); set => SetValue(AccentColorProperty, value); }

    private readonly SKPaint _dialPaint = new() { Color = new SKColor(0x13, 0x18, 0x24), IsAntialias = true, Style = SKPaintStyle.Fill };
    private readonly SKPaint _dialRimPaint = new() { Color = new SKColor(0x2D, 0x37, 0x48), IsAntialias = true, Style = SKPaintStyle.Stroke };
    private readonly SKPaint _trackPaint = new() { Color = new SKColor(0x26, 0x2E, 0x3F), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeCap = SKStrokeCap.Round };
    private readonly SKPaint _arcPaint = new() { Color = new SKColor(0x38, 0xBD, 0xF8), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeCap = SKStrokeCap.Round };
    private readonly SKPaint _pointerPaint = new() { Color = new SKColor(0xF8, 0xFA, 0xFC), IsAntialias = true, Style = SKPaintStyle.Stroke, StrokeCap = SKStrokeCap.Round };
    private readonly SKPaint _textPaint = new() { Color = new SKColor(0xE2, 0xE8, 0xF0), IsAntialias = true };
    private readonly SKPaint _labelPaint = new() { Color = new SKColor(0x94, 0xA3, 0xB8), IsAntialias = true };
    private readonly SKFont _valueFont = new() { Size = 10f };
    private readonly SKFont _labelFont = new() { Size = 10f };
    private readonly SKPath _arcPath = new();

    private bool _isDragging;
    private double _dragStartY;
    private double _dragStartValue;

    public RotaryKnobControl()
    {
        IgnorePixelScaling = false;
        MinWidth = 64;
        MinHeight = 76;
        Cursor = Cursors.SizeNS;
        MouseLeftButtonDown += OnMouseDown;
        MouseMove += OnMouseMove;
        MouseLeftButtonUp += OnMouseUp;
        MouseWheel += OnMouseWheel;
    }

    private static void OnVisualPropChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        => (d as RotaryKnobControl)?.InvalidateVisual();

    private static void OnAccentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is RotaryKnobControl ctrl && e.NewValue is string hex && SKColor.TryParse(hex, out SKColor col))
        {
            ctrl._arcPaint.Color = col;
            ctrl.InvalidateVisual();
        }
    }

    private void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            Value = Math.Clamp(DefaultValue, Minimum, Maximum);
            return;
        }

        _isDragging = true;
        _dragStartY = e.GetPosition(this).Y;
        _dragStartValue = Value;
        CaptureMouse();
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        if (!_isDragging) return;

        double dy = _dragStartY - e.GetPosition(this).Y;
        double range = Math.Max(1e-6, Maximum - Minimum);
        double sensitivity = Keyboard.IsKeyDown(Key.LeftShift) || Keyboard.IsKeyDown(Key.RightShift) ? 600.0 : 160.0;
        Value = Math.Clamp(_dragStartValue + (dy / sensitivity) * range, Minimum, Maximum);
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
        double range = Math.Max(1e-6, Maximum - Minimum);
        double step = (Keyboard.IsKeyDown(Key.LeftShift) || Keyboard.IsKeyDown(Key.RightShift) ? 0.005 : 0.025) * range;
        Value = Math.Clamp(Value + (e.Delta > 0 ? step : -step), Minimum, Maximum);
        e.Handled = true;
    }

    protected override void OnPaintSurface(SKPaintSurfaceEventArgs e)
    {
        base.OnPaintSurface(e);
        SKCanvas canvas = e.Surface.Canvas;
        int w = e.Info.Width;
        int h = e.Info.Height;
        canvas.Clear(SKColors.Transparent);
        if (w < 24 || h < 24) return;

        float dpi = ActualWidth > 0 ? w / (float)ActualWidth : 1f;
        float topAreaH = h - (26f * dpi);
        float radius = MathF.Max(10f, MathF.Min(w, topAreaH) * 0.36f);
        float cx = w * 0.5f;
        float cy = (topAreaH * 0.5f) + (2f * dpi);

        float strokeW = MathF.Max(2.5f, 3.2f * dpi);
        _trackPaint.StrokeWidth = strokeW;
        _arcPaint.StrokeWidth = strokeW;
        _dialRimPaint.StrokeWidth = MathF.Max(1f, 1.2f * dpi);
        _pointerPaint.StrokeWidth = MathF.Max(2f, 2.2f * dpi);

        var arcRect = new SKRect(cx - radius, cy - radius, cx + radius, cy + radius);
        _arcPath.Reset();
        _arcPath.AddArc(arcRect, StartAngleDeg, SweepAngleDeg);
        canvas.DrawPath(_arcPath, _trackPaint);

        double norm = Math.Clamp((Value - Minimum) / Math.Max(1e-6, Maximum - Minimum), 0.0, 1.0);
        float sweep = (float)(norm * SweepAngleDeg);
        if (sweep > 0.5f)
        {
            _arcPath.Reset();
            _arcPath.AddArc(arcRect, StartAngleDeg, sweep);
            canvas.DrawPath(_arcPath, _arcPaint);
        }

        float innerR = radius * 0.72f;
        canvas.DrawCircle(cx, cy, innerR, _dialPaint);
        canvas.DrawCircle(cx, cy, innerR, _dialRimPaint);

        float angleRad = (StartAngleDeg + sweep) * MathF.PI / 180f;
        float px1 = cx + (MathF.Cos(angleRad) * innerR * 0.35f);
        float py1 = cy + (MathF.Sin(angleRad) * innerR * 0.35f);
        float px2 = cx + (MathF.Cos(angleRad) * innerR * 0.85f);
        float py2 = cy + (MathF.Sin(angleRad) * innerR * 0.85f);
        canvas.DrawLine(px1, py1, px2, py2, _pointerPaint);

        _valueFont.Size = 9.5f * dpi;
        _labelFont.Size = 9.5f * dpi;
        string valText = string.IsNullOrEmpty(Unit) ? $"{Value:0.0}" : $"{Value:0.0}{Unit}";
        canvas.DrawText(valText, cx, h - (14f * dpi), SKTextAlign.Center, _valueFont, _textPaint);

        if (!string.IsNullOrEmpty(Label))
        {
            canvas.DrawText(Label, cx, h - (3.5f * dpi), SKTextAlign.Center, _labelFont, _labelPaint);
        }
    }
}
