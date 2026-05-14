using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System;
using System.IO;
using System.IO.Ports;
using System.Threading;
using System.Threading.Tasks;
using Windows.Media.Control;
using System.Drawing;
using System.Drawing.Imaging;

namespace MusicToArduino
{
    public class MusicToArduinoService : BackgroundService
    {
        private readonly ILogger<MusicToArduinoService> _logger;
        private SerialPort? _serialPort;
        private string _lastSongKey = "";
        private DateTime _lastSendTime = DateTime.MinValue;
        private TimeSpan _minSendInterval = TimeSpan.FromSeconds(3);
        private readonly object _serialLock = new object();

        // Configuration
        private string? _comPort;
        private int _baudRate;
        private const int MAX_RETRIES = 3;
        private const string CONFIG_FILE = "MusicToArduino.config";

        public MusicToArduinoService(ILogger<MusicToArduinoService> logger)
        {
            _logger = logger;
        }

        protected override async Task ExecuteAsync(CancellationToken stoppingToken)
        {
            _logger.LogInformation("MusicToArduino Service starting...");

            // Load configuration
            if (!LoadConfiguration())
            {
                _logger.LogError("Failed to load configuration. Service will stop.");
                return;
            }

            // Initialize serial port
            if (!InitializeSerialPort())
            {
                _logger.LogError("Failed to initialize serial port. Service will stop.");
                return;
            }

            // Start monitoring tasks
            var monitorTask = MonitorMusicAsync(stoppingToken);
            var readTask = ReadArduinoOutputAsync(stoppingToken);

            // Wait for either task to complete (they should run until cancelled)
            await Task.WhenAny(monitorTask, readTask);

            _logger.LogInformation("MusicToArduino Service stopping...");

            // Cleanup
            lock (_serialLock)
            {
                if (_serialPort != null && _serialPort.IsOpen)
                {
                    _serialPort.Close();
                    _serialPort.Dispose();
                }
            }
        }

        private bool LoadConfiguration()
        {
            try
            {
                string configPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, CONFIG_FILE);

                if (!File.Exists(configPath))
                {
                    CreateDefaultConfig(configPath);
                    _logger.LogWarning($"Created default configuration file at {configPath}. Please edit it with your COM port and restart the service.");
                    return false;
                }

                // Read config file (simple key=value format)
                string[] lines = File.ReadAllLines(configPath);
                foreach (string line in lines)
                {
                    if (string.IsNullOrWhiteSpace(line) || line.StartsWith("#")) continue;

                    string[] parts = line.Split('=');
                    if (parts.Length == 2)
                    {
                        string key = parts[0].Trim().ToLower();
                        string value = parts[1].Trim();

                        switch (key)
                        {
                            case "comport":
                                _comPort = value;
                                break;
                            case "baudrate":
                                _baudRate = int.Parse(value);
                                break;
                            case "minintervalseconds":
                                _minSendInterval = TimeSpan.FromSeconds(int.Parse(value));
                                break;
                        }
                    }
                }

                // Validate configuration
                if (string.IsNullOrEmpty(_comPort))
                {
                    _logger.LogError("COM port not specified in configuration file");
                    return false;
                }

                if (_baudRate <= 0) _baudRate = 115200;

                _logger.LogInformation($"Configuration loaded: COM={_comPort}, Baud={_baudRate}, Interval={_minSendInterval.TotalSeconds}s");
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error loading configuration");
                return false;
            }
        }

        private void CreateDefaultConfig(string configPath)
        {
            string defaultConfig = @"# MusicToArduino Configuration File
# Place this file in the same directory as the service executable

# COM port for Arduino/ESP32 device (required)
# Examples: COM3, COM4, COM5
COMPort=COM3

# Baud rate (default: 115200)
BaudRate=115200

# Minimum interval between sending song updates (seconds)
MinIntervalSeconds=3

# Important: After editing this file, restart the service for changes to take effect
";
            File.WriteAllText(configPath, defaultConfig);
        }

        private bool InitializeSerialPort()
        {
            try
            {
                _serialPort = new SerialPort(_comPort, _baudRate, Parity.None, 8, StopBits.One);
                _serialPort.ReadTimeout = 1000;
                _serialPort.WriteTimeout = 1000;
                _serialPort.Open();
                _logger.LogInformation($"Serial port {_comPort} opened successfully");
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, $"Failed to open serial port {_comPort}");
                return false;
            }
        }

        private async Task MonitorMusicAsync(CancellationToken stoppingToken)
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    var songData = await GetCurrentSongDataAsync();
                    if (songData != null)
                    {
                        string currentKey = $"{songData.Title}|{songData.Artist}";
                        if ((DateTime.Now - _lastSendTime) >= _minSendInterval)
                        {
                            _lastSongKey = currentKey;
                            _lastSendTime = DateTime.Now;

                            bool success = await SendToArduinoWithRetryAsync(songData);
                            if (success)
                            {
                                _logger.LogInformation($"Sent: {songData.Title} - {songData.Artist}");
                            }
                        }
                    }
                    await Task.Delay(1000, stoppingToken);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "Monitor error");
                    await Task.Delay(5000, stoppingToken);
                }
            }
        }

        private async Task ReadArduinoOutputAsync(CancellationToken stoppingToken)
        {
            try
            {
                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (_serialLock)
                    {
                        if (_serialPort != null && _serialPort.IsOpen && _serialPort.BytesToRead > 0)
                        {
                            try
                            {
                                string line = _serialPort.ReadLine();
                                if (!string.IsNullOrWhiteSpace(line))
                                {
                                    _logger.LogInformation($"[ARDUINO] {line}");
                                }
                            }
                            catch (TimeoutException)
                            {
                                // Normal timeout, just continue
                            }
                        }
                    }
                    await Task.Delay(10, stoppingToken);
                }
            }
            catch (OperationCanceledException)
            {
                // Normal shutdown
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error reading from Arduino");
            }
        }

        private async Task<bool> SendToArduinoWithRetryAsync(SongData data)
        {
            for (int attempt = 1; attempt <= MAX_RETRIES; attempt++)
            {
                if (await SendCompleteDataPacketAsync(data))
                {
                    return true;
                }

                if (attempt < MAX_RETRIES)
                {
                    await Task.Delay(1000);
                }
            }
            return false;
        }

        private async Task<bool> SendCompleteDataPacketAsync(SongData data)
        {
            try
            {
                lock (_serialLock)
                {
                    if (_serialPort == null || !_serialPort.IsOpen)
                        return false;

                    _serialPort.DiscardInBuffer();
                    _serialPort.DiscardOutBuffer();
                    _serialPort.Write("S");
                    Thread.Sleep(50);
                }

                string metadata = $"{data.Title}|{data.Artist}|{data.Duration}|{data.Position}|{data.Source}|{data.IsPlaying}\n";
                byte[] metadataBytes = System.Text.Encoding.UTF8.GetBytes(metadata);
                byte[] thumbnail = data.Thumbnail ?? CreateTestThumbnail();

                lock (_serialLock)
                {
                    if (_serialPort == null || !_serialPort.IsOpen)
                        return false;

                    _serialPort.Write(metadataBytes, 0, metadataBytes.Length);
                }

                await Task.Delay(50);

                lock (_serialLock)
                {
                    if (_serialPort == null || !_serialPort.IsOpen)
                        return false;

                    _serialPort.Write(thumbnail, 0, thumbnail.Length);
                }

                await Task.Delay(500);
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Send error");
                return false;
            }
        }

        private async Task<SongData> GetCurrentSongDataAsync()
        {
            try
            {
                var sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                var currentSession = sessionManager.GetCurrentSession();

                if (currentSession == null) return null;

                var mediaProperties = await currentSession.TryGetMediaPropertiesAsync();
                if (string.IsNullOrWhiteSpace(mediaProperties.Title)) return null;

                var timelineProperties = currentSession.GetTimelineProperties();
                var playbackProperties = currentSession.GetPlaybackInfo();

                bool isPlaying = playbackProperties.PlaybackStatus ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;

                string sourceApp = currentSession.SourceAppUserModelId ?? "Unknown Source";
                if (sourceApp.Contains("!"))
                    sourceApp = sourceApp.Substring(sourceApp.LastIndexOf('!') + 1);
                sourceApp = sourceApp.Replace("Microsoft.", "").Replace("Spotify", "Spotify").Replace("AppleInc.", "Apple ");

                string title = mediaProperties.Title.Replace("|", "").Trim();
                string artist = (mediaProperties.Artist ?? "Unknown Artist").Replace("|", "").Trim();

                byte[] thumbnailBytes = null;
                if (mediaProperties.Thumbnail != null)
                {
                    thumbnailBytes = await GetAndConvertThumbnailAsync(mediaProperties.Thumbnail);
                }

                return new SongData
                {
                    Title = title,
                    Artist = artist,
                    Duration = (int)timelineProperties.EndTime.TotalSeconds,
                    Position = (int)timelineProperties.Position.TotalSeconds,
                    Source = sourceApp,
                    IsPlaying = isPlaying,
                    Thumbnail = thumbnailBytes ?? CreateTestThumbnail()
                };
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error getting song data");
                return null;
            }
        }

        private async Task<byte[]> GetAndConvertThumbnailAsync(Windows.Storage.Streams.IRandomAccessStreamReference thumbnailRef)
        {
            try
            {
                using (var stream = await thumbnailRef.OpenReadAsync())
                {
                    byte[] rawBytes = new byte[stream.Size];
                    using (var dataReader = new Windows.Storage.Streams.DataReader(stream))
                    {
                        await dataReader.LoadAsync((uint)stream.Size);
                        dataReader.ReadBytes(rawBytes);
                    }

                    using (var ms = new MemoryStream(rawBytes))
                    using (var originalImage = Image.FromStream(ms))
                    using (var resizedBitmap = new Bitmap(80, 80))
                    using (var graphics = Graphics.FromImage(resizedBitmap))
                    {
                        graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                        graphics.DrawImage(originalImage, 0, 0, 80, 80);

                        byte[] rgb565 = new byte[80 * 80 * 2];

                        for (int y = 0; y < 80; y++)
                        {
                            for (int x = 0; x < 80; x++)
                            {
                                Color pixel = resizedBitmap.GetPixel(x, y);
                                int r = (pixel.R >> 3) & 0x1F;
                                int g = (pixel.G >> 2) & 0x3F;
                                int b = (pixel.B >> 3) & 0x1F;
                                ushort rgb565Color = (ushort)((r << 11) | (g << 5) | b);
                                int index = (y * 80 + x) * 2;
                                rgb565[index] = (byte)(rgb565Color & 0xFF);
                                rgb565[index + 1] = (byte)((rgb565Color >> 8) & 0xFF);
                            }
                        }
                        return rgb565;
                    }
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Thumbnail conversion failed");
                return null;
            }
        }

        private byte[] CreateTestThumbnail()
        {
            byte[] rgb565 = new byte[80 * 80 * 2];
            for (int y = 0; y < 80; y++)
            {
                for (int x = 0; x < 80; x++)
                {
                    int r = (x * 31) / 80;
                    int g = (y * 63) / 80;
                    int b = ((x + y) * 31) / 160;
                    ushort color = (ushort)((r << 11) | (g << 5) | b);
                    int index = (y * 80 + x) * 2;
                    rgb565[index] = (byte)(color & 0xFF);
                    rgb565[index + 1] = (byte)((color >> 8) & 0xFF);
                }
            }
            return rgb565;
        }

        private void AttemptReconnect()
        {
            try
            {
                lock (_serialLock)
                {
                    if (_serialPort != null)
                    {
                        if (_serialPort.IsOpen)
                            _serialPort.Close();
                        _serialPort.Dispose();
                    }

                    InitializeSerialPort();
                    _logger.LogInformation("Serial port reconnected successfully");
                }
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to reconnect serial port");
            }
        }
    }

    public class SongData
    {
        public string Title { get; set; }
        public string Artist { get; set; }
        public int Duration { get; set; }
        public int Position { get; set; }
        public string Source { get; set; }
        public bool IsPlaying { get; set; }
        public byte[] Thumbnail { get; set; }
    }
}