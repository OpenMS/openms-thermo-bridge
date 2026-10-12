using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using ThermoFisher.CommonCore.Data;
using ThermoFisher.CommonCore.Data.Business;
using ThermoFisher.CommonCore.Data.FilterEnums;
using ThermoFisher.CommonCore.Data.Interfaces;

namespace ThermoWrapperManaged
{
    // ----------------------------------------------------------------
    // Fixed-layout records shared with src/thermo_bridge.cpp. Keep the
    // field order and sizes identical to NativeScanHeader / NativeReaction /
    // NativeTrailerLabel there.
    // ----------------------------------------------------------------
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeScanHeader
    {
        public int ScanNumber;
        public int ControllerType;
        public int ControllerNumber;
        public int MsOrder;
        public int Polarity;
        public int Analyzer;
        public int Ionization;
        public int Centroid;
        public int PeakSource;
        public int Reserved;
        public double RetentionTime;
        public double Tic;
        public double BasePeakMass;
        public double BasePeakIntensity;
        public double LowMass;
        public double HighMass;
        public double LowWavelength;
        public double HighWavelength;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeReaction
    {
        public double PrecursorMass;
        public double IsolationWidth;
        public double IsolationOffset;
        public double CollisionEnergy;
        public double FirstPrecursorMass;
        public double LastPrecursorMass;
        public int Activation;
        public int Flags; // 1 = collision energy valid, 2 = precursor range valid, 4 = multiple activation
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeTrailerLabel
    {
        public int Kind;
        public int Slot;
        public int VendorType;
        public int Selected;
    }

    /// Buffer kinds requested through the native allocator callback.
    internal static class Sink
    {
        public const int Header = 0;          // 1 NativeScanHeader; starts a new scan
        public const int Filter = 1;          // UTF-8 bytes
        public const int Reactions = 2;       // NativeReaction[]
        public const int Mz = 3;              // double[]
        public const int Intensities = 4;     // double[]
        public const int Charges = 5;         // double[]
        public const int NoiseMz = 6;         // double[]
        public const int NoiseIntensities = 7;// double[]
        public const int NoiseBaselines = 8;  // double[]
        public const int TrailerDoubles = 9;  // double[]
        public const int TrailerInts = 10;    // long[]
        public const int TrailerBools = 11;   // byte[]
        public const int TrailerStrings = 12; // packed UTF-8, every value NUL-terminated
        public const int TrailerText = 13;    // packed UTF-8, every value NUL-terminated
        public const int LayoutLabels = 20;   // NativeTrailerLabel[]
        public const int LayoutNames = 21;    // packed UTF-8, every label NUL-terminated
    }

    internal static class ReadFlags
    {
        public const int Centroid = 1;
        public const int Peaks = 2;
        public const int Trailers = 4;
        public const int TrailerText = 8;
        public const int Charges = 16;
        public const int Noise = 32;
    }

    internal static class TrailerKinds
    {
        public const int Double = 0, Int = 1, Bool = 2, String = 3;

        public static int FromVendor(GenericDataTypes type) => type switch
        {
            GenericDataTypes.FLOAT or GenericDataTypes.DOUBLE => Double,
            GenericDataTypes.CHAR or GenericDataTypes.UCHAR or GenericDataTypes.SHORT or GenericDataTypes.USHORT
                or GenericDataTypes.LONG or GenericDataTypes.ULONG => Int,
            GenericDataTypes.TRUEFALSE or GenericDataTypes.YESNO or GenericDataTypes.ONOFF => Bool,
            _ => String
        };
    }

    /// Trailer layout of one controller, plus the label selection applied to it.
    internal sealed class TrailerLayoutCache
    {
        public Device Device;
        public int Number;
        public string[] Labels = Array.Empty<string>();
        public int[] Kinds = Array.Empty<int>();
        public int[] Slots = Array.Empty<int>();
        public int[] VendorTypes = Array.Empty<int>();
        public bool[] Selected = Array.Empty<bool>();
        public int[] SelectedIndices = Array.Empty<int>();
        public int DoubleCount, IntCount, BoolCount, StringCount;
        public string[]? Selection;
    }

    internal readonly unsafe struct Allocator
    {
        private readonly delegate* unmanaged<IntPtr, int, long, IntPtr> _fn;
        private readonly IntPtr _ctx;

        public Allocator(delegate* unmanaged<IntPtr, int, long, IntPtr> fn, IntPtr ctx) { _fn = fn; _ctx = ctx; }

        public IntPtr Raw(int kind, long count)
        {
            IntPtr p = _fn(_ctx, kind, count);
            if (p == IntPtr.Zero) throw new OutOfMemoryException("Native allocator failed");
            return p;
        }

        public Span<T> Get<T>(int kind, int count) where T : unmanaged =>
            new Span<T>((void*)Raw(kind, count), count);

        public void Doubles(int kind, ReadOnlySpan<double> values)
        {
            if (values.Length > 0) values.CopyTo(Get<double>(kind, values.Length));
        }

        public void Utf8(int kind, string? value)
        {
            if (string.IsNullOrEmpty(value)) return;
            int n = Encoding.UTF8.GetByteCount(value);
            Encoding.UTF8.GetBytes(value, Get<byte>(kind, n));
        }

        /// Writes every value followed by a NUL byte.
        public void Packed(int kind, string?[] values)
        {
            if (values.Length == 0) return;
            int total = 0;
            foreach (var v in values) total += (v == null ? 0 : Encoding.UTF8.GetByteCount(v)) + 1;
            var span = Get<byte>(kind, total);
            int pos = 0;
            foreach (var v in values)
            {
                if (!string.IsNullOrEmpty(v)) pos += Encoding.UTF8.GetBytes(v, span.Slice(pos));
                span[pos++] = 0;
            }
        }
    }

    public static partial class RawBridge
    {
        private const int ErrOutOfMemory = -6;

        // =============================================================
        //  Single-pass scan reading
        // =============================================================

        /// Reads scans first..last (inclusive). For every scan the native
        /// allocator is asked for a Header record first, then for the
        /// variable-length buffers. Each returned pointer is written
        /// completely before the next allocator call. Returns the number of
        /// scans read or a negative error code.
        [UnmanagedCallersOnly(EntryPoint = "H_ReadScans")]
        public static unsafe int H_ReadScans(int handle, int firstScan, int lastScan, int flags,
            delegate* unmanaged<IntPtr, int, long, IntPtr> alloc, IntPtr ctx)
        {
            try
            {
                if (!FilePool.TryGetEntry(handle, out var entry)) return Err.BadHandle;
                if (!SelectedControllers.TryGetValue(handle, out var selected)) return Err.BadHandle;
                var rf = entry.Raw;
                var header = rf.RunHeaderEx;
                if (firstScan > lastScan || firstScan < header.FirstSpectrum || lastScan > header.LastSpectrum)
                    return Err.BadScan;
                var sink = new Allocator(alloc, ctx);
                var layout = (flags & ReadFlags.Trailers) != 0 && selected.Type == Device.MS
                    ? Layout(entry, selected) : null;
                for (int scan = firstScan; scan <= lastScan; ++scan)
                    ReadScan(rf, selected, scan, flags, layout, sink);
                return lastScan - firstScan + 1;
            }
            catch (OutOfMemoryException) { return ErrOutOfMemory; }
            catch (Exception) { return Err.Exception; }
        }

        private static void ReadScan(IRawDataPlus rf, (Device Type, int Number) selected, int scanNumber, int flags,
            TrailerLayoutCache? layout, Allocator sink)
        {
            bool isMs = selected.Type == Device.MS;
            var stats = rf.GetScanStatsForScanNumber(scanNumber);
            IScanEvent? scanEvent = isMs ? rf.GetScanEventForScanNumber(scanNumber) : null;

            var h = new NativeScanHeader
            {
                ScanNumber = scanNumber,
                ControllerType = (int)selected.Type,
                ControllerNumber = selected.Number,
                MsOrder = 0,
                Polarity = 2,
                Analyzer = -1,
                Ionization = -1,
                RetentionTime = rf.RetentionTimeFromScanNumber(scanNumber),
                Tic = stats.TIC,
                BasePeakMass = stats.BasePeakMass,
                BasePeakIntensity = stats.BasePeakIntensity,
                LowMass = stats.LowMass,
                HighMass = stats.HighMass,
                LowWavelength = stats.ShortWavelength,
                HighWavelength = stats.LongWavelength
            };
            if (scanEvent != null)
            {
                h.MsOrder = (int)scanEvent.MSOrder;
                h.Polarity = (int)scanEvent.Polarity;
                h.Analyzer = (int)scanEvent.MassAnalyzer;
                h.Ionization = (int)scanEvent.IonizationMode;
                h.Centroid = scanEvent.ScanData == ScanDataType.Centroid ? 1 : 0;
            }

            // Decode peaks before emitting the header so PeakSource is known.
            double[]? mz = null, intensities = null, charges = null;
            double[]? noiseMz = null, noises = null, baselines = null;
            int source = 0;
            if ((flags & ReadFlags.Peaks) != 0)
                source = ReadPeaks(rf, scanNumber, stats, scanEvent, flags, out mz, out intensities, out charges,
                    out noiseMz, out noises, out baselines);
            else if ((flags & ReadFlags.Noise) != 0)
                ReadNoise(Scan.FromFile(rf, scanNumber), out noiseMz, out noises, out baselines);
            h.PeakSource = source;

            sink.Get<NativeScanHeader>(Sink.Header, 1)[0] = h;
            if (scanEvent != null)
            {
                sink.Utf8(Sink.Filter, scanEvent.ToString());
                if (scanEvent.MSOrder != MSOrderType.Ms) WriteReactions(scanEvent, sink);
            }

            if (mz != null && intensities != null)
            {
                if (mz.Length != intensities.Length) throw new InvalidOperationException("Inconsistent spectrum array lengths");
                if (charges != null && charges.Length != mz.Length) charges = null;
                SortByMz(ref mz, ref intensities, ref charges);
                sink.Doubles(Sink.Mz, mz);
                sink.Doubles(Sink.Intensities, intensities);
                if (charges != null) sink.Doubles(Sink.Charges, charges);
            }
            if (noiseMz != null) sink.Doubles(Sink.NoiseMz, noiseMz);
            if (noises != null) sink.Doubles(Sink.NoiseIntensities, noises);
            if (baselines != null) sink.Doubles(Sink.NoiseBaselines, baselines);

            if (layout != null) WriteTrailers(rf, scanNumber, layout, sink);
            if ((flags & ReadFlags.TrailerText) != 0 && isMs)
            {
                var extra = rf.GetTrailerExtraInformation(scanNumber);
                if (extra?.Values != null) sink.Packed(Sink.TrailerText, extra.Values);
            }
        }

        // Same representation choice as v0.3 SpectrumArrays: native centroid stream if present,
        // otherwise the segmented scan, centroided by RawFileReader if it was acquired as profile.
        private static int ReadPeaks(IRawDataPlus rf, int scanNumber, ScanStatistics stats, IScanEvent? scanEvent, int flags,
            out double[]? mz, out double[]? intensities, out double[]? charges,
            out double[]? noiseMz, out double[]? noises, out double[]? baselines)
        {
            bool centroid = (flags & ReadFlags.Centroid) != 0;
            bool wantCharges = (flags & ReadFlags.Charges) != 0;
            bool wantNoise = (flags & ReadFlags.Noise) != 0;
            charges = null; noiseMz = null; noises = null; baselines = null;

            if (scanEvent == null || wantNoise)
            {
                var scan = Scan.FromFile(rf, scanNumber);
                if (wantNoise) ReadNoise(scan, out noiseMz, out noises, out baselines);
                if (centroid && scan.HasCentroidStream)
                {
                    mz = scan.CentroidScan.Masses; intensities = scan.CentroidScan.Intensities;
                    if (wantCharges) charges = scan.CentroidScan.Charges;
                    return 1;
                }
                var seg = scan.SegmentedScan;
                if (centroid && seg.PositionCount > 0 && scanEvent?.ScanData == ScanDataType.Profile)
                {
                    seg = Scan.ToCentroid(scan).SegmentedScan;
                    mz = seg.Positions; intensities = seg.Intensities;
                    return 3;
                }
                mz = seg.Positions; intensities = seg.Intensities;
                return 2;
            }

            if (centroid)
            {
                var stream = rf.GetCentroidStream(scanNumber, false);
                if (stream != null && stream.Length > 0)
                {
                    mz = stream.Masses; intensities = stream.Intensities;
                    if (wantCharges) charges = stream.Charges;
                    return 1;
                }
                if (scanEvent.ScanData == ScanDataType.Profile)
                {
                    var scan = Scan.FromFile(rf, scanNumber);
                    var seg = scan.SegmentedScan;
                    if (seg.PositionCount > 0)
                    {
                        seg = Scan.ToCentroid(scan).SegmentedScan;
                        mz = seg.Positions; intensities = seg.Intensities;
                        return 3;
                    }
                    mz = seg.Positions; intensities = seg.Intensities;
                    return 2;
                }
            }
            var segmented = rf.GetSegmentedScanFromScanNumber(scanNumber, stats);
            mz = segmented.Positions; intensities = segmented.Intensities;
            return 2;
        }

        private static void ReadNoise(Scan scan, out double[]? noiseMz, out double[]? noises, out double[]? baselines)
        {
            noiseMz = scan.PreferredMasses;
            noises = scan.PreferredNoises;
            baselines = scan.PreferredBaselines;
        }

        /// Guarantees ascending m/z; vendor arrays are only copied when out of order.
        private static void SortByMz(ref double[] mz, ref double[] intensities, ref double[]? charges)
        {
            bool sorted = true;
            for (int i = 1; i < mz.Length; ++i)
                if (mz[i] < mz[i - 1]) { sorted = false; break; }
            if (sorted) return;
            var order = new int[mz.Length];
            for (int i = 0; i < order.Length; ++i) order[i] = i;
            var m = mz;
            // Stable: equal m/z keep vendor order.
            Array.Sort(order, (a, b) => { int c = m[a].CompareTo(m[b]); return c != 0 ? c : a.CompareTo(b); });
            var keys = new double[order.Length];
            for (int i = 0; i < order.Length; ++i) keys[i] = m[order[i]];
            var ints = new double[order.Length];
            for (int i = 0; i < order.Length; ++i) ints[i] = intensities[order[i]];
            if (charges != null)
            {
                var z = new double[order.Length];
                for (int i = 0; i < order.Length; ++i) z[i] = charges[order[i]];
                charges = z;
            }
            mz = keys;
            intensities = ints;
        }

        private static void WriteReactions(IScanEvent scanEvent, Allocator sink)
        {
            int count = scanEvent.MassCount;
            if (count <= 0) return;
            var reactions = sink.Get<NativeReaction>(Sink.Reactions, count);
            for (int i = 0; i < count; ++i)
            {
                var r = scanEvent.GetReaction(i);
                reactions[i] = new NativeReaction
                {
                    PrecursorMass = r.PrecursorMass,
                    IsolationWidth = r.IsolationWidth,
                    IsolationOffset = r.IsolationWidthOffset,
                    CollisionEnergy = r.CollisionEnergy,
                    FirstPrecursorMass = r.FirstPrecursorMass,
                    LastPrecursorMass = r.LastPrecursorMass,
                    Activation = (int)r.ActivationType,
                    Flags = (r.CollisionEnergyValid ? 1 : 0) | (r.PrecursorRangeIsValid ? 2 : 0) | (r.MultipleActivation ? 4 : 0)
                };
            }
        }

        // =============================================================
        //  Typed trailers
        // =============================================================

        private static TrailerLayoutCache Layout(OpenFile entry, (Device Type, int Number) selected)
        {
            var cached = entry.Layout;
            if (cached != null && cached.Device == selected.Type && cached.Number == selected.Number
                && ReferenceEquals(cached.Selection, entry.TrailerSelection))
                return cached;

            var layout = new TrailerLayoutCache { Device = selected.Type, Number = selected.Number, Selection = entry.TrailerSelection };
            if (selected.Type == Device.MS)
            {
                var items = entry.Raw.GetTrailerExtraHeaderInformation() ?? Array.Empty<HeaderItem>();
                int n = items.Length;
                layout.Labels = new string[n];
                layout.Kinds = new int[n];
                layout.Slots = new int[n];
                layout.VendorTypes = new int[n];
                layout.Selected = new bool[n];
                var selection = entry.TrailerSelection;
                var picked = new System.Collections.Generic.List<int>(n);
                for (int i = 0; i < n; ++i)
                {
                    layout.Labels[i] = items[i].Label ?? "";
                    layout.VendorTypes[i] = (int)items[i].DataType;
                    int kind = TrailerKinds.FromVendor(items[i].DataType);
                    layout.Kinds[i] = kind;
                    layout.Slots[i] = kind switch
                    {
                        TrailerKinds.Double => layout.DoubleCount++,
                        TrailerKinds.Int => layout.IntCount++,
                        TrailerKinds.Bool => layout.BoolCount++,
                        _ => layout.StringCount++
                    };
                    bool isSelected = selection == null || Array.IndexOf(selection, layout.Labels[i]) >= 0;
                    layout.Selected[i] = isSelected;
                    if (isSelected && items[i].DataType != GenericDataTypes.NULL) picked.Add(i);
                }
                layout.SelectedIndices = picked.ToArray();
            }
            entry.Layout = layout;
            return layout;
        }

        private static void WriteTrailers(IRawDataPlus rf, int scanNumber, TrailerLayoutCache layout, Allocator sink)
        {
            var doubles = layout.DoubleCount > 0 ? sink.Get<double>(Sink.TrailerDoubles, layout.DoubleCount) : default;
            if (doubles.Length > 0) doubles.Fill(double.NaN);
            foreach (int i in layout.SelectedIndices)
                if (layout.Kinds[i] == TrailerKinds.Double) doubles[layout.Slots[i]] = ToDouble(Value(rf, scanNumber, i));

            var ints = layout.IntCount > 0 ? sink.Get<long>(Sink.TrailerInts, layout.IntCount) : default;
            if (ints.Length > 0) ints.Clear();
            foreach (int i in layout.SelectedIndices)
                if (layout.Kinds[i] == TrailerKinds.Int) ints[layout.Slots[i]] = ToInt64(Value(rf, scanNumber, i));

            var bools = layout.BoolCount > 0 ? sink.Get<byte>(Sink.TrailerBools, layout.BoolCount) : default;
            if (bools.Length > 0) bools.Clear();
            foreach (int i in layout.SelectedIndices)
                if (layout.Kinds[i] == TrailerKinds.Bool) bools[layout.Slots[i]] = ToBool(Value(rf, scanNumber, i)) ? (byte)1 : (byte)0;

            if (layout.StringCount > 0)
            {
                var strings = new string?[layout.StringCount];
                foreach (int i in layout.SelectedIndices)
                    if (layout.Kinds[i] == TrailerKinds.String) strings[layout.Slots[i]] = Value(rf, scanNumber, i)?.ToString();
                sink.Packed(Sink.TrailerStrings, strings);
            }
        }

        private static object? Value(IRawDataPlus rf, int scanNumber, int index)
        {
            try { return rf.GetTrailerExtraValue(scanNumber, index); }
            catch (Exception) { return null; }
        }

        private static double ToDouble(object? value) => value switch
        {
            double d => d,
            float f => f,
            null => double.NaN,
            string s => double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : double.NaN,
            bool b => b ? 1.0 : 0.0,
            IConvertible c => SafeConvert(() => c.ToDouble(CultureInfo.InvariantCulture), double.NaN),
            _ => double.NaN
        };

        private static long ToInt64(object? value) => value switch
        {
            int i => i,
            uint u => u,
            short s => s,
            ushort us => us,
            sbyte sb => sb,
            byte b => b,
            long l => l,
            char c => c,
            null => 0,
            string s => long.TryParse(s.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var l) ? l : 0,
            IConvertible c => SafeConvert(() => c.ToInt64(CultureInfo.InvariantCulture), 0L),
            _ => 0
        };

        private static bool ToBool(object? value) => value switch
        {
            bool b => b,
            null => false,
            string s => s.Trim().ToLowerInvariant() is "true" or "yes" or "on" or "1",
            IConvertible c => SafeConvert(() => c.ToDouble(CultureInfo.InvariantCulture) != 0.0, false),
            _ => false
        };

        private static T SafeConvert<T>(Func<T> convert, T fallback)
        {
            try { return convert(); }
            catch (Exception) { return fallback; }
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetTrailerLayout")]
        public static unsafe int H_GetTrailerLayout(int handle, delegate* unmanaged<IntPtr, int, long, IntPtr> alloc, IntPtr ctx)
        {
            try
            {
                if (!FilePool.TryGetEntry(handle, out var entry)) return Err.BadHandle;
                if (!SelectedControllers.TryGetValue(handle, out var selected)) return Err.BadHandle;
                var layout = Layout(entry, selected);
                int n = layout.Labels.Length;
                if (n == 0) return 0;
                var sink = new Allocator(alloc, ctx);
                var labels = sink.Get<NativeTrailerLabel>(Sink.LayoutLabels, n);
                for (int i = 0; i < n; ++i)
                    labels[i] = new NativeTrailerLabel { Kind = layout.Kinds[i], Slot = layout.Slots[i],
                        VendorType = layout.VendorTypes[i], Selected = layout.Selected[i] ? 1 : 0 };
                sink.Packed(Sink.LayoutNames, layout.Labels);
                return n;
            }
            catch (OutOfMemoryException) { return ErrOutOfMemory; }
            catch (Exception) { return Err.Exception; }
        }

        /// Packed NUL-terminated labels; size 0 selects all labels.
        [UnmanagedCallersOnly(EntryPoint = "H_SetTrailerSelection")]
        public static int H_SetTrailerSelection(int handle, IntPtr packed, int size)
        {
            try
            {
                if (!FilePool.TryGetEntry(handle, out var entry)) return Err.BadHandle;
                string[]? selection = null;
                if (packed != IntPtr.Zero && size > 0)
                {
                    var text = Marshal.PtrToStringUTF8(packed, size);
                    selection = text.Split('\0', StringSplitOptions.RemoveEmptyEntries);
                }
                entry.TrailerSelection = selection;
                entry.Layout = null;
                return 0;
            }
            catch (Exception) { return Err.Exception; }
        }

        // =============================================================
        //  Thread accessors and checksum
        // =============================================================

        [UnmanagedCallersOnly(EntryPoint = "H_CreateThreadAccessor")]
        public static int H_CreateThreadAccessor(int handle)
        {
            try { return FilePool.CreateAccessor(handle); }
            catch (Exception) { return Err.Exception; }
        }

        [UnmanagedCallersOnly(EntryPoint = "H_GetFileSha1")]
        public static int H_GetFileSha1(int handle, IntPtr buffer, int size)
        {
            try
            {
                if (!FilePool.TryGetEntry(handle, out var entry)) return Err.BadHandle;
                using Stream stream = entry.Shared.Remote != null
                    ? new BufferedStream(entry.Shared.Remote.OpenSequentialStream(), 8 << 20)
                    : new FileStream(entry.Raw.FileName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite,
                        1 << 20, FileOptions.SequentialScan);
                var hex = Convert.ToHexString(SHA1.HashData(stream)).ToLowerInvariant();
                return WriteString(hex, buffer, size);
            }
            catch (Exception) { return Err.Exception; }
        }
    }
}
