using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System.Diagnostics;

namespace MusicToArduino
{
    class Program
    {
        private static Mutex? _appMutex;
        private const string MUTEX_NAME = @"Global\MusicToArduinoBridge";

        static async Task Main(string[] args)
        {
            // Try to create a named mutex
            bool createdNew;
            _appMutex = new Mutex(true, MUTEX_NAME, out createdNew);

            if (!createdNew)
            {
                // Another instance is already running - kill it
                Console.WriteLine("Another instance detected. Terminating the old instance...");

                // Kill the existing process
                KillExistingInstance();

                // Wait a moment for the old process to terminate
                await Task.Delay(1000);

                // Now try to create the mutex again
                _appMutex = new Mutex(true, MUTEX_NAME, out createdNew);
                if (!createdNew)
                {
                    Console.WriteLine("Failed to terminate the existing instance. Exiting.");
                    return;
                }
            }

            try
            {
                // Run the application
                Console.WriteLine("Music To Arduino Bridge starting...");
                Console.WriteLine("Press Ctrl+C to exit");

                var host = CreateHostBuilder(args).Build();
                await host.StartAsync();

                var waitHandle = new ManualResetEvent(false);
                Console.CancelKeyPress += (sender, e) =>
                {
                    e.Cancel = true;
                    waitHandle.Set();
                };
                waitHandle.WaitOne();

                await host.StopAsync();
            }
            finally
            {
                // Release the mutex when done
                _appMutex?.ReleaseMutex();
                _appMutex?.Dispose();
            }
        }

        private static void KillExistingInstance()
        {
            try
            {
                string currentProcessName = Process.GetCurrentProcess().ProcessName;
                string currentProcessId = Process.GetCurrentProcess().Id.ToString();

                foreach (var process in Process.GetProcessesByName(currentProcessName))
                {
                    // Skip the current process
                    if (process.Id.ToString() == currentProcessId)
                        continue;

                    // Kill the old process
                    Console.WriteLine($"Killing existing process: {process.Id} - {process.ProcessName}");
                    process.Kill();
                    process.WaitForExit(5000); // Wait up to 5 seconds for it to exit
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error killing existing instance: {ex.Message}");
            }
        }

        static IHostBuilder CreateHostBuilder(string[] args) =>
            Host.CreateDefaultBuilder(args)
                .ConfigureServices((hostContext, services) =>
                {
                    services.AddHostedService<MusicToArduinoService>();
                })
                .ConfigureLogging((context, logging) =>
                {
                    logging.ClearProviders();
                    logging.AddConsole();
                });
    }
}