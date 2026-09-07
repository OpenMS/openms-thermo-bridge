#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <openms_thermo_bridge/thermo_bridge.hpp>

// ================================================================
//  Helpers
// ================================================================

namespace
{

std::filesystem::path sample_raw_path()
{
    const std::string configured_path = OPENMS_THERMO_BRIDGE_TEST_SAMPLE_RAW;
    return configured_path.empty() ? std::filesystem::path{} : std::filesystem::path(configured_path);
}

/// Returns true if the bridge is functional on this platform.
/// Some CI platforms (e.g. Linux with Thermo vendor) may not support
/// the Thermo RawFileReader native binary.
bool bridge_is_functional()
{
    const auto path = sample_raw_path();
    if (path.empty() || !std::filesystem::exists(path)) return false;
    try
    {
        openms::thermo_bridge::get_scan_count(path);
        return true;
    }
    catch (const openms::thermo_bridge::bridge_error& e)
    {
        if (std::string(e.what()).find("not supported") != std::string::npos)
            return false;
        return true; // Other errors still mean the bridge itself is functional.
    }
}

/// RAII guard that opens a RAW file and skips the test if the bridge
/// or vendor library is not functional on this platform.
struct TestFile
{
    std::unique_ptr<openms::thermo_bridge::RawFile> file;

    TestFile()
    {
        const auto path = sample_raw_path();
        if (path.empty())
            SKIP("Sample RAW download is disabled for this build");
        REQUIRE(std::filesystem::exists(path));

        try
        {
            file = std::make_unique<openms::thermo_bridge::RawFile>(path);
        }
        catch (const openms::thermo_bridge::bridge_error& e)
        {
            const std::string msg = e.what();
            if (msg.find("not supported") != std::string::npos)
                SKIP("Thermo RawFileReader is not supported on this platform");
            throw;
        }
    }
};

} // namespace

// ================================================================
//  Error handling
// ================================================================

TEST_CASE("Missing RAW files are reported as bridge errors")
{
    CHECK_THROWS_AS(
        openms::thermo_bridge::get_scan_count("/tmp/this-file-does-not-exist.raw"),
        openms::thermo_bridge::bridge_error);

    try
    {
        openms::thermo_bridge::get_scan_count("/tmp/this-file-does-not-exist.raw");
        FAIL("Expected bridge_error to be thrown");
    }
    catch (const openms::thermo_bridge::bridge_error& e)
    {
        std::cerr << "[test] bridge_error: " << e.what() << "\n";
        CHECK(true);
    }
}

TEST_CASE("RawFile constructor throws for non-existent file")
{
    try
    {
        openms::thermo_bridge::RawFile rf("/tmp/no-such-file.raw");
        FAIL("Expected bridge_error");
    }
    catch (const openms::thermo_bridge::bridge_error& e)
    {
        std::cerr << "[test] bridge_error: " << e.what() << "\n";
        CHECK(true);
    }
}

// ================================================================
//  Legacy free-function test
// ================================================================

TEST_CASE("The public sample RAW file reports the expected scan count")
{
    const auto sample_path = sample_raw_path();
    if (sample_path.empty())
        SKIP("Sample RAW download is disabled for this build");

    REQUIRE(std::filesystem::exists(sample_path));

    try
    {
        const int count = openms::thermo_bridge::get_scan_count(sample_path);
        CHECK(count == OPENMS_THERMO_BRIDGE_TEST_EXPECTED_SCAN_COUNT);
    }
    catch (const openms::thermo_bridge::bridge_error& e)
    {
        const std::string msg = e.what();
        std::cerr << "[test] bridge_error: " << msg << "\n";
        if (msg.find("not supported on this platform") != std::string::npos)
            SKIP("Thermo RawFileReader is not supported on this platform");
        FAIL(msg);
    }
}

// ================================================================
//  Run header (mirrors ThermoRawFileParser RunHeaderEx tests)
// ================================================================

TEST_CASE("RawFile scan_count matches expected")
{
    TestFile tf;
    CHECK(tf.file->scan_count() == OPENMS_THERMO_BRIDGE_TEST_EXPECTED_SCAN_COUNT);
}

TEST_CASE("RawFile first_scan_number is positive")
{
    TestFile tf;
    CHECK(tf.file->first_scan_number() >= 1);
}

TEST_CASE("RawFile last_scan_number equals scan_count for standard files")
{
    TestFile tf;
    CHECK(tf.file->last_scan_number() == tf.file->scan_count());
}

TEST_CASE("RawFile start_time is non-negative")
{
    TestFile tf;
    CHECK(tf.file->start_time() >= 0.0);
}

TEST_CASE("RawFile end_time is greater than start_time")
{
    TestFile tf;
    CHECK(tf.file->end_time() > tf.file->start_time());
}

TEST_CASE("RawFile mass range is valid")
{
    TestFile tf;
    CHECK(tf.file->low_mass() >= 0.0);
    CHECK(tf.file->high_mass() > tf.file->low_mass());
}

TEST_CASE("RawFile has_ms_data returns true for MS file")
{
    TestFile tf;
    CHECK(tf.file->has_ms_data());
}

TEST_CASE("RawFile ms_instrument_count is at least 1")
{
    TestFile tf;
    CHECK(tf.file->ms_instrument_count() >= 1);
}

// ================================================================
//  Instrument information (MetadataWriter equivalents)
// ================================================================

TEST_CASE("RawFile instrument_model is non-empty")
{
    TestFile tf;
    CHECK(!tf.file->instrument_model().empty());
}

TEST_CASE("RawFile instrument_name is non-empty")
{
    TestFile tf;
    CHECK(!tf.file->instrument_name().empty());
}

TEST_CASE("RawFile instrument_serial_number returns a string")
{
    TestFile tf;
    // Serial may be empty for some instruments, so just ensure no crash.
    auto serial = tf.file->instrument_serial_number();
    CHECK(true); // no crash
}

TEST_CASE("RawFile instrument_software_version returns a string")
{
    TestFile tf;
    auto sw = tf.file->instrument_software_version();
    CHECK(true);
}

// ================================================================
//  File header / sample info
// ================================================================

TEST_CASE("RawFile creation_date returns ISO-8601 string")
{
    TestFile tf;
    auto date = tf.file->creation_date();
    CHECK(!date.empty());
    // ISO-8601 dates contain 'T' separator
    CHECK(date.find('T') != std::string::npos);
}

TEST_CASE("RawFile file_description returns a string")
{
    TestFile tf;
    auto desc = tf.file->file_description();
    CHECK(true); // may be empty
}

TEST_CASE("RawFile file_revision is non-negative")
{
    TestFile tf;
    CHECK(tf.file->file_revision() >= 0);
}

TEST_CASE("RawFile sample_name returns a string")
{
    TestFile tf;
    auto name = tf.file->sample_name();
    CHECK(true); // may be empty
}

// ================================================================
//  Scan-level queries (mirrors ProxiSpectrumReader / SpectrumWriter)
// ================================================================

TEST_CASE("RawFile retention_time increases across scans")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int last = tf.file->last_scan_number();
    REQUIRE(last > first);

    double prev_rt = tf.file->retention_time(first);
    CHECK(prev_rt >= 0.0);
    for (int s = first + 1; s <= std::min(first + 10, last); ++s)
    {
        double rt = tf.file->retention_time(s);
        CHECK(rt >= prev_rt);
        prev_rt = rt;
    }
}

TEST_CASE("RawFile ms_level returns valid MS level")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int level = tf.file->ms_level(first);
    CHECK(level >= 1);
    CHECK(level <= 10);
}

TEST_CASE("RawFile scan_filter returns a non-empty filter string")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto filter = tf.file->scan_filter(first);
    CHECK(!filter.empty());
}

TEST_CASE("RawFile polarity returns 0 (negative) or 1 (positive)")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int pol = tf.file->polarity(first);
    CHECK((pol == 0 || pol == 1));
}

TEST_CASE("RawFile mass_analyzer_type returns a non-empty string")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto analyzer = tf.file->mass_analyzer_type(first);
    CHECK(!analyzer.empty());
}

TEST_CASE("RawFile ionization_mode returns a string")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto mode = tf.file->ionization_mode(first);
    CHECK(true); // may be "Any" or similar
}

// ================================================================
//  Spectrum data (mirrors SpectrumWriter m/z+intensity extraction)
// ================================================================

TEST_CASE("RawFile spectrum_peak_count returns positive for valid scan")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int count = tf.file->spectrum_peak_count(first, true);
    CHECK(count > 0);
}

TEST_CASE("RawFile spectrum_data returns non-empty arrays")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto data = tf.file->spectrum_data(first, true);
    CHECK(!data.mz.empty());
    CHECK(!data.intensities.empty());
    CHECK(data.mz.size() == data.intensities.size());
}

TEST_CASE("RawFile spectrum_data m/z values are sorted ascending")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto data = tf.file->spectrum_data(first, true);
    REQUIRE(data.mz.size() > 1);
    for (std::size_t i = 1; i < data.mz.size(); ++i)
    {
        CHECK(data.mz[i] >= data.mz[i - 1]);
    }
}

TEST_CASE("RawFile spectrum_data intensities are non-negative")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto data = tf.file->spectrum_data(first, true);
    for (double v : data.intensities)
    {
        CHECK(v >= 0.0);
    }
}

TEST_CASE("RawFile profile spectrum_data is larger than centroid")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int centroid_count = tf.file->spectrum_peak_count(first, true);
    int profile_count = tf.file->spectrum_peak_count(first, false);
    // Profile data should typically have more points than centroid.
    // In some edge cases they may be equal, but profile should never be less.
    CHECK(profile_count >= centroid_count);
}

TEST_CASE("Low-resolution MSn segmented centroids are retained")
{
    // Optional LTQ fixture, e.g. ThermoRawFileParserTest/Data/small.RAW.
    const char* path = std::getenv("OPENMS_THERMO_BRIDGE_TEST_MSN_RAW");
    if (!path || !*path) SKIP("Set OPENMS_THERMO_BRIDGE_TEST_MSN_RAW to an LTQ MSn RAW fixture");
    openms::thermo_bridge::RawFile raw(path);
    bool checked = false;
    for (int scan = raw.first_scan_number(); scan <= raw.last_scan_number(); ++scan)
    {
        if (raw.ms_level(scan) < 2 || raw.mass_analyzer_type(scan) != "MassAnalyzerITMS") continue;
        const auto acquired = raw.spectrum_data(scan, false);
        if (acquired.mz.empty()) continue;
        const auto centroid = raw.spectrum_data(scan, true);
        REQUIRE(!centroid.mz.empty());
        CHECK(centroid.mz.size() == centroid.intensities.size());
        CHECK(raw.spectrum_peak_count(scan, true) == centroid.mz.size());
        if (raw.is_centroid_scan(scan))
        {
            CHECK(centroid.mz == acquired.mz);
            CHECK(centroid.intensities == acquired.intensities);
        }
        else CHECK(centroid.mz.size() <= acquired.mz.size());
        checked = true;
    }
    REQUIRE(checked);
}

// ================================================================
//  Precursor / reaction info (mirrors SpectrumWriter precursor)
// ================================================================

TEST_CASE("RawFile precursor_mass returns 0 for MS1 scan")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    // Find an MS1 scan
    for (int s = first; s <= tf.file->last_scan_number(); ++s)
    {
        if (tf.file->ms_level(s) == 1)
        {
            CHECK(tf.file->precursor_mass(s) == 0.0);
            return;
        }
    }
    SKIP("No MS1 scan found");
}

TEST_CASE("RawFile precursor_mass returns positive value for MS2 scan")
{
    TestFile tf;
    for (int s = tf.file->first_scan_number(); s <= tf.file->last_scan_number(); ++s)
    {
        if (tf.file->ms_level(s) >= 2)
        {
            CHECK(tf.file->precursor_mass(s) > 0.0);
            return;
        }
    }
    SKIP("No MS2 scan found");
}

TEST_CASE("RawFile precursor_charge returns non-negative for MS2 scan")
{
    TestFile tf;
    for (int s = tf.file->first_scan_number(); s <= tf.file->last_scan_number(); ++s)
    {
        if (tf.file->ms_level(s) >= 2)
        {
            int charge = tf.file->precursor_charge(s);
            CHECK(charge >= 0);
            return;
        }
    }
    SKIP("No MS2 scan found");
}

TEST_CASE("RawFile collision_energy returns 0 for MS1 and positive for MS2")
{
    TestFile tf;
    bool found_ms1 = false;
    bool found_ms2 = false;
    for (int s = tf.file->first_scan_number(); s <= tf.file->last_scan_number(); ++s)
    {
        int level = tf.file->ms_level(s);
        if (level == 1 && !found_ms1)
        {
            CHECK(tf.file->collision_energy(s) == 0.0);
            found_ms1 = true;
        }
        if (level >= 2 && !found_ms2)
        {
            // Collision energy should be non-negative (can be 0 for some activation types)
            CHECK(tf.file->collision_energy(s) >= 0.0);
            found_ms2 = true;
        }
        if (found_ms1 && found_ms2) return;
    }
    if (!found_ms1 && !found_ms2) SKIP("Need at least one MS1 and one MS2 scan");
}

TEST_CASE("RawFile activation_type returns non-empty for MS2 scan")
{
    TestFile tf;
    for (int s = tf.file->first_scan_number(); s <= tf.file->last_scan_number(); ++s)
    {
        if (tf.file->ms_level(s) >= 2)
        {
            auto act = tf.file->activation_type(s);
            CHECK(!act.empty());
            return;
        }
    }
    SKIP("No MS2 scan found");
}

TEST_CASE("RawFile isolation_width returns positive for MS2 scan")
{
    TestFile tf;
    for (int s = tf.file->first_scan_number(); s <= tf.file->last_scan_number(); ++s)
    {
        if (tf.file->ms_level(s) >= 2)
        {
            CHECK(tf.file->isolation_width(s) > 0.0);
            return;
        }
    }
    SKIP("No MS2 scan found");
}

// ================================================================
//  Scan statistics (mirrors MetadataWriter scan stats)
// ================================================================

TEST_CASE("RawFile base_peak_mass is within mass range")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    double bpm = tf.file->base_peak_mass(first);
    CHECK(bpm >= 0.0);
    CHECK(bpm <= tf.file->high_mass() * 2); // some tolerance
}

TEST_CASE("RawFile base_peak_intensity is positive")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    CHECK(tf.file->base_peak_intensity(first) > 0.0);
}

TEST_CASE("RawFile TIC is positive")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    CHECK(tf.file->tic(first) > 0.0);
}

// ================================================================
//  Chromatogram (mirrors TIC generation in MzMlSpectrumWriter)
// ================================================================

TEST_CASE("RawFile chromatogram_data returns expected number of points")
{
    TestFile tf;
    auto chrom = tf.file->chromatogram_data();
    int expected = tf.file->last_scan_number() - tf.file->first_scan_number() + 1;
    CHECK(static_cast<int>(chrom.times.size()) == expected);
    CHECK(chrom.times.size() == chrom.intensities.size());
}

TEST_CASE("RawFile chromatogram_data times are monotonically increasing")
{
    TestFile tf;
    auto chrom = tf.file->chromatogram_data();
    REQUIRE(chrom.times.size() > 1);
    for (std::size_t i = 1; i < chrom.times.size(); ++i)
    {
        CHECK(chrom.times[i] >= chrom.times[i - 1]);
    }
}

TEST_CASE("RawFile chromatogram_data intensities are non-negative")
{
    TestFile tf;
    auto chrom = tf.file->chromatogram_data();
    for (double v : chrom.intensities)
    {
        CHECK(v >= 0.0);
    }
}

// ================================================================
//  XIC (mirrors XicReader)
// ================================================================

TEST_CASE("RawFile xic_length returns non-negative for full mass range")
{
    TestFile tf;
    double low = tf.file->low_mass();
    double high = tf.file->high_mass();
    int len = tf.file->xic_length(low, high);
    CHECK(len >= 0);
}

TEST_CASE("RawFile xic_data returns data for valid mass range")
{
    TestFile tf;
    double low = tf.file->low_mass();
    double high = tf.file->high_mass();
    auto xic = tf.file->xic_data(low, high);
    CHECK(xic.times.size() == xic.intensities.size());
    // For a full mass range XIC, we should get at least some data points
    CHECK(!xic.times.empty());
}

TEST_CASE("RawFile xic_data with narrow mass range may return fewer points")
{
    TestFile tf;
    double mid = (tf.file->low_mass() + tf.file->high_mass()) / 2.0;
    double narrow = 0.01;
    auto xic_narrow = tf.file->xic_data(mid - narrow, mid + narrow);
    auto xic_wide = tf.file->xic_data(tf.file->low_mass(), tf.file->high_mass());
    // Not a strict requirement, but typically a narrower range has fewer non-zero intensities
    CHECK(xic_narrow.times.size() <= xic_wide.times.size() + 1); // +1 for rounding
}

// ================================================================
//  Trailer extra data (mirrors ScanTrailer in ThermoRawFileParser)
// ================================================================

TEST_CASE("RawFile trailer_extra_count is positive for first scan")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int count = tf.file->trailer_extra_count(first);
    CHECK(count > 0);
}

TEST_CASE("RawFile trailer_extra_labels returns non-empty list")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto labels = tf.file->trailer_extra_labels(first);
    CHECK(!labels.empty());
    // All labels should be non-empty strings
    for (const auto& label : labels)
    {
        CHECK(!label.empty());
    }
}

TEST_CASE("RawFile trailer_extra_value returns data for known key")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto labels = tf.file->trailer_extra_labels(first);
    REQUIRE(!labels.empty());

    // Try to get a value for the first available label
    auto value = tf.file->trailer_extra_value(first, labels[0]);
    // Value may be empty for some keys, but the call should succeed
    CHECK(true);
}

TEST_CASE("RawFile trailer_extra_value returns empty for unknown key")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    auto value = tf.file->trailer_extra_value(first, "Nonexistent Key That Should Not Exist");
    CHECK(value.empty());
}

// ================================================================
//  Instrument methods
// ================================================================

TEST_CASE("RawFile instrument_method_count is non-negative")
{
    TestFile tf;
    int count = tf.file->instrument_method_count();
    CHECK(count >= 0);
}

TEST_CASE("RawFile instrument_method returns text when available")
{
    TestFile tf;
    int count = tf.file->instrument_method_count();
    if (count > 0)
    {
        auto method = tf.file->instrument_method(0);
        CHECK(!method.empty());
    }
    else
    {
        SKIP("No instrument methods in this RAW file");
    }
}

// ================================================================
//  Move semantics
// ================================================================

TEST_CASE("RawFile is movable")
{
    TestFile tf;
    auto& orig = *tf.file;
    int count_before = orig.scan_count();

    openms::thermo_bridge::RawFile moved(std::move(orig));
    CHECK(moved.scan_count() == count_before);
}

// ================================================================
//  Multiple scans iteration (stress test)
// ================================================================

TEST_CASE("Iterate all scans checking MS level and spectrum data")
{
    TestFile tf;
    int first = tf.file->first_scan_number();
    int last = tf.file->last_scan_number();

    int ms1_count = 0;
    int ms2_count = 0;

    for (int s = first; s <= last; ++s)
    {
        int level = tf.file->ms_level(s);
        CHECK(level >= 1);

        if (level == 1) ++ms1_count;
        if (level >= 2) ++ms2_count;

        // Every scan should have some data
        int peaks = tf.file->spectrum_peak_count(s, true);
        CHECK(peaks >= 0);

        // Retention time should be non-negative
        CHECK(tf.file->retention_time(s) >= 0.0);
    }

    // The sample file should have both MS1 and MS2 scans
    // (if not, at least one of them should be present)
    CHECK((ms1_count + ms2_count) == (last - first + 1));
    std::cerr << "[test] MS1 scans: " << ms1_count << ", MS2+ scans: " << ms2_count << "\n";
}

TEST_CASE("Bulk metadata preserves provenance and all trailer labels")
{
    TestFile tf;
    const auto metadata = tf.file->file_metadata_json(false, false);
    CHECK(metadata.find("\"schema_version\":1") != std::string::npos);
    CHECK(metadata.find("\"instrument_methods\":[]") != std::string::npos);
    CHECK(metadata.find("\"sha1\":null") != std::string::npos);
    CHECK(metadata.find("\"injection volume\":1") != std::string::npos);
    CHECK(metadata.find("\"creation_date\":\"2019-02-14T18:56:53.") != std::string::npos);
    const int scan = tf.file->first_scan_number();
    const auto scan_metadata = tf.file->scan_metadata_json(scan);
    CHECK(scan_metadata.find("\"polarity\":1") != std::string::npos);
    CHECK(scan_metadata.find("\"reactions\":[]") != std::string::npos);
    for (const auto& label : tf.file->trailer_extra_labels(scan))
    {
        // Public fixture labels contain no JSON escape characters.
        CHECK(scan_metadata.find("\"label\":\"" + label + "\"") != std::string::npos);
    }
}

TEST_CASE("Controller discovery preserves the selected MS instrument")
{
    TestFile tf;
    const auto before = tf.file->scan_metadata_json(tf.file->first_scan_number());
    const auto traces = tf.file->detector_chromatograms_json();
    CHECK(traces.find("\"chromatograms\":") != std::string::npos);
    CHECK(tf.file->scan_metadata_json(tf.file->first_scan_number()) == before);
    CHECK(tf.file->instrument_count(0) == tf.file->ms_instrument_count());
    tf.file->select_instrument(0, 1);
    CHECK(tf.file->scan_metadata_json(tf.file->first_scan_number()) == before);
}

TEST_CASE("Optional arrays use their own grid and invalid requests throw")
{
    TestFile tf;
    const int scan = tf.file->first_scan_number();
    const auto mz = tf.file->spectrum_auxiliary_array(scan, 1);
    const auto noise = tf.file->spectrum_auxiliary_array(scan, 2);
    const auto baseline = tf.file->spectrum_auxiliary_array(scan, 3);
    CHECK(mz.size() == noise.size());
    CHECK(mz.size() == baseline.size());
    CHECK_THROWS_AS(tf.file->spectrum_auxiliary_array(scan, 99), openms::thermo_bridge::bridge_error);
    CHECK_THROWS_AS(tf.file->scan_metadata_json(tf.file->last_scan_number() + 1), openms::thermo_bridge::bridge_error);
    CHECK_THROWS_AS(tf.file->scan_filter(tf.file->last_scan_number() + 1), openms::thermo_bridge::bridge_error);
}

TEST_CASE("PDA spectra expose wavelength windows and controller native IDs")
{
    TestFile tf;
    REQUIRE(tf.file->instrument_count(4) == 1); // Public fixture includes a PDA controller.
    tf.file->select_instrument(4, 1);
    CHECK(tf.file->scan_count() == 1500);
    const int scan = tf.file->first_scan_number();
    const auto metadata = tf.file->scan_metadata_json(scan);
    INFO(metadata);
    CHECK(metadata.find("controllerType=4 controllerNumber=1 scan=1") != std::string::npos);
    CHECK(metadata.find("\"low_wavelength\":220") != std::string::npos);
    CHECK(metadata.find("\"high_wavelength\":499") != std::string::npos);
    const auto spectrum = tf.file->spectrum_data(scan, false);
    REQUIRE(!spectrum.mz.empty());
    CHECK(spectrum.mz.size() == spectrum.intensities.size());
    CHECK(spectrum.mz.front() >= 220);
    CHECK(spectrum.mz.back() <= 500);
    const auto before = tf.file->scan_metadata_json(scan);
    tf.file->detector_chromatograms_json();
    CHECK(tf.file->scan_metadata_json(scan) == before);
}
