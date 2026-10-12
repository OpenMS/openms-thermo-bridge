# OpenMS Thermo Raw File Bridge

A CMake-packaged native C++ bridge that embeds the .NET runtime, calls the official ThermoFisher RawFileReader libraries through a managed shim, and exposes Thermo RAW spectra, chromatograms and metadata to native consumers.

Version 1.0.0 (`OPENMS_THERMO_BRIDGE_API_VERSION 1`) is a performance redesign with
a typed, single-pass scan API. It contains breaking changes; see
[Migrating from 0.3.x](#migrating-from-03x).

## What This Does
- Builds a native bridge library and the `thermo_host` CLI with CMake
- Publishes a managed bridge with `UnmanagedCallersOnly` entry points, resolved once per process
- Packages exported CMake targets for `add_subdirectory`/FetchContent and `find_package(... CONFIG)` workflows
- Runs a small Catch2-based test suite through CTest
- Supports Linux, macOS, and Windows builds

## Build
The project does **not** download the Thermo vendor packages by default.

### Option 1: provide the vendor `.nupkg` files yourself
Place the following files in `vendor/thermo-feed` or point CMake at another directory with `-DOPENMS_THERMO_BRIDGE_VENDOR_DIR=/absolute/path`:
- `ThermoFisher.CommonCore.BackgroundSubtraction.8.0.6.nupkg`
- `ThermoFisher.CommonCore.Data.8.0.6.nupkg`
- `ThermoFisher.CommonCore.MassPrecisionEstimator.8.0.6.nupkg`
- `ThermoFisher.CommonCore.RandomAccessReaderPlugin.8.0.6.nupkg`
- `ThermoFisher.CommonCore.RawfileReader.8.0.6.nupkg`

Then configure and build:

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Option 2: explicitly allow CMake to fetch the vendor packages
```bash
cmake -S . -B build \
  -DOPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD=ON
cmake --build build --parallel
```

### Option 3: use pre-built managed DLLs

Pre-built managed artifacts for each platform are published as GitHub Release assets, so CMake does not need to run `dotnet publish` for `ThermoWrapperManaged.csproj`.
The final executable still needs the .NET 8 **runtime** at runtime, and the native bridge build still needs the platform-specific `nethost` headers and library. In practice those `nethost` development files usually come from the .NET SDK/host pack rather than a runtime-only install.

Download the zip for your platform from the [GitHub Releases page](https://github.com/jpfeuffer/openms-thermo-bridge/releases), unpack it, and point CMake at the extracted directory:

```bash
cmake -S . -B build \
  -DOPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR=/path/to/unpacked/zip
cmake --build build --parallel
```

#### Automatic download during CMake configure

CMake will fetch and extract the correct platform zip from GitHub Releases automatically:

```bash
cmake -S . -B build \
  -DOPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED=ON
cmake --build build --parallel
```

For Linux convenience, `build_linux.sh` configures, builds, and runs the tests with the vendor-download option enabled.

## Test
```bash
ctest --test-dir build --output-on-failure
```

## Run
```bash
./build/thermo_host /path/to/file.raw
```

Expected output:

```text
Scan count: <number>
```

The public `ginkgotoxin-ms-positive.raw` sample used in CI currently reports:

```text
Scan count: 90
```

## CMake Consumption
### FetchContent / add_subdirectory
```cmake
FetchContent_Declare(OpenMSThermoBridge
  GIT_REPOSITORY https://github.com/jpfeuffer/openms-thermo-bridge.git
  GIT_TAG main)
FetchContent_MakeAvailable(OpenMSThermoBridge)

add_executable(my_tool main.cpp)
target_link_libraries(my_tool PRIVATE OpenMSThermoBridge::openms_thermo_bridge)
openms_thermo_bridge_copy_runtime_files(TARGET my_tool)
```

`openms_thermo_bridge_copy_runtime_files()` stages everything needed beside the given target's output directory:
- `managed/ThermoWrapperManaged.dll`
- `managed/ThermoWrapperManaged.runtimeconfig.json`
- the dynamic `nethost` runtime library on platforms where it is needed

For executables, call it on the final executable target. If your code uses OpenMSThermoBridge from a shared library/plugin, call it on that shared library target instead so the runtime payload lives next to the library that actually ships to users.

### Installed package / `find_package(... CONFIG)`
```cmake
find_package(OpenMSThermoBridge CONFIG REQUIRED)

add_executable(my_tool main.cpp)
target_link_libraries(my_tool PRIVATE OpenMSThermoBridge::openms_thermo_bridge)
openms_thermo_bridge_copy_runtime_files(TARGET my_tool)
```

At runtime the bridge first looks for `managed/` next to the executable, then next to the loaded `openms_thermo_bridge` binary, and also supports the installed package layout under `openms_thermo_bridge/managed`. That means:
- build-tree consumers can stage runtime files next to their executable or shared library with `openms_thermo_bridge_copy_runtime_files()`
- installed-package consumers can either stage the runtime files the same way or rely on the package's installed `lib/openms_thermo_bridge/managed` layout

If you bypass the helper and pass an explicit managed directory to `openms::thermo_bridge::RawFile` or `get_scan_count(...)`, then you are responsible for shipping that directory yourself.

## Requirements
- CMake 3.21+
- .NET 8 SDK when CMake builds `ThermoWrapperManaged.csproj` locally with `dotnet publish`
- Platform-specific .NET `nethost` headers and library to build the native bridge; these usually come from the .NET SDK/host pack even when using pre-built managed artifacts
- .NET 8 runtime for the final executable at runtime
- A C++17 compiler
- Network access to `api.nuget.org` and, when the relevant options are enabled, `raw.githubusercontent.com`
- Pre-built artifact downloads (`OPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED=ON`) additionally require network access to `github.com`

## Licenses
The bridge is licensed under the BSD 3-Clause License (`LICENSE`). `THIRD-PARTY-NOTICES.txt` lists
the licenses of the open-source assemblies that are published with the managed bridge and of
`nethost`, which the native library links; `dotnet publish` puts it into the managed output folder,
and so into the release archives. The Thermo Fisher assemblies are licensed under the RawFileReader
license (`License.doc` in https://github.com/thermofisherlsms/RawFileReader).

## Status
- [x] Linux, macOS, and Windows CMake builds
- [x] Native C++ bridge library and CLI
- [x] Catch2/CTest integration tests
- [x] Exported CMake package config
- [x] Single-pass typed scan API, batch reads and thread accessors (1.0.0)

## mzML metadata API

The bulk metadata API preserves full sample/run metadata, scan trailers, ordered
MSn reactions, detector channels and independent noise arrays. See
[the schema and usage notes](docs/metadata.md). For an inspectable JSON Lines
snapshot, run `thermo_host input.raw --metadata`.

## Scan reading API (1.0)

```cpp
using namespace openms::thermo_bridge;
RawFile raw("input.raw");
const TrailerLayout& layout = raw.trailer_layout();          // once per file/controller
const int inj = layout.find("Ion Injection Time (ms):");      // -1 if absent
raw.set_trailer_selection({"Ion Injection Time (ms):", "Charge State:",
                           "Monoisotopic M/Z:", "Master Scan Number:"}); // optional

Spectrum s;                                                    // buffers are reused
for (int n = raw.first_scan_number(); n <= raw.last_scan_number(); ++n)
{
  raw.read_scan(n, s);                // one managed call, scan decoded/centroided once
  const ScanHeader& h = s.header;     // ms_order, polarity, RT, TIC, base peak, filter, reactions...
  double it = inj >= 0 ? h.trailer_as_double(layout, inj) : 0.0;
  // s.mz is sorted ascending; s.intensities has the same length
}
```

- **Single pass.** `read_scan` returns the `ScanHeader`, typed trailer values and
  the peaks from one managed call. The native side passes an allocator callback,
  so the managed code writes straight into the `std::vector`s. `ReadOptions`
  selects centroid/profile, header-only reads (`peaks = false`), trailers,
  vendor-formatted trailer text, centroid charges and noise arrays.
- **Sorted output.** Peak arrays are always sorted by ascending m/z (a stable
  sort is applied only when the vendor data is not already sorted), so callers
  can drop their sort checks. `header.peak_source` reports whether peaks came
  from the native centroid stream, the segmented scan or the RawFileReader
  centroider.
- **Typed trailers.** `trailer_layout()` lists every trailer label of the
  selected controller once, with its value kind and slot. Each header carries
  only the value arrays (`trailer_doubles/ints/bools/strings`). Look up a label
  index once with `layout.find(label)`, then use `trailer_double/int/bool/string`
  or `trailer_as_double`. `set_trailer_selection` skips parsing labels you do
  not need; slots stay stable. Labels and the vendor-formatted strings are
  returned only if `ReadOptions::trailer_text` is set.
- **Batches.** `read_scans(first, last)` reads an inclusive scan range in one
  call into a `ScanBatch`, which packs the peaks into flat arrays with
  `peak_offsets` (size n+1) and also has offsets for charges and noise.
- **Enums.** `MsOrder` (`Par`/`Nl`/`Ng`/`Ms..Ms10`), `Polarity`,
  `analyzer`/`ionization` and `Reaction::activation` use the RawFileReader enum
  values. `ms_order_name`, `mass_analyzer_name`, `ionization_mode_name` and
  `activation_type_name` give the vendor names used in the JSON/string APIs.
- **No hidden full-file passes.** `file_metadata_json()` no longer hashes the
  file. Call `file_sha1()` explicitly when a checksum is needed.
  `chromatogram_data()` (TIC) is a cheap vendor index read; it can also be built
  from `ScanHeader::tic`.
- **Call overhead.** All managed entry points are resolved once when the
  runtime starts and kept in a function-pointer table. The hot path takes no
  lock and does no string lookups.

### Threads

`RawFile::thread_accessor()` returns a new handle on the same open file. It is
backed by the RawFileReader `IRawFileThreadManager`, so the file index is
shared. Each accessor owns its vendor reader, controller selection and trailer
selection, and stays valid after the source `RawFile` is closed. Use one
`RawFile` per thread: one handle must not be used by two threads at once, but
distinct handles and accessors may run concurrently. The test suite checks
that concurrent reads through accessors give results identical to sequential
reads. Scaling is limited, with about 1.5-1.8x at 2-4 threads on the DIA
fixture below, because of vendor-internal work and .NET GC allocation.
`DOTNET_gcServer=1` helps a bit. Beyond 4 threads, throughput drops.

### Performance

These numbers are from macOS arm64 running x64 .NET under Rosetta, on a
92,778-scan FAIMS-DIA Orbitrap file (PXD029902, `20210213_WTC11_DIA_5CV.raw`).
They are for comparison only.

| Path | Wall time | per scan |
| --- | --- | --- |
| 0.3.1: `scan_metadata_json` + `spectrum_data` (centroid) | 6.08 s | 65.5 µs |
| 1.0: `read_scan` (header + all typed trailers + centroid peaks) | 0.78 s | 8.5 µs |
| 1.0: `read_scan` (header + centroid peaks) | 0.29 s | 3.1 µs |
| 1.0: `read_scans`, batch 256 | 0.80 s | 8.6 µs |
| 1.0: 4 thread accessors, `read_scans` | 0.48 s | 5.1 µs |
| 0.3.1: `spectrum_data` (profile) | 0.76 s | 8.2 µs |
| 1.0: `read_scan` (profile) | 0.45 s | 4.8 µs |
| 0.3.1: `file_metadata_json(…, checksum=true)` | +0.6 s before the first scan | |
| 1.0: `file_sha1()` (explicit) | 0.63 s | |

`tools/thermo_bridge_bench.cpp` measures these paths. Build it with
`-DOPENMS_THERMO_BRIDGE_BUILD_BENCH=ON`, then run
`thermo_bridge_bench file.raw [--threads N] [--batch B] [--dump out.jsonl]`.
`tools/compare_parity.py baseline.jsonl new.jsonl` compares a 1.0 dump with a
0.3.1 dump: peaks bit for bit, auxiliary arrays, headers, reactions, trailers,
legacy JSON, SHA-1 and TIC. Both tools are for local use only and need RAW
files that are not part of the repository. They are not run in CI.

### Remote files (experimental)

`RawFile` also accepts `http://`, `https://` and `s3://bucket/key` locations.
These are read with HTTP `Range` requests through a block cache plugged into
RawFileReader's delegated random-access interface. That interface is not
documented by Thermo, so this feature may break with a vendor package update.

- `http(s)://` works with any server that supports `Range`, including public
  S3 objects and presigned URLs. It needs no extra dependency.
- `s3://` works for public objects in the default build: requests are not signed
  and go to `https://bucket.s3[.region].amazonaws.com/key`, or to
  `AWS_ENDPOINT_URL` if it is set. Configure with
  `-DOPENMS_THERMO_BRIDGE_ENABLE_S3=ON` to use AWSSDK.S3 instead. You then get
  the standard credential chain, plus `AWS_REGION` and `AWS_ENDPOINT_URL` for
  MinIO and similar services. This adds about 3.5 MB of managed DLLs.
- Tuning and diagnostic environment variables (`THERMO_BRIDGE_REMOTE_BLOCK_KB`,
  `_CACHE_MB`, `_READAHEAD_KB`, `_PARALLEL`, `_STATS`, ...) are documented at the top
  of `RemoteRawAccess.cs`. `THERMO_BRIDGE_FORCE_RANGE_READER=1` routes local
  files through the same code path for testing.
- `tools/range_http_server.py` is a small Range-capable test server with optional
  added latency and request counters.

## Migrating from 0.3.x

| 0.3.x | 1.0 |
| --- | --- |
| `spectrum_peak_count(scan, centroid)` + `spectrum_data(scan, centroid)` | `read_scan(scan, spectrum, opts)`; `spectrum_peak_count` is removed |
| `scan_metadata_json(scan)` per scan | `read_scan` / `scan_header` → `ScanHeader` (JSON kept for debugging) |
| `file_metadata_json(methods, checksum)` | `file_metadata_json(methods)` + `file_sha1()`; `file.sha1` is now always `null` |
| `trailer_extra_value(scan, "Charge State:")` | `layout.find("Charge State:")` once, then `header.trailer_int(layout, idx)` |
| per-field getters (`retention_time`, `ms_level`, `scan_filter`, …) | `ScanHeader` fields (getters are kept but each one is a separate managed call) |
| loop over scans | `read_scans(first, last)` for a range |
| one `RawFile` per thread (reopens the file) | `raw.thread_accessor()` |
| check/sort m/z order | peaks are guaranteed sorted ascending |

Other changes:

- The managed `H_GetSpectrumPeakCount` / `H_GetSpectrumData` entry points are
  removed.
- Native and managed builds must come from the same revision. A 0.3.x managed
  DLL is rejected at runtime start because it lacks the 1.0 entry points.
- `spectrum_data(scan, centroid)` remains as a thin wrapper around `read_scan`.
- `chromatogram_data`, XIC, `detector_chromatograms_json` and
  `spectrum_auxiliary_array` are unchanged.
