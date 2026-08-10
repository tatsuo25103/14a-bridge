using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace StampPlcRseConfigurator
{
    internal sealed class MainForm : Form
    {
        private const string ReleaseVersion = "V1.0.0";
        private static readonly Color Surface = Color.FromArgb(16, 22, 30);
        private static readonly Color Panel = Color.FromArgb(25, 34, 45);
        private static readonly Color Accent = Color.FromArgb(0, 220, 210);
        private static readonly Color TextColor = Color.FromArgb(225, 235, 240);
        private static readonly Color Muted = Color.FromArgb(135, 155, 170);
        private readonly ComboBox _ports = new ComboBox();
        private readonly Button _connect = new Button();
        private readonly Button _flashFirmware = new Button();
        private readonly Label _connection = new Label();
        private readonly Label _rse = new Label();
        private readonly Label _mode = new Label();
        private readonly Label _result = new Label();
        private readonly DataGridView _grid = new DataGridView();
        private readonly TextBox _baud = new TextBox();
        private readonly TextBox _register = new TextBox();
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
            ScanPorts();
            FormClosing += delegate { Disconnect(); };
        }

        private void BuildUi()
        {
            var root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                Padding = new Padding(10),
                ColumnCount = 1,
                RowCount = 4,
                AutoScroll = false
            };
            // Compact desktop layout: live display at left, settings and
            // testing at right, then the event log below.
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 76));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 190));
            Controls.Add(root);

            root.Controls.Add(BuildHeader());

            var connectionBox = NewGroup("USB Connection");
            var connectionFlow = NewFlow();
            _ports.Width = 180;
            _ports.Height = 27;
            _ports.DropDownStyle = ComboBoxStyle.DropDownList;
            _ports.BackColor = Color.FromArgb(7, 13, 19);
            _ports.ForeColor = TextColor;
            _ports.FlatStyle = FlatStyle.Flat;
            _connect.Text = "Connect";
            StyleButton(_connect);
            _connect.Click += ToggleConnection;
            var scan = NewButton("Scan", delegate { ScanPorts(); });
            scan.AutoSize = false;
            _connect.AutoSize = false;
            scan.Size = new Size(76, 26);
            _connect.Size = new Size(86, 26);
            scan.Margin = new Padding(4, 0, 0, 0);
            _connect.Margin = new Padding(8, 0, 0, 0);
            connectionFlow.WrapContents = false;
            connectionFlow.AutoSize = false;
            connectionFlow.Padding = new Padding(0, 2, 0, 0);
            _connection.AutoSize = true;
            _connection.Padding = new Padding(14, 5, 0, 0);
            _connection.Text = "\u25CF  DISCONNECTED";
            _connection.ForeColor = Muted;
            _flashFirmware.Text = "Flash firmware";
            StyleButton(_flashFirmware);
            _flashFirmware.AutoSize = false;
            _flashFirmware.Size = new Size(116, 26);
            _flashFirmware.Margin = new Padding(8, 0, 0, 0);
            _flashFirmware.Click += FlashFirmware;
            connectionFlow.Controls.AddRange(new Control[] { _ports, scan, _connect, _flashFirmware, _connection });
            connectionBox.Controls.Add(connectionFlow);
            root.Controls.Add(connectionBox);

            var workspace = new TableLayoutPanel
            {
                Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1,
                Padding = new Padding(0, 2, 0, 2)
            };
            workspace.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 44));
            workspace.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 56));
            root.Controls.Add(workspace);

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
            workspace.Controls.Add(displayBox, 0, 0);

            var rightWorkArea = new TableLayoutPanel
            {
                Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2,
                Padding = new Padding(6, 0, 0, 0)
            };
            rightWorkArea.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            rightWorkArea.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
            workspace.Controls.Add(rightWorkArea, 1, 0);

            var configurationBox = NewGroup("Configuration  |  saved in StampPLC");
            var configurationLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
            configurationLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            configurationLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
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
                NewButton("Save all settings", SaveAll),
                NewTextLabel("Offline inverter: settings stay saved as PENDING")
            });
            configurationLayout.Controls.Add(settingsFlow, 0, 1);
            configurationBox.Controls.Add(configurationLayout);
            rightWorkArea.Controls.Add(configurationBox, 0, 0);

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
            rightWorkArea.Controls.Add(actionBox, 0, 1);

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
            root.Controls.Add(logBox);
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
            string previous = _ports.Text;
            string[] ports = SerialPort.GetPortNames();
            Array.Sort(ports, StringComparer.OrdinalIgnoreCase);
            _ports.Items.Clear();
            _ports.Items.AddRange(ports);
            if (Array.IndexOf(ports, previous) >= 0) _ports.SelectedItem = previous;
            else if (ports.Length > 0) _ports.SelectedIndex = 0;
        }

        private void ToggleConnection(object sender, EventArgs e)
        {
            if (_serial != null && _serial.IsOpen)
            {
                Disconnect();
                return;
            }
            if (string.IsNullOrWhiteSpace(_ports.Text))
            {
                MessageBox.Show(this, "Connect the StampPLC by USB and scan again.", "No COM port",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                _serial = new SerialPort(_ports.Text, 115200, Parity.None, 8, StopBits.One)
                {
                    NewLine = "\n",
                    ReadTimeout = 200,
                    WriteTimeout = 1000
                };
                _serial.DataReceived += SerialDataReceived;
                _serial.Open();
                _connect.Text = "Disconnect";
                _connection.Text = "\u25CF  CONNECTED  " + _ports.Text;
                _connection.ForeColor = Accent;
                AppendLog("[PC] Connected to " + _ports.Text);
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
            if (string.IsNullOrWhiteSpace(_ports.Text))
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
                "Firmware will be compiled and written to " + _ports.Text + ".\r\n" +
                "The USB connection will be closed during flashing. Continue?",
                "Flash StampPLC firmware", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;

            Disconnect();
            _flashingFirmware = true;
            _flashFirmware.Enabled = false;
            _connection.Text = "\u25CF  FLASHING " + _ports.Text;
            _connection.ForeColor = Color.FromArgb(255, 184, 55);
            AppendLog("[FLASH] Starting firmware update on " + _ports.Text);
            string port = _ports.Text;
            ThreadPool.QueueUserWorkItem(delegate
            {
                int exitCode = -1;
                try
                {
                    var info = new ProcessStartInfo
                    {
                        FileName = hasBundledFlasher ? bundledEsptool : platformIo,
                        Arguments = hasBundledFlasher
                            ? "--chip esp32s3 --port " + port +
                              " --baud 1500000 --before default_reset --after hard_reset write_flash -z" +
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
                            if (!string.IsNullOrEmpty(a.Data)) BeginInvoke(new Action<string>(AppendLog), "[FLASH] " + a.Data);
                        };
                        process.ErrorDataReceived += delegate(object s, DataReceivedEventArgs a)
                        {
                            if (!string.IsNullOrEmpty(a.Data)) BeginInvoke(new Action<string>(AppendLog), "[FLASH] " + a.Data);
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
                    AppendLog(exitCode == 0
                        ? "[FLASH] Completed. Reconnect to read the new StampPLC."
                        : "[FLASH] Failed. Check the COM port and USB cable, then retry.");
                }));
            });
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
            _connection.Text = "\u25CF  DISCONNECTED";
            _connection.ForeColor = Muted;
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
            return RsePattern.IsMatch("RSE DI mask: 0x02  level: 60%") &&
                   ModePattern.IsMatch("Mode: DRY-RUN  RS485: 19200 baud  register: 0x04E5  quantity: 2") &&
                   IdPattern.IsMatch("3   yes        10000  10000   6000   3000      0     6000      6000  yes") &&
                   ProbePattern.IsMatch("PROBE ID=3 REGISTER=0x04E5 VALUE=10000 STATUS=OK DETAIL=readback verified");
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
            Application.Run(new MainForm());
            return 0;
        }
    }
}
