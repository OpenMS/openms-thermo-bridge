#pragma once

#include "openms_thermo_bridge/openms_thermo_bridge_export.h"

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace openms::thermo_bridge
{

// ----------------------------------------------------------------
//  Error type
// ----------------------------------------------------------------
class OPENMS_THERMO_BRIDGE_EXPORT bridge_error : public std::runtime_error
{
public:
    explicit bridge_error(const char* msg);
    explicit bridge_error(const std::string& msg);
    ~bridge_error() override;
};

// ----------------------------------------------------------------
//  Data structures returned by the bridge
// ----------------------------------------------------------------

/// Paired m/z and intensity arrays for a single scan.
struct SpectrumData
{
    std::vector<double> mz;
    std::vector<double> intensities;
};

/// Paired retention-time and intensity arrays.
struct ChromatogramData
{
    std::vector<double> times;       // retention times in minutes
    std::vector<double> intensities;
};

// ----------------------------------------------------------------
//  Legacy (convenience) free-functions
// ----------------------------------------------------------------

/// Returns the managed runtime directory resolved from the executable/library layout.
OPENMS_THERMO_BRIDGE_EXPORT std::filesystem::path default_managed_directory();

/// Open, read scan count, close.  Throws bridge_error on failure.
OPENMS_THERMO_BRIDGE_EXPORT int get_scan_count(const std::filesystem::path& raw_file_path);
OPENMS_THERMO_BRIDGE_EXPORT int get_scan_count(const std::filesystem::path& raw_file_path,
                   const std::filesystem::path& managed_directory);

// ----------------------------------------------------------------
//  Handle-based RAW file accessor
//
//  Wraps all Thermo RawFileReader functionality exposed by
//  ThermoRawFileParser (compomics/ThermoRawFileParser):
//    - Run header (scan range, time range, mass range)
//    - Instrument info (model, name, serial, software version)
//    - Scan-level data (RT, MS level, centroids/profile, filter)
//    - Precursor / reaction info (m/z, charge, CE, activation)
//    - Chromatograms (TIC and XIC)
//    - Trailer extra data
//    - Instrument methods
//    - Scan statistics (base peak, TIC)
//    - File header (creation date, description, revision)
//    - Sample information
// ----------------------------------------------------------------
class OPENMS_THERMO_BRIDGE_EXPORT RawFile
{
public:
    /// Open a RAW file using the default managed directory.
    explicit RawFile(const std::filesystem::path& raw_file_path);

    /// Open a RAW file with an explicit managed directory.
    RawFile(const std::filesystem::path& raw_file_path,
            const std::filesystem::path& managed_directory);

    ~RawFile();

    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;
    RawFile(RawFile&& other) noexcept;
    RawFile& operator=(RawFile&& other) noexcept;

    // -- Run header ------------------------------------------------

    int scan_count() const;
    int first_scan_number() const;
    int last_scan_number() const;
    double start_time() const;   // minutes
    double end_time() const;     // minutes
    double low_mass() const;
    double high_mass() const;
    bool has_ms_data() const;
    int ms_instrument_count() const;

    // -- Instrument info -------------------------------------------

    std::string instrument_model() const;
    std::string instrument_name() const;
    std::string instrument_serial_number() const;
    std::string instrument_software_version() const;

    // -- File header / sample info ---------------------------------

    std::string creation_date() const;    // ISO-8601
    std::string file_description() const;
    int file_revision() const;
    std::string sample_name() const;

    // -- Scan-level scalar queries ---------------------------------

    double retention_time(int scan_number) const;   // minutes
    int ms_level(int scan_number) const;
    bool is_centroid_scan(int scan_number) const;
    std::string scan_filter(int scan_number) const;
    int polarity(int scan_number) const;  // 0=positive, 1=negative
    std::string mass_analyzer_type(int scan_number) const;
    std::string ionization_mode(int scan_number) const;

    // -- Spectrum data ---------------------------------------------

    /// Peak count for centroid or profile representation.
    int spectrum_peak_count(int scan_number, bool centroid = true) const;

    /// Retrieve m/z + intensity arrays for a single scan.
    SpectrumData spectrum_data(int scan_number, bool centroid = true) const;

    // -- Precursor / reaction info (MSn scans) ---------------------

    double precursor_mass(int scan_number) const;    // monoisotopic m/z
    int precursor_charge(int scan_number) const;
    double collision_energy(int scan_number) const;
    std::string activation_type(int scan_number) const;
    double isolation_width(int scan_number) const;

    // -- Scan statistics -------------------------------------------

    double base_peak_mass(int scan_number) const;
    double base_peak_intensity(int scan_number) const;
    double tic(int scan_number) const;

    // -- Chromatograms ---------------------------------------------

    /// Full TIC chromatogram.
    ChromatogramData chromatogram_data() const;

    /// Extracted-ion chromatogram for the given m/z window.
    int xic_length(double mz_start, double mz_end,
                   double rt_start_minutes = 0.0,
                   double rt_end_minutes = 1e30) const;

    ChromatogramData xic_data(double mz_start, double mz_end,
                              double rt_start_minutes = 0.0,
                              double rt_end_minutes = 1e30) const;

    // -- Trailer extra data ----------------------------------------

    int trailer_extra_count(int scan_number) const;
    std::vector<std::string> trailer_extra_labels(int scan_number) const;
    std::string trailer_extra_value(int scan_number,
                                    const std::string& key) const;

    // -- Instrument methods ----------------------------------------

    int instrument_method_count() const;
    std::string instrument_method(int index = 0) const;

private:
    int handle_ = 0;
};

} // namespace openms::thermo_bridge
