using System;
using System.IO;
using System.IO.Ports;
using System.Threading.Tasks;
using Windows.Media.Control;
using Windows.Storage.Streams;
using System.Drawing;
using System.Drawing.Imaging;
using System.Collections.Generic;
using System.Linq;

namespace MusicToArduino
{
    class Program
    {
        private static SerialPort _serialPort;
        private static string _lastSongKey = "";
        private static byte[] _lastThumbnail = null;
        private static DateTime _lastSendTime = DateTime.MinValue;
        private static TimeSpan _minSendInterval = TimeSpan.FromSeconds(3);

        // SLIP special characters
        private const byte SLIP_END = 0xC0;
        private const byte SLIP_ESC = 0xDB;
        private const byte SLIP_ESC_END = 0xDC;
        private const byte SLIP_ESC_ESC = 0xDD;

        static async Task Main(string[] args)
        {
            Console.WriteLine("╔═══════════════════════════════════════╗");
            Console.WriteLine("║    Music Player to Arduino Bridge     ║");
            Console.WriteLine("║         (SLIP Protocol v1.0)          ║");
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
                Console.WriteLine($"\n✓ Connected to {comPort} at {baudRate} baud\n");

                // Start reading SLIP packets
                _ = Task.Run(() => ReadSlipPackets());
                _ = Task.Run(() => ReadArduinoOutput());

                await MonitorMusic();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"✗ Error: {ex.Message}");
                Console.ReadKey();
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

        static async Task MonitorMusic()
        {
            await Task.Delay(2000); // Wait for Arduino to boot

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
                            SendSlipPacket(songData);
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

        static void SendSlipPacket(SongData data)
        {
            if (_serialPort?.IsOpen != true) return;

            try
            {
                // Build packet data
                using (var ms = new MemoryStream())
                {
                    // Write header as UTF-8
                    string header = $"{data.Title}|{data.Artist}|{data.Duration}|{data.Position}";
                    byte[] headerBytes = System.Text.Encoding.UTF8.GetBytes(header);
                    ms.Write(headerBytes, 0, headerBytes.Length);

                    // Add separator
                    ms.WriteByte(0x00); // Null separator between header and thumbnail

                    // Write thumbnail if available
                    if (data.Thumbnail != null && data.Thumbnail.Length == 12800)
                    {
                        ms.Write(data.Thumbnail, 0, data.Thumbnail.Length);
                    }

                    byte[] packetData = ms.ToArray();

                    // SLIP encode and send
                    byte[] slipPacket = SlipEncode(packetData);
                    _serialPort.Write(slipPacket, 0, slipPacket.Length);
                    _serialPort.BaseStream.Flush();

                    Console.WriteLine($"  → Sent SLIP packet: {packetData.Length} bytes data → {slipPacket.Length} bytes SLIP");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Send error: {ex.Message}");
            }
        }

        static byte[] SlipEncode(byte[] data)
        {
            using (var ms = new MemoryStream())
            {
                ms.WriteByte(SLIP_END); // Start with END

                foreach (byte b in data)
                {
                    if (b == SLIP_END)
                    {
                        ms.WriteByte(SLIP_ESC);
                        ms.WriteByte(SLIP_ESC_END);
                    }
                    else if (b == SLIP_ESC)
                    {
                        ms.WriteByte(SLIP_ESC);
                        ms.WriteByte(SLIP_ESC_ESC);
                    }
                    else
                    {
                        ms.WriteByte(b);
                    }
                }

                ms.WriteByte(SLIP_END); // End with END
                return ms.ToArray();
            }
        }

        static async Task ReadSlipPackets()
        {
            List<byte> packetBuffer = new List<byte>();
            bool inPacket = false;

            while (_serialPort != null && _serialPort.IsOpen)
            {
                try
                {
                    if (_serialPort.BytesToRead > 0)
                    {
                        byte b = (byte)_serialPort.ReadByte();

                        if (b == SLIP_END)
                        {
                            if (inPacket && packetBuffer.Count > 0)
                            {
                                // Decode and process packet
                                byte[] decoded = SlipDecode(packetBuffer.ToArray());
                                ProcessArduinoPacket(decoded);
                                packetBuffer.Clear();
                                inPacket = false;
                            }
                            else
                            {
                                inPacket = true;
                            }
                        }
                        else if (inPacket)
                        {
                            packetBuffer.Add(b);
                        }
                    }
                    await Task.Delay(5);
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"Read error: {ex.Message}");
                }
            }
        }

        static byte[] SlipDecode(byte[] data)
        {
            using (var ms = new MemoryStream())
            {
                for (int i = 0; i < data.Length; i++)
                {
                    if (data[i] == SLIP_ESC)
                    {
                        if (i + 1 < data.Length)
                        {
                            if (data[i + 1] == SLIP_ESC_END)
                            {
                                ms.WriteByte(SLIP_END);
                                i++;
                            }
                            else if (data[i + 1] == SLIP_ESC_ESC)
                            {
                                ms.WriteByte(SLIP_ESC);
                                i++;
                            }
                        }
                    }
                    else
                    {
                        ms.WriteByte(data[i]);
                    }
                }
                return ms.ToArray();
            }
        }

        static void ProcessArduinoPacket(byte[] packet)
        {
            string message = System.Text.Encoding.UTF8.GetString(packet);
            Console.ForegroundColor = ConsoleColor.DarkGray;
            Console.WriteLine($"[ARDUINO] {message}");
            Console.ResetColor();
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

                string currentKey = $"{title}|{artist}";
                if (currentKey != _lastSongKey && mediaProperties.Thumbnail != null)
                {
                    thumbnailBytes = await GetAndConvertThumbnail(mediaProperties.Thumbnail);
                }

                return new SongData
                {
                    Title = title,
                    Artist = artist,
                    Duration = (int)timelineProperties.EndTime.TotalSeconds,
                    Position = (int)timelineProperties.Position.TotalSeconds,
                    Thumbnail = thumbnailBytes ?? _lastThumbnail
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
                    byte[] rawBytes = new byte[stream.Size];
                    using (var dataReader = new DataReader(stream))
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

                                ushort color = (ushort)((r << 11) | (g << 5) | b);

                                int index = (y * 80 + x) * 2;
                                rgb565[index] = (byte)(color & 0xFF);
                                rgb565[index + 1] = (byte)((color >> 8) & 0xFF);
                            }
                        }

                        return rgb565;
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Thumbnail error: {ex.Message}");
                return null;
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