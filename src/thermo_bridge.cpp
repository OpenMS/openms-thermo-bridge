#include <openms_thermo_bridge/thermo_bridge.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define OPENMS_THERMO_BRIDGE_CHAR_T_LITERAL(value) L##value
#else
#define OPENMS_THERMO_BRIDGE_CHAR_T_LITERAL(value) value
#endif

// ================================================================
//  Managed function-pointer typedefs
// ================================================================
using hostfxr_initialize_for_runtime_config_fn =
    int(*)(const char_t*, const hostfxr_initialize_parameters*, hostfxr_handle*);
using hostfxr_get_runtime_delegate_fn =
    int(*)(hostfxr_handle, hostfxr_delegate_type, void**);
using hostfxr_close_fn = int(*)(hostfxr_handle);

// C# entry-point signatures (match [UnmanagedCallersOnly] in ThermoWrapperManaged.cs)
using fn_int_from_ptr         = int(*)(void*);                        // GetScanCount(IntPtr)
using fn_int_from_int         = int(*)(int);                          // H_Get*(handle)
using fn_double_from_int      = double(*)(int);                       // H_GetStartTime(handle)
using fn_int_from_int_int     = int(*)(int, int);                     // H_GetMSLevel(handle, scan)
using fn_double_from_int_int  = double(*)(int, int);                  // H_GetRetentionTime(handle, scan)
using fn_str_from_int         = int(*)(int, void*, int);              // H_GetInstrumentModel(handle, buf, size)
using fn_str_from_int_int     = int(*)(int, int, void*, int);         // H_GetScanFilter(handle, scan, buf, size)
using fn_spectrum_count       = int(*)(int, int, int);                // H_GetSpectrumPeakCount(handle, scan, centroid)
using fn_spectrum_data        = int(*)(int, int, int, void*, void*, int); // H_GetSpectrumData(handle, scan, centroid, mzBuf, intBuf, sz)
using fn_chrom_len            = int(*)(int);                          // H_GetChromatogramLength(handle)
using fn_chrom_data           = int(*)(int, void*, void*, int);       // H_GetChromatogramData(handle, rtBuf, intBuf, sz)
using fn_xic_len              = int(*)(int, double, double, double, double); // H_GetXicLength(handle, mzS, mzE, rtS, rtE)
using fn_xic_data             = int(*)(int, double, double, double, double, void*, void*, int); // H_GetXicData(...)
using fn_trailer_labels       = int(*)(int, int, void*, int);         // H_GetTrailerExtraLabels(handle, scan, buf, sz)
using fn_trailer_value        = int(*)(int, int, void*, void*, int);  // H_GetTrailerExtraValue(handle, scan, key, buf, sz)
using fn_method               = int(*)(int, int, void*, int);         // H_GetInstrumentMethod(handle, idx, buf, sz)

namespace openms::thermo_bridge
{
namespace
{
// ================================================================
//  Platform helpers (unchanged from original)
// ================================================================

void module_path_anchor()
{
}

#if defined(_WIN32)
using library_handle = HMODULE;
#else
using library_handle = void*;
#endif

std::filesystem::path executable_path()
{
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw bridge_error("Unable to resolve executable path");
        if (length < buffer.size() - 1)
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::vector<char> buffer(1024);
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            throw bridge_error("Unable to resolve executable path");
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()));
#elif defined(__linux__)
    std::array<char, PATH_MAX> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) throw bridge_error("Unable to resolve executable path");
    buffer[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data());
#else
    throw bridge_error("Executable path resolution is not implemented on this platform");
#endif
}

std::filesystem::path module_path()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_path_anchor), &module)
        || module == nullptr)
    {
        throw bridge_error("Unable to resolve module path");
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    while (true)
    {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw bridge_error("Unable to resolve module path");
        if (length < buffer.size() - 1)
            return std::filesystem::path(buffer.data(), buffer.data() + length);
        buffer.resize(buffer.size() * 2);
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&module_path_anchor), &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0')
        throw bridge_error("Unable to resolve module path");
    return std::filesystem::weakly_canonical(std::filesystem::path(info.dli_fname));
#endif
}

bool try_module_path(std::filesystem::path& result)
{
    try
    {
        result = module_path();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

#if defined(_WIN32)
std::string last_error_message()
{
    const DWORD error_code = GetLastError();
    if (error_code == 0) return "unknown error";
    LPSTR message_buffer = nullptr;
    const DWORD message_length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code, 0, reinterpret_cast<LPSTR>(&message_buffer), 0, nullptr);
    std::string message = message_length > 0 ? std::string(message_buffer, message_length) : "unknown error";
    if (message_buffer != nullptr) LocalFree(message_buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message;
}
#endif

library_handle load_library(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const HMODULE handle = LoadLibraryW(path.c_str());
    if (handle == nullptr) throw bridge_error("LoadLibraryW failed: " + last_error_message());
    return handle;
#else
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) throw bridge_error(std::string("dlopen failed: ") + dlerror());
    return handle;
#endif
}

void* get_export(library_handle library, const char* export_name)
{
#if defined(_WIN32)
    void* symbol = reinterpret_cast<void*>(GetProcAddress(library, export_name));
    if (symbol == nullptr) throw bridge_error(std::string("GetProcAddress failed for ") + export_name + ": " + last_error_message());
    return symbol;
#else
    dlerror();
    void* symbol = dlsym(library, export_name);
    const char* error = dlerror();
    if (error != nullptr) throw bridge_error(std::string("dlsym failed for ") + export_name + ": " + error);
    return symbol;
#endif
}

std::filesystem::path hostfxr_path()
{
    std::array<char_t, 4096> buffer{};
    std::size_t buffer_size = buffer.size();
    if (get_hostfxr_path(buffer.data(), &buffer_size, nullptr) != 0)
        throw bridge_error("get_hostfxr_path failed");
    return std::filesystem::path(buffer.data());
}

std::filesystem::path discover_dotnet_root()
{
    const char* dotnet_root_env = std::getenv("DOTNET_ROOT");
    if (dotnet_root_env != nullptr && dotnet_root_env[0] != '\0')
    {
        std::filesystem::path root(dotnet_root_env);
        if (std::filesystem::is_directory(root)) return root;
    }
    try
    {
        const auto fxr = hostfxr_path();
        auto candidate = fxr.parent_path().parent_path().parent_path().parent_path();
        if (std::filesystem::is_directory(candidate / "shared")) return candidate;
    }
    catch (...) {}
    return {};
}

#if defined(_WIN32)
void HOSTFXR_CALLTYPE hostfxr_error_writer_callback(const char_t* message)
{
    std::wcerr << L"[hostfxr] " << message << L'\n';
}
#else
void HOSTFXR_CALLTYPE hostfxr_error_writer_callback(const char_t* message)
{
    std::cerr << "[hostfxr] " << message << '\n';
}
#endif

std::basic_string<char_t> to_char_t_path(const std::filesystem::path& path)
{
#if defined(_WIN32)
    return path.native();
#else
    return path.string();
#endif
}

// ================================================================
//  CLR Runtime singleton – initialises once, caches delegates
// ================================================================
class Runtime
{
public:
    static Runtime& instance()
    {
        static Runtime rt;
        return rt;
    }

    /// Ensure the runtime is initialised for the given managed_directory.
    void ensure_init(const std::filesystem::path& managed_directory)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialised_) return;

        managed_directory_ = managed_directory;
        runtime_config_ = managed_directory / "ThermoWrapperManaged.runtimeconfig.json";
        assembly_path_  = managed_directory / "ThermoWrapperManaged.dll";

        if (!std::filesystem::exists(runtime_config_) || !std::filesystem::exists(assembly_path_))
        {
            throw bridge_error("Managed bridge runtime files are missing (managed_dir=" + managed_directory.string()
                + ", runtime_config_exists=" + (std::filesystem::exists(runtime_config_) ? "yes" : "no")
                + ", assembly_exists=" + (std::filesystem::exists(assembly_path_) ? "yes" : "no") + ")");
        }

        auto fxr_path = hostfxr_path();
        auto hostfxr_library = load_library(fxr_path);

        auto hostfxr_initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
            get_export(hostfxr_library, "hostfxr_initialize_for_runtime_config"));
        auto hostfxr_get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
            get_export(hostfxr_library, "hostfxr_get_runtime_delegate"));
        auto hostfxr_close = reinterpret_cast<hostfxr_close_fn>(
            get_export(hostfxr_library, "hostfxr_close"));

        try
        {
            auto set_error_writer = reinterpret_cast<hostfxr_set_error_writer_fn>(
                get_export(hostfxr_library, "hostfxr_set_error_writer"));
            if (set_error_writer != nullptr) set_error_writer(&hostfxr_error_writer_callback);
        }
        catch (...) {}

        const auto dotnet_root = discover_dotnet_root();
        const auto dotnet_root_native = to_char_t_path(dotnet_root);
        const auto host_path_native = to_char_t_path(executable_path());

        hostfxr_initialize_parameters init_params{};
        init_params.size = sizeof(init_params);
        init_params.host_path = host_path_native.c_str();
        if (!dotnet_root.empty()) init_params.dotnet_root = dotnet_root_native.c_str();

        hostfxr_handle context = nullptr;
        const auto runtime_config_native = to_char_t_path(runtime_config_);
        int rc = hostfxr_initialize(runtime_config_native.c_str(), &init_params, &context);
        if (rc < 0) throw bridge_error("hostfxr_initialize_for_runtime_config failed with code " + std::to_string(rc));
        if (context == nullptr)
            throw bridge_error("hostfxr_initialize_for_runtime_config returned null context (code " + std::to_string(rc) + ")");

        rc = hostfxr_get_delegate(context, hdt_load_assembly_and_get_function_pointer,
                                  reinterpret_cast<void**>(&load_assembly_));
        hostfxr_close(context);
        if (rc != 0 || load_assembly_ == nullptr)
            throw bridge_error("hostfxr_get_runtime_delegate failed with code " + std::to_string(rc));

        initialised_ = true;
    }

    /// Resolve a C# entry-point and return its function pointer (cached).
    template <typename FnType>
    FnType resolve(const char* entry_point_name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialised_) throw bridge_error("Runtime not initialised");

        auto it = fn_cache_.find(entry_point_name);
        if (it != fn_cache_.end()) return reinterpret_cast<FnType>(it->second);

        const auto assembly_path_native = to_char_t_path(assembly_path_);

        // Convert entry_point_name to char_t string at runtime.
        // Entry point names are C# method identifiers, guaranteed ASCII.
#if defined(_WIN32)
        std::wstring entry_wide;
        for (const char* p = entry_point_name; *p; ++p)
            entry_wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        const char_t* entry_native = entry_wide.c_str();
#else
        const char_t* entry_native = entry_point_name;
#endif

        void* fn = nullptr;
        int rc = load_assembly_(
            assembly_path_native.c_str(),
            OPENMS_THERMO_BRIDGE_CHAR_T_LITERAL("ThermoWrapperManaged.RawBridge, ThermoWrapperManaged"),
            entry_native,
            UNMANAGEDCALLERSONLY_METHOD,
            nullptr,
            &fn);
        if (rc != 0 || fn == nullptr)
            throw bridge_error(std::string("Failed to resolve managed entry point '") + entry_point_name + "' (code " + std::to_string(rc) + ")");

        fn_cache_[entry_point_name] = fn;
        return reinterpret_cast<FnType>(fn);
    }

private:
    Runtime() = default;

    std::mutex mutex_;
    bool initialised_ = false;
    std::filesystem::path managed_directory_;
    std::filesystem::path runtime_config_;
    std::filesystem::path assembly_path_;
    load_assembly_and_get_function_pointer_fn load_assembly_ = nullptr;
    std::unordered_map<std::string, void*> fn_cache_;
};

// ================================================================
//  Internal helpers
// ================================================================

void check_int_result(int result, const char* context)
{
    if (result == -1) throw bridge_error(std::string(context) + ": file not found");
    if (result == -2) throw bridge_error(std::string(context) + ": managed exception");
    if (result == -3) throw bridge_error(std::string(context) + ": platform not supported");
    if (result == -4) throw bridge_error(std::string(context) + ": invalid file handle");
    if (result == -5) throw bridge_error(std::string(context) + ": invalid scan number");
    if (result < 0)   throw bridge_error(std::string(context) + ": unknown error code " + std::to_string(result));
}

void check_double_result(double result, const char* context)
{
    if (std::isnan(result)) throw bridge_error(std::string(context) + ": managed exception");
    if (std::isinf(result) && result < 0) throw bridge_error(std::string(context) + ": invalid file handle");
}

std::vector<char> make_utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    std::vector<char> v(s.begin(), s.end());
    v.push_back('\0');
    return v;
}

std::vector<char> make_utf8(const std::string& s)
{
    std::vector<char> v(s.begin(), s.end());
    v.push_back('\0');
    return v;
}

/// Call a string-returning entry point that writes into a caller buffer.
/// The C# function signature is: int Fn(int handle, ..., IntPtr buf, int bufSize)
/// It returns the byte count needed (excl. NUL).
template <typename FnType, typename... ExtraArgs>
std::string call_string_fn(FnType fn, int handle, ExtraArgs... extra)
{
    // First call: discover size needed.
    int needed = fn(handle, extra..., nullptr, 0);
    if (needed <= 0) return {};  // empty or error
    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    fn(handle, extra..., buf.data(), static_cast<int>(buf.size()));
    return std::string(buf.data());
}

} // anonymous namespace

// ================================================================
//  Legacy free-functions
// ================================================================

std::filesystem::path default_managed_directory()
{
    std::vector<std::filesystem::path> candidates;
    const auto exe_dir = executable_path().parent_path();
    candidates.push_back(exe_dir / "managed");

    std::filesystem::path module;
    if (try_module_path(module))
    {
        const auto module_dir = module.parent_path();
        candidates.push_back(module_dir / "managed");
        candidates.push_back(module_dir / "openms_thermo_bridge" / "managed");
    }

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate / "ThermoWrapperManaged.runtimeconfig.json")
            && std::filesystem::exists(candidate / "ThermoWrapperManaged.dll"))
        {
            return candidate;
        }
    }

    return candidates.front();
}

int get_scan_count(const std::filesystem::path& raw_file_path)
{
    try
    {
        return get_scan_count(raw_file_path, default_managed_directory());
    }
    catch (const bridge_error&) { throw; }
    catch (const std::exception& e) { throw bridge_error(std::string("Thermo bridge internal error: ") + e.what()); }
    catch (...) { throw bridge_error("Thermo bridge failed with an unknown internal error"); }
}

int get_scan_count(const std::filesystem::path& raw_file_path, const std::filesystem::path& managed_directory)
{
    auto& rt = Runtime::instance();
    rt.ensure_init(managed_directory);

    auto fn = rt.resolve<fn_int_from_ptr>("GetScanCount");
    auto utf8 = make_utf8(raw_file_path);
    int result = fn(utf8.data());

    if (result == -1) throw bridge_error("Could not open RAW file");
    if (result == -3) throw bridge_error("Thermo RawFileReader is not supported on this platform");
    if (result < 0)   throw bridge_error("Managed bridge failed while reading RAW file (code " + std::to_string(result) + ")");
    return result;
}

// ================================================================
//  RawFile implementation
// ================================================================

RawFile::RawFile(const std::filesystem::path& raw_file_path)
    : RawFile(raw_file_path, default_managed_directory())
{
}

RawFile::RawFile(const std::filesystem::path& raw_file_path,
                 const std::filesystem::path& managed_directory)
{
    auto& rt = Runtime::instance();
    rt.ensure_init(managed_directory);

    auto fn = rt.resolve<fn_int_from_ptr>("OpenRawFile");
    auto utf8 = make_utf8(raw_file_path);
    handle_ = fn(utf8.data());

    if (handle_ == -1) throw bridge_error("Could not open RAW file: " + raw_file_path.string());
    if (handle_ == -3) throw bridge_error("Thermo RawFileReader is not supported on this platform");
    if (handle_ < 0)   throw bridge_error("Failed to open RAW file (code " + std::to_string(handle_) + ")");
}

RawFile::~RawFile()
{
    if (handle_ > 0)
    {
        try
        {
            auto fn = Runtime::instance().resolve<fn_int_from_int>("CloseRawFile");
            fn(handle_);
        }
        catch (...) {}
        handle_ = 0;
    }
}

RawFile::RawFile(RawFile&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = 0;
}

RawFile& RawFile::operator=(RawFile&& other) noexcept
{
    if (this != &other)
    {
        if (handle_ > 0)
        {
            try
            {
                auto fn = Runtime::instance().resolve<fn_int_from_int>("CloseRawFile");
                fn(handle_);
            }
            catch (...) {}
        }
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

// -- Run header ----------------------------------------------------

int RawFile::scan_count() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetScanCount");
    int r = fn(handle_);
    check_int_result(r, "scan_count");
    return r;
}

int RawFile::first_scan_number() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetFirstScanNumber");
    int r = fn(handle_);
    check_int_result(r, "first_scan_number");
    return r;
}

int RawFile::last_scan_number() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetLastScanNumber");
    int r = fn(handle_);
    check_int_result(r, "last_scan_number");
    return r;
}

double RawFile::start_time() const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int>("H_GetStartTime");
    double r = fn(handle_);
    check_double_result(r, "start_time");
    return r;
}

double RawFile::end_time() const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int>("H_GetEndTime");
    double r = fn(handle_);
    check_double_result(r, "end_time");
    return r;
}

double RawFile::low_mass() const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int>("H_GetLowMass");
    double r = fn(handle_);
    check_double_result(r, "low_mass");
    return r;
}

double RawFile::high_mass() const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int>("H_GetHighMass");
    double r = fn(handle_);
    check_double_result(r, "high_mass");
    return r;
}

bool RawFile::has_ms_data() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_HasMSData");
    int r = fn(handle_);
    check_int_result(r, "has_ms_data");
    return r != 0;
}

int RawFile::ms_instrument_count() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetMSInstrumentCount");
    int r = fn(handle_);
    check_int_result(r, "ms_instrument_count");
    return r;
}

// -- Instrument info -----------------------------------------------

std::string RawFile::instrument_model() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetInstrumentModel");
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_name() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetInstrumentName");
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_serial_number() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetInstrumentSerialNumber");
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_software_version() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetInstrumentSoftwareVersion");
    return call_string_fn(fn, handle_);
}

// -- File header / sample info -------------------------------------

std::string RawFile::creation_date() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetCreationDate");
    return call_string_fn(fn, handle_);
}

std::string RawFile::file_description() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetFileDescription");
    return call_string_fn(fn, handle_);
}

int RawFile::file_revision() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetFileRevision");
    int r = fn(handle_);
    check_int_result(r, "file_revision");
    return r;
}

std::string RawFile::sample_name() const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int>("H_GetSampleName");
    return call_string_fn(fn, handle_);
}

// -- Scan-level scalar queries -------------------------------------

double RawFile::retention_time(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetRetentionTime");
    double r = fn(handle_, scan_number);
    check_double_result(r, "retention_time");
    return r;
}

int RawFile::ms_level(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int_int>("H_GetMSLevel");
    int r = fn(handle_, scan_number);
    check_int_result(r, "ms_level");
    return r;
}

bool RawFile::is_centroid_scan(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int_int>("H_IsCentroidScan");
    int r = fn(handle_, scan_number);
    check_int_result(r, "is_centroid_scan");
    return r != 0;
}

std::string RawFile::scan_filter(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int_int>("H_GetScanFilter");
    return call_string_fn(fn, handle_, scan_number);
}

int RawFile::polarity(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int_int>("H_GetPolarity");
    int r = fn(handle_, scan_number);
    check_int_result(r, "polarity");
    return r;
}

std::string RawFile::mass_analyzer_type(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int_int>("H_GetMassAnalyzerType");
    return call_string_fn(fn, handle_, scan_number);
}

std::string RawFile::ionization_mode(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int_int>("H_GetIonizationMode");
    return call_string_fn(fn, handle_, scan_number);
}

// -- Spectrum data -------------------------------------------------

int RawFile::spectrum_peak_count(int scan_number, bool centroid) const
{
    auto fn = Runtime::instance().resolve<fn_spectrum_count>("H_GetSpectrumPeakCount");
    int r = fn(handle_, scan_number, centroid ? 1 : 0);
    check_int_result(r, "spectrum_peak_count");
    return r;
}

SpectrumData RawFile::spectrum_data(int scan_number, bool centroid) const
{
    const int count = spectrum_peak_count(scan_number, centroid);
    if (count == 0) return {};

    SpectrumData result;
    result.mz.resize(static_cast<std::size_t>(count));
    result.intensities.resize(static_cast<std::size_t>(count));

    auto fn = Runtime::instance().resolve<fn_spectrum_data>("H_GetSpectrumData");
    int written = fn(handle_, scan_number, centroid ? 1 : 0,
                     result.mz.data(), result.intensities.data(), count);
    if (written < 0)
    {
        check_int_result(written, "spectrum_data");
    }
    result.mz.resize(static_cast<std::size_t>(written));
    result.intensities.resize(static_cast<std::size_t>(written));
    return result;
}

// -- Precursor / reaction ------------------------------------------

double RawFile::precursor_mass(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetPrecursorMass");
    double r = fn(handle_, scan_number);
    check_double_result(r, "precursor_mass");
    return r;
}

int RawFile::precursor_charge(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int_int>("H_GetPrecursorCharge");
    int r = fn(handle_, scan_number);
    check_int_result(r, "precursor_charge");
    return r;
}

double RawFile::collision_energy(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetCollisionEnergy");
    double r = fn(handle_, scan_number);
    check_double_result(r, "collision_energy");
    return r;
}

std::string RawFile::activation_type(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_str_from_int_int>("H_GetActivationType");
    return call_string_fn(fn, handle_, scan_number);
}

double RawFile::isolation_width(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetIsolationWidth");
    double r = fn(handle_, scan_number);
    check_double_result(r, "isolation_width");
    return r;
}

// -- Scan statistics -----------------------------------------------

double RawFile::base_peak_mass(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetBasePeakMass");
    double r = fn(handle_, scan_number);
    check_double_result(r, "base_peak_mass");
    return r;
}

double RawFile::base_peak_intensity(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetBasePeakIntensity");
    double r = fn(handle_, scan_number);
    check_double_result(r, "base_peak_intensity");
    return r;
}

double RawFile::tic(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_double_from_int_int>("H_GetTIC");
    double r = fn(handle_, scan_number);
    check_double_result(r, "tic");
    return r;
}

// -- Chromatograms -------------------------------------------------

ChromatogramData RawFile::chromatogram_data() const
{
    auto fn_len = Runtime::instance().resolve<fn_chrom_len>("H_GetChromatogramLength");
    int len = fn_len(handle_);
    check_int_result(len, "chromatogram_data");
    if (len == 0) return {};

    ChromatogramData result;
    result.times.resize(static_cast<std::size_t>(len));
    result.intensities.resize(static_cast<std::size_t>(len));

    auto fn_data = Runtime::instance().resolve<fn_chrom_data>("H_GetChromatogramData");
    int written = fn_data(handle_, result.times.data(), result.intensities.data(), len);
    if (written < 0) check_int_result(written, "chromatogram_data");
    result.times.resize(static_cast<std::size_t>(written));
    result.intensities.resize(static_cast<std::size_t>(written));
    return result;
}

int RawFile::xic_length(double mz_start, double mz_end,
                         double rt_start_minutes, double rt_end_minutes) const
{
    auto fn = Runtime::instance().resolve<fn_xic_len>("H_GetXicLength");
    int r = fn(handle_, mz_start, mz_end, rt_start_minutes, rt_end_minutes);
    check_int_result(r, "xic_length");
    return r;
}

ChromatogramData RawFile::xic_data(double mz_start, double mz_end,
                                    double rt_start_minutes, double rt_end_minutes) const
{
    int len = xic_length(mz_start, mz_end, rt_start_minutes, rt_end_minutes);
    if (len == 0) return {};

    ChromatogramData result;
    result.times.resize(static_cast<std::size_t>(len));
    result.intensities.resize(static_cast<std::size_t>(len));

    auto fn = Runtime::instance().resolve<fn_xic_data>("H_GetXicData");
    int written = fn(handle_, mz_start, mz_end, rt_start_minutes, rt_end_minutes,
                     result.times.data(), result.intensities.data(), len);
    if (written < 0) check_int_result(written, "xic_data");
    result.times.resize(static_cast<std::size_t>(written));
    result.intensities.resize(static_cast<std::size_t>(written));
    return result;
}

// -- Trailer extra -------------------------------------------------

int RawFile::trailer_extra_count(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int_int>("H_GetTrailerExtraCount");
    int r = fn(handle_, scan_number);
    check_int_result(r, "trailer_extra_count");
    return r;
}

std::vector<std::string> RawFile::trailer_extra_labels(int scan_number) const
{
    auto fn = Runtime::instance().resolve<fn_trailer_labels>("H_GetTrailerExtraLabels");

    // First call: get total needed size.
    int needed = fn(handle_, scan_number, nullptr, 0);
    if (needed <= 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    fn(handle_, scan_number, buf.data(), static_cast<int>(buf.size()));

    // The buffer contains NUL-delimited labels.
    std::vector<std::string> labels;
    const char* p = buf.data();
    const char* end = p + needed;
    while (p < end)
    {
        std::string label(p);
        labels.push_back(std::move(label));
        p += labels.back().size() + 1;
    }
    return labels;
}

std::string RawFile::trailer_extra_value(int scan_number, const std::string& key) const
{
    auto fn = Runtime::instance().resolve<fn_trailer_value>("H_GetTrailerExtraValue");
    auto key_utf8 = make_utf8(key);

    int needed = fn(handle_, scan_number, key_utf8.data(), nullptr, 0);
    if (needed <= 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    fn(handle_, scan_number, key_utf8.data(), buf.data(), static_cast<int>(buf.size()));
    return std::string(buf.data());
}

// -- Instrument methods --------------------------------------------

int RawFile::instrument_method_count() const
{
    auto fn = Runtime::instance().resolve<fn_int_from_int>("H_GetInstrumentMethodCount");
    int r = fn(handle_);
    check_int_result(r, "instrument_method_count");
    return r;
}

std::string RawFile::instrument_method(int index) const
{
    auto fn = Runtime::instance().resolve<fn_method>("H_GetInstrumentMethod");
    int needed = fn(handle_, index, nullptr, 0);
    if (needed <= 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    fn(handle_, index, buf.data(), static_cast<int>(buf.size()));
    return std::string(buf.data());
}

} // namespace openms::thermo_bridge
