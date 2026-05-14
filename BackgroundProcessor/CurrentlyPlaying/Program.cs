using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;

namespace MusicToArduino
{
    class Program
    {
        static async Task Main(string[] args)
        {
            var isService = !(Debugger.IsAttached || args.Contains("--console"));

            if (isService)
            {
                await CreateHostBuilder(args).Build().RunAsync();
            }
            else
            {
                // Run as console app for debugging
                Console.WriteLine("Running in console mode for debugging...");
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
                })
                .ConfigureLogging((context, logging) =>
                {
                    logging.ClearProviders();
                    logging.AddEventLog();
                    logging.AddConsole();
                });
    }
}