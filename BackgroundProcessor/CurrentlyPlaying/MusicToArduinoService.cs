using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System.IO;
using System.IO.Ports;
using System.Runtime.InteropServices;
using Windows.Media.Control;

namespace MusicToArduino
{
    public class MusicToArduinoService : BackgroundService
    {
        private readonly ILogger<MusicToArduinoService> _logger;
        private SerialPort? _serialPort;
        private bool _lastIsPlaying = false;
        private string _lastSongKey = "";

        // Full packets (title/artist/thumbnail) are slow - the 80x80
        // thumbnail alone is 12,800 bytes, which takes over a second to
        // transmit at 115200 baud plus render on the Arduino. Sending that
        // on every tick is what was making position tracking feel laggy
        // AND starving the audio-bar packets of serial time. So position/
        // duration/play-state now go out on their own small, frequent
        // "timing" packet, and the full packet is only sent when the song
        // actually changes (plus an occasional resync as a safety net).
        private DateTime _lastFullSendTime = DateTime.MinValue;
        private DateTime _lastTimingSendTime = DateTime.MinValue;
        private TimeSpan _timingUpdateInterval = TimeSpan.FromSeconds(1);
        private TimeSpan _fullResyncInterval = TimeSpan.FromSeconds(60);

        // Guards every access to _serialPort. A SemaphoreSlim (rather than a
        // plain lock/object) is used because we need to hold it across
        // `await` points while a multi-part packet (start marker + metadata
        // + thumbnail) is being written, so the fast audio-update loop can't
        // interleave a packet in the middle and desync the Arduino's parser.
        private readonly SemaphoreSlim _serialSemaphore = new SemaphoreSlim(1, 1);
        private bool _isReconnecting = false;
        private readonly Random _random = new Random();

        private AudioAnalyzer? _audioAnalyzer;

        // Configuration
        private string? _comPort;
        private int _baudRate;
        private bool _enableAudioVisualization = true;
        private int _audioUpdateIntervalMs = 50; // ~20Hz
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

            if (!LoadConfiguration())
            {
                _logger.LogError("Failed to load configuration. Service will stop.");
                return;
            }

            if (_enableAudioVisualization)
            {
                _audioAnalyzer = new AudioAnalyzer(_logger);
                _audioAnalyzer.Start();
            }

            var tasks = new List<Task>
            {
                MonitorMusicAsync(stoppingToken),
                ReadArduinoOutputAsync(stoppingToken),
                MonitorAndReconnectAsync(stoppingToken)
            };

            if (_enableAudioVisualization)
            {
                tasks.Add(SendAudioDataAsync(stoppingToken));
            }

            await Task.WhenAny(tasks);

            _logger.LogInformation("MusicToArduino Service stopping...");

            _audioAnalyzer?.Dispose();

            await _serialSemaphore.WaitAsync(CancellationToken.None);
            try
            {
                if (_serialPort != null && _serialPort.IsOpen)
                {
                    _serialPort.Close();
                    _serialPort.Dispose();
                }
            }
            finally
            {
                _serialSemaphore.Release();
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
                                if (int.TryParse(value, out int baud)) _baudRate = baud;
                                break;
                            case "timingupdateintervalseconds":
                                if (double.TryParse(value, out double timingInterval) && timingInterval > 0)
                                    _timingUpdateInterval = TimeSpan.FromSeconds(timingInterval);
                                break;
                            case "fullresyncintervalseconds":
                                if (int.TryParse(value, out int resyncInterval))
                                    _fullResyncInterval = resyncInterval > 0 ? TimeSpan.FromSeconds(resyncInterval) : TimeSpan.MaxValue;
                                break;
                            case "enableaudiovisualization":
                                if (bool.TryParse(value, out bool enableAudio)) _enableAudioVisualization = enableAudio;
                                break;
                            case "audioupdateintervalms":
                                if (int.TryParse(value, out int audioMs) && audioMs > 0) _audioUpdateIntervalMs = audioMs;
                                break;
                        }
                    }
                }

                if (string.IsNullOrEmpty(_comPort))
                {
                    _logger.LogError("COM port not specified in configuration file");
                    return false;
                }

                if (_baudRate <= 0) _baudRate = 115200;

                _logger.LogInformation(
                    "Configuration loaded: COM={ComPort}, Baud={Baud}, TimingInterval={Timing}s, FullResync={Resync}s, Audio={Audio}, AudioIntervalMs={AudioMs}",
                    _comPort, _baudRate, _timingUpdateInterval.TotalSeconds, _fullResyncInterval.TotalSeconds, _enableAudioVisualization, _audioUpdateIntervalMs);
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

# How often to send lightweight position/duration/play-state updates
# (seconds, decimals allowed e.g. 0.5). This is cheap - it does NOT
# resend the thumbnail - so it's safe to set this low for accurate timing.
TimingUpdateIntervalSeconds=1

# How often to resend the FULL packet (title/artist/thumbnail) as a
# safety-net resync even if the song hasn't changed, in case a display
# reboot or dropped connection caused it to miss the real change event.
# This is the slow, ~1 second transfer, so keep it infrequent.
FullResyncIntervalSeconds=60

# Enable/disable real-time audio (volume + frequency band) visualization
EnableAudioVisualization=true

# How often to send audio level updates to the display, in milliseconds
AudioUpdateIntervalMs=50

# Important: After editing this file, restart the service for changes to take effect
";
            File.WriteAllText(configPath, defaultConfig);
        }

        private bool InitializeSerialPort()
        {
            _serialSemaphore.Wait();
            try
            {
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

                _serialPort.DiscardInBuffer();
                _serialPort.DiscardOutBuffer();

                _logger.LogInformation($"Serial port {_comPort} opened successfully");
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, $"Failed to open serial port {_comPort}");
                return false;
            }
            finally
            {
                _serialSemaphore.Release();
            }
        }

        private async Task MonitorAndReconnectAsync(CancellationToken stoppingToken)
        {
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

            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    bool isConnected = await IsConnectedAsync();

                    if (!isConnected && !_isReconnecting)
                    {
                        _logger.LogWarning("Serial port is disconnected. Attempting to reconnect...");
                        await AttemptReconnectAsync(stoppingToken);
                    }
                    else if (isConnected)
                    {
                        await CheckConnectionHealthAsync();
                    }

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

        private async Task<bool> IsConnectedAsync()
        {
            await _serialSemaphore.WaitAsync();
            try
            {
                return _serialPort != null && _serialPort.IsOpen;
            }
            finally
            {
                _serialSemaphore.Release();
            }
        }

        private async Task CheckConnectionHealthAsync()
        {
            try
            {
                bool isHealthy = false;
                await _serialSemaphore.WaitAsync();
                try
                {
                    if (_serialPort != null && _serialPort.IsOpen)
                    {
                        try
                        {
                            _serialPort.Write("P"); // Ping command
                            isHealthy = true;
                        }
                        catch (Exception)
                        {
                            isHealthy = false;
                        }
                    }
                }
                finally
                {
                    _serialSemaphore.Release();
                }

                if (!isHealthy)
                {
                    _logger.LogWarning("Serial port health check failed. Closing and will reconnect.");
                    await CloseSerialPortAsync();
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Health check error");
            }
        }

        private async Task CloseSerialPortAsync()
        {
            await _serialSemaphore.WaitAsync();
            try
            {
                if (_serialPort != null && _serialPort.IsOpen)
                {
                    try { _serialPort.Close(); } catch { }
                }
            }
            finally
            {
                _serialSemaphore.Release();
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
                    int delay = _random.Next(MIN_RECONNECT_DELAY_SECONDS, MAX_RECONNECT_DELAY_SECONDS + 1);
                    _logger.LogInformation($"Waiting {delay} seconds before reconnect attempt...");
                    await Task.Delay(TimeSpan.FromSeconds(delay), stoppingToken);

                    if (stoppingToken.IsCancellationRequested)
                        break;

                    _logger.LogInformation("Attempting to reconnect...");

                    await _serialSemaphore.WaitAsync(stoppingToken);
                    try
                    {
                        if (_serialPort != null)
                        {
                            if (_serialPort.IsOpen)
                                _serialPort.Close();
                            _serialPort.Dispose();
                            _serialPort = null;
                        }
                    }
                    finally
                    {
                        _serialSemaphore.Release();
                    }

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
                    bool isConnected = await IsConnectedAsync();

                    if (!isConnected)
                    {
                        await Task.Delay(1000, stoppingToken);
                        continue;
                    }

                    var songData = await GetCurrentSongDataAsync();
                    if (songData != null)
                    {
                        string currentKey = $"{songData.Title}|{songData.Artist}";
                        bool currentIsPlaying = songData.IsPlaying;
                        bool isNewSong = _lastSongKey != currentKey;
                        bool dueForFullResync = (DateTime.Now - _lastFullSendTime) >= _fullResyncInterval;

                        if (isNewSong || dueForFullResync)
                        {
                            _lastSongKey = currentKey;
                            _lastIsPlaying = currentIsPlaying;
                            _lastFullSendTime = DateTime.Now;
                            _lastTimingSendTime = DateTime.Now;

                            bool success = await SendToArduinoWithRetryAsync(songData);
                            if (success)
                            {
                                _logger.LogInformation($"Sent (full): {songData.Title} - {songData.Artist}");
                            }
                            else
                            {
                                _logger.LogWarning("Failed to send data to Arduino - connection may be lost");
                                await CloseSerialPortAsync();
                            }
                        }
                        else
                        {
                            bool playStateChanged = _lastIsPlaying != currentIsPlaying;
                            bool dueForTimingUpdate = (DateTime.Now - _lastTimingSendTime) >= _timingUpdateInterval;

                            if (playStateChanged || dueForTimingUpdate)
                            {
                                _lastIsPlaying = currentIsPlaying;
                                _lastTimingSendTime = DateTime.Now;

                                bool success = await SendTimingUpdateAsync(songData);
                                if (!success)
                                {
                                    _logger.LogWarning("Failed to send timing update - connection may be lost");
                                    await CloseSerialPortAsync();
                                }
                            }
                        }
                    }

                    // Poll faster than the timing interval itself so the
                    // actual send doesn't drift by up to a full interval
                    // due to loop granularity.
                    await Task.Delay(250, stoppingToken);
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

        // Sends the compact real-time audio packet: 'A' + volL + volR + 7 left
        // bands + 7 right bands (17 bytes total). Runs on its own fast loop,
        // independent from the song-metadata/thumbnail packet.
        private async Task SendAudioDataAsync(CancellationToken stoppingToken)
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    if (_audioAnalyzer != null && await IsConnectedAsync())
                    {
                        await SendAudioPacketAsync();
                    }

                    await Task.Delay(_audioUpdateIntervalMs, stoppingToken);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    _logger.LogWarning(ex, "Audio send loop error");
                    await Task.Delay(1000, stoppingToken);
                }
            }
        }

        private async Task SendAudioPacketAsync()
        {
            if (_audioAnalyzer == null) return;

            byte[] packet = new byte[1 + 2 + 7 + 7];
            packet[0] = (byte)'A';
            packet[1] = _audioAnalyzer.VolumeLeft;
            packet[2] = _audioAnalyzer.VolumeRight;
            Array.Copy(_audioAnalyzer.LeftBands, 0, packet, 3, 7);
            Array.Copy(_audioAnalyzer.RightBands, 0, packet, 10, 7);

            await _serialSemaphore.WaitAsync();
            try
            {
                if (_serialPort != null && _serialPort.IsOpen)
                {
                    _serialPort.Write(packet, 0, packet.Length);
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Failed to send audio packet");
            }
            finally
            {
                _serialSemaphore.Release();
            }
        }

        private async Task ReadArduinoOutputAsync(CancellationToken stoppingToken)
        {
            try
            {
                while (!stoppingToken.IsCancellationRequested)
                {
                    bool canRead = false;
                    await _serialSemaphore.WaitAsync(stoppingToken);
                    try
                    {
                        canRead = _serialPort != null && _serialPort.IsOpen && _serialPort.BytesToRead > 0;
                        if (canRead && _serialPort != null)
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
                            catch (Exception ex)
                            {
                                _logger.LogWarning(ex, "Error reading from Arduino - connection may be lost");
                                try { _serialPort.Close(); } catch { }
                            }
                        }
                    }
                    finally
                    {
                        _serialSemaphore.Release();
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

        // Lightweight packet for position/duration/play-state only - no
        // thumbnail, no title/artist. A few bytes, transmits in under a
        // millisecond at 115200 baud, so it's safe to send this often
        // without starving the audio-bar packets of serial time.
        private async Task<bool> SendTimingUpdateAsync(SongData data)
        {
            await _serialSemaphore.WaitAsync();
            try
            {
                if (_serialPort == null || !_serialPort.IsOpen)
                    return false;

                string payload = $"{data.Duration}|{data.Position}|{data.IsPlaying}\n";
                byte[] bytes = System.Text.Encoding.UTF8.GetBytes(payload);

                _serialPort.Write("T");
                _serialPort.Write(bytes, 0, bytes.Length);
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Timing update send error");
                return false;
            }
            finally
            {
                _serialSemaphore.Release();
            }
        }

        // Holds the semaphore for the WHOLE packet (marker + metadata +
        // thumbnail), including the delays between chunks. This guarantees
        // the fast audio-send loop can't write an 'A' packet in the middle
        // of a song packet and desync the Arduino's parser.
        private async Task<bool> SendCompleteDataPacketAsync(SongData data)
        {
            await _serialSemaphore.WaitAsync();
            try
            {
                if (_serialPort == null || !_serialPort.IsOpen)
                    return false;

                _serialPort.DiscardInBuffer();
                _serialPort.DiscardOutBuffer();
                _serialPort.Write("S");
                await Task.Delay(50);

                string metadata = $"{data.Title}|{data.Artist}|{data.Duration}|{data.Position}|{data.Source}|{data.IsPlaying}\n";
                byte[] metadataBytes = System.Text.Encoding.UTF8.GetBytes(metadata);
                byte[] thumbnail = data.Thumbnail ?? CreateTestThumbnail();

                if (!_serialPort.IsOpen) return false;
                _serialPort.Write(metadataBytes, 0, metadataBytes.Length);

                await Task.Delay(50);

                if (!_serialPort.IsOpen) return false;
                _serialPort.Write(thumbnail, 0, thumbnail.Length);

                await Task.Delay(500);
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Send error");
                return false;
            }
            finally
            {
                _serialSemaphore.Release();
            }
        }

        private async Task<SongData?> GetCurrentSongDataAsync()
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

                byte[]? thumbnailBytes = null;
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

        // Rewritten to use LockBits + Marshal.Copy instead of GetPixel, which
        // was doing 6400 individual (slow) calls per thumbnail. This matters
        // more now that the process also has a live audio pipeline competing
        // for CPU time.
        private async Task<byte[]?> GetAndConvertThumbnailAsync(Windows.Storage.Streams.IRandomAccessStreamReference thumbnailRef)
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

                    using var ms = new MemoryStream(rawBytes);
                    using var originalImage = System.Drawing.Image.FromStream(ms);
                    using var resizedBitmap = new System.Drawing.Bitmap(80, 80, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
                    using (var graphics = System.Drawing.Graphics.FromImage(resizedBitmap))
                    {
                        graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                        graphics.DrawImage(originalImage, 0, 0, 80, 80);
                    }

                    var rect = new System.Drawing.Rectangle(0, 0, 80, 80);
                    var bmpData = resizedBitmap.LockBits(rect, System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format24bppRgb);

                    try
                    {
                        int stride = bmpData.Stride;
                        byte[] pixelData = new byte[stride * 80];
                        Marshal.Copy(bmpData.Scan0, pixelData, 0, pixelData.Length);

                        byte[] rgb565 = new byte[80 * 80 * 2];
                        for (int y = 0; y < 80; y++)
                        {
                            int rowStart = y * stride;
                            for (int x = 0; x < 80; x++)
                            {
                                int px = rowStart + x * 3;
                                byte b = pixelData[px];
                                byte g = pixelData[px + 1];
                                byte r = pixelData[px + 2];

                                int r5 = (r >> 3) & 0x1F;
                                int g6 = (g >> 2) & 0x3F;
                                int b5 = (b >> 3) & 0x1F;
                                ushort color = (ushort)((r5 << 11) | (g6 << 5) | b5);

                                int idx = (y * 80 + x) * 2;
                                rgb565[idx] = (byte)(color & 0xFF);
                                rgb565[idx + 1] = (byte)((color >> 8) & 0xFF);
                            }
                        }
                        return rgb565;
                    }
                    finally
                    {
                        resizedBitmap.UnlockBits(bmpData);
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
        public string Title { get; set; } = "";
        public string Artist { get; set; } = "";
        public int Duration { get; set; }
        public int Position { get; set; }
        public string Source { get; set; } = "";
        public bool IsPlaying { get; set; }
        public byte[]? Thumbnail { get; set; }
    }
}