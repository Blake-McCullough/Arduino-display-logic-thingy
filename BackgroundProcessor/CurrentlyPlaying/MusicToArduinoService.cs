using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System.IO;
using System.IO.Ports;
using Windows.Media.Control;

namespace MusicToArduino
{
    public class MusicToArduinoService : BackgroundService
    {
        private readonly ILogger<MusicToArduinoService> _logger;
        private SerialPort? _serialPort;
        private bool _lastIsPlaying = false;
        private string _lastSongKey = "";
        private DateTime _lastSendTime = DateTime.MinValue;
        private TimeSpan _minSendInterval = TimeSpan.FromSeconds(3);
        private readonly object _serialLock = new object();
        private bool _isReconnecting = false;
        private readonly Random _random = new Random();

        // Configuration
        private string? _comPort;
        private int _baudRate;
        private const int MAX_RETRIES = 3;
        private const string CONFIG_FILE = "MusicToArduino.config";
        private const int MIN_RECONNECT_DELAY_SECONDS = 5;
        private const int MAX_RECONNECT_DELAY_SECONDS = 10;

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

            // Start monitoring tasks
            var monitorTask = MonitorMusicAsync(stoppingToken);
            var readTask = ReadArduinoOutputAsync(stoppingToken);
            var reconnectTask = MonitorAndReconnectAsync(stoppingToken);

            // Wait for either task to complete (they should run until cancelled)
            await Task.WhenAny(monitorTask, readTask, reconnectTask);

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
                lock (_serialLock)
                {
                    // Dispose existing port if any
                    if (_serialPort != null)
                    {
                        if (_serialPort.IsOpen)
                            _serialPort.Close();
                        _serialPort.Dispose();
                    }

                    _serialPort = new SerialPort(_comPort, _baudRate, Parity.None, 8, StopBits.One);
                    _serialPort.ReadTimeout = 1000;
                    _serialPort.WriteTimeout = 1000;
                    _serialPort.Open();

                    // Clear buffers
                    _serialPort.DiscardInBuffer();
                    _serialPort.DiscardOutBuffer();

                    _logger.LogInformation($"Serial port {_comPort} opened successfully");
                    return true;
                }
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, $"Failed to open serial port {_comPort}");
                return false;
            }
        }

        private async Task MonitorAndReconnectAsync(CancellationToken stoppingToken)
        {
            // Initial connection attempt
            bool initialConnected = false;
            while (!stoppingToken.IsCancellationRequested && !initialConnected)
            {
                _logger.LogInformation($"Attempting initial connection to {_comPort}...");
                if (InitializeSerialPort())
                {
                    initialConnected = true;
                    _logger.LogInformation("Initial connection successful!");
                    break;
                }

                int delay = _random.Next(MIN_RECONNECT_DELAY_SECONDS, MAX_RECONNECT_DELAY_SECONDS + 1);
                _logger.LogWarning($"Initial connection failed. Retrying in {delay} seconds...");
                await Task.Delay(TimeSpan.FromSeconds(delay), stoppingToken);
            }

            // Continuously monitor connection
            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    bool isConnected = false;
                    lock (_serialLock)
                    {
                        isConnected = _serialPort != null && _serialPort.IsOpen;
                    }

                    if (!isConnected && !_isReconnecting)
                    {
                        _logger.LogWarning("Serial port is disconnected. Attempting to reconnect...");
                        await AttemptReconnectAsync(stoppingToken);
                    }
                    else if (isConnected)
                    {
                        // Check connection health
                        await CheckConnectionHealthAsync(stoppingToken);
                    }

                    // Check every 2 seconds for connection issues
                    await Task.Delay(TimeSpan.FromSeconds(2), stoppingToken);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "Error in connection monitor");
                    await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken);
                }
            }
        }

        private async Task CheckConnectionHealthAsync(CancellationToken stoppingToken)
        {
            try
            {
                bool isHealthy = false;
                lock (_serialLock)
                {
                    if (_serialPort != null && _serialPort.IsOpen)
                    {
                        try
                        {
                            // Try a simple write to check if the port is responsive
                            _serialPort.Write("P"); // Ping command
                            isHealthy = true;
                        }
                        catch (Exception)
                        {
                            // Connection is unhealthy
                            isHealthy = false;
                        }
                    }
                }

                if (!isHealthy)
                {
                    _logger.LogWarning("Serial port health check failed. Closing and will reconnect.");
                    lock (_serialLock)
                    {
                        if (_serialPort != null && _serialPort.IsOpen)
                        {
                            try
                            {
                                _serialPort.Close();
                            }
                            catch { }
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Health check error");
            }
        }

        private async Task AttemptReconnectAsync(CancellationToken stoppingToken)
        {
            if (_isReconnecting) return;

            _isReconnecting = true;
            try
            {
                while (!stoppingToken.IsCancellationRequested)
                {
                    // Generate random delay between 5-10 seconds
                    int delay = _random.Next(MIN_RECONNECT_DELAY_SECONDS, MAX_RECONNECT_DELAY_SECONDS + 1);
                    _logger.LogInformation($"Waiting {delay} seconds before reconnect attempt...");
                    await Task.Delay(TimeSpan.FromSeconds(delay), stoppingToken);

                    if (stoppingToken.IsCancellationRequested)
                        break;

                    _logger.LogInformation("Attempting to reconnect...");

                    // Close and dispose existing port
                    lock (_serialLock)
                    {
                        if (_serialPort != null)
                        {
                            if (_serialPort.IsOpen)
                                _serialPort.Close();
                            _serialPort.Dispose();
                            _serialPort = null;
                        }
                    }

                    // Try to reconnect
                    if (InitializeSerialPort())
                    {
                        _logger.LogInformation("Reconnection successful!");
                        break;
                    }
                    else
                    {
                        _logger.LogWarning("Reconnection failed. Will retry again...");
                    }
                }
            }
            catch (OperationCanceledException)
            {
                // Normal shutdown
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error during reconnection attempt");
            }
            finally
            {
                _isReconnecting = false;
            }
        }

        private async Task MonitorMusicAsync(CancellationToken stoppingToken)
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    // Check if serial port is connected before attempting to send
                    bool isConnected = false;
                    lock (_serialLock)
                    {
                        isConnected = _serialPort != null && _serialPort.IsOpen;
                    }

                    if (!isConnected)
                    {
                        // Wait and try again later
                        await Task.Delay(1000, stoppingToken);
                        continue;
                    }

                    var songData = await GetCurrentSongDataAsync();
                    if (songData != null)
                    {
                        string currentKey = $"{songData.Title}|{songData.Artist}";
                        bool currentIsPlaying = songData.IsPlaying;

                        // Calculate cooldown
                        bool hasBeenCooldownTime = (DateTime.Now - _lastSendTime) >= TimeSpan.FromSeconds(3);

                        if ((_lastSongKey != currentKey && hasBeenCooldownTime) ||
                            (_lastIsPlaying != currentIsPlaying && hasBeenCooldownTime) ||
                            ((DateTime.Now - _lastSendTime) >= _minSendInterval))
                        {
                            _lastIsPlaying = currentIsPlaying;
                            _lastSongKey = currentKey;
                            _lastSendTime = DateTime.Now;

                            bool success = await SendToArduinoWithRetryAsync(songData);
                            if (success)
                            {
                                _logger.LogInformation($"Sent: {songData.Title} - {songData.Artist}");
                            }
                            else
                            {
                                _logger.LogWarning("Failed to send data to Arduino - connection may be lost");
                                // Mark connection as failed so reconnection triggers
                                lock (_serialLock)
                                {
                                    if (_serialPort != null && _serialPort.IsOpen)
                                    {
                                        try
                                        {
                                            _serialPort.Close();
                                        }
                                        catch { }
                                    }
                                }
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
                    bool canRead = false;
                    lock (_serialLock)
                    {
                        canRead = _serialPort != null && _serialPort.IsOpen && _serialPort.BytesToRead > 0;
                    }

                    if (canRead)
                    {
                        try
                        {
                            lock (_serialLock)
                            {
                                if (_serialPort != null && _serialPort.IsOpen)
                                {
                                    string line = _serialPort.ReadLine();
                                    if (!string.IsNullOrWhiteSpace(line))
                                    {
                                        _logger.LogInformation($"[ARDUINO] {line}");
                                    }
                                }
                            }
                        }
                        catch (TimeoutException)
                        {
                            // Normal timeout, just continue
                        }
                        catch (Exception ex)
                        {
                            _logger.LogWarning(ex, "Error reading from Arduino - connection may be lost");
                            // Mark for reconnection
                            lock (_serialLock)
                            {
                                if (_serialPort != null && _serialPort.IsOpen)
                                {
                                    try
                                    {
                                        _serialPort.Close();
                                    }
                                    catch { }
                                }
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
                    _logger.LogWarning($"Send attempt {attempt} failed. Retrying...");
                    await Task.Delay(1000 * attempt);
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
                    using (var originalImage = System.Drawing.Image.FromStream(ms))
                    using (var resizedBitmap = new System.Drawing.Bitmap(80, 80))
                    using (var graphics = System.Drawing.Graphics.FromImage(resizedBitmap))
                    {
                        graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                        graphics.DrawImage(originalImage, 0, 0, 80, 80);

                        byte[] rgb565 = new byte[80 * 80 * 2];

                        for (int y = 0; y < 80; y++)
                        {
                            for (int x = 0; x < 80; x++)
                            {
                                System.Drawing.Color pixel = resizedBitmap.GetPixel(x, y);
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