#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <openms_thermo_bridge/cv_mapping.hpp>

using namespace openms::thermo_bridge;

// ================================================================
//  Instrument model -> PSI-MS accession
// ================================================================

TEST_CASE("cv_instrument_model: exact LTQ Orbitrap match")
{
    auto t = cv_instrument_model("LTQ Orbitrap");
    CHECK(t.accession == "MS:1000449");
    CHECK(t.cv_ref == "MS");
    CHECK(t.name == "LTQ Orbitrap");
}

TEST_CASE("cv_instrument_model: partial match (model with extra text)")
{
    // Mirrors ThermoRawFileParser's OntologyMappingTests.cs:
    // "LTQ Orbitrap XXL" -> longest prefix "LTQ Orbitrap" -> MS:1000449
    auto t = cv_instrument_model("LTQ Orbitrap XXL");
    CHECK(t.accession == "MS:1000449");
}

TEST_CASE("cv_instrument_model: unknown model falls back to generic")
{
    auto t = cv_instrument_model("non existing model");
    CHECK(t.accession == "MS:1000483");
    CHECK(t.name == "Thermo Fisher Scientific instrument model");
}

TEST_CASE("cv_instrument_model: case insensitive")
{
    auto t = cv_instrument_model("ltq orbitrap");
    CHECK(t.accession == "MS:1000449");
}

TEST_CASE("cv_instrument_model: Orbitrap Fusion Lumos")
{
    auto t = cv_instrument_model("Orbitrap Fusion Lumos");
    CHECK(t.accession == "MS:1002732");
}

TEST_CASE("cv_instrument_model: Q Exactive HF-X")
{
    auto t = cv_instrument_model("Q Exactive HF-X");
    CHECK(t.accession == "MS:1002877");
}

TEST_CASE("cv_instrument_model: Q Exactive HF")
{
    auto t = cv_instrument_model("Q Exactive HF");
    CHECK(t.accession == "MS:1002523");
}

TEST_CASE("cv_instrument_model: Q Exactive Plus")
{
    auto t = cv_instrument_model("Q Exactive Plus");
    CHECK(t.accession == "MS:1002634");
}

TEST_CASE("cv_instrument_model: Q Exactive (base)")
{
    auto t = cv_instrument_model("Q Exactive");
    CHECK(t.accession == "MS:1001911");
}

TEST_CASE("cv_instrument_model: Exactive Plus")
{
    auto t = cv_instrument_model("Exactive Plus");
    CHECK(t.accession == "MS:1002526");
}

TEST_CASE("cv_instrument_model: Exactive")
{
    auto t = cv_instrument_model("Exactive");
    CHECK(t.accession == "MS:1000649");
}

TEST_CASE("cv_instrument_model: Orbitrap Eclipse")
{
    auto t = cv_instrument_model("Orbitrap Eclipse");
    CHECK(t.accession == "MS:1003029");
}

TEST_CASE("cv_instrument_model: Orbitrap Ascend")
{
    auto t = cv_instrument_model("Orbitrap Ascend");
    CHECK(t.accession == "MS:1003356");
}

TEST_CASE("cv_instrument_model: Orbitrap ID-X")
{
    auto t = cv_instrument_model("Orbitrap ID-X");
    CHECK(t.accession == "MS:1003112");
}

TEST_CASE("cv_instrument_model: Orbitrap Exploris 480")
{
    auto t = cv_instrument_model("Orbitrap Exploris 480");
    CHECK(t.accession == "MS:1003028");
}

TEST_CASE("cv_instrument_model: Orbitrap Exploris 240")
{
    auto t = cv_instrument_model("Orbitrap Exploris 240");
    CHECK(t.accession == "MS:1003094");
}

TEST_CASE("cv_instrument_model: Orbitrap Exploris 120")
{
    auto t = cv_instrument_model("Orbitrap Exploris 120");
    CHECK(t.accession == "MS:1003095");
}

TEST_CASE("cv_instrument_model: Orbitrap Astral")
{
    auto t = cv_instrument_model("Orbitrap Astral");
    CHECK(t.accession == "MS:1003378");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap Velos")
{
    auto t = cv_instrument_model("LTQ Orbitrap Velos");
    CHECK(t.accession == "MS:1001742");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap Elite")
{
    auto t = cv_instrument_model("LTQ Orbitrap Elite");
    CHECK(t.accession == "MS:1001910");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap Discovery")
{
    auto t = cv_instrument_model("LTQ Orbitrap Discovery");
    CHECK(t.accession == "MS:1000555");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap XL ETD")
{
    auto t = cv_instrument_model("LTQ Orbitrap XL ETD");
    CHECK(t.accession == "MS:1000639");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap XL")
{
    auto t = cv_instrument_model("LTQ Orbitrap XL");
    CHECK(t.accession == "MS:1000556");
}

TEST_CASE("cv_instrument_model: LTQ FT Ultra")
{
    auto t = cv_instrument_model("LTQ FT Ultra");
    CHECK(t.accession == "MS:1000557");
}

TEST_CASE("cv_instrument_model: LTQ FT")
{
    auto t = cv_instrument_model("LTQ FT");
    CHECK(t.accession == "MS:1000448");
}

TEST_CASE("cv_instrument_model: LTQ")
{
    auto t = cv_instrument_model("LTQ");
    CHECK(t.accession == "MS:1000447");
}

TEST_CASE("cv_instrument_model: Orbitrap Fusion")
{
    auto t = cv_instrument_model("Orbitrap Fusion");
    CHECK(t.accession == "MS:1002416");
}

TEST_CASE("cv_instrument_model: Orbitrap Fusion ETD")
{
    auto t = cv_instrument_model("Orbitrap Fusion ETD");
    CHECK(t.accession == "MS:1002417");
}

TEST_CASE("cv_instrument_model: TSQ Quantum Access MAX")
{
    auto t = cv_instrument_model("TSQ Quantum Access MAX");
    CHECK(t.accession == "MS:1003498");
}

TEST_CASE("cv_instrument_model: MALDI LTQ Orbitrap")
{
    auto t = cv_instrument_model("MALDI LTQ Orbitrap");
    CHECK(t.accession == "MS:1000643");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap Velos Pro")
{
    auto t = cv_instrument_model("LTQ Orbitrap Velos Pro");
    CHECK(t.accession == "MS:1003096");
}

TEST_CASE("cv_instrument_model: LTQ Orbitrap Classic")
{
    auto t = cv_instrument_model("LTQ Orbitrap Classic");
    CHECK(t.accession == "MS:1002835");
}

TEST_CASE("cv_instrument_model: DeltaPlus")
{
    auto t = cv_instrument_model("DeltaPlus Advantage");
    CHECK(t.accession == "MS:1003504");
}

// ================================================================
//  Mass analyzer -> PSI-MS accession
// ================================================================

TEST_CASE("cv_mass_analyzer: FTMS with Orbitrap instrument -> Orbitrap")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS", "Orbitrap Fusion Lumos");
    CHECK(t.accession == "MS:1000484");
    CHECK(t.name == "orbitrap");
}

TEST_CASE("cv_mass_analyzer: FTMS with Exactive instrument -> Orbitrap")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS", "Q Exactive HF");
    CHECK(t.accession == "MS:1000484");
}

TEST_CASE("cv_mass_analyzer: FTMS with Exploris instrument -> Orbitrap")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS", "Orbitrap Exploris 480");
    CHECK(t.accession == "MS:1000484");
}

TEST_CASE("cv_mass_analyzer: FTMS with Astral instrument -> Orbitrap")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS", "Orbitrap Astral");
    CHECK(t.accession == "MS:1000484");
}

TEST_CASE("cv_mass_analyzer: FTMS with FT instrument -> FT-ICR")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS", "LTQ FT Ultra");
    CHECK(t.accession == "MS:1000079");
}

TEST_CASE("cv_mass_analyzer: FTMS without model -> FT-ICR fallback")
{
    auto t = cv_mass_analyzer("MassAnalyzerFTMS");
    CHECK(t.accession == "MS:1000079");
}

TEST_CASE("cv_mass_analyzer: ion trap (ITMS)")
{
    auto t = cv_mass_analyzer("MassAnalyzerITMS");
    CHECK(t.accession == "MS:1000264");
    CHECK(t.name == "ion trap");
}

TEST_CASE("cv_mass_analyzer: triple quadrupole (TQMS)")
{
    auto t = cv_mass_analyzer("MassAnalyzerTQMS");
    CHECK(t.accession == "MS:1000081");
}

TEST_CASE("cv_mass_analyzer: single quadrupole (SQMS)")
{
    auto t = cv_mass_analyzer("MassAnalyzerSQMS");
    CHECK(t.accession == "MS:1000081");
}

TEST_CASE("cv_mass_analyzer: TOF")
{
    auto t = cv_mass_analyzer("MassAnalyzerTOFMS");
    CHECK(t.accession == "MS:1000084");
}

TEST_CASE("cv_mass_analyzer: magnetic sector")
{
    auto t = cv_mass_analyzer("MassAnalyzerSector");
    CHECK(t.accession == "MS:1000080");
}

TEST_CASE("cv_mass_analyzer: Astral (ASTMS)")
{
    auto t = cv_mass_analyzer("MassAnalyzerASTMS");
    CHECK(t.accession == "MS:1003379");
}

TEST_CASE("cv_mass_analyzer: unknown -> generic fallback")
{
    auto t = cv_mass_analyzer("SomethingUnknown");
    CHECK(t.accession == "MS:1000443");
}

// ================================================================
//  Ionization mode -> PSI-MS accession
// ================================================================

TEST_CASE("cv_ionization_mode: ElectroSpray")
{
    auto t = cv_ionization_mode("ElectroSpray");
    CHECK(t.accession == "MS:1000073");
}

TEST_CASE("cv_ionization_mode: NanoSpray")
{
    auto t = cv_ionization_mode("NanoSpray");
    CHECK(t.accession == "MS:1000398");
}

TEST_CASE("cv_ionization_mode: APCI")
{
    auto t = cv_ionization_mode("AtmosphericPressureChemicalIonization");
    CHECK(t.accession == "MS:1000070");
}

TEST_CASE("cv_ionization_mode: MALDI")
{
    auto t = cv_ionization_mode("MatrixAssistedLaserDesorptionIonization");
    CHECK(t.accession == "MS:1000239");
}

TEST_CASE("cv_ionization_mode: Paper spray")
{
    auto t = cv_ionization_mode("PaperSprayIonization");
    CHECK(t.accession == "MS:1003235");
}

TEST_CASE("cv_ionization_mode: EI")
{
    auto t = cv_ionization_mode("ElectronImpact");
    CHECK(t.accession == "MS:1000389");
}

TEST_CASE("cv_ionization_mode: unknown -> generic fallback")
{
    auto t = cv_ionization_mode("UnknownMode");
    CHECK(t.accession == "MS:1000008");
}

// ================================================================
//  Activation / dissociation -> PSI-MS accession
// ================================================================

TEST_CASE("cv_activation_type: CID")
{
    auto t = cv_activation_type("CollisionInducedDissociation");
    CHECK(t.accession == "MS:1000133");
}

TEST_CASE("cv_activation_type: HCD")
{
    auto t = cv_activation_type("HigherEnergyCollisionalDissociation");
    CHECK(t.accession == "MS:1000422");
    CHECK(t.name == "beam-type collision-induced dissociation");
}

TEST_CASE("cv_activation_type: ETD")
{
    auto t = cv_activation_type("ElectronTransferDissociation");
    CHECK(t.accession == "MS:1000598");
}

TEST_CASE("cv_activation_type: MPD / IRMPD")
{
    auto t = cv_activation_type("MultiPhotonDissociation");
    CHECK(t.accession == "MS:1000262");
}

TEST_CASE("cv_activation_type: ECD")
{
    auto t = cv_activation_type("ElectronCaptureDissociation");
    CHECK(t.accession == "MS:1000250");
}

TEST_CASE("cv_activation_type: PQD")
{
    auto t = cv_activation_type("PQD");
    CHECK(t.accession == "MS:1000599");
}

TEST_CASE("cv_activation_type: UVPD")
{
    auto t = cv_activation_type("UltraVioletPhotoDissociation");
    CHECK(t.accession == "MS:1003246");
}

TEST_CASE("cv_activation_type: NETD")
{
    auto t = cv_activation_type("NegativeElectronTransferDissociation");
    CHECK(t.accession == "MS:1003247");
}

TEST_CASE("cv_activation_type: PTR")
{
    auto t = cv_activation_type("ProtonTransferReaction");
    CHECK(t.accession == "MS:1003249");
}

TEST_CASE("cv_activation_type: unknown -> generic fallback")
{
    auto t = cv_activation_type("SomethingElse");
    CHECK(t.accession == "MS:1000044");
}

// ================================================================
//  Detector types
// ================================================================

TEST_CASE("cv_detector_types: Orbitrap has inductive detector")
{
    auto dets = cv_detector_types("LTQ Orbitrap Velos");
    REQUIRE(!dets.empty());
    CHECK(dets[0].accession == "MS:1000624");
}

TEST_CASE("cv_detector_types: Q Exactive has inductive detector")
{
    auto dets = cv_detector_types("Q Exactive HF");
    REQUIRE(!dets.empty());
    CHECK(dets[0].accession == "MS:1000624");
}

TEST_CASE("cv_detector_types: LTQ has electron multiplier")
{
    auto dets = cv_detector_types("LTQ XL");
    REQUIRE(!dets.empty());
    CHECK(dets[0].accession == "MS:1000253");
}

TEST_CASE("cv_detector_types: TSQ has electron multiplier")
{
    auto dets = cv_detector_types("TSQ Quantum Access MAX");
    REQUIRE(!dets.empty());
    CHECK(dets[0].accession == "MS:1000253");
}

TEST_CASE("cv_detector_types: Orbitrap ID-X has two detectors")
{
    auto dets = cv_detector_types("Orbitrap ID-X");
    REQUIRE(dets.size() >= 2);
    // Should have both inductive and electron multiplier
    bool has_inductive = false;
    bool has_em = false;
    for (const auto& d : dets)
    {
        if (d.accession == "MS:1000624") has_inductive = true;
        if (d.accession == "MS:1000253") has_em = true;
    }
    CHECK(has_inductive);
    CHECK(has_em);
}

TEST_CASE("cv_detector_types: Orbitrap Astral has inductive + conversion dynode")
{
    auto dets = cv_detector_types("Orbitrap Astral");
    REQUIRE(dets.size() >= 2);
    bool has_inductive = false;
    bool has_conversion = false;
    for (const auto& d : dets)
    {
        if (d.accession == "MS:1000624") has_inductive = true;
        if (d.accession == "MS:1000108") has_conversion = true;
    }
    CHECK(has_inductive);
    CHECK(has_conversion);
}

TEST_CASE("cv_detector_types: DeltaPlus has faraday cup")
{
    auto dets = cv_detector_types("DeltaPlus Advantage");
    REQUIRE(!dets.empty());
    bool has_faraday = false;
    for (const auto& d : dets)
        if (d.accession == "MS:1000112") has_faraday = true;
    CHECK(has_faraday);
}

TEST_CASE("cv_detector_types: unknown instrument -> empty")
{
    auto dets = cv_detector_types("Unknown Instrument Model 9000");
    CHECK(dets.empty());
}

// ================================================================
//  Convenience functions
// ================================================================

TEST_CASE("cv_polarity: positive")
{
    auto t = cv_polarity(1);
    CHECK(t.accession == "MS:1000130");
    CHECK(t.name == "positive scan");
}

TEST_CASE("cv_polarity: negative")
{
    auto t = cv_polarity(0);
    CHECK(t.accession == "MS:1000129");
    CHECK(t.name == "negative scan");
}

TEST_CASE("cv_spectrum_type: MS1")
{
    auto t = cv_spectrum_type(1);
    CHECK(t.accession == "MS:1000579");
    CHECK(t.name == "MS1 spectrum");
}

TEST_CASE("cv_spectrum_type: MS2")
{
    auto t = cv_spectrum_type(2);
    CHECK(t.accession == "MS:1000580");
    CHECK(t.name == "MSn spectrum");
}

TEST_CASE("cv_spectrum_type: MS3 also returns MSn")
{
    auto t = cv_spectrum_type(3);
    CHECK(t.accession == "MS:1000580");
}

TEST_CASE("cv_spectrum_representation: centroid")
{
    auto t = cv_spectrum_representation(true);
    CHECK(t.accession == "MS:1000127");
    CHECK(t.name == "centroid spectrum");
}

TEST_CASE("cv_spectrum_representation: profile")
{
    auto t = cv_spectrum_representation(false);
    CHECK(t.accession == "MS:1000128");
    CHECK(t.name == "profile spectrum");
}

// ================================================================
//  CV constant accessibility
// ================================================================

TEST_CASE("CV constants are accessible")
{
    CHECK(std::string(cv::ms1_spectrum) == "MS:1000579");
    CHECK(std::string(cv::msn_spectrum) == "MS:1000580");
    CHECK(std::string(cv::centroid_spectrum) == "MS:1000127");
    CHECK(std::string(cv::profile_spectrum) == "MS:1000128");
    CHECK(std::string(cv::positive_scan) == "MS:1000130");
    CHECK(std::string(cv::negative_scan) == "MS:1000129");
    CHECK(std::string(cv::ms_level) == "MS:1000511");
    CHECK(std::string(cv::scan_start_time) == "MS:1000016");
    CHECK(std::string(cv::filter_string) == "MS:1000512");
    CHECK(std::string(cv::total_ion_current) == "MS:1000285");
    CHECK(std::string(cv::base_peak_mz) == "MS:1000504");
    CHECK(std::string(cv::base_peak_intensity) == "MS:1000505");
    CHECK(std::string(cv::selected_ion_mz) == "MS:1000744");
    CHECK(std::string(cv::charge_state) == "MS:1000041");
    CHECK(std::string(cv::collision_energy) == "MS:1000045");
    CHECK(std::string(cv::mz_array) == "MS:1000514");
    CHECK(std::string(cv::intensity_array) == "MS:1000515");
    CHECK(std::string(cv::charge_array) == "MS:1000516");
    CHECK(std::string(cv::tic_chromatogram) == "MS:1000235");
    CHECK(std::string(cv::thermo_raw_format) == "MS:1000563");
    CHECK(std::string(cv::thermo_nativeid_format) == "MS:1000768");
    CHECK(std::string(cv::isolation_window_target) == "MS:1000827");
    CHECK(std::string(cv::isolation_window_lower) == "MS:1000828");
    CHECK(std::string(cv::isolation_window_upper) == "MS:1000829");
}

TEST_CASE("Unit ontology constants are accessible")
{
    CHECK(std::string(cv::uo_minute) == "UO:0000031");
    CHECK(std::string(cv::uo_millisecond) == "UO:0000028");
    CHECK(std::string(cv::uo_electronvolt) == "UO:0000266");
    CHECK(std::string(cv::uo_mz) == "UO:0000040");
}

TEST_CASE("Metadata constants are accessible")
{
    CHECK(std::string(cv::ncit_pathname) == "NCIT:C47922");
    CHECK(std::string(cv::ncit_creation_date) == "NCIT:C69199");
    CHECK(std::string(cv::pride_num_ms1) == "PRIDE:0000481");
    CHECK(std::string(cv::pride_num_ms2) == "PRIDE:0000482");
    CHECK(std::string(cv::sample_name) == "MS:1000002");
    CHECK(std::string(cv::instrument_serial_number) == "MS:1000529");
}

TEST_CASE("cv_polarity: unspecified does not fabricate positive polarity")
{
    CHECK(cv_polarity(2).accession.empty());
    CHECK(cv_polarity(-1).accession.empty());
}
