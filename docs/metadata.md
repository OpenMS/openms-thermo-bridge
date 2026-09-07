# Metadata for mzML writers

Version 0.3.0 adds bulk metadata alongside the existing scalar convenience API.
Use the bulk scan snapshot when constructing mzML: the old precursor getters
represent a single reaction and cannot describe an MSn/SPS hierarchy.

```cpp
openms::thermo_bridge::RawFile raw("input.raw");
auto file = raw.file_metadata_json(true, true); // methods, SHA-1
auto scan = raw.scan_metadata_json(raw.first_scan_number());
auto detectors = raw.detector_chromatograms_json();
```

Each JSON document has `schema_version: 1`. Strings are UTF-8 JSON; missing or
nonfinite numbers are `null`, not zero. Numbers retain vendor units (retention
and chromatogram times are minutes, mass coordinates are m/z, wavelengths nm,
collision energy eV). Trailer values remain the original strings, including
empty values and vendor label punctuation. Array order and duplicate labels
are preserved. Callers must serialize access to the same `RawFile` handle,
including instrument selection; independent handles can be used independently.

| Snapshot | Fields |
| --- | --- |
| File `file` | Path, RAW revision, full ISO creation timestamp, description, optional SHA-1 |
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
PDA spectra use the normal `spectrum_data(scan, false)` API after selecting PDA;
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
Missing optional arrays are empty. Invalid handles, scan numbers, or array
kinds throw `bridge_error`; string-query failures also throw instead of
appearing to be empty metadata. These additions are ABI-additive. The polarity
CV helper now follows the actual vendor enum: 0 negative, 1 positive, 2 unknown.

Use `thermo_host input.raw --metadata` for JSON Lines output: file metadata,
all scan snapshots of the default controller, then detector chromatograms.
The full sample and method snapshots can contain instrument paths and user
entered text; callers can omit methods and SHA-1 using the API flags.

Build the managed and native components from the same revision, or use the
matching v0.3.0 pre-built managed artifact via
`OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR`; older managed DLLs do not contain
the new entry points.

## Low-resolution spectrum regression test

Centroid requests use the native centroid stream when available. Otherwise they
retain acquired segmented centroids or centroid profile data into the segmented
representation, matching TRFP's selection. This also handles empty scans.

Set `OPENMS_THERMO_BRIDGE_TEST_MSN_RAW` to an LTQ MSn fixture (for example
`ThermoRawFileParserTest/Data/small.RAW`) when running `thermo_bridge_tests` to
exercise the low-resolution fallback. This optional fixture is not downloaded
automatically.
