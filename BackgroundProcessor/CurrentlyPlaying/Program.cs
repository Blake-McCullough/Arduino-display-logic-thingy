using System;
using System.IO;
using System.IO.Ports;
using System.Threading.Tasks;
using Windows.Media.Control;
using Windows.Storage.Streams;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace MusicToArduino
{
    class Program
    {
        private static SerialPort _serialPort;
        private static string _lastSongKey = "";
        private static byte[] _lastThumbnail = null;
        private static DateTime _lastSendTime = DateTime.MinValue;
        private static TimeSpan _minSendInterval = TimeSpan.FromSeconds(3);

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
                        if (currentKey != _lastSongKey && (DateTime.Now - _lastSendTime) >= _minSendInterval)
                        {
                            _lastSongKey = currentKey;
                            _lastSendTime = DateTime.Now;
                            SendToArduino(songData);
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
                            // Format Arduino output nicely
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


        static async Task<SongData> GetCurrentSongData()
        {
            try
            {
                var sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                var currentSession = sessionManager.GetCurrentSession();
                if (currentSession == null) return null;

                var mediaProperties = await currentSession.TryGetMediaPropertiesAsync();
                if (string.IsNullOrWhiteSpace(mediaProperties.Title)) return null;

                var timelineProperties = currentSession.GetTimelineProperties();

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
                        // Save for debugging
                        File.WriteAllBytes("latest_thumbnail.raw", thumbnailBytes);
                        Console.WriteLine($"  Thumbnail: {thumbnailBytes.Length} bytes saved to latest_thumbnail.raw");

                        // Verify first few bytes are valid
                        Console.WriteLine($"  First bytes: {BitConverter.ToString(thumbnailBytes.Take(16).ToArray())}");
                    }
                }

                return new SongData
                {
                    Title = title,
                    Artist = artist,
                    Duration = (int)timelineProperties.EndTime.TotalSeconds,
                    Position = (int)timelineProperties.Position.TotalSeconds,
                    Thumbnail = CreateTestThumbnail()
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
                // Open the stream
                using (var stream = await thumbnailRef.OpenReadAsync())
                {
                    Console.WriteLine($"  Stream size: {stream.Size} bytes");

                    // Read raw image bytes
                    byte[] rawBytes = new byte[stream.Size];
                    using (var dataReader = new DataReader(stream))
                    {
                        await dataReader.LoadAsync((uint)stream.Size);
                        dataReader.ReadBytes(rawBytes);
                    }

                    // Save original for debugging
                    File.WriteAllBytes("original_thumbnail.jpg", rawBytes);
                    Console.WriteLine($"  Saved original thumbnail ({(rawBytes.Length / 1024)} KB)");

                    // Convert to bitmap
                    using (var ms = new MemoryStream(rawBytes))
                    using (var originalImage = Image.FromStream(ms))
                    {
                        Console.WriteLine($"  Original size: {originalImage.Width}x{originalImage.Height}");

                        // Create resized 80x80 image
                        using (var resizedBitmap = new Bitmap(80, 80))
                        using (var graphics = Graphics.FromImage(resizedBitmap))
                        {
                            graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                            graphics.DrawImage(originalImage, 0, 0, 80, 80);

                            // Convert to RGB565
                            byte[] rgb565 = new byte[80 * 80 * 2];

                            for (int y = 0; y < 80; y++)
                            {
                                for (int x = 0; x < 80; x++)
                                {
                                    Color pixel = resizedBitmap.GetPixel(x, y);

                                    // RGB565 conversion
                                    int r = (pixel.R >> 3) & 0x1F;
                                    int g = (pixel.G >> 2) & 0x3F;
                                    int b = (pixel.B >> 3) & 0x1F;

                                    ushort rgb565Color = (ushort)((r << 11) | (g << 5) | b);

                                    int index = (y * 80 + x) * 2;
                                    rgb565[index] = (byte)(rgb565Color & 0xFF);     // Low byte
                                    rgb565[index + 1] = (byte)((rgb565Color >> 8) & 0xFF); // High byte
                                }
                            }

                            // Create a test BMP to verify the resized image
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
            // Create a simple 80x80 gradient pattern for testing
            byte[] rgb565 = new byte[80 * 80 * 2];

            for (int y = 0; y < 80; y++)
            {
                for (int x = 0; x < 80; x++)
                {
                    // Create a rainbow pattern
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

        // Use this to test if the Arduino can display ANY image:
        // thumbnailBytes = CreateTestThumbnail();
        static void SendToArduino(SongData data)
        {
            if (_serialPort?.IsOpen != true) return;

            try
            {
                // Send header
                string header = $"{data.Title}|{data.Artist}|{data.Duration}|{data.Position}|";
                byte[] headerBytes = System.Text.Encoding.UTF8.GetBytes(header);
                _serialPort.Write(headerBytes, 0, headerBytes.Length);

                // Send thumbnail if available
                if (data.Thumbnail != null && data.Thumbnail.Length == 12800)
                {
                    _serialPort.Write(data.Thumbnail, 0, data.Thumbnail.Length);
                    Console.WriteLine($"  Sent thumbnail: 12800 bytes");
                }
                else
                {
                    // Send placeholder data (all zeros)
                    byte[] placeholder = new byte[12800];
                    _serialPort.Write(placeholder, 0, placeholder.Length);
                    Console.WriteLine($"  Sent placeholder thumbnail");
                }

                _serialPort.Write(new byte[] { (byte)'\n' }, 0, 1);
                _serialPort.BaseStream.Flush();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Send error: {ex.Message}");
            }
        }
    }

    class SongData
    {
        public string Title { get; set; }
        public string Artist { get; set; }
        public int Duration { get; set; }
        public int Position { get; set; }
        public byte[] Thumbnail { get; set; }
    }
}