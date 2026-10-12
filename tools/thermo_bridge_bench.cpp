// Benchmark and parity dump for the Thermo bridge (local use, not run in CI).
//
//   thermo_bridge_bench <file.raw> [--threads N] [--batch B] [--dump out.jsonl]
//
// Prints wall time per scan for the legacy per-call path and the single-pass
// read_scan / read_scans / thread_accessor paths, and checks that batched and
// concurrent reads give the same output as sequential reads. --dump writes one
// JSON line per scan (peak digests, typed header, typed trailers, legacy JSON)
// for tools/compare_parity.py.
#include <openms_thermo_bridge/thermo_bridge.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace openms::thermo_bridge;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;

std::uint64_t fnv(const void* data, std::size_t bytes, std::uint64_t h = kFnvOffset)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

std::uint64_t fnv(const std::vector<double>& v, std::uint64_t h = kFnvOffset)
{
    return fnv(v.data(), v.size() * sizeof(double), h);
}

std::string hex(std::uint64_t h)
{
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

std::string num(double v)
{
    if (!std::isfinite(v)) return "null";
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

std::string quote(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20)
        {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

/// Same format as the v0.3.1 baseline dump: h = returned order, hs = stable m/z order.
std::string digest(const std::vector<double>& mz, const std::vector<double>& in)
{
    const bool sorted = std::is_sorted(mz.begin(), mz.end());
    std::vector<std::size_t> idx(mz.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return mz[a] < mz[b]; });
    std::vector<double> smz, sint;
    for (auto i : idx) { smz.push_back(mz[i]); sint.push_back(in[i]); }
    std::ostringstream o;
    o << "{\"n\":" << mz.size() << ",\"h\":\"" << hex(fnv(in, fnv(mz))) << "\",\"hs\":\"" << hex(fnv(sint, fnv(smz)))
      << "\",\"sorted\":" << (sorted ? "true" : "false") << "}";
    return o.str();
}

std::string array_digest(const std::vector<double>& v)
{
    return "{\"n\":" + std::to_string(v.size()) + ",\"h\":\"" + hex(fnv(v)) + "\"}";
}

/// Digest of everything a reader consumes, to compare batched/threaded vs sequential output.
std::uint64_t scan_digest(const ScanHeader& h, const double* mz, const double* in, std::size_t n)
{
    std::uint64_t d = fnv(mz, n * sizeof(double));
    d = fnv(in, n * sizeof(double), d);
    const double scalars[] = {h.retention_time, h.tic, h.base_peak_mass, h.base_peak_intensity, h.low_mass, h.high_mass};
    d = fnv(scalars, sizeof scalars, d);
    const std::int32_t ints[] = {h.scan_number, static_cast<std::int32_t>(h.ms_order),
        static_cast<std::int32_t>(h.polarity), h.analyzer, h.ionization, h.centroid ? 1 : 0};
    d = fnv(ints, sizeof ints, d);
    d = fnv(h.filter.data(), h.filter.size(), d);
    for (const auto& r : h.reactions)
    {
        const double rv[] = {r.precursor_mass, r.isolation_width, r.collision_energy};
        d = fnv(rv, sizeof rv, d);
    }
    d = fnv(h.trailer_doubles.data(), h.trailer_doubles.size() * sizeof(double), d);
    d = fnv(h.trailer_ints.data(), h.trailer_ints.size() * sizeof(std::int64_t), d);
    d = fnv(h.trailer_bools.data(), h.trailer_bools.size(), d);
    for (const auto& s : h.trailer_strings) d = fnv(s.data(), s.size() + 1, d);
    return d;
}

std::string header_json(const ScanHeader& h)
{
    std::ostringstream o;
    o << "{\"scan_number\":" << h.scan_number << ",\"controller_type\":" << h.controller_type
      << ",\"controller_number\":" << h.controller_number << ",\"native_id\":" << quote(h.native_id())
      << ",\"retention_time\":" << num(h.retention_time) << ",\"ms_level\":" << h.ms_level()
      << ",\"ms_order_name\":" << quote(ms_order_name(h.ms_order)) << ",\"centroid\":" << (h.centroid ? "true" : "false")
      << ",\"polarity\":" << static_cast<int>(h.polarity) << ",\"filter\":" << quote(h.filter)
      << ",\"analyzer\":" << quote(h.analyzer < 0 ? "" : mass_analyzer_name(h.analyzer))
      << ",\"ionization\":" << quote(h.ionization < 0 ? "" : ionization_mode_name(h.ionization))
      << ",\"low_mass\":" << num(h.low_mass) << ",\"high_mass\":" << num(h.high_mass)
      << ",\"low_wavelength\":" << num(h.low_wavelength) << ",\"high_wavelength\":" << num(h.high_wavelength)
      << ",\"tic\":" << num(h.tic) << ",\"base_peak_mass\":" << num(h.base_peak_mass)
      << ",\"base_peak_intensity\":" << num(h.base_peak_intensity) << ",\"peak_source\":" << static_cast<int>(h.peak_source)
      << ",\"reactions\":[";
    for (std::size_t i = 0; i < h.reactions.size(); ++i)
    {
        const auto& r = h.reactions[i];
        o << (i ? "," : "") << "{\"index\":" << i << ",\"precursor_mass\":" << num(r.precursor_mass)
          << ",\"isolation_width\":" << num(r.isolation_width) << ",\"isolation_offset\":" << num(r.isolation_offset)
          << ",\"activation\":" << quote(activation_type_name(r.activation))
          << ",\"collision_energy\":" << num(r.collision_energy)
          << ",\"collision_energy_valid\":" << (r.collision_energy_valid ? "true" : "false") << "}";
    }
    o << "]}";
    return o.str();
}

std::string trailers_json(const ScanHeader& h, const TrailerLayout& layout)
{
    std::ostringstream o;
    o << "[";
    for (std::size_t i = 0; i < layout.labels.size(); ++i)
    {
        const int idx = static_cast<int>(i);
        o << (i ? "," : "");
        switch (layout.labels[i].kind)
        {
        case TrailerKind::Double: o << num(h.trailer_double(layout, idx)); break;
        case TrailerKind::Int: o << h.trailer_int(layout, idx); break;
        case TrailerKind::Bool: o << (h.trailer_bool(layout, idx) ? "true" : "false"); break;
        default: o << quote(h.trailer_string(layout, idx)); break;
        }
    }
    o << "]";
    return o.str();
}

void dump(const RawFile& raw, int first, int last, const std::string& path)
{
    std::ofstream out(path);
    const auto& layout = raw.trailer_layout();
    const auto tic = raw.chromatogram_data();
    out << "{\"type\":\"file\",\"first\":" << first << ",\"last\":" << last << ",\"scan_count\":" << raw.scan_count()
        << ",\"sha1\":" << quote(raw.file_sha1()) << ",\"meta\":" << raw.file_metadata_json(true)
        << ",\"tic\":{\"n\":" << tic.times.size() << ",\"h\":\"" << hex(fnv(tic.intensities, fnv(tic.times))) << "\"}"
        << ",\"layout\":[";
    for (std::size_t i = 0; i < layout.labels.size(); ++i)
        out << (i ? "," : "") << "{\"label\":" << quote(layout.labels[i].label)
            << ",\"kind\":" << static_cast<int>(layout.labels[i].kind) << ",\"vendor_type\":" << layout.labels[i].vendor_type << "}";
    out << "]}\n";

    ReadOptions centroid_opts;
    centroid_opts.trailer_text = true;
    ReadOptions profile_opts;
    profile_opts.centroid = false;
    profile_opts.trailers = false;
    ReadOptions aux_opts;
    aux_opts.trailers = false;
    aux_opts.charges = true;
    aux_opts.noise = true;
    Spectrum c, p, a;
    for (int s = first; s <= last; ++s)
    {
        raw.read_scan(s, c, centroid_opts);
        raw.read_scan(s, p, profile_opts);
        raw.read_scan(s, a, aux_opts);
        out << "{\"type\":\"scan\",\"scan\":" << s << ",\"header\":" << header_json(c.header)
            << ",\"trailers\":" << trailers_json(c.header, layout) << ",\"trailer_text\":[";
        for (std::size_t i = 0; i < c.header.trailer_text.size(); ++i)
            out << (i ? "," : "") << quote(c.header.trailer_text[i]);
        out << "],\"legacy_meta\":" << raw.scan_metadata_json(s) << ",\"c\":" << digest(c.mz, c.intensities)
            << ",\"p\":" << digest(p.mz, p.intensities) << ",\"aux_c\":" << digest(a.mz, a.intensities)
            << ",\"aux0\":" << array_digest(a.charges) << ",\"aux1\":" << array_digest(a.noise_mz)
            << ",\"aux2\":" << array_digest(a.noise_intensities) << ",\"aux3\":" << array_digest(a.noise_baselines) << "}\n";
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: thermo_bridge_bench file.raw [--threads N] [--batch B] [--dump out.jsonl]\n";
        return 2;
    }
    unsigned threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    int batch_size = 256;
    std::string dump_path;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string opt = argv[i];
        if (opt == "--threads") threads = static_cast<unsigned>(std::max(1, std::atoi(argv[i + 1])));
        else if (opt == "--batch") batch_size = std::max(1, std::atoi(argv[i + 1]));
        else if (opt == "--dump") dump_path = argv[i + 1];
        else { std::cerr << "unknown option " << opt << "\n"; return 2; }
    }

    try
    {
        const auto t_open0 = Clock::now();
        RawFile raw(argv[1]);
        const auto t_open1 = Clock::now();
        const int first = raw.first_scan_number();
        const int last = raw.last_scan_number();
        const auto n = static_cast<std::size_t>(last - first + 1);
        std::cout << argv[1] << ": " << n << " scans, open " << secs(t_open0, t_open1) << " s\n";

        // Warm up so the first timed loop is not penalised by JIT and vendor caches.
        for (int s = first; s <= std::min(last, first + 20); ++s) { raw.read_scan(s); raw.scan_metadata_json(s); }

        auto report = [&](const std::string& name, double seconds) {
            std::printf("  %-46s %8.3f s  %8.2f us/scan\n", name.c_str(), seconds, seconds * 1e6 / static_cast<double>(n));
        };

        // Legacy path as used by OpenMS with v0.3: JSON metadata + centroided peaks.
        auto t0 = Clock::now();
        std::size_t peaks = 0;
        for (int s = first; s <= last; ++s)
        {
            const auto meta = raw.scan_metadata_json(s);
            peaks += raw.spectrum_data(s, true).mz.size() + (meta.empty() ? 1 : 0);
        }
        report("legacy scan_metadata_json + spectrum_data", secs(t0, Clock::now()));

        // Single pass: typed header + typed trailers + centroided peaks.
        std::vector<std::uint64_t> sequential(n);
        Spectrum spectrum;
        t0 = Clock::now();
        for (int s = first; s <= last; ++s)
        {
            raw.read_scan(s, spectrum);
            sequential[static_cast<std::size_t>(s - first)] =
                scan_digest(spectrum.header, spectrum.mz.data(), spectrum.intensities.data(), spectrum.mz.size());
        }
        report("read_scan (header+trailers+centroid peaks)", secs(t0, Clock::now()));

        ReadOptions no_trailers;
        no_trailers.trailers = false;
        t0 = Clock::now();
        for (int s = first; s <= last; ++s) raw.read_scan(s, spectrum, no_trailers);
        report("read_scan (header+centroid peaks)", secs(t0, Clock::now()));

        ReadOptions header_only;
        header_only.peaks = false;
        t0 = Clock::now();
        for (int s = first; s <= last; ++s) raw.read_scan(s, spectrum, header_only);
        report("read_scan (header+trailers, no peaks)", secs(t0, Clock::now()));

        ScanBatch batch;
        std::size_t mismatches = 0;
        t0 = Clock::now();
        for (int s = first; s <= last; s += batch_size)
        {
            raw.read_scans(s, std::min(last, s + batch_size - 1), batch);
            for (std::size_t i = 0; i < batch.size(); ++i)
            {
                const auto b = batch.peak_offsets[i];
                const auto d = scan_digest(batch.headers[i], batch.mz.data() + b, batch.intensities.data() + b,
                    batch.peak_count(i));
                if (d != sequential[static_cast<std::size_t>(batch.headers[i].scan_number - first)]) ++mismatches;
            }
        }
        report("read_scans batch=" + std::to_string(batch_size), secs(t0, Clock::now()));

        // One thread accessor per thread; threads pull blocks of scans.
        std::vector<std::uint64_t> concurrent(n, 0);
        std::atomic<int> errors{0};
        std::atomic<int> next{first};
        t0 = Clock::now();
        {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t)
            {
                pool.emplace_back([&] {
                    try
                    {
                        RawFile accessor = raw.thread_accessor();
                        ScanBatch local;
                        for (int s = next.fetch_add(batch_size); s <= last; s = next.fetch_add(batch_size))
                        {
                            accessor.read_scans(s, std::min(last, s + batch_size - 1), local);
                            for (std::size_t i = 0; i < local.size(); ++i)
                            {
                                const auto b = local.peak_offsets[i];
                                concurrent[static_cast<std::size_t>(local.headers[i].scan_number - first)] = scan_digest(
                                    local.headers[i], local.mz.data() + b, local.intensities.data() + b, local.peak_count(i));
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cerr << "thread error: " << e.what() << "\n";
                        ++errors;
                    }
                });
            }
            for (auto& th : pool) th.join();
        }
        report(std::to_string(threads) + " thread_accessors, read_scans", secs(t0, Clock::now()));
        for (std::size_t i = 0; i < n; ++i)
            if (concurrent[i] != sequential[i]) ++mismatches;

        ReadOptions profile;
        profile.centroid = false;
        profile.trailers = false;
        t0 = Clock::now();
        for (int s = first; s <= last; ++s) raw.read_scan(s, spectrum, profile);
        report("read_scan profile (no trailers)", secs(t0, Clock::now()));

        t0 = Clock::now();
        const auto tic = raw.chromatogram_data();
        std::printf("  %-46s %8.3f s  (%zu points)\n", "chromatogram_data (TIC)", secs(t0, Clock::now()), tic.times.size());
        t0 = Clock::now();
        const auto sha1 = raw.file_sha1();
        std::printf("  %-46s %8.3f s  %s\n", "file_sha1", secs(t0, Clock::now()), sha1.c_str());
        std::printf("  batch/thread vs sequential mismatches: %zu, thread errors: %d, peaks: %zu\n",
            mismatches, errors.load(), peaks);

        if (!dump_path.empty())
        {
            t0 = Clock::now();
            dump(raw, first, last, dump_path);
            std::printf("  dump written to %s (%.1f s)\n", dump_path.c_str(), secs(t0, Clock::now()));
        }
        return (mismatches == 0 && errors == 0) ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
