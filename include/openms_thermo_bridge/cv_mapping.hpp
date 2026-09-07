#pragma once

/// @file cv_mapping.hpp
/// @brief PSI-MS and related ontology term mappings for Thermo RAW data.
///
/// This header replicates the controlled-vocabulary term mappings found in
/// compomics/ThermoRawFileParser (Writer/OntologyMapping.cs) so that C++
/// consumers — in particular OpenMS — can construct properly-annotated
/// MSExperiment objects from the raw data returned by the thermo_bridge API.

#include <openms_thermo_bridge/openms_thermo_bridge_export.h>
#include <string>
#include <vector>

namespace openms::thermo_bridge
{

// ================================================================
//  Core CV-term types
// ================================================================

/// A single controlled-vocabulary term, optionally carrying a value.
struct CVTerm
{
    std::string accession;  ///< e.g. "MS:1000449"
    std::string cv_ref;     ///< e.g. "MS", "UO", "NCIT"
    std::string name;       ///< Human-readable name.
    std::string value;      ///< Value string (may be empty).
};

/// A CV term together with its unit term (commonly used in mzML).
struct CVParam
{
    CVTerm term;
    CVTerm unit;  ///< Unit term; if accession is empty, no unit.
};

// ================================================================
//  Instrument model -> PSI-MS accession
//
//  Ported from ThermoRawFileParser Writer/OntologyMapping.cs
//  GetInstrumentModel().  The lookup tries longest-prefix matching
//  so that e.g. "LTQ Orbitrap XXL" matches "LTQ Orbitrap" -> MS:1000449.
// ================================================================

/// Map a Thermo instrument model string to a PSI-MS accession + name.
/// Falls back to MS:1000483 ("Thermo Fisher Scientific instrument model").
OPENMS_THERMO_BRIDGE_EXPORT CVTerm cv_instrument_model(const std::string& model);

// ================================================================
//  Mass analyzer -> PSI-MS accession
//
//  The input is the Thermo MassAnalyzerType enum .ToString() value
//  as returned by RawFile::mass_analyzer_type().
//  For FTMS analyzers the instrument model is needed to distinguish
//  FT-ICR (MS:1000079) from Orbitrap (MS:1000484).
// ================================================================

OPENMS_THERMO_BRIDGE_EXPORT CVTerm cv_mass_analyzer(const std::string& analyzer_type,
                        const std::string& instrument_model = {});

// ================================================================
//  Ionization mode -> PSI-MS accession
// ================================================================

OPENMS_THERMO_BRIDGE_EXPORT CVTerm cv_ionization_mode(const std::string& ionization_type);

// ================================================================
//  Activation / dissociation type -> PSI-MS accession
// ================================================================

OPENMS_THERMO_BRIDGE_EXPORT CVTerm cv_activation_type(const std::string& activation_type);

// ================================================================
//  Detector type for an instrument model -> PSI-MS accession(s)
//
//  Some instruments have multiple detectors (e.g. Orbitrap ID-X).
// ================================================================

OPENMS_THERMO_BRIDGE_EXPORT std::vector<CVTerm> cv_detector_types(const std::string& instrument_model);

// ================================================================
//  PSI-MS accession constants — Spectrum types
// ================================================================

namespace cv
{

// ---- Spectrum types ------------------------------------------------
inline constexpr const char* ms1_spectrum          = "MS:1000579"; // MS1 spectrum
inline constexpr const char* msn_spectrum          = "MS:1000580"; // MSn spectrum
inline constexpr const char* precursor_ion_spectrum = "MS:1000341";
inline constexpr const char* constant_neutral_loss = "MS:1000326";
inline constexpr const char* constant_neutral_gain = "MS:1000325";
inline constexpr const char* absorption_spectrum   = "MS:1000806";

// ---- Spectrum representation ---------------------------------------
inline constexpr const char* centroid_spectrum      = "MS:1000127";
inline constexpr const char* profile_spectrum       = "MS:1000128";

// ---- Polarity ------------------------------------------------------
inline constexpr const char* positive_scan          = "MS:1000130";
inline constexpr const char* negative_scan          = "MS:1000129";

// ---- MS level ------------------------------------------------------
inline constexpr const char* ms_level               = "MS:1000511";

// ---- Scan properties -----------------------------------------------
inline constexpr const char* scan_start_time        = "MS:1000016";
inline constexpr const char* filter_string          = "MS:1000512";
inline constexpr const char* ion_injection_time     = "MS:1000927";
inline constexpr const char* scan_window_lower      = "MS:1000501";
inline constexpr const char* scan_window_upper      = "MS:1000500";

// ---- Total-ion current ---------------------------------------------
inline constexpr const char* total_ion_current      = "MS:1000285";

// ---- Peak properties -----------------------------------------------
inline constexpr const char* base_peak_mz           = "MS:1000504";
inline constexpr const char* base_peak_intensity    = "MS:1000505";
inline constexpr const char* lowest_observed_mz     = "MS:1000528";
inline constexpr const char* highest_observed_mz    = "MS:1000527";
inline constexpr const char* lowest_observed_wl     = "MS:1000619";
inline constexpr const char* highest_observed_wl    = "MS:1000618";

// ---- Precursor / isolation window ----------------------------------
inline constexpr const char* isolation_window_target   = "MS:1000827";
inline constexpr const char* isolation_window_lower    = "MS:1000828";
inline constexpr const char* isolation_window_upper    = "MS:1000829";
inline constexpr const char* selected_ion_mz           = "MS:1000744";
inline constexpr const char* charge_state              = "MS:1000041";
inline constexpr const char* peak_intensity            = "MS:1000042";

// ---- Activation / collision energy ---------------------------------
inline constexpr const char* collision_energy          = "MS:1000045";
inline constexpr const char* activation_method         = "MS:1000044";
inline constexpr const char* supplemental_ce           = "MS:1002680";
inline constexpr const char* supplemental_beam_cid     = "MS:1002678";
inline constexpr const char* supplemental_cid          = "MS:1002679";

// ---- Data arrays ---------------------------------------------------
inline constexpr const char* mz_array                  = "MS:1000514";
inline constexpr const char* intensity_array           = "MS:1000515";
inline constexpr const char* charge_array              = "MS:1000516";
inline constexpr const char* noise_baseline_array      = "MS:1002745";
inline constexpr const char* noise_intensity_array     = "MS:1002744";
inline constexpr const char* noise_mz_array            = "MS:1002743";
inline constexpr const char* wavelength_array          = "MS:1000617";
inline constexpr const char* non_standard_data_array   = "MS:1000786";
inline constexpr const char* time_array                = "MS:1000595";
inline constexpr const char* flow_rate_array           = "MS:1000820";
inline constexpr const char* pressure_array            = "MS:1000821";

// ---- Chromatogram types --------------------------------------------
inline constexpr const char* basepeak_chromatogram     = "MS:1000628";
inline constexpr const char* tic_chromatogram          = "MS:1000235";
inline constexpr const char* ion_current_chromatogram  = "MS:1000810";
inline constexpr const char* em_radiation_chromatogram = "MS:1000811";
inline constexpr const char* absorption_chromatogram   = "MS:1000812";
inline constexpr const char* emission_chromatogram     = "MS:1000813";
inline constexpr const char* pressure_chromatogram     = "MS:1003019";
inline constexpr const char* flow_rate_chromatogram    = "MS:1003020";
inline constexpr const char* chromatogram_type         = "MS:1000626";

// ---- File format / source ------------------------------------------
inline constexpr const char* thermo_nativeid_format    = "MS:1000768";
inline constexpr const char* thermo_raw_format         = "MS:1000563";
inline constexpr const char* sha1_checksum             = "MS:1000569";
inline constexpr const char* float_64bit               = "MS:1000523";
inline constexpr const char* zlib_compression          = "MS:1000574";
inline constexpr const char* no_compression            = "MS:1000576";
inline constexpr const char* no_combination            = "MS:1000795";

// ---- Software / processing ----------------------------------------
inline constexpr const char* thermo_raw_file_parser    = "MS:1003145";
inline constexpr const char* conversion_to_mzml        = "MS:1000544";
inline constexpr const char* peak_picking              = "MS:1000035";

// ---- FAIMS ---------------------------------------------------------
inline constexpr const char* faims_cv                  = "MS:1001581";

// ---- Thermo instrument model (generic) -----------------------------
inline constexpr const char* thermo_instrument_model   = "MS:1000483";
inline constexpr const char* thermo_scientific_model   = "MS:1000494";
inline constexpr const char* instrument_attribute      = "MS:1000496";
inline constexpr const char* instrument_serial_number  = "MS:1000529";

// ---- Sample --------------------------------------------------------
inline constexpr const char* sample_name               = "MS:1000002";
inline constexpr const char* sample_number             = "MS:1000001";
inline constexpr const char* sample_volume             = "MS:1000005";

// ---- Unit ontology terms (UO) --------------------------------------
inline constexpr const char* uo_minute                 = "UO:0000031";
inline constexpr const char* uo_millisecond            = "UO:0000028";
inline constexpr const char* uo_electronvolt           = "UO:0000266";
inline constexpr const char* uo_mz                     = "UO:0000040";
inline constexpr const char* uo_absorbance_unit        = "UO:0000269";
inline constexpr const char* uo_volumetric_flow_rate   = "UO:0000270";
inline constexpr const char* uo_pressure_unit          = "UO:0000109";
inline constexpr const char* uo_nanometer              = "UO:0000018";
inline constexpr const char* uo_unit                   = "UO:0000000";
inline constexpr const char* uo_mass_unit              = "UO:0000002";

// ---- NCIT terms (used in metadata) ---------------------------------
inline constexpr const char* ncit_pathname             = "NCIT:C47922";
inline constexpr const char* ncit_version              = "NCIT:C25714";
inline constexpr const char* ncit_creation_date        = "NCIT:C69199";
inline constexpr const char* ncit_description          = "NCIT:C25365";
inline constexpr const char* ncit_software_version     = "NCIT:C111093";
inline constexpr const char* ncit_type                 = "NCIT:C25284";
inline constexpr const char* ncit_comment              = "NCIT:C25393";
inline constexpr const char* ncit_vial                 = "NCIT:C41275";
inline constexpr const char* ncit_row                  = "NCIT:C43378";

// ---- PRIDE terms (used in metadata statistics) ---------------------
inline constexpr const char* pride_num_ms1             = "PRIDE:0000481";
inline constexpr const char* pride_num_ms2             = "PRIDE:0000482";
inline constexpr const char* pride_num_ms3             = "PRIDE:0000483";
inline constexpr const char* pride_min_charge          = "PRIDE:0000472";
inline constexpr const char* pride_max_charge          = "PRIDE:0000473";
inline constexpr const char* pride_min_rt              = "PRIDE:0000474";
inline constexpr const char* pride_max_rt              = "PRIDE:0000475";
inline constexpr const char* pride_min_mz              = "PRIDE:0000476";
inline constexpr const char* pride_max_mz              = "PRIDE:0000477";
inline constexpr const char* pride_num_scans           = "PRIDE:0000478";
inline constexpr const char* pride_ms_scan_range       = "PRIDE:0000479";
inline constexpr const char* pride_rt_range            = "PRIDE:0000484";
inline constexpr const char* pride_mz_range            = "PRIDE:0000485";

// ---- AFR/AFO/AFQ terms (used for sample metadata) ------------------
inline constexpr const char* afr_firmware_version      = "AFR:0001259";
inline constexpr const char* afr_injection_volume      = "AFR:0001577";
inline constexpr const char* afr_acquisition_method    = "AFR:0002045";
inline constexpr const char* afr_calibration_level     = "AFR:0001849";
inline constexpr const char* afr_processing_method     = "AFR:0002175";
inline constexpr const char* afr_sample_weight         = "AFR:0001982";
inline constexpr const char* afq_dilution_factor       = "AFQ:0000178";

// ---- Mass resolution -----------------------------------------------
inline constexpr const char* mass_resolution           = "MS:1000011";

} // namespace cv

// ================================================================
//  Convenience: map polarity int (0/1) to CV term
// ================================================================

inline CVTerm cv_polarity(int polarity)
{
    if (polarity == 1)
        return {"MS:1000130", "MS", "positive scan", ""};
    if (polarity == 0)
        return {"MS:1000129", "MS", "negative scan", ""};
    return {}; // Any/unknown must not be asserted as a positive or negative scan.
}

// ================================================================
//  Convenience: spectrum type CV term from MS level
// ================================================================

inline CVTerm cv_spectrum_type(int ms_level)
{
    if (ms_level == 1)
        return {"MS:1000579", "MS", "MS1 spectrum", ""};
    else
        return {"MS:1000580", "MS", "MSn spectrum", ""};
}

// ================================================================
//  Convenience: data representation CV term
// ================================================================

inline CVTerm cv_spectrum_representation(bool centroid)
{
    if (centroid)
        return {"MS:1000127", "MS", "centroid spectrum", ""};
    else
        return {"MS:1000128", "MS", "profile spectrum", ""};
}

} // namespace openms::thermo_bridge
