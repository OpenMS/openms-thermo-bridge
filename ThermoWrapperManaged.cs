using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using ThermoFisher.CommonCore.Data;
using ThermoFisher.CommonCore.Data.Business;
using ThermoFisher.CommonCore.Data.FilterEnums;
using ThermoFisher.CommonCore.Data.Interfaces;
using ThermoFisher.CommonCore.RawFileReader;

namespace ThermoWrapperManaged
{
    // ----------------------------------------------------------------
    // Shared error codes used by all bridge entry points.
    // ----------------------------------------------------------------
    internal static class Err
    {
        public const int FileNotFound = -1;
        public const int Exception    = -2;
        public const int NotSupported = -3;
        public const int BadHandle    = -4;
        public const int BadScan      = -5;
    }

    // ----------------------------------------------------------------
    // Handle-based file pool so a RAW file can be opened once and
    // queried many times without repeated open/close overhead.
    // ----------------------------------------------------------------
    internal static class FilePool
    {
        private static readonly ConcurrentDictionary<int, IRawDataPlus> _files = new();
        private static int _nextHandle = 0;

        public static int Open(string filePath)
        {
            var rawFile = RawFileReaderAdapter.FileFactory(filePath);
            if (rawFile == null || !rawFile.IsOpen || rawFile.IsError)
                return Err.FileNotFound;
            int handle = System.Threading.Interlocked.Increment(ref _nextHandle);
            foreach (var device in RawBridge.SupportedDevices)
            {
                if (rawFile.GetInstrumentCountOfType(device) == 0) continue;
                rawFile.SelectInstrument(device, 1);
                RawBridge.SelectedControllers[handle] = (device, 1);
                break;
            }
            _files[handle] = rawFile;
            return handle;
        }

        public static bool TryGet(int handle, out IRawDataPlus rawFile)
        {
            return _files.TryGetValue(handle, out rawFile!);
        }

        public static void Close(int handle)
        {
            if (_files.TryRemove(handle, out var rawFile))
            {
                rawFile.Dispose();
            }
        }
    }

    // ----------------------------------------------------------------
    // Public bridge entry points called from native C++ code.
    //
    // Convention:
    //   - Negative return = error (see Err constants).
    //   - For int-returning queries a non-negative value is success.
    //   - For double-returning queries NaN signals error; negative
    //     infinity signals bad-handle; positive infinity signals
    //     bad-scan.
    //   - String-returning helpers write UTF-8 into a caller-provided
    //     buffer.  They return the number of bytes (excluding the NUL
    //     terminator) needed.  If the buffer is too small the string
    //     is truncated but the full required size is still returned
    //     so the caller can retry with a larger buffer.
    // ----------------------------------------------------------------
    public static partial class RawBridge
    {
        // =============================================================
        //  Legacy free-function – kept for backward compatibility.
        // =============================================================
        [UnmanagedCallersOnly(EntryPoint = "GetScanCount")]
        public static int GetScanCount(IntPtr filePathPtr)
        {
            try
            {
                string? filePath = Marshal.PtrToStringUTF8(filePathPtr);
                if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath)) return Err.FileNotFound;
                return GetScanCountImpl(filePath);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int GetScanCountImpl(string filePath)
        {
            var rawFile = RawFileReaderAdapter.FileFactory(filePath);
            if (rawFile == null) return Err.FileNotFound;
            using (rawFile)
            {
                rawFile.SelectInstrument(Device.MS, 1);
                return rawFile.RunHeaderEx.SpectraCount;
            }
        }

        // =============================================================
        //  Handle management
        // =============================================================

        /// <summary>Open a RAW file and return a positive handle, or a negative error code.</summary>
        [UnmanagedCallersOnly(EntryPoint = "OpenRawFile")]
        public static int OpenRawFile(IntPtr filePathPtr)
        {
            try
            {
                string? filePath = Marshal.PtrToStringUTF8(filePathPtr);
                if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath)) return Err.FileNotFound;
                return OpenRawFileImpl(filePath);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int OpenRawFileImpl(string filePath)
        {
            return FilePool.Open(filePath);
        }

        /// <summary>Close a previously opened handle.  Returns 0 on success.</summary>
        [UnmanagedCallersOnly(EntryPoint = "CloseRawFile")]
        public static int CloseRawFile(int handle)
        {
            try { CloseRawFileImpl(handle); return 0; }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void CloseRawFileImpl(int handle) { MetadataCache.TryRemove(handle, out _); SelectedControllers.TryRemove(handle, out _); FilePool.Close(handle); }

        // =============================================================
        //  Run-header queries (int-returning)
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetFirstScanNumber")]
        public static int H_GetFirstScanNumber(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetFirstScanNumberImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetFirstScanNumberImpl(IRawDataPlus rf) => rf.RunHeaderEx.FirstSpectrum;

        [UnmanagedCallersOnly(EntryPoint = "H_GetLastScanNumber")]
        public static int H_GetLastScanNumber(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetLastScanNumberImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetLastScanNumberImpl(IRawDataPlus rf) => rf.RunHeaderEx.LastSpectrum;

        [UnmanagedCallersOnly(EntryPoint = "H_GetScanCount")]
        public static int H_GetScanCount(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetScanCountImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetScanCountImpl(IRawDataPlus rf) => rf.RunHeaderEx.SpectraCount;

        // =============================================================
        //  Run-header queries (double-returning)
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetStartTime")]
        public static double H_GetStartTime(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetStartTimeImpl(rf);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetStartTimeImpl(IRawDataPlus rf) => rf.RunHeaderEx.StartTime;

        [UnmanagedCallersOnly(EntryPoint = "H_GetEndTime")]
        public static double H_GetEndTime(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetEndTimeImpl(rf);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetEndTimeImpl(IRawDataPlus rf) => rf.RunHeaderEx.EndTime;

        // =============================================================
        //  Instrument information (string-returning)
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentModel")]
        public static int H_GetInstrumentModel(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetInstrumentModelImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetInstrumentModelImpl(IRawDataPlus rf) => rf.GetInstrumentData().Model ?? "";

        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentName")]
        public static int H_GetInstrumentName(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetInstrumentNameImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetInstrumentNameImpl(IRawDataPlus rf) => rf.GetInstrumentData().Name ?? "";

        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentSerialNumber")]
        public static int H_GetInstrumentSerialNumber(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetInstrumentSerialImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetInstrumentSerialImpl(IRawDataPlus rf) => rf.GetInstrumentData().SerialNumber ?? "";

        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentSoftwareVersion")]
        public static int H_GetInstrumentSoftwareVersion(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetInstrumentSoftwareVersionImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetInstrumentSoftwareVersionImpl(IRawDataPlus rf) => rf.GetInstrumentData().SoftwareVersion ?? "";

        [UnmanagedCallersOnly(EntryPoint = "H_GetCreationDate")]
        public static int H_GetCreationDate(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetCreationDateImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetCreationDateImpl(IRawDataPlus rf) => rf.FileHeader.CreationDate.ToString("o");

        [UnmanagedCallersOnly(EntryPoint = "H_GetSampleName")]
        public static int H_GetSampleName(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetSampleNameImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetSampleNameImpl(IRawDataPlus rf)
        {
            var info = rf.SampleInformation;
            return info?.SampleName ?? "";
        }

        // =============================================================
        //  Scan-level queries (scalar)
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetRetentionTime")]
        public static double H_GetRetentionTime(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetRetentionTimeImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetRetentionTimeImpl(IRawDataPlus rf, int scanNumber) =>
            rf.RetentionTimeFromScanNumber(scanNumber);

        [UnmanagedCallersOnly(EntryPoint = "H_GetMSLevel")]
        public static int H_GetMSLevel(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetMSLevelImpl(rf, scanNumber);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetMSLevelImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            if (filter == null) return Err.BadScan;
            return (int)filter.MSOrder;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_IsCentroidScan")]
        public static int H_IsCentroidScan(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_IsCentroidScanImpl(rf, scanNumber);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_IsCentroidScanImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            if (filter == null) return Err.BadScan;
            return filter.ScanData == ScanDataType.Centroid ? 1 : 0;
        }

        // =============================================================
        //  Scan-level queries (string)
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetScanFilter")]
        public static int H_GetScanFilter(int handle, int scanNumber, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetScanFilterImpl(rf, scanNumber), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetScanFilterImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            return filter?.ToString() ?? "";
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetPolarity")]
        public static int H_GetPolarity(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetPolarityImpl(rf, scanNumber);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetPolarityImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            if (filter == null) return Err.BadScan;
            // Raw vendor enum: Negative = 0, Positive = 1, Any = 2.
            return (int)filter.Polarity;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetMassAnalyzerType")]
        public static int H_GetMassAnalyzerType(int handle, int scanNumber, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetMassAnalyzerTypeImpl(rf, scanNumber), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetMassAnalyzerTypeImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            return filter?.MassAnalyzer.ToString() ?? "";
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetIonizationMode")]
        public static int H_GetIonizationMode(int handle, int scanNumber, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetIonizationModeImpl(rf, scanNumber), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetIonizationModeImpl(IRawDataPlus rf, int scanNumber)
        {
            var filter = rf.GetFilterForScanNumber(scanNumber);
            return filter?.IonizationMode.ToString() ?? "";
        }

        // =============================================================
        //  Precursor / reaction queries
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetPrecursorMass")]
        public static double H_GetPrecursorMass(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetPrecursorMassImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetPrecursorMassImpl(IRawDataPlus rf, int scanNumber)
        {
            var scanEvent = rf.GetScanEventForScanNumber(scanNumber);
            if (scanEvent == null || (int)scanEvent.MSOrder < 2) return 0.0;

            // First try monoisotopic m/z from trailer extra data
            var trailer = rf.GetTrailerExtraInformation(scanNumber);
            if (trailer?.Labels != null)
            {
                for (int i = 0; i < trailer.Labels.Length; i++)
                {
                    if (trailer.Labels[i].Contains("Monoisotopic M/Z", StringComparison.OrdinalIgnoreCase))
                    {
                        if (double.TryParse(trailer.Values[i], System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out double monoMz) && monoMz > 0)
                            return monoMz;
                    }
                }
            }

            // Fall back to reaction precursor mass
            var reaction = scanEvent.GetReaction(0);
            return reaction?.PrecursorMass ?? 0.0;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetPrecursorCharge")]
        public static int H_GetPrecursorCharge(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetPrecursorChargeImpl(rf, scanNumber);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetPrecursorChargeImpl(IRawDataPlus rf, int scanNumber)
        {
            var trailer = rf.GetTrailerExtraInformation(scanNumber);
            if (trailer?.Labels == null) return 0;

            for (int i = 0; i < trailer.Labels.Length; i++)
            {
                if (trailer.Labels[i].Contains("Charge State", StringComparison.OrdinalIgnoreCase))
                {
                    if (int.TryParse(trailer.Values[i]?.Trim(), out int charge))
                        return charge;
                }
            }
            return 0;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetCollisionEnergy")]
        public static double H_GetCollisionEnergy(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetCollisionEnergyImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetCollisionEnergyImpl(IRawDataPlus rf, int scanNumber)
        {
            var scanEvent = rf.GetScanEventForScanNumber(scanNumber);
            if (scanEvent == null || (int)scanEvent.MSOrder < 2) return 0.0;
            var reaction = scanEvent.GetReaction(0);
            return reaction?.CollisionEnergy ?? 0.0;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetActivationType")]
        public static int H_GetActivationType(int handle, int scanNumber, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetActivationTypeImpl(rf, scanNumber), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetActivationTypeImpl(IRawDataPlus rf, int scanNumber)
        {
            var scanEvent = rf.GetScanEventForScanNumber(scanNumber);
            if (scanEvent == null || (int)scanEvent.MSOrder < 2) return "";
            var reaction = scanEvent.GetReaction(0);
            return reaction?.ActivationType.ToString() ?? "";
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetIsolationWidth")]
        public static double H_GetIsolationWidth(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetIsolationWidthImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetIsolationWidthImpl(IRawDataPlus rf, int scanNumber)
        {
            var scanEvent = rf.GetScanEventForScanNumber(scanNumber);
            if (scanEvent == null || (int)scanEvent.MSOrder < 2) return 0.0;
            var reaction = scanEvent.GetReaction(0);
            return reaction?.IsolationWidth ?? 0.0;
        }

        // =============================================================
        //  Spectrum data (m/z + intensity arrays)
        // =============================================================

        /// <summary>
        /// Returns the peak count for the given scan.
        /// If centroid != 0, centroided data is returned; otherwise profile.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetSpectrumPeakCount")]
        public static int H_GetSpectrumPeakCount(int handle, int scanNumber, int centroid)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetSpectrumPeakCountImpl(rf, scanNumber, centroid != 0);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetSpectrumPeakCountImpl(IRawDataPlus rf, int scanNumber, bool centroid)
        {
            return SpectrumArrays(rf, scanNumber, centroid).Masses.Length;
        }

        // Low-resolution scans can contain centroided segmented data without a
        // centroid stream. Software centroiding also returns segmented data.
        // Use the same selection for the size query and the following copy.
        private static (double[] Masses, double[] Intensities) SpectrumArrays(IRawDataPlus rf, int scanNumber, bool centroid)
        {
            var scan = Scan.FromFile(rf, scanNumber);
            if (centroid && scan.HasCentroidStream)
                return (scan.CentroidScan.Masses, scan.CentroidScan.Intensities);

            var segmented = scan.SegmentedScan;
            if (centroid && segmented.PositionCount > 0 &&
                rf.GetScanEventForScanNumber(scanNumber).ScanData == ThermoFisher.CommonCore.Data.FilterEnums.ScanDataType.Profile)
                segmented = Scan.ToCentroid(scan).SegmentedScan;
            return (segmented.Positions ?? Array.Empty<double>(), segmented.Intensities ?? Array.Empty<double>());
        }

        /// <summary>
        /// Fills mzBuffer and intBuffer with the spectrum data for the given scan.
        /// Returns the number of peaks actually written (may be less than bufferSize).
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetSpectrumData")]
        public static int H_GetSpectrumData(int handle, int scanNumber, int centroid,
            IntPtr mzBuffer, IntPtr intBuffer, int bufferSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetSpectrumDataImpl(rf, scanNumber, centroid != 0, mzBuffer, intBuffer, bufferSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetSpectrumDataImpl(IRawDataPlus rf, int scanNumber, bool centroid,
            IntPtr mzBuffer, IntPtr intBuffer, int bufferSize)
        {
            var (masses, intensities) = SpectrumArrays(rf, scanNumber, centroid);
            if (masses.Length != intensities.Length) throw new InvalidOperationException("Inconsistent spectrum array lengths");
            int count = Math.Min(masses.Length, bufferSize);
            Marshal.Copy(masses, 0, mzBuffer, count);
            Marshal.Copy(intensities, 0, intBuffer, count);
            return count;
        }

        // =============================================================
        //  Chromatogram (TIC) data
        // =============================================================

        /// <summary>
        /// Returns the number of data points in the TIC chromatogram.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetChromatogramLength")]
        public static int H_GetChromatogramLength(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetChromatogramLengthImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetChromatogramLengthImpl(IRawDataPlus rf)
        {
            int first = rf.RunHeaderEx.FirstSpectrum;
            int last = rf.RunHeaderEx.LastSpectrum;
            return last - first + 1;
        }

        /// <summary>
        /// Fills rtBuffer and intBuffer with TIC chromatogram data.
        /// Returns the number of points written.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetChromatogramData")]
        public static int H_GetChromatogramData(int handle, IntPtr rtBuffer, IntPtr intBuffer, int bufferSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetChromatogramDataImpl(rf, rtBuffer, intBuffer, bufferSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetChromatogramDataImpl(IRawDataPlus rf, IntPtr rtBuffer, IntPtr intBuffer, int bufferSize)
        {
            int first = rf.RunHeaderEx.FirstSpectrum;
            int last = rf.RunHeaderEx.LastSpectrum;
            int count = Math.Min(last - first + 1, bufferSize);

            var rts = new double[count];
            var ints = new double[count];

            for (int i = 0; i < count; i++)
            {
                int scan = first + i;
                rts[i] = rf.RetentionTimeFromScanNumber(scan);
                var stats = rf.GetScanStatsForScanNumber(scan);
                ints[i] = stats.TIC;
            }

            Marshal.Copy(rts, 0, rtBuffer, count);
            Marshal.Copy(ints, 0, intBuffer, count);
            return count;
        }

        // =============================================================
        //  Extracted Ion Chromatogram (XIC)
        // =============================================================

        /// <summary>
        /// Returns the number of data points for an XIC in the given m/z range.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetXicLength")]
        public static int H_GetXicLength(int handle, double mzStart, double mzEnd,
            double rtStartMinutes, double rtEndMinutes)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetXicLengthImpl(rf, mzStart, mzEnd, rtStartMinutes, rtEndMinutes);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetXicLengthImpl(IRawDataPlus rf, double mzStart, double mzEnd,
            double rtStartMinutes, double rtEndMinutes)
        {
            var settings = new ChromatogramTraceSettings(TraceType.MassRange)
            {
                Filter = "ms",
                MassRanges = new[] { new ThermoFisher.CommonCore.Data.Business.Range(mzStart, mzEnd) }
            };
            int first = rf.RunHeaderEx.FirstSpectrum;
            int last = rf.RunHeaderEx.LastSpectrum;
            // Respect RT filter by adjusting scan range
            if (rtStartMinutes > 0 || rtEndMinutes < double.MaxValue)
            {
                first = Math.Max(first, rf.ScanNumberFromRetentionTime(rtStartMinutes));
                last = Math.Min(last, rf.ScanNumberFromRetentionTime(rtEndMinutes));
            }
            var data = rf.GetChromatogramData(new IChromatogramSettings[] { settings }, first, last);
            if (data?.PositionsArray == null || data.PositionsArray.Length == 0)
                return 0;
            return data.PositionsArray[0]?.Length ?? 0;
        }

        /// <summary>
        /// Fills rtBuffer and intBuffer with XIC data for the given m/z range.
        /// Returns the number of points written.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetXicData")]
        public static int H_GetXicData(int handle, double mzStart, double mzEnd,
            double rtStartMinutes, double rtEndMinutes,
            IntPtr rtBuffer, IntPtr intBuffer, int bufferSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetXicDataImpl(rf, mzStart, mzEnd, rtStartMinutes, rtEndMinutes,
                    rtBuffer, intBuffer, bufferSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetXicDataImpl(IRawDataPlus rf, double mzStart, double mzEnd,
            double rtStartMinutes, double rtEndMinutes,
            IntPtr rtBuffer, IntPtr intBuffer, int bufferSize)
        {
            var settings = new ChromatogramTraceSettings(TraceType.MassRange)
            {
                Filter = "ms",
                MassRanges = new[] { new ThermoFisher.CommonCore.Data.Business.Range(mzStart, mzEnd) }
            };
            int first = rf.RunHeaderEx.FirstSpectrum;
            int last = rf.RunHeaderEx.LastSpectrum;
            if (rtStartMinutes > 0 || rtEndMinutes < double.MaxValue)
            {
                first = Math.Max(first, rf.ScanNumberFromRetentionTime(rtStartMinutes));
                last = Math.Min(last, rf.ScanNumberFromRetentionTime(rtEndMinutes));
            }
            var data = rf.GetChromatogramData(new IChromatogramSettings[] { settings }, first, last);
            if (data?.PositionsArray == null || data.PositionsArray.Length == 0)
                return 0;

            var rts = data.PositionsArray[0];
            var ints = data.IntensitiesArray[0];
            if (rts == null || ints == null) return 0;

            int count = Math.Min(rts.Length, bufferSize);
            Marshal.Copy(rts, 0, rtBuffer, count);
            Marshal.Copy(ints, 0, intBuffer, count);
            return count;
        }

        // =============================================================
        //  Trailer extra data
        // =============================================================

        /// <summary>
        /// Returns the number of trailer extra labels for a scan.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetTrailerExtraCount")]
        public static int H_GetTrailerExtraCount(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetTrailerExtraCountImpl(rf, scanNumber);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetTrailerExtraCountImpl(IRawDataPlus rf, int scanNumber)
        {
            var trailer = rf.GetTrailerExtraInformation(scanNumber);
            return trailer?.Labels?.Length ?? 0;
        }

        /// <summary>
        /// Returns a NUL-delimited concatenation of all trailer extra labels.
        /// The return value is the total byte count needed (excluding final NUL).
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetTrailerExtraLabels")]
        public static int H_GetTrailerExtraLabels(int handle, int scanNumber, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetTrailerExtraLabelsImpl(rf, scanNumber, outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetTrailerExtraLabelsImpl(IRawDataPlus rf, int scanNumber, IntPtr outBuf, int bufSize)
        {
            var trailer = rf.GetTrailerExtraInformation(scanNumber);
            if (trailer?.Labels == null) return 0;
            string joined = string.Join("\0", trailer.Labels);
            return WriteString(joined, outBuf, bufSize);
        }

        /// <summary>
        /// Gets a single trailer extra value by label key.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetTrailerExtraValue")]
        public static int H_GetTrailerExtraValue(int handle, int scanNumber, IntPtr keyPtr,
            IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                string? key = Marshal.PtrToStringUTF8(keyPtr);
                if (key == null) return 0;
                return H_GetTrailerExtraValueImpl(rf, scanNumber, key, outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetTrailerExtraValueImpl(IRawDataPlus rf, int scanNumber, string key,
            IntPtr outBuf, int bufSize)
        {
            var trailer = rf.GetTrailerExtraInformation(scanNumber);
            if (trailer?.Labels == null) return 0;
            for (int i = 0; i < trailer.Labels.Length; i++)
            {
                if (string.Equals(trailer.Labels[i]?.Trim(), key.Trim(), StringComparison.OrdinalIgnoreCase))
                {
                    return WriteString(trailer.Values[i]?.Trim() ?? "", outBuf, bufSize);
                }
            }
            return 0; // key not found
        }

        // =============================================================
        //  Instrument method
        // =============================================================

        /// <summary>
        /// Returns the number of instrument methods available.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentMethodCount")]
        public static int H_GetInstrumentMethodCount(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetInstrumentMethodCountImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetInstrumentMethodCountImpl(IRawDataPlus rf) =>
            rf.InstrumentMethodsCount;

        /// <summary>
        /// Gets the text of an instrument method by index.
        /// </summary>
        [UnmanagedCallersOnly(EntryPoint = "H_GetInstrumentMethod")]
        public static int H_GetInstrumentMethod(int handle, int index, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetInstrumentMethodImpl(rf, index), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetInstrumentMethodImpl(IRawDataPlus rf, int index) =>
            rf.GetInstrumentMethod(index) ?? "";

        // =============================================================
        //  Scan statistics
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetBasePeakMass")]
        public static double H_GetBasePeakMass(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetBasePeakMassImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetBasePeakMassImpl(IRawDataPlus rf, int scanNumber)
        {
            var stats = rf.GetScanStatsForScanNumber(scanNumber);
            return stats.BasePeakMass;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetBasePeakIntensity")]
        public static double H_GetBasePeakIntensity(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetBasePeakIntensityImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetBasePeakIntensityImpl(IRawDataPlus rf, int scanNumber)
        {
            var stats = rf.GetScanStatsForScanNumber(scanNumber);
            return stats.BasePeakIntensity;
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetTIC")]
        public static double H_GetTIC(int handle, int scanNumber)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetTICImpl(rf, scanNumber);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetTICImpl(IRawDataPlus rf, int scanNumber)
        {
            var stats = rf.GetScanStatsForScanNumber(scanNumber);
            return stats.TIC;
        }

        // =============================================================
        //  File description / header
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetFileDescription")]
        public static int H_GetFileDescription(int handle, IntPtr outBuf, int bufSize)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return WriteString(H_GetFileDescriptionImpl(rf), outBuf, bufSize);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string H_GetFileDescriptionImpl(IRawDataPlus rf) =>
            rf.FileHeader.FileDescription ?? "";

        [UnmanagedCallersOnly(EntryPoint = "H_GetFileRevision")]
        public static int H_GetFileRevision(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetFileRevisionImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetFileRevisionImpl(IRawDataPlus rf) => rf.FileHeader.Revision;

        // =============================================================
        //  Low / High mass from run header
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetLowMass")]
        public static double H_GetLowMass(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetLowMassImpl(rf);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetLowMassImpl(IRawDataPlus rf) => rf.RunHeaderEx.LowMass;

        [UnmanagedCallersOnly(EntryPoint = "H_GetHighMass")]
        public static double H_GetHighMass(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return double.NegativeInfinity;
                return H_GetHighMassImpl(rf);
            }
            catch (Exception) { return double.NaN; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static double H_GetHighMassImpl(IRawDataPlus rf) => rf.RunHeaderEx.HighMass;

        // =============================================================
        //  Has MS data
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_HasMSData")]
        public static int H_HasMSData(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_HasMSDataImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_HasMSDataImpl(IRawDataPlus rf)
        {
            return rf.GetInstrumentCountOfType(Device.MS) > 0 ? 1 : 0;
        }

        // =============================================================
        //  Number of MS instruments
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_GetMSInstrumentCount")]
        public static int H_GetMSInstrumentCount(int handle)
        {
            try
            {
                if (!FilePool.TryGet(handle, out var rf)) return Err.BadHandle;
                return H_GetMSInstrumentCountImpl(rf);
            }
            catch (Exception) { return Err.Exception; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int H_GetMSInstrumentCountImpl(IRawDataPlus rf) =>
            rf.GetInstrumentCountOfType(Device.MS);

        // =============================================================
        //  Helpers
        // =============================================================

        /// <summary>
        /// Writes a .NET string as UTF-8 into an unmanaged buffer.
        /// Returns the number of UTF-8 bytes in the string (excl. NUL).
        /// When outBuf is zero or bufSize is zero the string is not
        /// copied – only the required size is returned.
        /// </summary>
        private static int WriteString(string value, IntPtr outBuf, int bufSize)
        {
            if (value == null) value = "";
            byte[] utf8 = Encoding.UTF8.GetBytes(value);
            if (outBuf != IntPtr.Zero && bufSize > 0)
            {
                int toCopy = Math.Min(utf8.Length, bufSize - 1);
                Marshal.Copy(utf8, 0, outBuf, toCopy);
                Marshal.WriteByte(outBuf + toCopy, 0); // NUL terminator
            }
            return utf8.Length;
        }
    }
}
