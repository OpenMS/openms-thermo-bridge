#include <iomanip>
#include <iostream>
#include <string>

#include <openms_thermo_bridge/thermo_bridge.hpp>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: thermo_host <path/to/file.raw> [--summary | --metadata]\n";
        return 1;
    }

    const bool summary = (argc >= 3 && std::string(argv[2]) == "--summary");

    try
    {
        if (argc >= 3 && std::string(argv[2]) == "--metadata")
        {
            openms::thermo_bridge::RawFile raw(argv[1]);
            std::cout << raw.file_metadata_json(true, true) << '\n';
            for (int scan = raw.first_scan_number(); scan <= raw.last_scan_number(); ++scan)
                std::cout << raw.scan_metadata_json(scan) << '\n';
            std::cout << raw.detector_chromatograms_json() << '\n';
            return 0;
        }
        if (!summary)
        {
            // Legacy behaviour: just print scan count.
            const int scan_count = openms::thermo_bridge::get_scan_count(argv[1]);
            std::cout << "Scan count: " << scan_count << '\n';
            return 0;
        }

        // Extended summary using the RawFile class.
        openms::thermo_bridge::RawFile raw(argv[1]);

        std::cout << "=== Run header ===\n";
        std::cout << "  First scan:   " << raw.first_scan_number() << '\n';
        std::cout << "  Last scan:    " << raw.last_scan_number() << '\n';
        std::cout << "  Scan count:   " << raw.scan_count() << '\n';
        std::cout << "  Start time:   " << raw.start_time() << " min\n";
        std::cout << "  End time:     " << raw.end_time() << " min\n";
        std::cout << "  Low mass:     " << raw.low_mass() << '\n';
        std::cout << "  High mass:    " << raw.high_mass() << '\n';
        std::cout << "  Has MS data:  " << (raw.has_ms_data() ? "yes" : "no") << '\n';

        std::cout << "\n=== Instrument ===\n";
        std::cout << "  Model:    " << raw.instrument_model() << '\n';
        std::cout << "  Name:     " << raw.instrument_name() << '\n';
        std::cout << "  Serial:   " << raw.instrument_serial_number() << '\n';
        std::cout << "  Software: " << raw.instrument_software_version() << '\n';

        std::cout << "\n=== File header ===\n";
        std::cout << "  Created:      " << raw.creation_date() << '\n';
        std::cout << "  Description:  " << raw.file_description() << '\n';
        std::cout << "  Revision:     " << raw.file_revision() << '\n';
        std::cout << "  Sample name:  " << raw.sample_name() << '\n';

        const int first = raw.first_scan_number();
        const int last = raw.last_scan_number();
        const int display_max = 5;

        std::cout << "\n=== First " << std::min(display_max, last - first + 1) << " scans ===\n";
        for (int s = first; s <= std::min(first + display_max - 1, last); ++s)
        {
            std::cout << "  Scan " << s << ": MS" << raw.ms_level(s)
                      << " RT=" << std::fixed << std::setprecision(4) << raw.retention_time(s) << " min"
                      << " centroid=" << (raw.is_centroid_scan(s) ? "yes" : "no")
                      << " peaks=" << raw.spectrum_peak_count(s)
                      << " TIC=" << std::scientific << raw.tic(s)
                      << " filter=" << raw.scan_filter(s) << '\n';
        }

        std::cout << "\n=== TIC chromatogram ===\n";
        auto chrom = raw.chromatogram_data();
        std::cout << "  Points: " << chrom.times.size() << '\n';
        if (!chrom.times.empty())
        {
            std::cout << "  First:  RT=" << std::fixed << std::setprecision(4) << chrom.times.front()
                      << " int=" << std::scientific << chrom.intensities.front() << '\n';
            std::cout << "  Last:   RT=" << std::fixed << std::setprecision(4) << chrom.times.back()
                      << " int=" << std::scientific << chrom.intensities.back() << '\n';
        }

        return 0;
    }
    catch (const openms::thermo_bridge::bridge_error& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
