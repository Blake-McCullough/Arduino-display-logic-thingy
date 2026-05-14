using System;
using System.IO;
using System.IO.Ports;
using System.Threading.Tasks;
using Windows.Media.Control;
using Windows.Storage.Streams;
using System.Drawing;
using System.Drawing.Imaging;
using System.Threading;

namespace MusicToArduino
{
    class Program
    {
        private static SerialPort _serialPort;
        private static string _lastSongKey = "";
        private static DateTime _lastSendTime = DateTime.MinValue;
        private static TimeSpan _minSendInterval = TimeSpan.FromSeconds(3);
        private static readonly object _serialLock = new object();

        // Chunk configuration
        private const int MAX_RETRIES = 3;

        static async Task Main(string[] args)
        {
            Console.WriteLine("╔═══════════════════════════════════════╗");
            Console.WriteLine("║    Music Player to Arduino Bridge     ║");
            Console.WriteLine("╚═══════════════════════════════════════╝\n");

            string[] ports = SerialPort.GetPortNames();
            if (ports.Length == 0)
            {
                Console.WriteLine("No COM ports found!");
                Console.ReadKey();
                return;
            }

            Console.WriteLine("Available COM ports:");
            foreach (string port in ports) Console.WriteLine($"  {port}");

            Console.Write("\nEnter COM port: ");
            string comPort = Console.ReadLine();
            Console.Write("Enter baud rate (default 115200): ");
            string baudInput = Console.ReadLine();
            int baudRate = string.IsNullOrEmpty(baudInput) ? 115200 : int.Parse(baudInput);

            try
            {
                _serialPort = new SerialPort(comPort, baudRate, Parity.None, 8, StopBits.One);
                _serialPort.ReadTimeout = 1000;
                _serialPort.Open();
                Console.WriteLine($"\n✓ Connected to {comPort}\n");
                await MonitorMusic();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"✗ Error: {ex.Message}");
                Console.ReadKey();
            }
        }

        static async Task MonitorMusic()
        {
            _ = Task.Run(() => ReadArduinoOutput());

            while (true)
            {
                try
                {
                    var songData = await GetCurrentSongData();
                    if (songData != null)
                    {
                        string currentKey = $"{songData.Title}|{songData.Artist}";
                        if ((DateTime.Now - _lastSendTime) >= _minSendInterval)
                        {
                            _lastSongKey = currentKey;
                            _lastSendTime = DateTime.Now;
                            await SendToArduinoWithRetry(songData);
                            Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] ✓ {songData.Title} - {songData.Artist}");
                        }

                    }
                    await Task.Delay(1000);
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"Error: {ex.Message}");
                    await Task.Delay(5000);
                }
            }
        }

        static async Task ReadArduinoOutput()
        {
            try
            {
                while (_serialPort != null && _serialPort.IsOpen)
                {
                    if (_serialPort.BytesToRead > 0)
                    {
                        string line = _serialPort.ReadLine();
                        if (!string.IsNullOrWhiteSpace(line))
                        {
                            Console.ForegroundColor = ConsoleColor.DarkGray;
                            Console.WriteLine($"[ARDUINO] {line}");
                            Console.ResetColor();
                        }
                    }
                    await Task.Delay(10);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error reading from Arduino: {ex.Message}");
            }
        }
        static async Task<bool> SendToArduinoWithRetry(SongData data)
        {
            for (int attempt = 1; attempt <= MAX_RETRIES; attempt++)
            {
                Console.WriteLine($"\n📤 Sending data (Attempt {attempt}/{MAX_RETRIES})...");

                if (await SendCompleteDataPacket(data))
                {
                    Console.WriteLine("✓ Data sent successfully!");
                    return true;
                }

                Console.WriteLine($"✗ Attempt {attempt} failed. {(attempt < MAX_RETRIES ? "Retrying..." : "Giving up.")}");

                if (attempt < MAX_RETRIES)
                {
                    await Task.Delay(1000);
                }
            }

            return false;
        }

        static async Task<bool> SendCompleteDataPacket(SongData data)
        {
            try
            {
                lock (_serialLock)
                {
                    // Clear buffers
                    _serialPort.DiscardInBuffer();
                    _serialPort.DiscardOutBuffer();
                }

                // Send start marker 'S'
                lock (_serialLock)
                {
                    _serialPort.Write(System.Text.Encoding.UTF8.GetBytes("S"), 0, 1);
                    Thread.Sleep(50);
                }

                // Prepare metadata string with newline terminator
                string metadata = $"{data.Title}|{data.Artist}|{data.Duration}|{data.Position}|{data.Source}|{data.IsPlaying}\n";
                byte[] metadataBytes = System.Text.Encoding.UTF8.GetBytes(metadata);

                // Get thumbnail data
                byte[] thumbnail = data.Thumbnail ?? CreateTestThumbnail();

                // Send metadata
                lock (_serialLock)
                {
                    _serialPort.Write(metadataBytes, 0, metadataBytes.Length);
                    _serialPort.BaseStream.Flush();
                }

                Console.WriteLine($"  Sent metadata: {metadata.Trim()} ({metadataBytes.Length} bytes)");

                // Small delay to ensure Arduino processes metadata
                await Task.Delay(50);

                // Send thumbnail
                lock (_serialLock)
                {
                    _serialPort.Write(thumbnail, 0, thumbnail.Length);
                    _serialPort.BaseStream.Flush();
                }

                Console.WriteLine($"  Sent thumbnail ({thumbnail.Length} bytes)");

                // Give Arduino time to process
                await Task.Delay(500);

                return true;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  Send error: {ex.Message}");
                return false;
            }
        }
       
        static async Task<SongData> GetCurrentSongData()
        {
            try
            {
                var sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                var currentSession = sessionManager.GetCurrentSession();
                //currentSession.SourceAppUserModelId
                if (currentSession == null) return null;

                var mediaProperties = await currentSession.TryGetMediaPropertiesAsync();
                if (string.IsNullOrWhiteSpace(mediaProperties.Title)) return null;

                var timelineProperties = currentSession.GetTimelineProperties();
                var playbackPropetires = currentSession.GetPlaybackInfo();

                bool isPlaying = playbackPropetires.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;

                // Get the source app ID
                string sourceApp = currentSession.SourceAppUserModelId ?? "Unknown Source";
                // Clean up the source string (remove common prefixes)
                if (sourceApp.Contains("!"))
                {
                    sourceApp = sourceApp.Substring(sourceApp.LastIndexOf('!') + 1);
                }
                sourceApp = sourceApp.Replace("Microsoft.", "").Replace("Spotify", "Spotify").Replace("AppleInc.", "Apple ");

                string title = mediaProperties.Title.Replace("|", "").Trim();
                string artist = (mediaProperties.Artist ?? "Unknown Artist").Replace("|", "").Trim();

                byte[] thumbnailBytes = null;

                // Only process thumbnail for new songs
                string currentKey = $"{title}|{artist}";
                if (currentKey != _lastSongKey && mediaProperties.Thumbnail != null)
                {
                    thumbnailBytes = await GetAndConvertThumbnail(mediaProperties.Thumbnail);
                    if (thumbnailBytes != null)
                    {
                        File.WriteAllBytes("latest_thumbnail.raw", thumbnailBytes);
                        Console.WriteLine($"  Thumbnail: {thumbnailBytes.Length} bytes saved to latest_thumbnail.raw");
                        Console.WriteLine($"  First bytes: {BitConverter.ToString(thumbnailBytes.Take(16).ToArray())}");
                    }
                }

                return new SongData
                {
                    Title = title,
                    Artist = artist,
                    Duration = (int)timelineProperties.EndTime.TotalSeconds,
                    Position = (int)timelineProperties.Position.TotalSeconds,
                    Source = sourceApp,
                    IsPlaying = isPlaying,  // Add this
                    Thumbnail = thumbnailBytes ?? CreateTestThumbnail()
                };
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error: {ex.Message}");
                return null;
            }
        }

        static async Task<byte[]> GetAndConvertThumbnail(IRandomAccessStreamReference thumbnailRef)
        {
            try
            {
                using (var stream = await thumbnailRef.OpenReadAsync())
                {
                    Console.WriteLine($"  Stream size: {stream.Size} bytes");
                    byte[] rawBytes = new byte[stream.Size];
                    using (var dataReader = new DataReader(stream))
                    {
                        await dataReader.LoadAsync((uint)stream.Size);
                        dataReader.ReadBytes(rawBytes);
                    }

                    File.WriteAllBytes("original_thumbnail.jpg", rawBytes);
                    Console.WriteLine($"  Saved original thumbnail ({(rawBytes.Length / 1024)} KB)");

                    using (var ms = new MemoryStream(rawBytes))
                    using (var originalImage = Image.FromStream(ms))
                    {
                        Console.WriteLine($"  Original size: {originalImage.Width}x{originalImage.Height}");

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

                            resizedBitmap.Save("resized_thumbnail.bmp", ImageFormat.Bmp);
                            Console.WriteLine($"  Saved resized thumbnail to resized_thumbnail.bmp");
                            return rgb565;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  Thumbnail conversion failed: {ex.Message}");
                return null;
            }
        }

        static byte[] CreateTestThumbnail()
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

    class SongData
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