#pragma once

#include "openms_thermo_bridge/openms_thermo_bridge_export.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#define OPENMS_THERMO_BRIDGE_API_VERSION 1

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
//  Typed scan header / single-pass scan reading (API version 1)
//
//  Enum values are the Thermo RawFileReader enum values; use the
//  *_name() helpers for the vendor ToString() names that the JSON and
//  string getters return.
// ----------------------------------------------------------------

/// Thermo MSOrderType. Values >= 1 are the MS level.
enum class MsOrder : std::int32_t
{
    Ng = -3,   ///< neutral gain
    Nl = -2,   ///< neutral loss
    Par = -1,  ///< parent (precursor ion) scan
    Any = 0,
    Ms = 1, Ms2 = 2, Ms3 = 3, Ms4 = 4, Ms5 = 5, Ms6 = 6, Ms7 = 7, Ms8 = 8, Ms9 = 9, Ms10 = 10
};

/// Thermo PolarityType.
enum class Polarity : std::int32_t { Negative = 0, Positive = 1, Any = 2 };

/// Where the peaks of a scan read came from.
enum class PeakSource : std::int32_t
{
    None = 0,             ///< peaks not requested
    CentroidStream = 1,   ///< native (label/FT) centroid stream
    Segmented = 2,        ///< segmented scan as acquired (profile or centroid)
    SoftwareCentroid = 3  ///< profile segmented scan centroided by RawFileReader
};

/// Value type of a trailer extra field, derived from the file's declared type.
enum class TrailerKind : std::int32_t
{
    Double = 0,  ///< FLOAT, DOUBLE
    Int = 1,     ///< CHAR, UCHAR, SHORT, USHORT, LONG, ULONG
    Bool = 2,    ///< TRUEFALSE, YESNO, ONOFF
    String = 3   ///< CHAR_STRING, WCHAR_STRING, unknown types
};

/// One trailer extra field of the selected controller (common to all scans).
struct TrailerLabel
{
    std::string label;     ///< vendor label including punctuation, e.g. "Charge State:"
    TrailerKind kind = TrailerKind::String;
    std::int32_t slot = 0; ///< index into the ScanHeader array of this kind
    std::int32_t vendor_type = 0;  ///< Thermo GenericDataTypes value
    bool selected = true;  ///< false if excluded by RawFile::set_trailer_selection
};

/// Per-controller trailer layout; identical for every scan of that controller.
struct TrailerLayout
{
    std::vector<TrailerLabel> labels;  ///< in vendor order
    std::size_t double_count = 0, int_count = 0, bool_count = 0, string_count = 0;

    /// Index into labels for an exact label match, or -1.
    int find(const std::string& label) const
    {
        for (std::size_t i = 0; i < labels.size(); ++i)
            if (labels[i].label == label) return static_cast<int>(i);
        return -1;
    }
};

/// One reaction (precursor step) of an MSn scan event, in vendor order.
struct Reaction
{
    double precursor_mass = 0.0;        ///< m/z
    double isolation_width = 0.0;       ///< m/z
    double isolation_offset = 0.0;      ///< m/z
    double collision_energy = 0.0;      ///< eV
    double first_precursor_mass = 0.0;  ///< valid if precursor_range_valid
    double last_precursor_mass = 0.0;
    std::int32_t activation = 0;        ///< Thermo ActivationType, see activation_type_name()
    bool collision_energy_valid = false;
    bool precursor_range_valid = false;
    bool multiple_activation = false;   ///< supplemental activation step
};

/// Fixed-layout scan header. Retention times are minutes; masses are m/z.
/// Non-finite vendor values are kept (NaN), not mapped to zero.
struct ScanHeader
{
    std::int32_t scan_number = 0;
    std::int32_t controller_type = 0;    ///< Thermo Device (0 = MS)
    std::int32_t controller_number = 0;  ///< 1-based
    MsOrder ms_order = MsOrder::Any;     ///< ms_level() for MS1..MS10
    Polarity polarity = Polarity::Any;
    std::int32_t analyzer = -1;          ///< Thermo MassAnalyzerType, -1 for non-MS controllers
    std::int32_t ionization = -1;        ///< Thermo IonizationModeType, -1 for non-MS controllers
    bool centroid = false;               ///< acquired as centroid (vendor ScanData)
    PeakSource peak_source = PeakSource::None;
    double retention_time = 0.0;
    double tic = 0.0;
    double base_peak_mass = 0.0;
    double base_peak_intensity = 0.0;
    double low_mass = 0.0;
    double high_mass = 0.0;
    double low_wavelength = 0.0;
    double high_wavelength = 0.0;
    std::string filter;                  ///< scan event / filter string, empty for non-MS
    std::vector<Reaction> reactions;     ///< empty for MS1 and non-MS

    /// Typed trailer values, indexed by TrailerLabel::slot of the matching kind.
    /// Filled when ReadOptions::trailers is set; unselected or unreadable
    /// entries are NaN / 0 / 0 / "".
    std::vector<double> trailer_doubles;
    std::vector<std::int64_t> trailer_ints;
    std::vector<std::uint8_t> trailer_bools;
    std::vector<std::string> trailer_strings;
    /// Vendor-formatted trailer strings in label order (v0.3 JSON values),
    /// only filled when ReadOptions::trailer_text is set.
    std::vector<std::string> trailer_text;

    int ms_level() const { return static_cast<int>(ms_order); }

    /// "controllerType=T controllerNumber=N scan=S" (Thermo nativeID format).
    std::string native_id() const
    {
        return "controllerType=" + std::to_string(controller_type) + " controllerNumber="
            + std::to_string(controller_number) + " scan=" + std::to_string(scan_number);
    }

    // Typed lookups by TrailerLayout label index; throw std::out_of_range on kind mismatch
    // or when trailer values were not read.
    OPENMS_THERMO_BRIDGE_EXPORT double trailer_double(const TrailerLayout& layout, int label_index) const;
    OPENMS_THERMO_BRIDGE_EXPORT std::int64_t trailer_int(const TrailerLayout& layout, int label_index) const;
    OPENMS_THERMO_BRIDGE_EXPORT bool trailer_bool(const TrailerLayout& layout, int label_index) const;
    OPENMS_THERMO_BRIDGE_EXPORT const std::string& trailer_string(const TrailerLayout& layout, int label_index) const;
    /// Any kind converted to double (bool -> 0/1, string parsed, NaN if unparsable).
    OPENMS_THERMO_BRIDGE_EXPORT double trailer_as_double(const TrailerLayout& layout, int label_index) const;
};

/// What read_scan / read_scans fetch. Every scan is decoded at most once.
struct ReadOptions
{
    bool centroid = true;       ///< centroid stream, else (software-)centroided segmented scan
    bool peaks = true;          ///< false: header only
    bool trailers = true;       ///< typed trailer values (subject to set_trailer_selection)
    bool trailer_text = false;  ///< also vendor-formatted trailer strings (slow)
    bool charges = false;       ///< native centroid charges (only with PeakSource::CentroidStream)
    bool noise = false;         ///< noise m/z, noise and baseline arrays (extra vendor read)
};

/// One scan. Peaks are sorted by ascending m/z (charges are permuted with them).
struct Spectrum
{
    ScanHeader header;
    std::vector<double> mz;
    std::vector<double> intensities;
    std::vector<double> charges;         ///< empty, or same length as mz
    /// Noise triplet on its own grid (see docs/metadata.md): all three have the same
    /// length, or all are empty (no noise data, e.g. scans without a centroid stream).
    std::vector<double> noise_mz;
    std::vector<double> noise_intensities;
    std::vector<double> noise_baselines;
};

/// Packed result of read_scans: scan i owns mz[peak_offsets[i] .. peak_offsets[i+1]).
/// All *_offsets arrays have headers.size() + 1 entries; noise_offsets index
/// noise_mz, noise_intensities and noise_baselines alike.
struct ScanBatch
{
    std::vector<ScanHeader> headers;
    std::vector<double> mz;
    std::vector<double> intensities;
    std::vector<std::size_t> peak_offsets;
    std::vector<double> charges;
    std::vector<std::size_t> charge_offsets;
    std::vector<double> noise_mz;
    std::vector<double> noise_intensities;
    std::vector<double> noise_baselines;
    std::vector<std::size_t> noise_offsets;

    std::size_t size() const { return headers.size(); }
    std::size_t peak_count(std::size_t i) const { return peak_offsets[i + 1] - peak_offsets[i]; }
};

/// Vendor enum names (ToString of the Thermo enum); decimal value for unknown values.
OPENMS_THERMO_BRIDGE_EXPORT std::string ms_order_name(MsOrder order);
OPENMS_THERMO_BRIDGE_EXPORT std::string mass_analyzer_name(int analyzer);
OPENMS_THERMO_BRIDGE_EXPORT std::string ionization_mode_name(int ionization);
OPENMS_THERMO_BRIDGE_EXPORT std::string activation_type_name(int activation);

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
    int polarity(int scan_number) const;  // 0=negative, 1=positive, 2=any
    std::string mass_analyzer_type(int scan_number) const;
    std::string ionization_mode(int scan_number) const;

    // -- Single-pass scan reading (preferred) ----------------------
    //
    //  Each call crosses into managed code once and decodes the scan once.
    //  Peaks are sorted by ascending m/z. Output buffers are reused.

    /// Trailer layout of the selected controller (cached per handle).
    const TrailerLayout& trailer_layout() const;

    /// Restrict typed trailer reads to these exact labels; empty = all labels.
    /// Slots in trailer_layout() stay stable; unselected labels get default values.
    /// Unknown labels are ignored.
    void set_trailer_selection(const std::vector<std::string>& labels);

    ScanHeader scan_header(int scan_number, const ReadOptions& options = {}) const;
    Spectrum read_scan(int scan_number, const ReadOptions& options = {}) const;
    void read_scan(int scan_number, Spectrum& out, const ReadOptions& options = {}) const;

    /// Read the inclusive scan range [first, last] in one managed call.
    ScanBatch read_scans(int first_scan, int last_scan, const ReadOptions& options = {}) const;
    void read_scans(int first_scan, int last_scan, ScanBatch& out, const ReadOptions& options = {}) const;

    /// Retrieve m/z + intensity arrays for a single scan (wraps read_scan, header-free).
    SpectrumData spectrum_data(int scan_number, bool centroid = true) const;

    // -- Concurrency -----------------------------------------------

    /// New handle on the same open file for use by another thread. Accessors
    /// share the vendor file index; each handle has its own controller and
    /// trailer selection (copied from this handle) and may outlive this one.
    /// One RawFile must not be used by two threads at once; distinct handles may.
    RawFile thread_accessor() const;

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

    /// Versioned JSON metadata (schema 1); numeric values retain vendor units.
    /// File properties, selected instrument/run, complete sample information and
    /// optional instrument methods. "file.sha1" is always null; use file_sha1().
    std::string file_metadata_json(bool include_methods = true) const;

    /// Lowercase hex SHA-1 of the whole RAW file (reads the entire file).
    std::string file_sha1() const;

    /// All scan statistics, reactions and trailer label/value pairs as JSON.
    /// Debug/compatibility path; prefer scan_header().
    std::string scan_metadata_json(int scan_number) const;

    /// Optional arrays: 0=centroid charges, 1=noise masses, 2=noises, 3=baselines.
    /// Charges refer to the native centroid stream; noise arrays have their own grid.
    std::vector<double> spectrum_auxiliary_array(int scan_number, int array_kind) const;

    /// Auxiliary detector channels, their units, and chromatograms (RT in minutes).
    /// Does not change the selected controller. Includes UV/PDA/Analog/MSAnalog.
    std::string detector_chromatograms_json() const;

    /// Enumerate controllers by vendor Device enum (type values are included in file_metadata_json).
    int instrument_count(int device_type) const;
    /// Select a 1-based controller. All subsequent scan queries use that controller.
    void select_instrument(int device_type, int instrument_number);

private:
    explicit RawFile(int handle) noexcept;
    void load_layout() const;

    int handle_ = 0;
    mutable TrailerLayout layout_;
    mutable bool layout_loaded_ = false;
};

} // namespace openms::thermo_bridge
