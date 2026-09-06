# OpenMS Thermo Raw File Bridge

A CMake-packaged native C++ bridge that embeds the .NET runtime, calls the official ThermoFisher RawFileReader libraries through a managed shim, and exposes Thermo RAW scan counting to native consumers.

## What This Does
- Builds a native bridge library and the `thermo_host` CLI with CMake
- Publishes a managed bridge with an `UnmanagedCallersOnly` `GetScanCount` entry point
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

## Status
- [x] Linux, macOS, and Windows CMake builds
- [x] Native C++ bridge library and CLI
- [x] Catch2/CTest integration tests
- [x] Exported CMake package config

## mzML metadata API

The bulk metadata API preserves full sample/run metadata, scan trailers, ordered
MSn reactions, detector channels and independent noise arrays. See
[the schema and usage notes](docs/metadata.md). For an inspectable JSON Lines
snapshot, run `thermo_host input.raw --metadata`.
