using System;
using System.IO.Ports;
using System.Threading;
using System.Threading.Tasks;
using Windows.Media.Control;

namespace MusicToArduino
{
    class Program
    {
        private static SerialPort _serialPort;
        private static string _lastSong = "";

        static async Task Main(string[] args)
        {
            // Find available COM ports
            Console.WriteLine("Available COM ports:");
            foreach (string port in SerialPort.GetPortNames())
            {
                Console.WriteLine($"  {port}");
            }

            Console.Write("\nEnter COM port (e.g., COM3): ");
            string comPort = Console.ReadLine();

            Console.Write("Enter baud rate (default 9600): ");
            string baudInput = Console.ReadLine();
            int baudRate = string.IsNullOrEmpty(baudInput) ? 9600 : int.Parse(baudInput);

            // Setup serial port
            _serialPort = new SerialPort(comPort, baudRate, Parity.None, 8, StopBits.One);
            _serialPort.Open();
            Console.WriteLine($"Connected to {comPort} at {baudRate} baud\n");

            // Start monitoring music
            Console.WriteLine("Monitoring music... Press Ctrl+C to stop");
            await MonitorMusic();
        }

        static async Task MonitorMusic()
        {
            while (true)
            {
                try
                {
                    string songInfo = await GetCurrentSong();

                    if (!string.IsNullOrEmpty(songInfo) && songInfo != _lastSong)
                    {
                        _lastSong = songInfo;
                        SendToArduino(songInfo);
                        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] Sent: {songInfo}");
                    }

                    await Task.Delay(2000); // Check every 2 seconds
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"Error: {ex.Message}");
                    await Task.Delay(5000);
                }
            }
        }

        static async Task<string> GetCurrentSong()
        {
            try
            {
                var sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                var currentSession = sessionManager.GetCurrentSession();

                if (currentSession != null)
                {
                    var mediaProperties = await currentSession.TryGetMediaPropertiesAsync();
                    var timelineProperties = currentSession.GetTimelineProperties();

                    if (!string.IsNullOrWhiteSpace(mediaProperties.Title))
                    {
                        string artist = string.IsNullOrWhiteSpace(mediaProperties.Artist) ? "Unknown" : mediaProperties.Artist;
                        string title = mediaProperties.Title;

                        // Format: ARTIST|TITLE|DURATION|POSITION
                        int duration = (int)timelineProperties.EndTime.TotalSeconds;
                        int position = (int)timelineProperties.Position.TotalSeconds;

                        // Encode to avoid special characters
                        return $"{artist}|{title}|{duration}|{position}";
                    }
                }
                return null;
            }
            catch
            {
                return null;
            }
        }

        static void SendToArduino(string data)
        {
            if (_serialPort != null && _serialPort.IsOpen)
            {
                // Old school style: send with start/end markers
                _serialPort.Write($"<{data}>\n");
            }
        }
    }
}