// StreamViewer -- a small GDI window on the shm stream: pick a section,
// pick a channel, watch that channel's waveform at 60 Hz.
//
// Single-threaded by design: pump messages, wait until the next 60 Hz frame
// boundary (MsgWaitForMultipleObjects on the producer event in event mode),
// drain new blocks into a scrolling trace, paint at the boundary. No reader
// thread, no queues -- the whole point of the demo is that Client's API is
// enough.
//
// Event vs. polling is decided once, after attach, from the header: the
// publish rate is sampleRateMilliHz / 1000 / samplesPerBlock blocks per
// second; at <= 60 Hz the frame boundary and the publish cadence are the same
// order, so waiting on the event buys real power savings. Above that, the
// frame loop drains whatever arrived anyway, so the event is ignored.
//
// ASSUMPTION: the producer's event is named after the section itself (this
// demo passes the section name as both names). Real producers append their
// own suffixes to the base names -- see samples/StreamReader for the
// resolved names -- and Client silently falls back to polling when no event
// of the given name exists, so a wrong event name costs latency, not
// function.
//
//     dotnet run --project samples/StreamViewer -- <resolvedShmName>
//
// SPDX-License-Identifier: MIT

using IrTactile.Shm.Stream;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace StreamViewer;

internal static class NativeMethods
{
    public const uint QS_ALLINPUT = 0x04FF;
    public const uint WAIT_OBJECT_0 = 0;

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint MsgWaitForMultipleObjects(
        uint nCount,
        IntPtr[]? pHandles,
        [MarshalAs(UnmanagedType.Bool)] bool bWaitAll,
        uint dwMilliseconds,
        uint dwWakeMask);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);
}

/// <summary>A simple double-buffered panel for the waveform plot.</summary>
internal sealed class PlotPanel : Panel
{
    public PlotPanel()
    {
        DoubleBuffered = true;
        SetStyle(ControlStyles.ResizeRedraw, true);
    }
}

internal sealed class ViewerForm : Form
{
    // ---------------------------------------------------------------------------
    // Constants
    // ---------------------------------------------------------------------------

    private const int kTopBarHeight = 32;        // control row
    private const int kStatusHeight = 20;        // status strip
    private const long kFramePeriodMs = 16;      // ~60 Hz
    private const double kEventModeMaxRateHz = 60.0;
    private const int kNameBufferChars = 256;
    private const string kDefaultSectionName = "irTactile_Stream_Primary_256k";

    private static readonly Color kBackgroundColor = Color.FromArgb(18, 20, 24);
    private static readonly Color kStatusTextColor = Color.FromArgb(190, 195, 200);
    private static readonly Color kZeroLineColor = Color.FromArgb(60, 64, 70);
    private static readonly Color kTraceColor = Color.FromArgb(90, 200, 250);

    // ---------------------------------------------------------------------------
    // Controls
    // ---------------------------------------------------------------------------

    private readonly Panel _topPanel;
    private readonly Label _deviceLabel;
    private readonly TextBox _deviceEdit;
    private readonly Button _connectButton;
    private readonly Label _channelLabel;
    private readonly ComboBox _channelCombo;
    private readonly PlotPanel _plotPanel;
    private readonly Label _statusLabel;

    // ---------------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------------

    private readonly Client _client = new();
    private readonly Queue<float> _trace = new();
    private bool _connected;
    private bool _eventMode;
    private int _selectedChannel;
    private int _traceCapacity = 640;
    private string _statusText = "Not connected. Enter the resolved shm section name and press Connect.";

    public ViewerForm()
    {
        Text = "irTactile shm stream viewer";
        BackColor = kBackgroundColor;
        ForeColor = kStatusTextColor;
        Font = SystemFonts.MessageBoxFont;
        MinimumSize = new Size(480, 240);
        Size = new Size(640, 360);
        StartPosition = FormStartPosition.CenterScreen;

        _topPanel = new Panel
        {
            Dock = DockStyle.Top,
            Height = kTopBarHeight,
            BackColor = BackColor,
        };

        _deviceLabel = new Label
        {
            Text = "Device:",
            AutoSize = true,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = ForeColor,
        };
        _deviceEdit = new TextBox
        {
            Text = kDefaultSectionName,
            MaxLength = kNameBufferChars - 1,
            BorderStyle = BorderStyle.Fixed3D,
        };
        _connectButton = new Button
        {
            Text = "Connect",
            AutoSize = true,
            UseVisualStyleBackColor = true,
        };
        _channelLabel = new Label
        {
            Text = "Channel:",
            AutoSize = true,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = ForeColor,
        };
        _channelCombo = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Enabled = false,
            Width = 64,
        };

        _topPanel.Controls.Add(_deviceLabel);
        _topPanel.Controls.Add(_deviceEdit);
        _topPanel.Controls.Add(_connectButton);
        _topPanel.Controls.Add(_channelLabel);
        _topPanel.Controls.Add(_channelCombo);
        _topPanel.Resize += (_, _) => LayoutTopBar();
        _topPanel.Layout += (_, _) => LayoutTopBar();

        _connectButton.Click += (_, _) => ConnectNow();
        _channelCombo.SelectedIndexChanged += (_, _) =>
        {
            _selectedChannel = _channelCombo.SelectedIndex;
            // The trace keeps its history and scrolls the new channel in from the right.
        };
        _deviceEdit.KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Enter)
            {
                // Enter in the name field means Connect. SuppressKeyPress also
                // keeps the edit control from beeping at the key.
                ConnectNow();
                e.SuppressKeyPress = true;
            }
        };

        _statusLabel = new Label
        {
            Dock = DockStyle.Bottom,
            Height = kStatusHeight,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = ForeColor,
            BackColor = BackColor,
            Padding = new Padding(8, 0, 0, 0),
        };

        _plotPanel = new PlotPanel
        {
            Dock = DockStyle.Fill,
            BackColor = kBackgroundColor,
        };
        _plotPanel.Paint += OnPlotPaint;
        _plotPanel.Resize += (_, _) =>
        {
            _traceCapacity = Math.Max(1, _plotPanel.ClientSize.Width);
            while (_trace.Count > _traceCapacity) _trace.Dequeue();
        };

        // Dock order: the plot panel must be first in the collection so it fills
        // the remaining space after the top and bottom strips.
        Controls.Add(_plotPanel);
        Controls.Add(_statusLabel);
        Controls.Add(_topPanel);

        // Initial layout.
        LayoutTopBar();
    }

    public bool Connected => _connected;
    public bool EventMode => _eventMode;
    public EventWaitHandle? ClientEvent => _client.NativeEvent;

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _client.Close();
        base.OnFormClosing(e);
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        _deviceEdit.Focus();
    }

    // ---------------------------------------------------------------------------
    // Layout
    // ---------------------------------------------------------------------------

    private void LayoutTopBar()
    {
        const int margin = 8;
        const int controlHeight = 22;
        const int centerY = kTopBarHeight / 2;
        int left = margin;
        int width = _topPanel.ClientSize.Width;

        _deviceLabel.Location = new Point(left, centerY - _deviceLabel.Height / 2);
        left += _deviceLabel.Width + 4;

        int fixedRight =
            margin +                           // before connect
            _connectButton.PreferredSize.Width + margin + 12 +
            _channelLabel.PreferredSize.Width + 2 +
            _channelCombo.Width + margin;

        int editWidth = Math.Max(120, width - left - fixedRight);
        _deviceEdit.SetBounds(left, centerY - controlHeight / 2, editWidth, controlHeight);
        left += editWidth + margin;

        _connectButton.SetBounds(left, centerY - _connectButton.PreferredSize.Height / 2,
            _connectButton.PreferredSize.Width, _connectButton.PreferredSize.Height);
        left += _connectButton.Width + 12;

        _channelLabel.Location = new Point(left, centerY - _channelLabel.Height / 2);
        left += _channelLabel.Width + 2;

        _channelCombo.SetBounds(left, centerY - _channelCombo.Height / 2,
            _channelCombo.Width, _channelCombo.Height);
    }

    // ---------------------------------------------------------------------------
    // Connect / stream plumbing
    // ---------------------------------------------------------------------------

    private void SetDisconnected()
    {
        _connected = false;
        _trace.Clear();
        _channelCombo.Enabled = false;
    }

    private void PopulateChannels()
    {
        uint channelCount = _client.Reader.ChannelCount;
        _channelCombo.Items.Clear();
        for (uint ch = 0; ch < channelCount; ++ch)
            _channelCombo.Items.Add(ch.ToString());

        if (_selectedChannel >= channelCount)
            _selectedChannel = (int)channelCount - 1;
        if (_selectedChannel < 0)
            _selectedChannel = 0;

        if (channelCount > 0)
            _channelCombo.SelectedIndex = _selectedChannel;
    }

    private void ConnectNow()
    {
        string sectionName = _deviceEdit.Text.Trim();
        if (string.IsNullOrEmpty(sectionName))
        {
            _statusText = "Enter a section name first.";
            UpdateStatus();
            return;
        }

        // Event name == section name by demo assumption; a missing event just
        // leaves Client in polling mode.
        OpenResult openResult = _client.Open(sectionName, sectionName, null);

        if (openResult != OpenResult.Ok)
        {
            string message = $"Open failed: {openResult}";
            if (openResult.ShouldRetry())
                message += " - producer not up yet, or wrong name; try again";
            else if (!string.IsNullOrEmpty(_client.Reader.MismatchDetail))
                message += $" ({_client.Reader.MismatchDetail})";

            _statusText = message;
            SetDisconnected();
            UpdateStatus();
            InvalidatePlot();
            return;
        }

        Reader reader = _client.Reader;
        double rateHz = reader.PublishHz;

        _connected = true;
        _eventMode = _client.NativeEvent != null && rateHz <= kEventModeMaxRateHz;
        _selectedChannel = 0;
        _trace.Clear();
        PopulateChannels();
        _channelCombo.Enabled = true;

        _statusText = $"Attached: \"{reader.DeviceName}\", {reader.ChannelCount} ch, " +
                      $"publish {rateHz:0.0} Hz, period {reader.PeriodMs:0.00} ms, " +
                      $"{(_eventMode ? "event" : "polling")} mode";
        UpdateStatus();
        InvalidatePlot();
    }

    /// <summary>Called once per 60 Hz frame while connected.</summary>
    public void ProcessStream()
    {
        if (!_connected) return;

        // Zero-timeout wait: consumes an event pulse if one is pending and
        // classifies layoutSeq / deviceState changes; the blocks themselves are
        // drained below (wakes coalesce, so always loop next()).
        WaitResult waitResult = _client.Wait(TimeSpan.Zero);

        if (waitResult == WaitResult.Closed)
        {
            _statusText = $"Producer closed. dropped={_client.Reader.DroppedBlocks}";
            _client.Close();
            SetDisconnected();
            UpdateStatus();
            InvalidatePlot();
            return;
        }

        if (waitResult == WaitResult.Reconfigured)
        {
            // Geometry changed in place: the channel count may differ, and the
            // old trace was a different channel's worth of data anyway.
            PopulateChannels();
            _trace.Clear();
        }

        var block = new BlockView();
        while (_client.Next(block))
        {
            ReadOnlySpan<float> samples = block.Channel(_selectedChannel);
            if (samples.IsEmpty) continue;
            foreach (float sample in samples)
            {
                _trace.Enqueue(sample);
                if (_trace.Count > _traceCapacity) _trace.Dequeue();
            }
        }

        Reader reader = _client.Reader;
        _statusText = $"\"{reader.DeviceName}\"  {reader.PublishHz:0.0} Hz  " +
                      $"{(_eventMode ? "event" : "polling")}  " +
                      $"state={_client.State()}  dropped={reader.DroppedBlocks}";
        UpdateStatus();
    }

    public void UpdateStatus() => _statusLabel.Text = _statusText;

    public void InvalidatePlot()
    {
        if (_plotPanel.IsHandleCreated && !_plotPanel.IsDisposed)
            _plotPanel.Invalidate();
    }

    // ---------------------------------------------------------------------------
    // Painting
    // ---------------------------------------------------------------------------

    private void OnPlotPaint(object? sender, PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        int width = _plotPanel.ClientSize.Width;
        int height = _plotPanel.ClientSize.Height;
        if (width <= 0 || height <= 0) return;

        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.None;
        g.Clear(kBackgroundColor);

        if (!_connected || _trace.Count < 2) return;

        // Zero line.
        int zeroY = height / 2;
        using (var zeroPen = new Pen(kZeroLineColor))
        {
            g.DrawLine(zeroPen, 0, zeroY, width, zeroY);
        }

        // Waveform: [-1, 1] mapped onto the plot area, newest sample at the right edge.
        int sampleCount = _trace.Count;
        var points = new PointF[sampleCount];
        int i = 0;
        foreach (float sample in _trace)
        {
            float v = Math.Clamp(sample, -1.0f, 1.0f);
            float x = width - sampleCount + i;
            float y = (1.0f - v) * 0.5f * height;
            points[i++] = new PointF(x, y);
        }

        using (var tracePen = new Pen(kTraceColor))
        {
            g.DrawLines(tracePen, points);
        }
    }
}

internal static class Program
{
    private const long kFramePeriodMs = 16; // ~60 Hz

    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();

        var form = new ViewerForm();
        if (args.Length > 0)
        {
            form.Controls.OfType<TextBox>().First().Text = args[0];
        }
        form.Show();

        long frameTicks = (long)(kFramePeriodMs * Stopwatch.Frequency / 1000.0);
        long nextFrame = Stopwatch.GetTimestamp() + frameTicks;

        while (!form.IsDisposed)
        {
            Application.DoEvents();

            long now = Stopwatch.GetTimestamp();
            long msToFrame = (nextFrame - now) * 1000 / Stopwatch.Frequency;

            if (msToFrame > 0)
            {
                if (form.EventMode && form.ClientEvent != null)
                {
                    IntPtr[] handles = new[] { form.ClientEvent.SafeWaitHandle.DangerousGetHandle() };
                    uint waitStatus = NativeMethods.MsgWaitForMultipleObjects(
                        1, handles, false, (uint)msToFrame, NativeMethods.QS_ALLINPUT);

                    if (waitStatus == NativeMethods.WAIT_OBJECT_0)
                    {
                        // MsgWaitForMultipleObjects does not consume auto-reset
                        // events: eat the pulse so the loop does not spin until
                        // the boundary. The block itself is drained in
                        // ProcessStream(). (Single waiter only -- if two viewers
                        // ran against one event, one could sleep through a wake;
                        // acceptable for a demo.)
                        NativeMethods.WaitForSingleObject(handles[0], 0);
                    }
                }
                else
                {
                    NativeMethods.MsgWaitForMultipleObjects(
                        0, null, false, (uint)msToFrame, NativeMethods.QS_ALLINPUT);
                }
                continue;
            }

            // Frame boundary.
            nextFrame += frameTicks;
            if (nextFrame < now) nextFrame = now + frameTicks;

            if (form.Connected) form.ProcessStream();
            form.InvalidatePlot();
        }

        return 0;
    }
}
