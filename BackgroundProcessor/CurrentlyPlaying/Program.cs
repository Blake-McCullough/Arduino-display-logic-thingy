using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System.Diagnostics;

namespace MusicToArduino
{
    class Program
    {
        static async Task Main(string[] args)
        {
            // Check if we should run in console mode
            bool isConsoleMode = args.Contains("--console") || Debugger.IsAttached;

            if (isConsoleMode)
            {
                Console.WriteLine("╔════════════════════════════════════════════════╗");
                Console.WriteLine("║   Music To Arduino Bridge - Console Mode      ║");
                Console.WriteLine("║   Press Ctrl+C to exit                        ║");
                Console.WriteLine("╚════════════════════════════════════════════════╝");
                Console.WriteLine();

                var host = CreateHostBuilder(args).Build();
                await host.StartAsync();

                var tcs = new TaskCompletionSource<bool>();
                Console.CancelKeyPress += (sender, e) =>
                {
                    e.Cancel = true;
                    tcs.SetResult(true);
                };

                await tcs.Task;
                await host.StopAsync();
            }
            else
            {
                // Run as Windows Service
                await CreateHostBuilder(args).Build().RunAsync();
            }
        }

        static IHostBuilder CreateHostBuilder(string[] args) =>
            Host.CreateDefaultBuilder(args)
                .UseWindowsService(options =>
                {
                    options.ServiceName = "MusicToArduinoBridge";
                })
                .ConfigureServices((hostContext, services) =>
                {
                    services.AddHostedService<MusicToArduinoService>();
                    services.AddLogging(builder =>
                    {
                        builder.AddEventLog();
                        builder.AddConsole();
                    });
                });
    }
}