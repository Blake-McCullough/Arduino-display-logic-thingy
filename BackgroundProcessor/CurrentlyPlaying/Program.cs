using System;
using System.Threading.Tasks;
using Windows.Media.Control;

namespace MusicIdentifier
{
    class Program
    {
        static async Task Main(string[] args)
        {
            // Get the song info when the program runs
            await GetNowPlaying();
            Console.ReadKey();
        }

        static async Task GetNowPlaying()
        {
            try
            {
                // 1. Request the global session manager
                var sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();

                // 2. Get the current active session
                var currentSession = sessionManager.GetCurrentSession();

                // 3. If a media session is active, get its properties
                if (currentSession != null)
                {
                    var mediaProperties = await currentSession.TryGetMediaPropertiesAsync();
                    // Get timeline info (duration, position, status)
                    var timelineProperties = currentSession.GetTimelineProperties();

                    // 4. Check if there's actually a song with a title
                    if (!string.IsNullOrWhiteSpace(mediaProperties.Title))
                    {
                        Console.WriteLine("═══════════════════════════════");
                        Console.WriteLine("═══════════════════════════════");
                        Console.WriteLine($"🎵 Now Playing: {mediaProperties.Title}");
                        Console.WriteLine($"🎤 Artist: {mediaProperties.Artist}");
                        Console.WriteLine($"💿 Album: {mediaProperties.AlbumTitle}");
                        //Console.WriteLine($"⏱️  Duration: {mediaProperties.Duration:hh\\:mm\\:ss}");
                        Console.WriteLine("═══════════════════════════════");
                    }
                    else
                    {
                        Console.WriteLine("Media is playing but no song info is available.");
                    }
                }
                else
                {
                    Console.WriteLine("No media session is currently active. Please start playing a song.");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"An error occurred: {ex.Message}");
                Console.WriteLine("Make sure you are playing media on your PC (e.g., Spotify, a YouTube video in a browser).");
            }
        }
    }
}