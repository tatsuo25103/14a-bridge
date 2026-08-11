using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace StampPlcRseConfigurator
{
    internal sealed class PortChoice
    {
        internal readonly string PortName;
        internal readonly bool IsSmartPlc;
        internal readonly string Version;

        internal PortChoice(string portName, bool isSmartPlc, string version)
        {
            PortName = portName;
            IsSmartPlc = isSmartPlc;
            Version = version ?? "";
        }

        public override string ToString()
        {
            if (!IsSmartPlc) return "[OTHER / UNPROGRAMMED]  " + PortName;
            string suffix = string.IsNullOrWhiteSpace(Version) ? "" : "  V" + Version.TrimStart('V', 'v');
            return "[SMARTPLC]  " + PortName + suffix;
        }
    }

    internal sealed class TechCircularProgress : Control
    {
        private readonly System.Windows.Forms.Timer _spinner = new System.Windows.Forms.Timer { Interval = 35 };
        private int _value;
        private float _rotation = -90F;
        private bool _indeterminate;
        private Color _ringColor = Color.FromArgb(0, 220, 210);

        internal TechCircularProgress()
        {
            DoubleBuffered = true;
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.UserPaint |
                ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                ControlStyles.SupportsTransparentBackColor, true);
            Size = new Size(52, 52);
            BackColor = Color.Transparent;
            _spinner.Tick += delegate
            {
                _rotation = (_rotation + 10F) % 360F;
                Invalidate();
            };
        }

        internal int Value
        {
            get { return _value; }
            set { _value = Math.Max(0, Math.Min(100, value)); Invalidate(); }
        }

        internal bool Indeterminate
        {
            get { return _indeterminate; }
            set
            {
                _indeterminate = value;
                if (value) _spinner.Start(); else _spinner.Stop();
                Invalidate();
            }
        }

        internal Color RingColor
        {
            get { return _ringColor; }
            set { _ringColor = value; Invalidate(); }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) _spinner.Dispose();
            base.Dispose(disposing);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            RectangleF ring = new RectangleF(6, 6, Width - 13, Height - 13);
            using (var track = new Pen(Color.FromArgb(58, 78, 92), 4F))
                e.Graphics.DrawEllipse(track, ring);

            float start = _indeterminate ? _rotation : -90F;
            float sweep = _indeterminate ? 105F : 360F * _value / 100F;
            if (sweep > 0F)
            {
                using (var glow = new Pen(Color.FromArgb(55, _ringColor), 8F))
                using (var arc = new Pen(_ringColor, 4F))
                {
                    glow.StartCap = glow.EndCap = LineCap.Round;
                    arc.StartCap = arc.EndCap = LineCap.Round;
                    e.Graphics.DrawArc(glow, ring, start, sweep);
                    e.Graphics.DrawArc(arc, ring, start, sweep);
                }
            }

            string center = _indeterminate ? "..." : _value + "%";
            using (var font = new Font("Segoe UI Semibold", _value == 100 ? 8F : 8.5F))
            using (var brush = new SolidBrush(Color.FromArgb(230, 242, 245)))
            using (var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
                e.Graphics.DrawString(center, font, brush, ClientRectangle, format);
        }
    }

    internal sealed class MainForm : Form
    {
        private const string ReleaseVersion = "V1.0.4";
        private static readonly Color Surface = Color.FromArgb(16, 22, 30);
        private static readonly Color Panel = Color.FromArgb(25, 34, 45);
        private static readonly Color Accent = Color.FromArgb(0, 220, 210);
        private static readonly Color TextColor = Color.FromArgb(225, 235, 240);
        private static readonly Color Muted = Color.FromArgb(135, 155, 170);
        private readonly ComboBox _ports = new ComboBox();
        private readonly Button _scanPorts = new Button();
        private readonly Button _connect = new Button();
        private readonly Button _flashFirmware = new Button();
        private readonly Label _connection = new Label();
        private readonly Label _rse = new Label();
        private readonly Label _mode = new Label();
        private readonly Label _result = new Label();
        private readonly DataGridView _grid = new DataGridView();
        private readonly TextBox _baud = new TextBox();
        private readonly TextBox _register = new TextBox();
        private readonly ComboBox _wifiSsid = new ComboBox();
        private readonly TextBox _wifiPassword = new TextBox();
        private readonly CheckBox _automaticOta = new CheckBox();
        private readonly Label _automaticOtaStatus = new Label();
        private readonly Label _wifiStatus = new Label();
        private readonly Label _firmwareStatus = new Label();
        private readonly Label _otaDiagnosticStatus = new Label();
        private readonly TechCircularProgress _flashProgress = new TechCircularProgress();
        private readonly Label _flashProgressText = new Label();
        private readonly TextBox _log = new TextBox();
        private readonly FlowLayoutPanel _gaugeFlow = new FlowLayoutPanel();
        private readonly LiquidGauge[] _gauges = new LiquidGauge[6];
        private readonly bool[] _ratingIssues = new bool[6];
        private readonly System.Windows.Forms.Timer _animationTimer = new System.Windows.Forms.Timer { Interval = 33 };
        private readonly System.Windows.Forms.Timer _statusTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        private int _rsePercent;
        private SerialPort _serial;
        private readonly StringBuilder _receiveBuffer = new StringBuilder();
        private readonly object _serialLock = new object();
        private readonly AutoResetEvent _commitResponse = new AutoResetEvent(false);
        private bool? _dryRun;
        private volatile bool _savingSettings;
        private volatile bool _flashingFirmware;
        private bool _updatingWifiUi;
        private bool? _pendingAutomaticOta;
        private volatile bool _scanningPorts;

        private static readonly Regex IdentityPattern = new Regex(
            @"(?:^|\n)IDENTITY PRODUCT=14A_BRIDGE MODEL=STAMPPLC VERSION=([^\s\r\n]+)",
            RegexOptions.Multiline | RegexOptions.IgnoreCase);
        private static readonly Regex LegacyIdentityPattern = new Regex(
            @"(?:^|\n)@\s+WIFI VERSION=([^\s\r\n]+)",
            RegexOptions.Multiline | RegexOptions.IgnoreCase);

        private static readonly Regex RsePattern = new Regex(
            @"^RSE DI mask:\s*(0x[0-9A-Fa-f]+)\s+level:\s*(\d+%|INVALID)$");
        private static readonly Regex ModePattern = new Regex(
            @"^Mode:\s*(DRY-RUN|LIVE)\s+RS485:\s*(\d+) baud\s+register:\s*(0x[0-9A-Fa-f]+)\s+quantity:\s*([12])$");
        private static readonly Regex IdPattern = new Regex(
            @"^([1-6])\s+(yes|no)\s+(\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+(\d+)\s+(\d+)\s+(yes|no)$");
        private static readonly Regex ProbePattern = new Regex(
            @"^PROBE ID=([1-6]) REGISTER=(0x[0-9A-Fa-f]+) VALUE=(\d+) STATUS=(OK|RETRY|ERROR) DETAIL=(.*)$");
        private static readonly Regex ScanPattern = new Regex(
            @"^SCAN ID=([1-6]) VALUE=(\d+) STATUS=(FOUND|NO_RESPONSE|ERROR) DETAIL=(.*)$");
        private static readonly Regex CommitPattern = new Regex(
            @"^COMMIT ID=([1-6]) STATUS=(OK|CLAMPED|PENDING|ERROR) CONFIG=(\d+) DETAIL=(.*)$");
        private static readonly Regex WifiPattern = new Regex(
            @"^WIFI VERSION=([^\s]+) SAVED=(yes|no) CONNECTED=(yes|no) AUTO=(yes|no) SSIDHEX=([^\s]*) IP=([^\s]+) RSSI=(-?\d+)$");
        private static readonly Regex OtaPattern = new Regex(
            @"^OTA STATUS=(OK|ERROR|REBOOT) CURRENT=([^\s]+) AVAILABLE=([^\s]+) DETAIL=(.*)$");
        private static readonly Regex OtaAutoPattern = new Regex(
            @"^OTA AUTO=(yes|no) STATUS=(OK|ERROR)(?: DETAIL=(.*))?$");
        private static readonly Regex OtaDiagnosticPattern = new Regex(
            @"^OTA LAST=([A-Z_]+) CHECK=(\d+) SUCCESS=(\d+) FAILS=(\d+) NEXT=(\d+) DETAILHEX=([0-9A-Fa-f]*)$");

        internal MainForm()
        {
            Text = "14a Bridge - USB Configurator " + ReleaseVersion;
            string iconPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mes_logo.ico");
            if (File.Exists(iconPath)) Icon = new Icon(iconPath);
            ClientSize = new Size(1120, 800);
            MinimumSize = new Size(1000, 700);
            Font = new Font("Segoe UI", 9F);
            BackColor = Surface;
            ForeColor = TextColor;
            BuildUi();
            _animationTimer.Tick += delegate { foreach (LiquidGauge gauge in _gauges) if (gauge != null) gauge.AdvanceFrame(); };
            _animationTimer.Start();
            _statusTimer.Tick += delegate { SendBackgroundStatus(); };
            Shown += delegate { ScanPorts(); };
            RefreshWifiNetworks();
            ScheduleGuiUpdateCheck();
            FormClosing += delegate { Disconnect(); };
        }

        private void BuildUi()
        {
            var root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                Padding = new Padding(10),
                ColumnCount = 1,
                RowCount = 3,
                AutoScroll = false
            };
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 76));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            Controls.Add(root);

            root.Controls.Add(BuildHeader());

            var connectionBox = NewGroup("USB Connection");
            var connectionFlow = NewFlow();
            _ports.Width = 270;
            _ports.Height = 27;
            _ports.DropDownStyle = ComboBoxStyle.DropDownList;
            _ports.BackColor = Color.FromArgb(7, 13, 19);
            _ports.ForeColor = TextColor;
            _ports.FlatStyle = FlatStyle.Flat;
            _connect.Text = "Connect";
            StyleButton(_connect);
            _connect.Click += ToggleConnection;
            _scanPorts.Text = "Scan";
            StyleButton(_scanPorts);
            _scanPorts.Click += delegate { ScanPorts(); };
            _scanPorts.AutoSize = false;
            _connect.AutoSize = false;
            _scanPorts.Size = new Size(76, 26);
            _connect.Size = new Size(86, 26);
            _scanPorts.Margin = new Padding(4, 0, 0, 0);
            _connect.Margin = new Padding(8, 0, 0, 0);
            connectionFlow.WrapContents = false;
            connectionFlow.AutoSize = false;
            connectionFlow.Padding = new Padding(0, 2, 0, 0);
            _connection.AutoSize = true;
            _connection.Padding = new Padding(14, 5, 0, 0);
            _connection.Text = "\u25CF  DISCONNECTED";
            _connection.ForeColor = Muted;
            connectionFlow.Controls.AddRange(new Control[] { _ports, _scanPorts, _connect, _connection });
            connectionBox.Controls.Add(connectionFlow);
            root.Controls.Add(connectionBox);

            var tabs = new TabControl
            {
                Name = "MainTabs",
                Dock = DockStyle.Fill, Appearance = TabAppearance.FlatButtons,
                DrawMode = TabDrawMode.OwnerDrawFixed, ItemSize = new Size(150, 34),
                SizeMode = TabSizeMode.Fixed, Padding = new Point(16, 6)
            };
            tabs.DrawItem += delegate(object sender, DrawItemEventArgs e)
            {
                TabPage page = tabs.TabPages[e.Index];
                bool selected = tabs.SelectedIndex == e.Index;
                using (var background = new SolidBrush(selected ? Color.FromArgb(18, 70, 78) : Panel))
                using (var textBrush = new SolidBrush(selected ? Color.White : Muted))
                using (var tabFont = new Font("Segoe UI Semibold", 10F))
                using (var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
                {
                    e.Graphics.FillRectangle(background, e.Bounds);
                    e.Graphics.DrawString(page.Text, tabFont, textBrush, e.Bounds, format);
                }
            };
            var settingsPage = new TabPage("SETTINGS") { BackColor = Surface, Padding = new Padding(4) };
            var debugPage = new TabPage("COMMISSIONING") { BackColor = Surface, Padding = new Padding(4) };
            tabs.TabPages.AddRange(new[] { settingsPage, debugPage });
            root.Controls.Add(tabs);

            var settingsLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
            settingsLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            settingsLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 112));
            settingsLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 118));
            settingsPage.Controls.Add(settingsLayout);

            var configurationBox = NewGroup("Inverter & RS485 settings  |  saved in StampPLC");
            var configurationLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
            configurationLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            configurationLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
            ConfigureGrid();
            configurationLayout.Controls.Add(_grid, 0, 0);
            var settingsFlow = NewFlow();
            _baud.Width = 90;
            _baud.Text = "19200";
            StyleTextBox(_baud);
            _register.Width = 90;
            _register.Text = "0x04E5";
            StyleTextBox(_register);
            settingsFlow.Controls.AddRange(new Control[]
            {
                NewTextLabel("RS485 baud"), _baud,
                NewTextLabel("Power register"), _register,
                NewButton("Save inverter settings", SaveAll),
                NewButton("Read SmartPLC settings", ReadSmartPlcSettings),
                NewTextLabel("Offline inverter settings remain pending")
            });
            configurationLayout.Controls.Add(settingsFlow, 0, 1);
            configurationBox.Controls.Add(configurationLayout);
            settingsLayout.Controls.Add(configurationBox, 0, 0);

            var wifiBox = NewGroup("Wi-Fi & automatic OTA");
            var wifiFlow = NewFlow();
            wifiFlow.Padding = new Padding(0, 5, 0, 0);
            _wifiSsid.Width = 170;
            _wifiSsid.DropDownStyle = ComboBoxStyle.DropDown;
            _wifiSsid.BackColor = Color.FromArgb(7, 13, 19);
            _wifiSsid.ForeColor = TextColor;
            _wifiSsid.FlatStyle = FlatStyle.Flat;
            _wifiPassword.Width = 160;
            _wifiPassword.UseSystemPasswordChar = true;
            StyleTextBox(_wifiPassword);
            _automaticOta.Text = "Enable automatic OTA";
            _automaticOta.AutoSize = true;
            _automaticOta.ForeColor = TextColor;
            _automaticOta.Padding = new Padding(8, 5, 4, 0);
            _automaticOta.CheckedChanged += AutomaticOtaChanged;
            _automaticOtaStatus.Text = "AUTO OTA: NOT READ";
            _automaticOtaStatus.AutoSize = true;
            _automaticOtaStatus.ForeColor = Muted;
            _automaticOtaStatus.Padding = new Padding(8, 7, 8, 0);
            _wifiStatus.Text = "Not read";
            _wifiStatus.AutoSize = true;
            _wifiStatus.ForeColor = Muted;
            _wifiStatus.Padding = new Padding(10, 7, 0, 0);
            wifiFlow.Controls.AddRange(new Control[]
            {
                NewTextLabel("SSID"), _wifiSsid,
                NewButton("Refresh PC Wi-Fi", delegate { RefreshWifiNetworks(); }),
                NewTextLabel("Password"), _wifiPassword,
                NewButton("Save & connect", SaveWifi),
                NewButton("Retry connection", delegate { Send("wifi connect"); }),
                _automaticOta, _automaticOtaStatus, _wifiStatus
            });
            wifiBox.Controls.Add(wifiFlow);
            settingsLayout.Controls.Add(wifiBox, 0, 1);

            var firmwareBox = NewGroup("SmartPLC firmware  |  device updates itself");
            var firmwareFlow = NewFlow();
            firmwareFlow.Padding = new Padding(0, 5, 0, 0);
            _flashFirmware.Text = "USB flash " + ReleaseVersion;
            StyleButton(_flashFirmware);
            _flashFirmware.AutoSize = true;
            _flashFirmware.Click += FlashFirmware;
            _firmwareStatus.Text = "Installed firmware: --";
            _firmwareStatus.AutoSize = true;
            _firmwareStatus.ForeColor = Muted;
            _firmwareStatus.Padding = new Padding(12, 7, 0, 0);
            _flashProgress.Size = new Size(52, 52);
            _flashProgress.Value = 0;
            _flashProgress.RingColor = Accent;
            _flashProgress.Margin = new Padding(12, 0, 4, 0);
            _flashProgressText.Text = "Ready";
            _flashProgressText.AutoSize = true;
            _flashProgressText.ForeColor = Muted;
            _flashProgressText.Padding = new Padding(4, 7, 0, 0);
            _otaDiagnosticStatus.Text = "OTA diagnostics: not read";
            _otaDiagnosticStatus.AutoSize = true;
            _otaDiagnosticStatus.MaximumSize = new Size(1040, 0);
            _otaDiagnosticStatus.ForeColor = Muted;
            _otaDiagnosticStatus.Padding = new Padding(12, 7, 0, 0);
            firmwareFlow.Controls.AddRange(new Control[]
            {
                _flashFirmware,
                NewButton("Check SmartPLC OTA", delegate { Send("ota check"); }),
                NewButton("Update SmartPLC OTA", InstallOta),
                _firmwareStatus, _flashProgress, _flashProgressText,
                _otaDiagnosticStatus
            });
            firmwareBox.Controls.Add(firmwareFlow);
            settingsLayout.Controls.Add(firmwareBox, 0, 2);

            var debugLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
            debugLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 56));
            debugLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
            debugLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 44));
            debugPage.Controls.Add(debugLayout);

            var displayBox = NewGroup("Live display  |  RES dashed / inverter readback filled");
            var displayLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
            displayLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 31));
            displayLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            var statusFlow = NewFlow();
            _rse.Text = "RSE: --";
            _mode.Text = "Mode: --";
            _result.Text = "Output: --";
            foreach (var label in new[] { _rse, _mode, _result })
            {
                label.AutoSize = true;
                label.Padding = new Padding(6, 7, 12, 0);
                label.ForeColor = TextColor;
            }
            statusFlow.Controls.AddRange(new Control[] { _rse, _mode, _result });
            displayLayout.Controls.Add(statusFlow, 0, 0);
            _gaugeFlow.Dock = DockStyle.Fill;
            _gaugeFlow.BackColor = Color.FromArgb(8, 13, 19);
            _gaugeFlow.WrapContents = false;
            _gaugeFlow.AutoScroll = true;
            _gaugeFlow.Padding = new Padding(5, 2, 5, 2);
            for (int i = 0; i < _gauges.Length; ++i)
            {
                _gauges[i] = new LiquidGauge(i + 1) { Visible = false };
                _gaugeFlow.Controls.Add(_gauges[i]);
            }
            displayLayout.Controls.Add(_gaugeFlow, 0, 1);
            displayBox.Controls.Add(displayLayout);
            debugLayout.Controls.Add(displayBox, 0, 0);

            var actionBox = NewGroup("Test & commissioning");
            var actionFlow = NewFlow();
            actionFlow.WrapContents = false;
            actionFlow.AutoSize = false;
            actionFlow.Controls.Add(NewButton("Scan all IDs", delegate { Send("scan all"); }));
            foreach (int level in new[] { 100, 60, 30, 0 })
            {
                int captured = level;
                actionFlow.Controls.Add(NewButton(captured + "% test", delegate { TestLevel(captured); }));
            }
            actionFlow.Controls.Add(NewButton("Enable LIVE", EnableLive));
            actionBox.Controls.Add(actionFlow);
            debugLayout.Controls.Add(actionBox, 0, 1);

            var logBox = NewGroup("Device event log");
            logBox.MinimumSize = new Size(0, 100);
            _log.Dock = DockStyle.Fill;
            _log.Multiline = true;
            _log.ReadOnly = true;
            _log.ScrollBars = ScrollBars.Both;
            _log.WordWrap = false;
            _log.Font = new Font("Consolas", 9F);
            _log.BackColor = Color.FromArgb(7, 11, 16);
            _log.ForeColor = Color.FromArgb(160, 240, 230);
            _log.BorderStyle = BorderStyle.FixedSingle;
            logBox.Controls.Add(_log);
            debugLayout.Controls.Add(logBox, 0, 2);
        }

        private void ConfigureGrid()
        {
            _grid.Dock = DockStyle.Fill;
            _grid.AllowUserToAddRows = false;
            _grid.AllowUserToDeleteRows = false;
            _grid.RowHeadersVisible = false;
            _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
            _grid.BackgroundColor = Color.FromArgb(8, 13, 19);
            _grid.BorderStyle = BorderStyle.None;
            _grid.GridColor = Color.FromArgb(55, 75, 90);
            _grid.EnableHeadersVisualStyles = false;
            _grid.ColumnHeadersDefaultCellStyle = new DataGridViewCellStyle
            {
                BackColor = Color.FromArgb(20, 48, 62), ForeColor = Accent,
                Font = new Font("Segoe UI Semibold", 9F), Alignment = DataGridViewContentAlignment.MiddleLeft
            };
            _grid.DefaultCellStyle = new DataGridViewCellStyle
            {
                BackColor = Panel, ForeColor = TextColor, SelectionBackColor = Color.FromArgb(0, 90, 100),
                SelectionForeColor = Color.White
            };
            _grid.CurrentCellDirtyStateChanged += delegate
            {
                if (_grid.IsCurrentCellDirty) _grid.CommitEdit(DataGridViewDataErrorContexts.Commit);
            };
            _grid.CellValueChanged += delegate(object sender, DataGridViewCellEventArgs e)
            {
                if (e.RowIndex >= 0 && _grid.Columns[e.ColumnIndex].Name == "Maximum")
                    _ratingIssues[e.RowIndex] = false;
                UpdateGauges();
            };
            _grid.CellPainting += delegate(object sender, DataGridViewCellPaintingEventArgs e)
            {
                if (e.RowIndex < 0 || e.ColumnIndex < 0 || !_ratingIssues[e.RowIndex] ||
                    _grid.Columns[e.ColumnIndex].Name != "Maximum") return;
                e.Paint(e.CellBounds, e.PaintParts);
                using (var pen = new Pen(Color.FromArgb(255, 70, 80), 2F))
                    e.Graphics.DrawRectangle(pen, e.CellBounds.X + 1, e.CellBounds.Y + 1,
                        e.CellBounds.Width - 3, e.CellBounds.Height - 3);
                e.Handled = true;
            };
            _grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "Id", HeaderText = "Modbus ID", ReadOnly = true, FillWeight = 55 });
            _grid.Columns.Add(new DataGridViewCheckBoxColumn { Name = "Enabled", HeaderText = "Control enabled", FillWeight = 80 });
            _grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "Maximum", HeaderText = "Maximum PV power (W)", FillWeight = 130 });
            _grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "Target", HeaderText = "Last target", ReadOnly = true });
            _grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "Readback", HeaderText = "Readback", ReadOnly = true });
            _grid.Columns.Add(new DataGridViewTextBoxColumn { Name = "Health", HeaderText = "Status", ReadOnly = true, FillWeight = 70 });
            for (int id = 1; id <= 6; ++id)
                _grid.Rows.Add(id.ToString(CultureInfo.InvariantCulture), false, "10000", "--", "--", "--");
        }

        private static GroupBox NewGroup(string title)
        {
            return new GroupBox
            {
                Text = title.ToUpperInvariant(), Dock = DockStyle.Fill, Padding = new Padding(10, 8, 10, 8),
                BackColor = Panel, ForeColor = Accent, FlatStyle = FlatStyle.Flat
            };
        }

        private static FlowLayoutPanel NewFlow()
        {
            return new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        }

        private static Button NewButton(string text, EventHandler action)
        {
            var button = new Button { Text = text, AutoSize = true, Margin = new Padding(4) };
            StyleButton(button);
            button.Click += action;
            return button;
        }

        private static void StyleButton(Button button)
        {
            button.FlatStyle = FlatStyle.Flat;
            button.BackColor = Color.FromArgb(18, 55, 68);
            button.ForeColor = TextColor;
            button.FlatAppearance.BorderColor = Accent;
            button.FlatAppearance.MouseOverBackColor = Color.FromArgb(0, 88, 96);
        }

        private static void StyleTextBox(TextBox textBox)
        {
            textBox.BackColor = Color.FromArgb(7, 13, 19);
            textBox.ForeColor = TextColor;
            textBox.BorderStyle = BorderStyle.FixedSingle;
        }

        private static Label NewTextLabel(string text)
        {
            return new Label { Text = text, AutoSize = true, ForeColor = Muted, Padding = new Padding(8, 7, 2, 0) };
        }

        private Control BuildHeader()
        {
            var header = new Panel { Dock = DockStyle.Fill, BackColor = Color.FromArgb(6, 10, 15), BorderStyle = BorderStyle.FixedSingle };
            var logo = new PictureBox { Dock = DockStyle.Right, Width = 185, SizeMode = PictureBoxSizeMode.Zoom, Cursor = Cursors.Hand, Margin = new Padding(8) };
            string logoPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mes_logo_light.png");
            if (File.Exists(logoPath)) logo.Image = Image.FromFile(logoPath);
            logo.Click += delegate { MessageBox.Show(this, "MES website link can be set after you provide the URL.", "MES"); };
            var title = new Label { Text = "14A  BRIDGE   " + ReleaseVersion, AutoSize = true, Location = new Point(18, 16), Font = new Font("Segoe UI Semibold", 16F), ForeColor = TextColor };
            var subtitle = new Label { Text = "STAMPPLC  |  USB CONFIGURATION CONSOLE  |  RS485 / MODBUS RTU", AutoSize = true, Location = new Point(21, 46), Font = new Font("Consolas", 9F), ForeColor = Accent };
            header.Controls.AddRange(new Control[] { logo, title, subtitle });
            return header;
        }

        private void ScanPorts()
        {
            if (_scanningPorts) return;
            _scanningPorts = true;

            string previous = SelectedPortName();
            string connectedPort = "";
            lock (_serialLock)
                if (_serial != null && _serial.IsOpen) connectedPort = _serial.PortName;

            string[] ports;
            try
            {
                ports = SerialPort.GetPortNames();
            }
            catch (Exception ex)
            {
                _scanningPorts = false;
                _connection.Text = "\u25CF  SERIAL PORT SCAN FAILED";
                _connection.ForeColor = Color.OrangeRed;
                AppendLog("[SCAN] Windows could not enumerate serial ports: " + ex.Message);
                return;
            }
            Array.Sort(ports, StringComparer.OrdinalIgnoreCase);
            _scanPorts.Enabled = false;
            _connect.Enabled = false;
            _ports.Enabled = false;
            if (string.IsNullOrEmpty(connectedPort))
            {
                _connection.Text = "\u25CF  SCANNING " + ports.Length + " SERIAL PORT" + (ports.Length == 1 ? "" : "S") + "...";
                _connection.ForeColor = Color.Gold;
            }
            AppendLog("[SCAN] Checking " + ports.Length + " serial port" + (ports.Length == 1 ? "" : "s") +
                " for SmartPLC identity (read-only).");

            ThreadPool.QueueUserWorkItem(delegate
            {
                var choices = new List<PortChoice>();
                int smartPlcCount = 0;
                foreach (string port in ports)
                {
                    if (!string.IsNullOrEmpty(connectedPort) &&
                        string.Equals(port, connectedPort, StringComparison.OrdinalIgnoreCase))
                    {
                        choices.Add(new PortChoice(port, true, ""));
                        smartPlcCount++;
                        continue;
                    }

                    string version;
                    bool identified = ProbeSmartPlcPort(port, out version);
                    choices.Add(new PortChoice(port, identified, version));
                    if (identified) smartPlcCount++;
                }
                choices.Sort(ComparePortChoices);

                if (IsDisposed || Disposing) return;
                try
                {
                    BeginInvoke(new Action(delegate
                    {
                        FinishPortScan(choices, previous, connectedPort, smartPlcCount);
                    }));
                }
                catch (InvalidOperationException) { }
            });
        }

        private static int ComparePortChoices(PortChoice left, PortChoice right)
        {
            if (left.IsSmartPlc != right.IsSmartPlc) return left.IsSmartPlc ? -1 : 1;
            int leftNumber = PortNumber(left.PortName);
            int rightNumber = PortNumber(right.PortName);
            int numeric = leftNumber.CompareTo(rightNumber);
            return numeric != 0 ? numeric : StringComparer.OrdinalIgnoreCase.Compare(left.PortName, right.PortName);
        }

        private static int PortNumber(string portName)
        {
            int number;
            return portName.StartsWith("COM", StringComparison.OrdinalIgnoreCase) &&
                int.TryParse(portName.Substring(3), out number) ? number : int.MaxValue;
        }

        private static bool TryParseSmartPlcIdentity(string response, out string version)
        {
            version = "";
            Match match = IdentityPattern.Match(response ?? "");
            if (!match.Success) match = LegacyIdentityPattern.Match(response ?? "");
            if (!match.Success) return false;
            version = match.Groups[1].Value;
            return true;
        }

        private static string ReadProbeResponse(SerialPort probe, int timeoutMilliseconds)
        {
            var response = new StringBuilder();
            var timer = Stopwatch.StartNew();
            while (timer.ElapsedMilliseconds < timeoutMilliseconds)
            {
                try
                {
                    string chunk = probe.ReadExisting();
                    if (!string.IsNullOrEmpty(chunk))
                    {
                        response.Append(chunk.Replace("\r", ""));
                        if (response.Length > 32768) break;
                    }
                }
                catch (InvalidOperationException) { break; }
                catch (IOException) { break; }
                Thread.Sleep(25);
            }
            return response.ToString();
        }

        private static bool ProbeSmartPlcPort(string portName, out string version)
        {
            version = "";
            try
            {
                using (var probe = new SerialPort(portName, 115200, Parity.None, 8, StopBits.One)
                {
                    NewLine = "\n",
                    ReadTimeout = 100,
                    WriteTimeout = 250,
                    DtrEnable = false,
                    RtsEnable = false
                })
                {
                    probe.Open();
                    Thread.Sleep(60);
                    probe.DiscardInBuffer();

                    // "identify" is read-only and unique to this project. V1.0.2
                    // devices without that command are detected by the read-only
                    // legacy "gui" response below.
                    probe.Write("identify\n");
                    string response = ReadProbeResponse(probe, 300);
                    if (TryParseSmartPlcIdentity(response, out version)) return true;

                    probe.Write("gui\n");
                    response += ReadProbeResponse(probe, 650);
                    return TryParseSmartPlcIdentity(response, out version);
                }
            }
            catch (UnauthorizedAccessException) { }
            catch (IOException) { }
            catch (InvalidOperationException) { }
            catch (TimeoutException) { }
            catch (ArgumentException) { }
            catch (Exception) { }
            return false;
        }

        private void FinishPortScan(List<PortChoice> choices, string previous,
            string connectedPort, int smartPlcCount)
        {
            _ports.BeginUpdate();
            _ports.Items.Clear();
            foreach (PortChoice choice in choices) _ports.Items.Add(choice);
            _ports.EndUpdate();

            int previousIndex = -1;
            int firstSmartPlc = -1;
            for (int i = 0; i < choices.Count; ++i)
            {
                if (firstSmartPlc < 0 && choices[i].IsSmartPlc) firstSmartPlc = i;
                if (string.Equals(choices[i].PortName, previous, StringComparison.OrdinalIgnoreCase))
                    previousIndex = i;
            }
            _ports.SelectedIndex = previousIndex >= 0 ? previousIndex : firstSmartPlc;

            _scanningPorts = false;
            _scanPorts.Enabled = true;
            _connect.Enabled = true;
            bool connected = !string.IsNullOrEmpty(connectedPort);
            _ports.Enabled = !connected;

            if (!connected)
            {
                if (smartPlcCount == 0)
                {
                    _connection.Text = "\u25CF  NO SMARTPLC FOUND  (" + choices.Count + " OTHER PORT" +
                        (choices.Count == 1 ? "" : "S") + ")";
                    _connection.ForeColor = choices.Count == 0 ? Muted : Color.Orange;
                }
                else if (smartPlcCount == 1)
                {
                    _connection.Text = "\u25CF  SMARTPLC FOUND  " + SelectedPortName();
                    _connection.ForeColor = Accent;
                }
                else
                {
                    _connection.Text = "\u25CF  " + smartPlcCount + " SMARTPLC DEVICES FOUND - SELECT ONE";
                    _connection.ForeColor = Accent;
                }
            }
            AppendLog("[SCAN] SmartPLC found=" + smartPlcCount + ", other/unavailable=" +
                (choices.Count - smartPlcCount) + ".");
        }

        private PortChoice SelectedPortChoice()
        {
            return _ports.SelectedItem as PortChoice;
        }

        private string SelectedPortName()
        {
            PortChoice choice = SelectedPortChoice();
            return choice == null ? "" : choice.PortName;
        }

        private void ToggleConnection(object sender, EventArgs e)
        {
            if (_serial != null && _serial.IsOpen)
            {
                Disconnect();
                return;
            }
            PortChoice selected = SelectedPortChoice();
            string selectedPort = SelectedPortName();
            if (string.IsNullOrWhiteSpace(selectedPort))
            {
                MessageBox.Show(this, "Scan and select an identified SmartPLC first.", "No SmartPLC selected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (selected == null || !selected.IsSmartPlc)
            {
                MessageBox.Show(this,
                    selectedPort + " did not answer the SmartPLC identity check.\r\n\r\n" +
                    "Connection is blocked to protect other serial devices. If this is a new, " +
                    "unprogrammed SmartPLC, use this port only with USB flash.",
                    "Unidentified serial port", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                _serial = new SerialPort(selectedPort, 115200, Parity.None, 8, StopBits.One)
                {
                    NewLine = "\n",
                    ReadTimeout = 200,
                    WriteTimeout = 1000
                };
                _serial.DataReceived += SerialDataReceived;
                _serial.Open();
                _connect.Text = "Disconnect";
                _ports.Enabled = false;
                _connection.Text = "\u25CF  CONNECTED  " + selectedPort;
                _connection.ForeColor = Accent;
                SetAutomaticOtaStatus(null);
                AppendLog("[PC] Connected to " + selectedPort);
                _statusTimer.Start();
                var timer = new System.Windows.Forms.Timer { Interval = 300 };
                timer.Tick += delegate { timer.Stop(); timer.Dispose(); Send("show"); };
                timer.Start();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Connection failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void FlashFirmware(object sender, EventArgs e)
        {
            if (_flashingFirmware) return;
            string selectedPort = SelectedPortName();
            if (string.IsNullOrWhiteSpace(selectedPort))
            {
                MessageBox.Show(this, "Select the COM port of the new StampPLC first.",
                    "No COM port", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string appDirectory = AppDomain.CurrentDomain.BaseDirectory;
            string firmwareDirectory = Path.Combine(appDirectory, "firmware");
            string bundledEsptool = Path.Combine(firmwareDirectory, "StampPLC_Esptool.exe");
            bool hasBundledFlasher = File.Exists(bundledEsptool) &&
                File.Exists(Path.Combine(firmwareDirectory, "bootloader.bin")) &&
                File.Exists(Path.Combine(firmwareDirectory, "partitions.bin")) &&
                File.Exists(Path.Combine(firmwareDirectory, "boot_app0.bin")) &&
                File.Exists(Path.Combine(firmwareDirectory, "firmware.bin"));
            string projectDirectory = Directory.GetParent(appDirectory).FullName;
            string workspaceDirectory = Directory.GetParent(projectDirectory).FullName;
            string platformIo = Path.Combine(workspaceDirectory, ".venv", "Scripts", "platformio.exe");
            bool hasEngineeringFlasher = File.Exists(Path.Combine(projectDirectory, "platformio.ini")) &&
                File.Exists(platformIo);
            if (!hasBundledFlasher && !hasEngineeringFlasher)
            {
                MessageBox.Show(this,
                    "Firmware files are missing. Reinstall the 14a Bridge package.",
                    "Flashing runtime not found", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (MessageBox.Show(this,
                "SmartPLC firmware " + ReleaseVersion + " will be written to " + selectedPort + ".\r\n" +
                "The USB connection will be closed during flashing. Continue?",
                "Flash StampPLC firmware", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;

            Disconnect();
            _flashingFirmware = true;
            _flashFirmware.Enabled = false;
            _connection.Text = "\u25CF  FLASHING " + selectedPort;
            _connection.ForeColor = Color.FromArgb(255, 184, 55);
            _flashProgress.Indeterminate = true;
            _flashProgress.RingColor = Color.FromArgb(255, 184, 55);
            _flashProgressText.Text = "Connecting / preparing...";
            _flashProgressText.ForeColor = Color.FromArgb(255, 184, 55);
            AppendLog("[FLASH] Starting firmware update on " + selectedPort);
            string port = selectedPort;
            ThreadPool.QueueUserWorkItem(delegate
            {
                int exitCode = -1;
                int lastReportedProgress = -1;
                long[] imageSizes = hasBundledFlasher
                    ? new long[]
                    {
                        new FileInfo(Path.Combine(firmwareDirectory, "bootloader.bin")).Length,
                        new FileInfo(Path.Combine(firmwareDirectory, "partitions.bin")).Length,
                        new FileInfo(Path.Combine(firmwareDirectory, "boot_app0.bin")).Length,
                        new FileInfo(Path.Combine(firmwareDirectory, "firmware.bin")).Length
                    }
                    : new long[] { 1 };
                long totalImageBytes = 0;
                foreach (long imageSize in imageSizes) totalImageBytes += imageSize;
                var diagnosticTail = new System.Collections.Generic.Queue<string>();
                object diagnosticLock = new object();
                Action<string> reportFlashLine = delegate(string line)
                {
                    Match progress = Regex.Match(line, @"^Writing at 0x([0-9A-Fa-f]+).*\((\d+) %\)$");
                    if (progress.Success)
                    {
                        long address = long.Parse(progress.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                        int phasePercent = int.Parse(progress.Groups[2].Value, CultureInfo.InvariantCulture);
                        int phase = address < 0x8000 ? 0 : address < 0xE000 ? 1 : address < 0x10000 ? 2 : 3;
                        long completedBytes = 0;
                        for (int i = 0; i < phase && i < imageSizes.Length; ++i) completedBytes += imageSizes[i];
                        if (phase < imageSizes.Length)
                            completedBytes += imageSizes[phase] * phasePercent / 100;
                        int overall = !hasBundledFlasher || totalImageBytes == 0 ? phasePercent :
                            (int)Math.Max(0, Math.Min(100, completedBytes * 100 / totalImageBytes));
                        if (overall == lastReportedProgress) return;
                        lastReportedProgress = overall;
                        BeginInvoke(new Action(delegate
                        {
                            _flashProgress.Indeterminate = false;
                            _flashProgress.RingColor = Accent;
                            _flashProgress.Value = overall;
                            _flashProgressText.Text = "Writing " + overall + "%";
                            _flashProgressText.ForeColor = TextColor;
                            if (overall % 5 == 0 || overall == 100)
                                AppendLog("[FLASH] Overall write progress: " + overall + "%");
                        }));
                        return;
                    }
                    lock (diagnosticLock)
                    {
                        diagnosticTail.Enqueue(line);
                        while (diagnosticTail.Count > 10) diagnosticTail.Dequeue();
                    }
                    BeginInvoke(new Action<string>(AppendLog), "[FLASH] " + line);
                };
                try
                {
                    var info = new ProcessStartInfo
                    {
                        FileName = hasBundledFlasher ? bundledEsptool : platformIo,
                        Arguments = hasBundledFlasher
                            ? "--chip esp32s3 --port " + port +
                              " --baud 460800 --before default_reset --after hard_reset --no-stub write_flash" +
                              " --no-compress --verify" +
                              " --flash_mode dio --flash_freq 80m --flash_size 8MB" +
                              " 0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin"
                            : "run -t upload --upload-port " + port,
                        WorkingDirectory = hasBundledFlasher ? firmwareDirectory : projectDirectory,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true
                    };
                    using (var process = new Process { StartInfo = info })
                    {
                        process.OutputDataReceived += delegate(object s, DataReceivedEventArgs a)
                        {
                            if (!string.IsNullOrEmpty(a.Data)) reportFlashLine(a.Data);
                        };
                        process.ErrorDataReceived += delegate(object s, DataReceivedEventArgs a)
                        {
                            if (!string.IsNullOrEmpty(a.Data)) reportFlashLine(a.Data);
                        };
                        process.Start();
                        process.BeginOutputReadLine();
                        process.BeginErrorReadLine();
                        process.WaitForExit();
                        exitCode = process.ExitCode;
                    }
                }
                catch (Exception ex)
                {
                    BeginInvoke(new Action<string>(AppendLog), "[FLASH ERROR] " + ex.Message);
                }
                BeginInvoke(new Action(delegate
                {
                    _flashingFirmware = false;
                    _flashFirmware.Enabled = true;
                    ScanPorts();
                    _connection.Text = exitCode == 0 ? "\u25CF  FLASH COMPLETE" : "\u25CF  FLASH FAILED";
                    _connection.ForeColor = exitCode == 0 ? Accent : Color.OrangeRed;
                    _flashProgress.Indeterminate = false;
                    _flashProgress.RingColor = exitCode == 0 ? Accent : Color.OrangeRed;
                    if (exitCode == 0) _flashProgress.Value = 100;
                    else if (lastReportedProgress >= 0) _flashProgress.Value = lastReportedProgress;
                    _flashProgressText.Text = exitCode == 0 ? "Complete & verified" :
                        "Failed" + (lastReportedProgress >= 0 ? " at " + lastReportedProgress + "%" : "");
                    _flashProgressText.ForeColor = exitCode == 0 ? Accent : Color.OrangeRed;
                    AppendLog(exitCode == 0
                        ? "[FLASH] Completed. Reconnect to read the new StampPLC."
                        : "[FLASH] Failed. Check the COM port and USB cable, then retry.");
                    if (exitCode == 0)
                    {
                        MessageBox.Show(this,
                            "SmartPLC firmware " + ReleaseVersion + " was written and verified successfully.\r\n\r\n" +
                            "Saved inverter and RS485 settings remain unchanged. Reconnect USB to continue.",
                            "Firmware flash complete", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    }
                    else
                    {
                        string details;
                        lock (diagnosticLock) details = string.Join("\r\n", diagnosticTail.ToArray());
                        MessageBox.Show(this,
                            "Firmware flashing failed.\r\n\r\n" + details +
                            "\r\n\r\nThe complete output is available on the Commissioning tab.",
                            "Firmware flash failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                }));
            });
        }

        internal void ShowFlashProgressPreview()
        {
            _flashProgress.Indeterminate = false;
            _flashProgress.RingColor = Accent;
            _flashProgress.Value = 64;
            _flashProgressText.Text = "Writing 64%";
            _flashProgressText.ForeColor = TextColor;
        }

        private void Disconnect()
        {
            _statusTimer.Stop();
            lock (_serialLock)
            {
                if (_serial != null)
                {
                    try
                    {
                        _serial.DataReceived -= SerialDataReceived;
                        if (_serial.IsOpen) _serial.Close();
                        _serial.Dispose();
                    }
                    catch { }
                    _serial = null;
                }
            }
            _connect.Text = "Connect";
            _ports.Enabled = !_scanningPorts;
            _connection.Text = "\u25CF  DISCONNECTED";
            _connection.ForeColor = Muted;
            SetAutomaticOtaStatus(null);
        }

        private void SerialDataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            try
            {
                string chunk = _serial.ReadExisting();
                lock (_receiveBuffer)
                {
                    _receiveBuffer.Append(chunk.Replace("\r", ""));
                    while (true)
                    {
                        string all = _receiveBuffer.ToString();
                        int newline = all.IndexOf('\n');
                        if (newline < 0) break;
                        string line = all.Substring(0, newline);
                        _receiveBuffer.Remove(0, newline + 1);
                        BeginInvoke(new Action<string>(HandleLine), line);
                    }
                }
            }
            catch (Exception ex)
            {
                BeginInvoke(new Action<string>(delegate(string message)
                {
                    AppendLog("[USB ERROR] " + message);
                    Disconnect();
                }), ex.Message);
            }
        }

        private bool Send(string command)
        {
            lock (_serialLock)
            {
                if (_serial == null || !_serial.IsOpen)
                {
                    MessageBox.Show(this, "Connect to the StampPLC first.", "Not connected",
                        MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return false;
                }
                try
                {
                    _serial.WriteLine(command);
                    AppendLog("[PC] > " + command);
                    return true;
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, ex.Message, "USB error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return false;
                }
            }
        }

        private void SendBackgroundStatus()
        {
            lock (_serialLock)
            {
                if (_savingSettings || _serial == null || !_serial.IsOpen) return;
                try { _serial.WriteLine("gui"); }
                catch { }
            }
        }

        private void SaveAll(object sender, EventArgs e)
        {
            var commands = new string[9];
            int commandIndex = 2;
            for (int row = 0; row < 6; ++row)
            {
                bool enabled = Convert.ToBoolean(_grid.Rows[row].Cells["Enabled"].Value ?? false);
                uint maximum;
                if (!uint.TryParse(Convert.ToString(_grid.Rows[row].Cells["Maximum"].Value), out maximum) || maximum == 0)
                {
                    MessageBox.Show(this, "ID " + (row + 1) + " maximum power is invalid.",
                        "Invalid setting", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                commands[commandIndex++] = "commit " + (row + 1) + " " + (enabled ? "on" : "off") + " " + maximum;
            }
            uint baud;
            if (!uint.TryParse(_baud.Text.Trim(), out baud) || baud < 1200 || baud > 1000000)
            {
                MessageBox.Show(this, "RS485 baud is invalid.", "Invalid setting");
                return;
            }
            ushort address;
            if (!TryParseUShort(_register.Text.Trim(), out address))
            {
                MessageBox.Show(this, "Power register must be 0..65535 or 0x0000..0xFFFF.", "Invalid setting");
                return;
            }
            commands[0] = "baud " + baud;
            commands[1] = "reg 0x" + address.ToString("X4");
            commands[commandIndex++] = "show";
            if (_serial == null || !_serial.IsOpen) { Send("show"); return; }
            ThreadPool.QueueUserWorkItem(delegate
            {
                _savingSettings = true;
                try
                {
                    foreach (string command in commands)
                    {
                        bool isCommit = command.StartsWith("commit ", StringComparison.Ordinal);
                        if (isCommit) _commitResponse.Reset();
                        lock (_serialLock)
                        {
                            if (_serial == null || !_serial.IsOpen) return;
                            try { _serial.WriteLine(command); }
                            catch { return; }
                        }
                        BeginInvoke(new Action<string>(AppendLog), "[PC] > " + command);
                        if (isCommit)
                        {
                            // A rating check performs FC03, a temporary
                            // FC16+FC03 validation and a restore.  Waiting
                            // for its actual response avoids queueing the
                            // next inverter while a slow device is busy.
                            if (!_commitResponse.WaitOne(6500))
                                BeginInvoke(new Action<string>(AppendLog),
                                    "[PC] commit response timeout; continuing with next ID");
                        }
                        else Thread.Sleep(80);
                    }
                }
                finally { _savingSettings = false; }
            });
        }

        private void ReadSmartPlcSettings(object sender, EventArgs e)
        {
            if (_serial == null || !_serial.IsOpen)
            {
                MessageBox.Show(this, "Connect to the SmartPLC by USB first.", "Not connected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            _result.Text = "Reading SmartPLC settings...";
            _wifiStatus.Text = "Reading...";
            SetAutomaticOtaStatus(null);
            Send("show");
            var timer = new System.Windows.Forms.Timer { Interval = 180 };
            timer.Tick += delegate
            {
                timer.Stop();
                timer.Dispose();
                Send("wifi show");
            };
            timer.Start();
        }

        private static bool TryParseUShort(string value, out ushort result)
        {
            if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return ushort.TryParse(value.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out result);
            return ushort.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out result);
        }

        private void SyncTime(object sender, EventArgs e)
        {
            Send("time " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        }

        private static string Utf8Hex(string value)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(value);
            var result = new StringBuilder(bytes.Length * 2);
            foreach (byte valueByte in bytes) result.Append(valueByte.ToString("X2", CultureInfo.InvariantCulture));
            return result.ToString();
        }

        private static string HexUtf8(string value)
        {
            if ((value.Length & 1) != 0) return "";
            try
            {
                byte[] bytes = new byte[value.Length / 2];
                for (int i = 0; i < bytes.Length; ++i)
                    bytes[i] = byte.Parse(value.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                return Encoding.UTF8.GetString(bytes);
            }
            catch { return ""; }
        }

        private static string FormatUnixTime(string value)
        {
            long seconds;
            if (!long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out seconds) || seconds <= 0) return "never";
            try
            {
                return new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)
                    .AddSeconds(seconds).ToLocalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
            }
            catch { return "invalid time"; }
        }

        private static string RunNetsh(string arguments)
        {
            var info = new ProcessStartInfo
            {
                FileName = "netsh.exe",
                Arguments = arguments,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (var process = Process.Start(info))
            {
                string output = process.StandardOutput.ReadToEnd();
                process.WaitForExit(8000);
                return output;
            }
        }

        private void RefreshWifiNetworks()
        {
            _wifiStatus.Text = "Reading Wi-Fi networks from this PC...";
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    string interfaces = RunNetsh("wlan show interfaces");
                    string networks = RunNetsh("wlan show networks mode=bssid");
                    string profiles = RunNetsh("wlan show profiles");
                    Match currentMatch = Regex.Match(interfaces, @"(?m)^\s*SSID\s*:\s*(.+?)\s*$");
                    string current = currentMatch.Success ? currentMatch.Groups[1].Value.Trim() : "";
                    var names = new System.Collections.Generic.List<string>();
                    if (!string.IsNullOrWhiteSpace(current)) names.Add(current);
                    foreach (Match match in Regex.Matches(networks, @"(?m)^\s*SSID\s+\d+\s*:\s*(.*?)\s*$"))
                    {
                        string name = match.Groups[1].Value.Trim();
                        if (!string.IsNullOrWhiteSpace(name) && !names.Contains(name)) names.Add(name);
                    }
                    // Windows 11 can hide live WLAN scan results when Location
                    // access is disabled. Saved profile names remain available
                    // and provide a useful selection list without reading keys.
                    foreach (Match match in Regex.Matches(profiles, @"(?m)^\s*[^:\r\n]+:\s*(.*?)\s*$"))
                    {
                        string name = match.Groups[1].Value.Trim();
                        if (!string.IsNullOrWhiteSpace(name) && name != "<None>" && !names.Contains(name))
                            names.Add(name);
                    }
                    BeginInvoke(new Action(delegate
                    {
                        string typed = _wifiSsid.Text;
                        _wifiSsid.Items.Clear();
                        _wifiSsid.Items.AddRange(names.ToArray());
                        if (!string.IsNullOrWhiteSpace(current)) _wifiSsid.Text = current;
                        else if (!string.IsNullOrWhiteSpace(typed)) _wifiSsid.Text = typed;
                        _wifiStatus.Text = names.Count == 0
                            ? "No PC Wi-Fi network found; SSID can still be typed"
                            : "PC Wi-Fi list refreshed" + (string.IsNullOrWhiteSpace(current)
                                ? "; choose a saved profile (enable Windows Location to identify the current SSID)"
                                : "; connected: " + current);
                    }));
                }
                catch (Exception ex)
                {
                    BeginInvoke(new Action(delegate
                    {
                        _wifiStatus.Text = "Could not read PC Wi-Fi: " + ex.Message;
                        _wifiStatus.ForeColor = Color.FromArgb(255, 184, 55);
                    }));
                }
            });
        }

        private static Version ParseReleaseVersion(string value)
        {
            Version parsed;
            return Version.TryParse(value.Trim().TrimStart('V', 'v'), out parsed)
                ? parsed : new Version(0, 0, 0);
        }

        private void ScheduleGuiUpdateCheck()
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                Thread.Sleep(2500);
                try
                {
                    ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
                    string json;
                    using (var client = new WebClient())
                    {
                        client.Headers[HttpRequestHeader.UserAgent] = "14a-Bridge-GUI-" + ReleaseVersion;
                        json = client.DownloadString("https://api.github.com/repos/tatsuo25103/14a-bridge/releases/latest");
                    }
                    Match match = Regex.Match(json, "\\\"tag_name\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
                    if (!match.Success) return;
                    string latest = match.Groups[1].Value.ToUpperInvariant();
                    if (ParseReleaseVersion(latest) <= ParseReleaseVersion(ReleaseVersion)) return;
                    string ignored = "";
                    using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\MES\14a Bridge"))
                        if (key != null) ignored = Convert.ToString(key.GetValue("IgnoredGuiVersion", ""));
                    if (string.Equals(ignored, latest, StringComparison.OrdinalIgnoreCase)) return;
                    if (IsDisposed || !IsHandleCreated) return;
                    BeginInvoke(new Action(delegate
                    {
                        using (var dialog = new GuiUpdatePrompt(ReleaseVersion, latest))
                        {
                            DialogResult result = dialog.ShowDialog(this);
                            if (dialog.DoNotRemind)
                            {
                                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(@"Software\MES\14a Bridge"))
                                    if (key != null) key.SetValue("IgnoredGuiVersion", latest, RegistryValueKind.String);
                            }
                            if (result == DialogResult.Yes)
                            {
                                Process.Start(new ProcessStartInfo
                                {
                                    FileName = "https://github.com/tatsuo25103/14a-bridge/releases/latest",
                                    UseShellExecute = true
                                });
                            }
                        }
                    }));
                }
                catch
                {
                    // GUI update checks must never interrupt configuration or
                    // show an error merely because the PC is offline.
                }
            });
        }

        private void SaveWifi(object sender, EventArgs e)
        {
            string ssid = _wifiSsid.Text;
            string password = _wifiPassword.Text;
            int ssidBytes = Encoding.UTF8.GetByteCount(ssid);
            int passwordBytes = Encoding.UTF8.GetByteCount(password);
            if (ssidBytes < 1 || ssidBytes > 32)
            {
                MessageBox.Show(this, "Wi-Fi SSID must contain 1 to 32 bytes.", "Invalid Wi-Fi setting",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (passwordBytes != 0 && (passwordBytes < 8 || passwordBytes > 63))
            {
                MessageBox.Show(this, "Wi-Fi password must be empty for an open network, or contain 8 to 63 bytes.",
                    "Invalid Wi-Fi setting", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            string passwordHex = passwordBytes == 0 ? "-" : Utf8Hex(password);
            if (Send("wifi sethex " + Utf8Hex(ssid) + " " + passwordHex))
            {
                _wifiStatus.Text = "Connecting to " + ssid + "...";
                var timer = new System.Windows.Forms.Timer { Interval = 2500 };
                timer.Tick += delegate { timer.Stop(); timer.Dispose(); Send("wifi show"); };
                timer.Start();
            }
        }

        private void AutomaticOtaChanged(object sender, EventArgs e)
        {
            if (_updatingWifiUi) return;
            bool requested = _automaticOta.Checked;
            if (requested)
            {
                if (MessageBox.Show(this,
                    "When enabled, the SmartPLC itself checks GitHub once after startup and every 24 hours. " +
                    "An update is installed only while Wi-Fi is connected, the RSE input is valid, and Modbus control is idle. Continue?",
                    "Enable automatic OTA", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                {
                    _updatingWifiUi = true;
                    _automaticOta.Checked = false;
                    _updatingWifiUi = false;
                    return;
                }
            }
            // Capture the operator's choice before opening the confirmation
            // dialog. A background status packet can arrive while that modal
            // dialog is open and must not turn an ON request into OFF.
            _pendingAutomaticOta = requested;
            _updatingWifiUi = true;
            _automaticOta.Checked = requested;
            _updatingWifiUi = false;
            if (Send("ota auto " + (requested ? "on" : "off")))
            {
                _automaticOtaStatus.Text = "AUTO OTA: SAVING...";
                _automaticOtaStatus.ForeColor = Color.FromArgb(255, 184, 55);
            }
            else _pendingAutomaticOta = null;
        }

        private void SetAutomaticOtaStatus(bool? enabled)
        {
            _automaticOtaStatus.Text = !enabled.HasValue ? "AUTO OTA: NOT READ" :
                (enabled.Value ? "AUTO OTA: ON" : "AUTO OTA: OFF");
            _automaticOtaStatus.ForeColor = !enabled.HasValue ? Muted :
                (enabled.Value ? Accent : Color.FromArgb(255, 184, 55));
        }

        private void InstallOta(object sender, EventArgs e)
        {
            if (MessageBox.Show(this,
                "Download and install the latest firmware from the official GitHub release?\r\n\r\n" +
                "The SmartPLC itself will use its saved Wi-Fi connection to download and install the update. " +
                "The Windows GUI is not being updated.\r\n\r\nKeep the StampPLC powered.",
                "Update SmartPLC firmware by OTA", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            _firmwareStatus.Text = "SmartPLC OTA in progress...";
            Send("ota update CONFIRM");
        }

        private void EnableLive(object sender, EventArgs e)
        {
            if (MessageBox.Show(this,
                "This enables real Modbus writes to every checked inverter ID. Continue?",
                "Enable live control", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
            {
                ClearManualTestDisplay();
                Send("dryrun off CONFIRM");
                DelayedShow();
            }
        }

        private void ClearManualTestDisplay()
        {
            // A test button can leave a temporary level in the dashboard.
            // LIVE must always return to the physical RSE input, never reuse
            // that GUI-only test display.
            _rsePercent = 0;
            _rse.Text = "RSE: reading actual input...";
            _result.Text = "LIVE enabled: reading physical RSE";
            for (int row = 0; row < _grid.Rows.Count; ++row)
                _grid.Rows[row].Cells["Target"].Value = "--";
            UpdateGauges();
        }

        private void TestLevel(int level)
        {
            string command = "test " + level;
            if (_dryRun == false)
            {
                if (MessageBox.Show(this,
                    "Write the calculated " + level + "% limit to all enabled inverter IDs?",
                    "Live inverter test", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
                command += " CONFIRM";
            }
            Send(command);
            DelayedShow();
        }

        private void ApplyCurrent(object sender, EventArgs e)
        {
            string command = "apply";
            if (_dryRun == false)
            {
                if (MessageBox.Show(this,
                    "Apply the current RSE level to all enabled inverter IDs?",
                    "Apply current RSE", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
                command += " CONFIRM";
            }
            Send(command);
            DelayedShow();
        }

        private void DelayedShow()
        {
            var timer = new System.Windows.Forms.Timer { Interval = 500 };
            timer.Tick += delegate { timer.Stop(); timer.Dispose(); Send("show"); };
            timer.Start();
        }

        private void HandleLine(string line)
        {
            bool background = line.StartsWith("@ ", StringComparison.Ordinal);
            if (background) line = line.Substring(2);
            else AppendLog(line);
            Match match = RsePattern.Match(line.Trim());
            if (match.Success)
            {
                _rse.Text = "RSE: " + match.Groups[2].Value + " (" + match.Groups[1].Value + ")";
                int.TryParse(match.Groups[2].Value.TrimEnd('%'), out _rsePercent);
                UpdateGauges();
                return;
            }
            match = ModePattern.Match(line.Trim());
            if (match.Success)
            {
                _mode.Text = "Mode: " + match.Groups[1].Value;
                _dryRun = match.Groups[1].Value == "DRY-RUN";
                _baud.Text = match.Groups[2].Value;
                _register.Text = match.Groups[3].Value.ToUpperInvariant();
                return;
            }
            match = WifiPattern.Match(line.Trim());
            if (match.Success)
            {
                string version = match.Groups[1].Value;
                bool saved = match.Groups[2].Value == "yes";
                bool connected = match.Groups[3].Value == "yes";
                bool automatic = match.Groups[4].Value == "yes";
                string ssid = HexUtf8(match.Groups[5].Value);
                if (!_wifiSsid.Focused && saved) _wifiSsid.Text = ssid;
                bool staleWhileSaving = _pendingAutomaticOta.HasValue &&
                    automatic != _pendingAutomaticOta.Value;
                if (!staleWhileSaving)
                {
                    if (_pendingAutomaticOta.HasValue) _pendingAutomaticOta = null;
                    _updatingWifiUi = true;
                    _automaticOta.Checked = automatic;
                    _updatingWifiUi = false;
                    SetAutomaticOtaStatus(automatic);
                }
                _firmwareStatus.Text = "Installed firmware: V" + version;
                _wifiStatus.Text = connected
                    ? "Connected: " + ssid + "  |  " + match.Groups[6].Value + "  |  " + match.Groups[7].Value + " dBm"
                    : (saved ? "Saved: " + ssid + "  |  not connected" : "No Wi-Fi saved");
                _wifiStatus.ForeColor = connected ? Accent : Color.FromArgb(255, 184, 55);
                return;
            }
            match = OtaAutoPattern.Match(line.Trim());
            if (match.Success)
            {
                bool enabled = match.Groups[1].Value == "yes";
                bool ok = match.Groups[2].Value == "OK";
                _pendingAutomaticOta = null;
                if (ok)
                {
                    _updatingWifiUi = true;
                    _automaticOta.Checked = enabled;
                    _updatingWifiUi = false;
                    SetAutomaticOtaStatus(enabled);
                }
                else
                {
                    _automaticOtaStatus.Text = "AUTO OTA: SAVE FAILED";
                    _automaticOtaStatus.ForeColor = Color.OrangeRed;
                }
                return;
            }
            match = OtaPattern.Match(line.Trim());
            if (match.Success)
            {
                _firmwareStatus.Text = "Installed V" + match.Groups[2].Value +
                    "  |  Available " + match.Groups[3].Value + "  |  " + match.Groups[4].Value;
                _firmwareStatus.ForeColor = match.Groups[1].Value == "ERROR" ? Color.OrangeRed : Accent;
                return;
            }
            match = OtaDiagnosticPattern.Match(line.Trim());
            if (match.Success)
            {
                string status = match.Groups[1].Value;
                string detail = HexUtf8(match.Groups[6].Value);
                string lastCheck = FormatUnixTime(match.Groups[2].Value);
                string lastSuccess = FormatUnixTime(match.Groups[3].Value);
                _otaDiagnosticStatus.Text = "OTA " + status + "  |  checked " + lastCheck +
                    "  |  last success " + lastSuccess + "  |  failures " + match.Groups[4].Value +
                    "  |  next " + match.Groups[5].Value + " s" +
                    (string.IsNullOrWhiteSpace(detail) ? "" : "  |  " + detail);
                _otaDiagnosticStatus.ForeColor = status == "ERROR" ? Color.OrangeRed :
                    (status == "NEVER" ? Muted : Accent);
                return;
            }
            match = IdPattern.Match(line.Trim());
            if (match.Success)
            {
                int row = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) - 1;
                // Background polling must never overwrite settings that the
                // operator has just ticked or typed but not saved yet.
                if (!background)
                {
                    _grid.Rows[row].Cells["Enabled"].Value = match.Groups[2].Value == "yes";
                    _grid.Rows[row].Cells["Maximum"].Value = match.Groups[3].Value;
                }
                _grid.Rows[row].Cells["Target"].Value = match.Groups[4].Value + " W";
                _grid.Rows[row].Cells["Readback"].Value = match.Groups[5].Value + " W";
                if (!background || !_ratingIssues[row])
                    _grid.Rows[row].Cells["Health"].Value = match.Groups[6].Value == "yes" ? "OK" : "CHECK";
                UpdateGauges();
                return;
            }
            match = ProbePattern.Match(line.Trim());
            if (match.Success)
            {
                int row = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) - 1;
                _grid.Rows[row].Cells["Readback"].Value = match.Groups[3].Value + " W";
                string probeStatus = match.Groups[4].Value;
                _grid.Rows[row].Cells["Health"].Value = probeStatus == "OK"
                    ? "FC03 OK" : (probeStatus == "RETRY" ? "RETRY" : "ERROR");
                _result.Text = "ID " + match.Groups[1].Value + ": " + match.Groups[4].Value + " - " + match.Groups[5].Value;
                UpdateGauges();
                return;
            }
            match = ScanPattern.Match(line.Trim());
            if (match.Success)
            {
                int row = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) - 1;
                string status = match.Groups[3].Value;
                if (status == "FOUND")
                    _grid.Rows[row].Cells["Readback"].Value = match.Groups[2].Value + " W";
                _grid.Rows[row].Cells["Health"].Value = status == "FOUND" ? "FOUND" :
                    (status == "NO_RESPONSE" ? "--" : "ERROR");
                _result.Text = "Scan ID " + match.Groups[1].Value + ": " + status +
                    (status == "FOUND" ? "  " + match.Groups[2].Value + " W" : "");
                UpdateGauges();
                return;
            }
            match = CommitPattern.Match(line.Trim());
            if (match.Success)
            {
                int row = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) - 1;
                string status = match.Groups[2].Value;
                // CLAMPED is not an unsaved error: the controller has adopted
                // the inverter's proven ceiling and returns that value here.
                _ratingIssues[row] = status == "ERROR";
                if (status != "ERROR") _grid.Rows[row].Cells["Maximum"].Value = match.Groups[3].Value;
                _grid.Rows[row].Cells["Health"].Value = status == "CLAMPED" ? "LIMITED" : status;
                _result.Text = "ID " + match.Groups[1].Value + ": " + status + " - " + match.Groups[4].Value;
                _commitResponse.Set();
                if (status == "CLAMPED")
                    MessageBox.Show(this, "ID " + match.Groups[1].Value + " accepted a lower maximum. The setting has been changed automatically to that inverter limit.\r\n\r\n" + match.Groups[4].Value, "Inverter rating adjusted", MessageBoxButtons.OK, MessageBoxIcon.Information);
                else if (status == "ERROR")
                    MessageBox.Show(this, "ID " + match.Groups[1].Value + " rating validation failed. The previous configuration was kept.\r\n\r\n" + match.Groups[4].Value, "Inverter validation error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                UpdateGauges();
                return;
            }
            if (line.StartsWith("PROBE SUMMARY") || line.StartsWith("SCAN SUMMARY")) _result.Text = line;
            if (line.StartsWith("Last result:")) _result.Text = line;
            if (line.StartsWith("WIFI STATUS="))
            {
                _wifiStatus.Text = line;
                _wifiStatus.ForeColor = line.Contains("ERROR") ? Color.OrangeRed : Accent;
            }
            if (line.StartsWith("OTA "))
            {
                _firmwareStatus.Text = line;
                _firmwareStatus.ForeColor = line.Contains("ERROR") ? Color.OrangeRed : Accent;
            }
        }

        private void UpdateGauges()
        {
            for (int row = 0; row < _gauges.Length && row < _grid.Rows.Count; ++row)
            {
                bool enabled = Convert.ToBoolean(_grid.Rows[row].Cells["Enabled"].Value ?? false);
                uint maximum = ParseWatts(_grid.Rows[row].Cells["Maximum"].Value);
                uint readback = ParseWatts(_grid.Rows[row].Cells["Readback"].Value);
                string health = Convert.ToString(_grid.Rows[row].Cells["Health"].Value) ?? "--";
                _gauges[row].SetState(enabled, _rsePercent, maximum, readback, health);
            }
        }

        private static uint ParseWatts(object value)
        {
            string text = Convert.ToString(value) ?? "0";
            Match match = Regex.Match(text, @"\d+");
            uint watts;
            return match.Success && uint.TryParse(match.Value, out watts) ? watts : 0;
        }

        private void AppendLog(string line)
        {
            _log.AppendText(line + Environment.NewLine);
            _log.SelectionStart = _log.TextLength;
            _log.ScrollToCaret();
        }

        internal static bool SelfTest()
        {
            string version;
            var choices = new List<PortChoice>
            {
                new PortChoice("COM1", false, ""),
                new PortChoice("COM7", true, "1.0.2"),
                new PortChoice("COM2", true, "1.0.1")
            };
            choices.Sort(ComparePortChoices);
            bool multipleDeviceOrder = choices[0].PortName == "COM2" && choices[0].IsSmartPlc &&
                choices[1].PortName == "COM7" && choices[1].IsSmartPlc &&
                choices[2].PortName == "COM1" && !choices[2].IsSmartPlc;
            return RsePattern.IsMatch("RSE DI mask: 0x02  level: 60%") &&
                   ModePattern.IsMatch("Mode: DRY-RUN  RS485: 19200 baud  register: 0x04E5  quantity: 2") &&
                   IdPattern.IsMatch("3   yes        10000  10000   6000   3000      0     6000      6000  yes") &&
                   ProbePattern.IsMatch("PROBE ID=3 REGISTER=0x04E5 VALUE=10000 STATUS=OK DETAIL=readback verified") &&
                   WifiPattern.IsMatch("WIFI VERSION=1.0.1 SAVED=yes CONNECTED=yes AUTO=no SSIDHEX=4D4553 IP=192.168.1.2 RSSI=-52") &&
                   OtaAutoPattern.IsMatch("OTA AUTO=yes STATUS=OK DETAIL=saved") &&
                   OtaAutoPattern.IsMatch("OTA AUTO=no STATUS=ERROR DETAIL=NVS save failed") &&
                   OtaDiagnosticPattern.IsMatch("OTA LAST=ERROR CHECK=1786455000 SUCCESS=0 FAILS=2 NEXT=1800 DETAILHEX=544C53206572726F72") &&
                   TryParseSmartPlcIdentity(
                       "IDENTITY PRODUCT=14A_BRIDGE MODEL=STAMPPLC VERSION=1.0.2\r\n", out version) &&
                   version == "1.0.2" &&
                   TryParseSmartPlcIdentity(
                       "@ WIFI VERSION=1.0.1 SAVED=yes CONNECTED=no AUTO=no SSIDHEX= IP=0.0.0.0 RSSI=0\r\n",
                       out version) && version == "1.0.1" &&
                   !TryParseSmartPlcIdentity("AT+GMR\r\nOTHER SERIAL DEVICE\r\n", out version) &&
                   new PortChoice("COM5", true, "1.0.2").ToString().Contains("[SMARTPLC]") &&
                   new PortChoice("COM9", false, "").ToString().Contains("UNPROGRAMMED") &&
                   multipleDeviceOrder;
        }
    }

    internal sealed class GuiUpdatePrompt : Form
    {
        private readonly CheckBox _doNotRemind = new CheckBox();
        internal bool DoNotRemind { get { return _doNotRemind.Checked; } }

        internal GuiUpdatePrompt(string currentVersion, string latestVersion)
        {
            Text = "14a Bridge GUI update available";
            ClientSize = new Size(470, 205);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            BackColor = Color.FromArgb(16, 22, 30);
            ForeColor = Color.FromArgb(225, 235, 240);
            Font = new Font("Segoe UI", 9F);

            var title = new Label
            {
                Text = "A NEW WINDOWS GUI VERSION IS AVAILABLE",
                AutoSize = true, Location = new Point(24, 22),
                Font = new Font("Segoe UI Semibold", 12F),
                ForeColor = Color.FromArgb(0, 220, 210)
            };
            var detail = new Label
            {
                Text = "Installed: " + currentVersion + "\r\nAvailable: " + latestVersion +
                       "\r\n\r\nThis updates only the Windows GUI. SmartPLC firmware is unchanged.",
                AutoSize = true, Location = new Point(25, 57),
                ForeColor = Color.FromArgb(205, 220, 228)
            };
            _doNotRemind.Text = "Do not remind me again for " + latestVersion;
            _doNotRemind.AutoSize = true;
            _doNotRemind.Location = new Point(25, 130);
            _doNotRemind.ForeColor = Color.FromArgb(165, 185, 198);

            var update = new Button
            {
                Text = "Open download page", DialogResult = DialogResult.Yes,
                Size = new Size(145, 30), Location = new Point(189, 164),
                FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(18, 70, 78),
                ForeColor = Color.White
            };
            update.FlatAppearance.BorderColor = Color.FromArgb(0, 220, 210);
            var later = new Button
            {
                Text = "Later", DialogResult = DialogResult.No,
                Size = new Size(95, 30), Location = new Point(345, 164),
                FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(25, 34, 45),
                ForeColor = Color.White
            };
            later.FlatAppearance.BorderColor = Color.FromArgb(100, 125, 140);
            AcceptButton = update;
            CancelButton = later;
            Controls.AddRange(new Control[] { title, detail, _doNotRemind, update, later });
        }
    }

    internal sealed class LiquidGauge : Control
    {
        private readonly int _id;
        private int _commandPercent;
        private uint _maximumWatts;
        private uint _readbackWatts;
        private string _health = "--";
        private float _displayPercent;
        private float _targetPercent;
        private float _phase;

        internal LiquidGauge(int id)
        {
            _id = id;
            Size = new Size(155, 104);
            Margin = new Padding(4, 2, 4, 2);
            DoubleBuffered = true;
            BackColor = Color.FromArgb(8, 13, 19);
        }

        internal void SetState(bool enabled, int commandPercent, uint maximumWatts, uint readbackWatts, string health)
        {
            Visible = enabled;
            _commandPercent = Math.Max(0, Math.Min(100, commandPercent));
            _maximumWatts = maximumWatts;
            _readbackWatts = readbackWatts;
            _health = health;
            _targetPercent = maximumWatts == 0 ? 0 : Math.Min(100F, readbackWatts * 100F / maximumWatts);
        }

        internal void AdvanceFrame()
        {
            _displayPercent += (_targetPercent - _displayPercent) * 0.14F;
            if (Math.Abs(_targetPercent - _displayPercent) < 0.1F) _displayPercent = _targetPercent;
            _phase += 0.13F;
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            Rectangle box = new Rectangle(3, 3, Width - 7, Height - 7);
            // Keep the PC gauge consistent with the StampPLC dashboard:
            // communications/control faults and a rejected rating are a
            // whole-card red condition, not a subtle status label.
            bool fault = _health == "ERROR" || _health == "CLAMPED";
            Color level = LevelColor(_displayPercent);
            using (GraphicsPath path = Rounded(box, 10))
            using (SolidBrush panel = new SolidBrush(fault ? Color.FromArgb(115, 32, 42) : Color.FromArgb(19, 31, 42)))
            using (Pen border = new Pen(fault ? Color.FromArgb(255, 75, 80) : level, fault ? 2F : 1.4F))
            {
                e.Graphics.FillPath(panel, path);
                e.Graphics.SetClip(path);
                float top = box.Bottom - 3 - (box.Height - 6) * _displayPercent / 100F;
                if (!fault)
                {
                    PointF[] wave = new PointF[box.Width + 4];
                    wave[0] = new PointF(box.Left, top);
                    for (int i = 0; i <= box.Width; i++)
                        wave[i + 1] = new PointF(box.Left + i, top + (float)Math.Sin(i * 0.17F + _phase) * 2.2F);
                    wave[wave.Length - 2] = new PointF(box.Right, box.Bottom);
                    wave[wave.Length - 1] = new PointF(box.Left, box.Bottom);
                    using (SolidBrush water = new SolidBrush(Color.FromArgb(155, level))) e.Graphics.FillPolygon(water, wave);
                    for (int bubble = 0; bubble < 3; bubble++)
                    {
                        float progress = (float)((_phase * 0.22F + bubble * 0.31F) % 1.0F);
                        float bx = box.Left + box.Width * (bubble + 1) / 4F + (float)Math.Sin(_phase + bubble) * 4F;
                        float by = box.Bottom - 8 - progress * Math.Max(4F, box.Bottom - top - 12);
                        if (by > top + 4) e.Graphics.DrawEllipse(Pens.White, bx, by, bubble == 1 ? 5 : 3, bubble == 1 ? 5 : 3);
                    }
                }
                e.Graphics.ResetClip();
                e.Graphics.DrawPath(border, path);
            }
            float commandY = box.Bottom - 3 - (box.Height - 6) * _commandPercent / 100F;
            using (Pen dash = new Pen(Color.White, 1.4F) { DashPattern = new float[] { 4, 3 } })
                e.Graphics.DrawLine(dash, box.Left + 4, commandY, box.Right - 4, commandY);
            using (StringFormat center = new StringFormat { Alignment = StringAlignment.Center })
            using (Font idFont = new Font("Segoe UI Semibold", 9F))
            using (Font valueFont = new Font("Segoe UI Semibold", 15F))
            using (Font small = new Font("Consolas", 8F))
            using (SolidBrush white = new SolidBrush(Color.White))
            using (SolidBrush muted = new SolidBrush(Color.FromArgb(160, 190, 205)))
            {
                e.Graphics.DrawString("ID " + _id, idFont, white, new RectangleF(box.Left, box.Top + 7, box.Width, 16), center);
                if (fault)
                {
                    using (Font faultFont = new Font("Segoe UI Semibold", 16F))
                        e.Graphics.DrawString(_health == "CLAMPED" ? "LIMIT" : "ERROR", faultFont, white,
                            new RectangleF(box.Left, box.Top + 42, box.Width, 32), center);
                }
                else
                {
                    e.Graphics.DrawString("R: " + _commandPercent + "%", small, muted, new RectangleF(box.Left, box.Top + 28, box.Width, 14), center);
                    e.Graphics.DrawString(FormatKw(_readbackWatts), valueFont, white, new RectangleF(box.Left, box.Top + 47, box.Width, 27), center);
                    e.Graphics.DrawString(_health, small, Brushes.LightGray, new RectangleF(box.Left, box.Bottom - 18, box.Width, 13), center);
                }
            }
        }

        private static Color LevelColor(float percent)
        {
            if (percent < 30) return Color.FromArgb(255, 86, 85);
            if (percent < 60) return Color.FromArgb(255, 184, 55);
            return Color.FromArgb(36, 220, 145);
        }

        private static string FormatKw(uint watts) { return (watts / 1000F).ToString("0.0", CultureInfo.InvariantCulture) + " kW"; }

        private static GraphicsPath Rounded(Rectangle box, int radius)
        {
            var path = new GraphicsPath();
            path.AddArc(box.Left, box.Top, radius, radius, 180, 90);
            path.AddArc(box.Right - radius, box.Top, radius, radius, 270, 90);
            path.AddArc(box.Right - radius, box.Bottom - radius, radius, radius, 0, 90);
            path.AddArc(box.Left, box.Bottom - radius, radius, radius, 90, 90);
            path.CloseFigure();
            return path;
        }
    }

    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length == 1 && args[0] == "--self-test") return MainForm.SelfTest() ? 0 : 1;
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length == 1 && args[0] == "--ui-self-test")
            {
                using (var form = new MainForm()) form.CreateControl();
                return 0;
            }
            if (args.Length == 2 && args[0] == "--render-update-prompt")
            {
                using (var prompt = new GuiUpdatePrompt("V1.0.4", "V1.0.5"))
                {
                    prompt.Show();
                    Application.DoEvents();
                    using (var bitmap = new Bitmap(prompt.Width, prompt.Height))
                    {
                        prompt.DrawToBitmap(bitmap, new Rectangle(Point.Empty, prompt.Size));
                        bitmap.Save(args[1]);
                    }
                }
                return 0;
            }
            if (args.Length == 2 && (args[0] == "--render-ui" || args[0] == "--render-ui-debug" ||
                args[0] == "--render-flash-progress"))
            {
                using (var form = new MainForm())
                {
                    form.Show();
                    Application.DoEvents();
                    DateTime renderReady = DateTime.UtcNow.AddSeconds(2);
                    while (DateTime.UtcNow < renderReady)
                    {
                        Application.DoEvents();
                        Thread.Sleep(25);
                    }
                    if (args[0] == "--render-ui-debug")
                    {
                        Control[] tabs = form.Controls.Find("MainTabs", true);
                        if (tabs.Length == 1) ((TabControl)tabs[0]).SelectedIndex = 1;
                        Application.DoEvents();
                    }
                    if (args[0] == "--render-flash-progress")
                    {
                        form.ShowFlashProgressPreview();
                        Application.DoEvents();
                    }
                    using (var bitmap = new Bitmap(form.Width, form.Height))
                    {
                        form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                        bitmap.Save(args[1]);
                    }
                }
                return 0;
            }
            Application.Run(new MainForm());
            return 0;
        }
    }
}
