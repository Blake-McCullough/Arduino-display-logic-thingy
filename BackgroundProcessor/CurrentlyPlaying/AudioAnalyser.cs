// AudioAnalyzer.cs
//
// Captures system audio (WASAPI loopback) and continuously computes:
//   - Left/Right overall volume (0-100)
//   - Left/Right magnitude for 7 frequency bands (0-100 each)
//
// Requires the NAudio package:
//   dotnet add package NAudio
//
// The analyzer runs entirely off the audio device's callback thread; the
// service just reads the latest snapshot (VolumeLeft/VolumeRight/LeftBands/
// RightBands) whenever it wants to send a packet. No polling/blocking here.

using Microsoft.Extensions.Logging;
using NAudio.Dsp;
using NAudio.Wave;

namespace MusicToArduino
{
    public class AudioAnalyzer : IDisposable
    {
        private readonly ILogger _logger;
        private WasapiLoopbackCapture? _capture;

        // Must be a power of two for the FFT.
        private const int FftLength = 2048;
        private readonly int _fftPower = (int)Math.Log2(FftLength);

        private readonly float[] _leftBuffer = new float[FftLength];
        private readonly float[] _rightBuffer = new float[FftLength];
        private int _bufferPos;

        private readonly object _resultLock = new object();
        private bool _loggedFormatWarning;

        public byte VolumeLeft { get; private set; }
        public byte VolumeRight { get; private set; }
        public byte[] LeftBands { get; } = new byte[7];
        public byte[] RightBands { get; } = new byte[7];

        // Sub-Bass, Bass, Low Mids, Midrange, Upper Mids, Presence, Brilliance/Air
        private static readonly (double Low, double High)[] BandRanges =
        {
            (20, 60),
            (60, 250),
            (250, 500),
            (500, 2000),
            (2000, 4000),
            (4000, 6000),
            (6000, 20000),
        };

        public AudioAnalyzer(ILogger logger)
        {
            _logger = logger;
        }

        public void Start()
        {
            try
            {
                _capture = new WasapiLoopbackCapture();
                _capture.DataAvailable += OnDataAvailable;
                _capture.RecordingStopped += OnRecordingStopped;
                _capture.StartRecording();
                _logger.LogInformation("Audio capture started ({SampleRate}Hz, {Channels}ch, {Bits}bit, {Encoding})",
                    _capture.WaveFormat.SampleRate, _capture.WaveFormat.Channels,
                    _capture.WaveFormat.BitsPerSample, _capture.WaveFormat.Encoding);
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to start audio capture. Audio visualization will be unavailable.");
            }
        }

        private void OnRecordingStopped(object? sender, StoppedEventArgs e)
        {
            if (e.Exception != null)
            {
                _logger.LogWarning(e.Exception, "Audio capture stopped unexpectedly");
            }
        }

        private void OnDataAvailable(object? sender, WaveInEventArgs e)
        {
            try
            {
                var format = _capture?.WaveFormat;
                if (format == null) return;

                // Loopback capture is essentially always 32-bit IEEE float.
                // If a device reports something else, bail out safely rather
                // than misinterpreting bytes.
                if (format.Encoding != WaveFormatEncoding.IeeeFloat || format.BitsPerSample != 32)
                {
                    if (!_loggedFormatWarning)
                    {
                        _logger.LogWarning("Unsupported audio format ({Encoding}, {Bits}bit) - audio visualization disabled",
                            format.Encoding, format.BitsPerSample);
                        _loggedFormatWarning = true;
                    }
                    return;
                }

                int channels = format.Channels;
                int bytesPerSample = 4; // 32-bit float
                int frameSize = bytesPerSample * channels;
                int frameCount = e.BytesRecorded / frameSize;

                for (int i = 0; i < frameCount; i++)
                {
                    int baseIndex = i * frameSize;
                    float left = BitConverter.ToSingle(e.Buffer, baseIndex);
                    float right = channels > 1
                        ? BitConverter.ToSingle(e.Buffer, baseIndex + bytesPerSample)
                        : left;

                    _leftBuffer[_bufferPos] = left;
                    _rightBuffer[_bufferPos] = right;
                    _bufferPos++;

                    if (_bufferPos >= FftLength)
                    {
                        ProcessBuffer(format.SampleRate);
                        _bufferPos = 0;
                    }
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error processing audio buffer");
            }
        }

        private void ProcessBuffer(int sampleRate)
        {
            byte volL = RmsToByte(CalculateRms(_leftBuffer));
            byte volR = RmsToByte(CalculateRms(_rightBuffer));

            byte[] leftBands = ComputeBands(_leftBuffer, sampleRate, _leftBandPeak);
            byte[] rightBands = ComputeBands(_rightBuffer, sampleRate, _rightBandPeak);

            lock (_resultLock)
            {
                VolumeLeft = volL;
                VolumeRight = volR;
                Array.Copy(leftBands, LeftBands, 7);
                Array.Copy(rightBands, RightBands, 7);
            }
        }

        private readonly Complex[] _fftBuffer = new Complex[FftLength];

        // Per-band, per-channel "recent peak" trackers used for auto-gain
        // (see ComputeBands). Without this, a single fixed dB range applied
        // to every band means bass/low-mid content (which naturally carries
        // far more energy in most music) reads loud while the upper bands
        // rarely cross the threshold at all - which is why only a couple of
        // bars were moving. Normalizing each band against its own recent
        // peak instead means every band uses its own dynamic range.
        private readonly double[] _leftBandPeak = InitPeaks();
        private readonly double[] _rightBandPeak = InitPeaks();
        private static double[] InitPeaks()
        {
            var p = new double[7];
            Array.Fill(p, PeakFloor);
            return p;
        }

        // How quickly a band's peak "forgets" a loud moment. Applied once
        // per FFT frame (~46ms at 44.1kHz/2048 samples), so 0.985 decays
        // to half over roughly 2 seconds - responsive, but not so twitchy
        // that a single transient throws off normalization.
        private const double PeakDecay = 0.985;
        private const double PeakFloor = 1e-5;

        // Scratch space for ComputeBands, reused across calls instead of
        // allocating two small arrays on every call (~86 times/second across
        // both channels) purely to add GC pressure to the audio callback
        // thread. Safe to share between the left/right calls because each
        // call finishes reading/writing these before ComputeBands returns.
        private readonly double[] _bandTotals = new double[7];
        private readonly int[] _bandCounts = new int[7];

        private byte[] ComputeBands(float[] samples, int sampleRate, double[] bandPeaks)
        {
            for (int i = 0; i < FftLength; i++)
            {
                float windowed = samples[i] * (float)FastFourierTransform.HammingWindow(i, FftLength);
                _fftBuffer[i].X = windowed;
                _fftBuffer[i].Y = 0;
            }

            FastFourierTransform.FFT(true, _fftPower, _fftBuffer);

            var bandTotals = _bandTotals;
            var bandCounts = _bandCounts;
            Array.Clear(bandTotals, 0, bandTotals.Length);
            Array.Clear(bandCounts, 0, bandCounts.Length);

            // Only the first half of the spectrum is meaningful (Nyquist).
            for (int i = 1; i < FftLength / 2; i++)
            {
                double freq = i * (double)sampleRate / FftLength;
                double magnitude = Math.Sqrt(_fftBuffer[i].X * _fftBuffer[i].X + _fftBuffer[i].Y * _fftBuffer[i].Y);

                for (int b = 0; b < BandRanges.Length; b++)
                {
                    if (freq >= BandRanges[b].Low && freq < BandRanges[b].High)
                    {
                        bandTotals[b] += magnitude;
                        bandCounts[b]++;
                        break;
                    }
                }
            }

            var result = new byte[7];
            for (int b = 0; b < 7; b++)
            {
                double avgMagnitude = bandCounts[b] > 0 ? bandTotals[b] / bandCounts[b] : 0;

                // Auto-gain per band: jump up instantly on a new peak,
                // decay slowly otherwise, then express the current value
                // as a fraction of that peak.
                if (avgMagnitude > bandPeaks[b])
                {
                    bandPeaks[b] = avgMagnitude;
                }
                else
                {
                    bandPeaks[b] = Math.Max(bandPeaks[b] * PeakDecay, PeakFloor);
                }

                double normalized = bandPeaks[b] > PeakFloor ? avgMagnitude / bandPeaks[b] : 0;
                normalized = Math.Clamp(normalized, 0, 1);
                result[b] = (byte)(normalized * 100);
            }
            return result;
        }

        private const double MinVolumeDb = -50;
        private const double MaxVolumeDb = 0;

        private static byte RmsToByte(float rms)
        {
            double db = 20 * Math.Log10(rms + 1e-6);
            double normalized = (db - MinVolumeDb) / (MaxVolumeDb - MinVolumeDb);
            normalized = Math.Clamp(normalized, 0, 1);
            return (byte)(normalized * 100);
        }

        private static float CalculateRms(float[] samples)
        {
            double sum = 0;
            for (int i = 0; i < samples.Length; i++)
                sum += samples[i] * samples[i];
            return (float)Math.Sqrt(sum / samples.Length);
        }

        public void Dispose()
        {
            try
            {
                if (_capture != null)
                {
                    _capture.DataAvailable -= OnDataAvailable;
                    _capture.RecordingStopped -= OnRecordingStopped;
                    _capture.StopRecording();
                    _capture.Dispose();
                    _capture = null;
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error disposing audio capture");
            }
        }
    }
}