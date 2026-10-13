# Metadata for mzML writers

Version 0.3.0 added bulk JSON metadata alongside the scalar convenience API.
Since 1.0.0, use the typed `ScanHeader` from `read_scan`/`read_scans` for
per-scan data (see the README); the JSON snapshots below are kept for full
dumps and debugging.
Use the bulk scan snapshot when constructing mzML: the old precursor getters
represent a single reaction and cannot describe an MSn/SPS hierarchy.

```cpp
openms::thermo_bridge::RawFile raw("input.raw");
auto file = raw.file_metadata_json(true); // with instrument methods
auto sha1 = raw.file_sha1();               // explicit, reads the whole file
auto scan = raw.scan_metadata_json(raw.first_scan_number());
auto detectors = raw.detector_chromatograms_json();
```

Each JSON document has `schema_version: 1`. Strings are UTF-8 JSON; missing or
nonfinite numbers are `null`, not zero. Numbers retain vendor units (retention
and chromatogram times are minutes, mass coordinates are m/z, wavelengths nm,
collision energy eV). Trailer values remain the original strings, including
empty values and vendor label punctuation. Array order and duplicate labels
are preserved. Callers must serialize access to the same `RawFile` handle,
including instrument selection; independent handles and `thread_accessor()`
handles can be used concurrently.

| Snapshot | Fields |
| --- | --- |
| File `file` | Path, RAW revision, full ISO creation timestamp, description; `sha1` is always `null` since 1.0 (use `file_sha1()`) |
| File `instrument` | Model, name, serial, acquisition software/hardware versions, units, selected controller |
| File `run` | First/last scan numbers, actual spectrum count, RT range, expected runtime, resolution, mass limits |
| File `sample` | Name, ID, type, comment, vial, volume, injection volume, row, dilution, method filename, internal standard, calibration level, processing method filename, weight |
| File | User sample label/value pairs; all embedded instrument method texts; RawFileReader assembly version; available MS/Analog/UV/PDA/MSAnalog controllers |
| Scan | Scan number, controller, complete Thermo native ID, RT, MS order and name, representation, polarity, filter, analyzer, ionization, acquisition mass/wavelength limits, TIC/base peak statistics |
| Scan `trailer` | All original `{label, value}` entries |
| Scan `reactions` | All ordered reactions: index, precursor target, isolation width/offset, activation type, collision energy and its validity flag |
| Detector `chromatograms` | Device/controller/channel, label, vendor units, native ID, time and signal arrays |

`file_metadata_json` describes the currently selected controller. Select any
controller listed in `controllers` with `select_instrument(type, number)`;
controller numbers start at one. A newly opened file selects the first MS
controller if present, otherwise the first supported detector. Detector trace
extraction restores the previously selected controller, including on failure.
PDA spectra use `read_scan` with `ReadOptions::centroid = false` (or `spectrum_data(scan, false)`) after selecting PDA;
its position array contains wavelength, not m/z.

`spectrum_auxiliary_array(scan, kind)` supports:

| Kind | Values | mzML CV |
| --- | --- | --- |
| 0 | Native centroid charges | MS:1000516 charge array |
| 1 | Independently sampled noise masses | MS:1002743 sampled noise m/z array |
| 2 | Noise amplitudes | MS:1002744 sampled noise intensity array |
| 3 | Baselines | MS:1002745 sampled noise baseline array |

Only attach charges when they match the native centroid peak stream. Noise
arrays have their own mass grid and length; they are not peak annotations.
Missing optional arrays are empty. With `ReadOptions::noise`, `read_scan` and
`read_scans` return the noise m/z, intensity and baseline arrays together (same
length, sharing `ScanBatch::noise_offsets`) or not at all; scans without a
centroid stream have no noise data. The legacy `spectrum_auxiliary_array(scan, 1)`
still returns the vendor's preferred masses for such scans. Invalid handles, scan numbers, or array
kinds throw `bridge_error`; string-query failures also throw instead of
appearing to be empty metadata. These additions are ABI-additive. The polarity
CV helper now follows the actual vendor enum: 0 negative, 1 positive, 2 unknown.

Use `thermo_host input.raw --metadata` for JSON Lines output: file metadata,
all scan snapshots of the default controller, then detector chromatograms.
The full sample and method snapshots can contain instrument paths and user
entered text; callers can omit methods with the API flag. The SHA-1 is only
computed by `file_sha1()`.

Build the managed and native components from the same revision: use the .NET 8
SDK source build or the release zip of the same version (linux-x64,
linux-arm64, osx-arm64, win-x64) as `OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR`;
managed DLLs of 0.x releases do not contain the 1.0 entry points.

## Low-resolution spectrum regression test

Centroid requests use the native centroid stream when available. Otherwise they
retain acquired segmented centroids or centroid profile data into the segmented
representation, matching TRFP's selection. This also handles empty scans.

Set `OPENMS_THERMO_BRIDGE_TEST_MSN_RAW` to an LTQ MSn fixture (for example
`ThermoRawFileParserTest/Data/small.RAW`) when running `thermo_bridge_tests` to
exercise the low-resolution fallback. This optional fixture is not downloaded
automatically.
