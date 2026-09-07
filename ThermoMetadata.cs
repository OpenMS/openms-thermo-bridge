using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using ThermoFisher.CommonCore.Data;
using ThermoFisher.CommonCore.Data.Business;
using ThermoFisher.CommonCore.Data.Interfaces;
using ThermoFisher.CommonCore.RawFileReader;

namespace ThermoWrapperManaged
{
    public static partial class RawBridge
    {
        // Keep at most one size-query snapshot per open file. The following copy returns
        // exactly those bytes, even for expensive metadata (methods/checksum/channels).
        internal static readonly ConcurrentDictionary<int, (string Key, string Json)> MetadataCache = new();
        internal static readonly ConcurrentDictionary<int, (Device Type, int Number)> SelectedControllers = new();
        internal static readonly Device[] SupportedDevices = { Device.MS, Device.Analog, Device.UV, Device.Pda, Device.MSAnalog };

        private static int MetadataString(int handle, string key, IntPtr buffer, int size, Func<object> read)
        {
            string json;
            if (buffer != IntPtr.Zero && MetadataCache.TryGetValue(handle, out var cached) && cached.Key == key)
            {
                json = cached.Json;
                MetadataCache.TryRemove(handle, out _);
            }
            else
            {
                json = JsonSerializer.Serialize(read());
                if (buffer == IntPtr.Zero) MetadataCache[handle] = (key, json);
            }
            return WriteString(json, buffer, size);
        }

        private static double? Finite(double value) => double.IsFinite(value) ? value : null;

        [UnmanagedCallersOnly(EntryPoint = "H_GetFileMetadata")]
        public static int H_GetFileMetadata(int handle, int flags, IntPtr buffer, int size)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return MetadataString(handle, "file:" + flags, buffer, size, () => FileMetadata(handle, rf, flags));
            }
            catch (Exception) { return Err.Exception; }
        }

        private static object FileMetadata(int handle, IRawDataPlus rf, int flags)
        {
            var controllers = new List<object>();
            foreach (var device in SupportedDevices)
                for (int number = 1; number <= rf.GetInstrumentCountOfType(device); ++number)
                    controllers.Add(new { type = (int)device, name = device.ToString(), number });

            object? instrument = null;
            object? run = null;
            var methods = new List<string>();
            if (SelectedControllers.TryGetValue(handle, out var selected))
            {
                var data = rf.GetInstrumentData();
                instrument = new
                {
                    model = data.Model, name = data.Name, serial_number = data.SerialNumber,
                    software_version = data.SoftwareVersion, hardware_version = data.HardwareVersion,
                    units = data.Units.ToString(), controller_type = (int)selected.Type,
                    controller_number = selected.Number
                };
                var header = rf.RunHeaderEx;
                run = new
                {
                    first_scan = header.FirstSpectrum, last_scan = header.LastSpectrum,
                    scan_count = header.SpectraCount, start_time = Finite(header.StartTime), end_time = Finite(header.EndTime),
                    expected_runtime = Finite(header.ExpectedRunTime), mass_resolution = Finite(header.MassResolution),
                    low_mass = Finite(header.LowMass), high_mass = Finite(header.HighMass)
                };
                if ((flags & 1) != 0)
                    for (int i = 0; i < rf.InstrumentMethodsCount; ++i) methods.Add(rf.GetInstrumentMethod(i) ?? "");
            }

            var si = rf.SampleInformation;
            var sample = new Dictionary<string, object?>();
            if (si != null)
            {
                sample["sample name"] = si.SampleName;
                sample["sample number"] = si.SampleId;
                sample["sample type"] = si.SampleType.ToString();
                sample["sample comment"] = si.Comment;
                sample["sample vial"] = si.Vial;
                sample["sample volume"] = Finite(si.SampleVolume);
                sample["injection volume"] = Finite(si.InjectionVolume);
                sample["sample row number"] = si.RowNumber;
                sample["dilution factor"] = Finite(si.DilutionFactor);
                sample["instrument method file"] = si.InstrumentMethodFile;
                sample["internal standard amount"] = Finite(si.IstdAmount);
                sample["calibration level"] = si.CalibrationLevel;
                sample["processing method file"] = si.ProcessingMethodFile;
                sample["sample weight"] = Finite(si.SampleWeight);
            }
            var user = new List<object>();
            var labels = rf.UserLabel;
            var values = si?.UserText;
            if (labels != null && values != null)
                for (int i = 0; i < Math.Min(labels.Length, values.Length); ++i)
                    user.Add(new { label = labels[i], value = values[i] });

            string? checksum = null;
            if ((flags & 2) != 0)
            {
                using var stream = File.OpenRead(rf.FileName);
                checksum = Convert.ToHexString(SHA1.HashData(stream)).ToLowerInvariant();
            }
            return new
            {
                schema_version = 1,
                file = new { path = rf.FileName, revision = rf.FileHeader.Revision,
                    creation_date = rf.FileHeader.CreationDate.ToString("o", CultureInfo.InvariantCulture),
                    description = rf.FileHeader.FileDescription, sha1 = checksum },
                reader_version = typeof(RawFileReaderAdapter).Assembly.GetName().Version?.ToString(),
                instrument, run, sample, user_sample_fields = user, instrument_methods = methods, controllers
            };
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetScanMetadata")]
        public static int H_GetScanMetadata(int handle, int scanNumber, IntPtr buffer, int size)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return MetadataString(handle, "scan:" + scanNumber, buffer, size, () => ScanMetadata(handle, rf, scanNumber));
            }
            catch (Exception) { return Err.Exception; }
        }

        private static object ScanMetadata(int handle, IRawDataPlus rf, int scanNumber)
        {
            var selected = SelectedControllers[handle];
            var stats = rf.GetScanStatsForScanNumber(scanNumber);
            var trailer = new List<object>();
            var reactions = new List<object>();
            string filterText = "", analyzer = "", ionization = "", msOrderName = "";
            int msLevel = 0, polarity = 2;
            bool centroid = false;
            if (selected.Type == Device.MS)
            {
                var filter = rf.GetFilterForScanNumber(scanNumber);
                var scanEvent = rf.GetScanEventForScanNumber(scanNumber);
                filterText = scanEvent.ToString();
                analyzer = filter.MassAnalyzer.ToString();
                ionization = filter.IonizationMode.ToString();
                msLevel = (int)filter.MSOrder;
                msOrderName = filter.MSOrder.ToString();
                polarity = (int)filter.Polarity;
                centroid = scanEvent.ScanData == ThermoFisher.CommonCore.Data.FilterEnums.ScanDataType.Centroid;
                var extra = rf.GetTrailerExtraInformation(scanNumber);
                if (extra?.Labels != null && extra.Values != null)
                    for (int i = 0; i < Math.Min(extra.Labels.Length, extra.Values.Length); ++i)
                        trailer.Add(new { label = extra.Labels[i], value = extra.Values[i] });
                if (msLevel != 1)
                {
                    for (int index = 0; ; ++index)
                    {
                        IReaction reaction;
                        try { reaction = scanEvent.GetReaction(index); }
                        catch (ArgumentOutOfRangeException) { break; }
                        catch (IndexOutOfRangeException) { break; }
                        if (reaction == null) break;
                        reactions.Add(new { index, precursor_mass = Finite(reaction.PrecursorMass),
                            isolation_width = Finite(reaction.IsolationWidth), isolation_offset = Finite(reaction.IsolationWidthOffset),
                            activation = reaction.ActivationType.ToString(), collision_energy = Finite(reaction.CollisionEnergy),
                            collision_energy_valid = reaction.CollisionEnergyValid });
                    }
                }
            }
            return new
            {
                schema_version = 1, scan_number = scanNumber,
                controller_type = (int)selected.Type, controller_number = selected.Number,
                native_id = $"controllerType={(int)selected.Type} controllerNumber={selected.Number} scan={scanNumber}",
                retention_time = Finite(rf.RetentionTimeFromScanNumber(scanNumber)), ms_level = msLevel, ms_order_name = msOrderName, centroid, polarity,
                filter = filterText, analyzer, ionization,
                low_mass = Finite(stats.LowMass), high_mass = Finite(stats.HighMass),
                low_wavelength = Finite(stats.ShortWavelength), high_wavelength = Finite(stats.LongWavelength),
                tic = Finite(stats.TIC), base_peak_mass = Finite(stats.BasePeakMass), base_peak_intensity = Finite(stats.BasePeakIntensity),
                trailer, reactions
            };
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetSpectrumAuxiliaryArray")]
        public static int H_GetSpectrumAuxiliaryArray(int handle, int scanNumber, int kind, IntPtr buffer, int size)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                var scan = Scan.FromFile(rf, scanNumber);
                double[]? values = kind switch
                {
                    0 => scan.HasCentroidStream ? scan.CentroidScan.Charges : null,
                    1 => scan.PreferredMasses,
                    2 => scan.PreferredNoises,
                    3 => scan.PreferredBaselines,
                    _ => throw new ArgumentOutOfRangeException(nameof(kind))
                };
                if (values == null) return 0;
                if (buffer != IntPtr.Zero)
                {
                    if (size < values.Length) return Err.Exception;
                    Marshal.Copy(values, 0, buffer, values.Length);
                }
                return values.Length;
            }
            catch (Exception) { return Err.Exception; }
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentCount")]
        public static int H_GetInstrumentCount(int handle, int type)
        {
            try { return FilePool.TryGet(handle, out var rf) ? rf.GetInstrumentCountOfType((Device)type) : Err.BadHandle; }
            catch (Exception) { return Err.Exception; }
        }

        [UnmanagedCallersOnly(EntryPoint = "H_SelectInstrument")]
        public static int H_SelectInstrument(int handle, int type, int number)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                rf.SelectInstrument((Device)type, number);
                SelectedControllers[handle] = ((Device)type, number);
                MetadataCache.TryRemove(handle, out _);
                return 0;
            }
            catch (Exception) { return Err.Exception; }
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetDetectorChromatograms")]
        public static int H_GetDetectorChromatograms(int handle, IntPtr buffer, int size)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return MetadataString(handle, "detectors", buffer, size, () => DetectorChromatograms(handle, rf));
            }
            catch (Exception) { return Err.Exception; }
        }

        private static object DetectorChromatograms(int handle, IRawDataPlus rf)
        {
            var result = new List<object>();
            bool restore = SelectedControllers.TryGetValue(handle, out var previous);
            try
            {
                foreach (var device in SupportedDevices.Where(d => d != Device.MS))
                {
                    for (int number = 1; number <= rf.GetInstrumentCountOfType(device); ++number)
                    {
                        rf.SelectInstrument(device, number);
                        var instrument = rf.GetInstrumentData();
                        int channelCount = device == Device.Pda ? 1 : instrument.ChannelLabels.Length;
                        for (int channel = 0; channel < channelCount; ++channel)
                        {
                            var traceType = device == Device.Pda ? TraceType.TotalAbsorbance :
                                device == Device.UV ? TraceType.StartUVChromatogramTraces + channel + 1 :
                                TraceType.StartAnalogChromatogramTraces + channel + 1;
                            var settings = new ChromatogramTraceSettings(traceType);
                            var data = rf.GetChromatogramData(new IChromatogramSettings[] { settings }, -1, -1);
                            var traces = ChromatogramSignal.FromChromatogramData(data);
                            string label = device == Device.Pda ? "TotalAbsorbance" : instrument.ChannelLabels[channel];
                            for (int i = 0; i < traces.Length; ++i)
                                result.Add(new { device = device.ToString(), controller_type = (int)device, controller_number = number,
                                    channel, label, units = instrument.Units.ToString(),
                                    native_id = $"{device}#{number}_{label}_{i}",
                                    times = traces[i].Times, intensities = traces[i].Intensities });
                        }
                    }
                }
            }
            finally { if (restore) rf.SelectInstrument(previous.Type, previous.Number); }
            return new { schema_version = 1, chromatograms = result };
        }
    }
}
