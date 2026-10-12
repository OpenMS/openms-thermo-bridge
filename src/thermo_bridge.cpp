#include <openms_thermo_bridge/thermo_bridge.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <limits>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
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
using fn_chrom_len            = int(*)(int);                          // H_GetChromatogramLength(handle)
using fn_chrom_data           = int(*)(int, void*, void*, int);       // H_GetChromatogramData(handle, rtBuf, intBuf, sz)
using fn_xic_len              = int(*)(int, double, double, double, double); // H_GetXicLength(handle, mzS, mzE, rtS, rtE)
using fn_xic_data             = int(*)(int, double, double, double, double, void*, void*, int); // H_GetXicData(...)
using fn_trailer_labels       = int(*)(int, int, void*, int);         // H_GetTrailerExtraLabels(handle, scan, buf, sz)
using fn_trailer_value        = int(*)(int, int, void*, void*, int);  // H_GetTrailerExtraValue(handle, scan, key, buf, sz)
using fn_method               = int(*)(int, int, void*, int);         // H_GetInstrumentMethod(handle, idx, buf, sz)
using fn_aux_array            = int(*)(int, int, int, void*, int);    // H_GetSpectrumAuxiliaryArray(handle, scan, kind, buf, sz)
using fn_select_instrument    = int(*)(int, int, int);                // H_SelectInstrument(handle, type, number)
using fn_set_selection        = int(*)(int, const void*, int);        // H_SetTrailerSelection(handle, packed, sz)

// Native allocator handed to managed code: returns a buffer for `count` elements of
// the buffer `kind`, or null on failure. Must not throw or call back into managed code.
using fn_alloc                = void*(*)(void*, std::int32_t, std::int64_t);
using fn_read_scans           = int(*)(int, int, int, int, fn_alloc, void*); // H_ReadScans(handle, first, last, flags, alloc, ctx)
using fn_trailer_layout       = int(*)(int, fn_alloc, void*);         // H_GetTrailerLayout(handle, alloc, ctx)

namespace openms::thermo_bridge
{

// Out-of-line bridge_error constructors.  Defining them here (rather than
// inline in the header) gives the class a key function, which anchors the
// typeinfo/vtable to this translation unit and ensures they are exported from
// the shared library regardless of the parent project's visibility settings.
bridge_error::bridge_error(const char* msg) : std::runtime_error(msg) {}
bridge_error::bridge_error(const std::string& msg) : std::runtime_error(msg) {}
bridge_error::~bridge_error() = default;

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

    // Honour DOTNET_ROOT when set — allows non-standard .NET installations
    // (e.g. Homebrew on macOS arm64: /opt/homebrew/opt/dotnet/libexec) to be
    // found without requiring the caller to have a standard system install.
    const char* dotnet_root_env = std::getenv("DOTNET_ROOT");
    if (dotnet_root_env != nullptr && dotnet_root_env[0] != '\0')
    {
        // Convert through std::filesystem::path so wchar_t is used on Windows.
        const std::filesystem::path dotnet_root_path(dotnet_root_env);
#if defined(_WIN32)
        const std::wstring dotnet_root_native = dotnet_root_path.native();
#else
        const std::string dotnet_root_native = dotnet_root_path.native();
#endif
        get_hostfxr_parameters params{};
        params.size = sizeof(params);
        params.dotnet_root = dotnet_root_native.c_str();
        if (get_hostfxr_path(buffer.data(), &buffer_size, &params) == 0)
            return std::filesystem::path(buffer.data());
        buffer = {};
        buffer_size = buffer.size();
    }

    if (get_hostfxr_path(buffer.data(), &buffer_size, nullptr) != 0)
        throw bridge_error("get_hostfxr_path failed: .NET runtime not found in standard locations. "
            "Set DOTNET_ROOT to your .NET installation directory (e.g. /opt/homebrew/opt/dotnet/libexec on macOS).");
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

// Every managed entry point, resolved once when the runtime starts.
#define OPENMS_THERMO_BRIDGE_ENTRY_POINTS(X) \
    X(GetScanCount, fn_int_from_ptr) \
    X(OpenRawFile, fn_int_from_ptr) \
    X(CloseRawFile, fn_int_from_int) \
    X(H_CreateThreadAccessor, fn_int_from_int) \
    X(H_GetFirstScanNumber, fn_int_from_int) \
    X(H_GetLastScanNumber, fn_int_from_int) \
    X(H_GetScanCount, fn_int_from_int) \
    X(H_GetFileRevision, fn_int_from_int) \
    X(H_GetInstrumentMethodCount, fn_int_from_int) \
    X(H_GetMSInstrumentCount, fn_int_from_int) \
    X(H_HasMSData, fn_int_from_int) \
    X(H_GetStartTime, fn_double_from_int) \
    X(H_GetEndTime, fn_double_from_int) \
    X(H_GetLowMass, fn_double_from_int) \
    X(H_GetHighMass, fn_double_from_int) \
    X(H_GetBasePeakIntensity, fn_double_from_int_int) \
    X(H_GetBasePeakMass, fn_double_from_int_int) \
    X(H_GetCollisionEnergy, fn_double_from_int_int) \
    X(H_GetIsolationWidth, fn_double_from_int_int) \
    X(H_GetPrecursorMass, fn_double_from_int_int) \
    X(H_GetRetentionTime, fn_double_from_int_int) \
    X(H_GetTIC, fn_double_from_int_int) \
    X(H_GetInstrumentCount, fn_int_from_int_int) \
    X(H_GetMSLevel, fn_int_from_int_int) \
    X(H_GetPolarity, fn_int_from_int_int) \
    X(H_GetPrecursorCharge, fn_int_from_int_int) \
    X(H_GetTrailerExtraCount, fn_int_from_int_int) \
    X(H_IsCentroidScan, fn_int_from_int_int) \
    X(H_GetCreationDate, fn_str_from_int) \
    X(H_GetDetectorChromatograms, fn_str_from_int) \
    X(H_GetFileDescription, fn_str_from_int) \
    X(H_GetInstrumentModel, fn_str_from_int) \
    X(H_GetInstrumentName, fn_str_from_int) \
    X(H_GetInstrumentSerialNumber, fn_str_from_int) \
    X(H_GetInstrumentSoftwareVersion, fn_str_from_int) \
    X(H_GetSampleName, fn_str_from_int) \
    X(H_GetFileSha1, fn_str_from_int) \
    X(H_GetActivationType, fn_str_from_int_int) \
    X(H_GetFileMetadata, fn_str_from_int_int) \
    X(H_GetIonizationMode, fn_str_from_int_int) \
    X(H_GetMassAnalyzerType, fn_str_from_int_int) \
    X(H_GetScanFilter, fn_str_from_int_int) \
    X(H_GetScanMetadata, fn_str_from_int_int) \
    X(H_GetInstrumentMethod, fn_method) \
    X(H_GetChromatogramLength, fn_chrom_len) \
    X(H_GetChromatogramData, fn_chrom_data) \
    X(H_GetXicLength, fn_xic_len) \
    X(H_GetXicData, fn_xic_data) \
    X(H_GetTrailerExtraLabels, fn_trailer_labels) \
    X(H_GetTrailerExtraValue, fn_trailer_value) \
    X(H_GetSpectrumAuxiliaryArray, fn_aux_array) \
    X(H_SelectInstrument, fn_select_instrument) \
    X(H_ReadScans, fn_read_scans) \
    X(H_GetTrailerLayout, fn_trailer_layout) \
    X(H_SetTrailerSelection, fn_set_selection)

/// Immutable after Runtime initialisation; read without locking.
struct ManagedApi
{
#define OPENMS_THERMO_BRIDGE_DECLARE_ENTRY(name, type) type name = nullptr;
    OPENMS_THERMO_BRIDGE_ENTRY_POINTS(OPENMS_THERMO_BRIDGE_DECLARE_ENTRY)
#undef OPENMS_THERMO_BRIDGE_DECLARE_ENTRY
};

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
    /// The first successful call loads the CLR and resolves all entry points;
    /// later calls only take the lock-free fast path.
    void ensure_init(const std::filesystem::path& managed_directory)
    {
        if (initialised_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialised_.load(std::memory_order_relaxed)) return;

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

        ManagedApi table;
#define OPENMS_THERMO_BRIDGE_RESOLVE_ENTRY(name, type) table.name = reinterpret_cast<type>(resolve(#name));
        OPENMS_THERMO_BRIDGE_ENTRY_POINTS(OPENMS_THERMO_BRIDGE_RESOLVE_ENTRY)
#undef OPENMS_THERMO_BRIDGE_RESOLVE_ENTRY
        api_ = table;
        initialised_.store(true, std::memory_order_release);
    }

    /// Entry-point table. Only valid after ensure_init(); RawFile objects exist only then.
    const ManagedApi& api() const noexcept { return api_; }

private:
    /// Resolve one [UnmanagedCallersOnly] entry point (init only, mutex held).
    void* resolve(const char* entry_point_name)
    {
        const auto assembly_path_native = to_char_t_path(assembly_path_);

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
            throw bridge_error(std::string("Failed to resolve managed entry point '") + entry_point_name
                + "' (code " + std::to_string(rc) + "); native and managed bridge versions must match");
        return fn;
    }

    Runtime() = default;

    std::mutex mutex_;
    std::atomic<bool> initialised_{false};
    ManagedApi api_;
    std::filesystem::path managed_directory_;
    std::filesystem::path runtime_config_;
    std::filesystem::path assembly_path_;
    load_assembly_and_get_function_pointer_fn load_assembly_ = nullptr;
};

const ManagedApi& api() noexcept { return Runtime::instance().api(); }

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
    // First call: discover size needed (metadata JSON is cached managed-side).
    int needed = fn(handle, extra..., nullptr, 0);
    check_int_result(needed, "string query");
    if (needed == 0) return {};
    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    const int written = fn(handle, extra..., buf.data(), static_cast<int>(buf.size()));
    check_int_result(written, "string query");
    if (written > needed) throw bridge_error("String changed between size and data query");
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

// ================================================================
//  Single-pass scan transfer (see ThermoScanReader.cs)
// ================================================================

// Mirrors of the managed [StructLayout(Sequential)] records.
struct NativeScanHeader
{
    std::int32_t scan_number;
    std::int32_t controller_type;
    std::int32_t controller_number;
    std::int32_t ms_order;
    std::int32_t polarity;
    std::int32_t analyzer;
    std::int32_t ionization;
    std::int32_t centroid;
    std::int32_t peak_source;
    std::int32_t reserved;
    double retention_time;
    double tic;
    double base_peak_mass;
    double base_peak_intensity;
    double low_mass;
    double high_mass;
    double low_wavelength;
    double high_wavelength;
};
static_assert(sizeof(NativeScanHeader) == 104, "NativeScanHeader layout must match ThermoScanReader.cs");

struct NativeReaction
{
    double precursor_mass;
    double isolation_width;
    double isolation_offset;
    double collision_energy;
    double first_precursor_mass;
    double last_precursor_mass;
    std::int32_t activation;
    std::int32_t flags;
};
static_assert(sizeof(NativeReaction) == 56, "NativeReaction layout must match ThermoScanReader.cs");

struct NativeTrailerLabel
{
    std::int32_t kind;
    std::int32_t slot;
    std::int32_t vendor_type;
    std::int32_t selected;
};
static_assert(sizeof(NativeTrailerLabel) == 16, "NativeTrailerLabel layout must match ThermoScanReader.cs");

enum SinkKind : std::int32_t
{
    kHeader = 0, kFilter = 1, kReactions = 2, kMz = 3, kIntensities = 4, kCharges = 5,
    kNoiseMz = 6, kNoiseIntensities = 7, kNoiseBaselines = 8,
    kTrailerDoubles = 9, kTrailerInts = 10, kTrailerBools = 11, kTrailerStrings = 12, kTrailerText = 13,
    kLayoutLabels = 20, kLayoutNames = 21
};

int read_flags(const ReadOptions& o)
{
    return (o.centroid ? 1 : 0) | (o.peaks ? 2 : 0) | (o.trailers ? 4 : 0) | (o.trailer_text ? 8 : 0)
        | (o.charges ? 16 : 0) | (o.noise ? 32 : 0);
}

/// Splits NUL-terminated packed UTF-8 values, reusing existing string capacity.
void unpack_strings(const std::string& packed, std::vector<std::string>& out)
{
    std::size_t count = 0;
    std::size_t start = 0;
    while (start < packed.size())
    {
        std::size_t end = packed.find('\0', start);
        if (end == std::string::npos) end = packed.size();
        if (count == out.size()) out.emplace_back();
        out[count++].assign(packed, start, end - start);
        start = end + 1;
    }
    out.resize(count);
}

/// Per-scan state collected while managed code writes one scan.
struct ScanStage
{
    NativeScanHeader fixed{};
    std::vector<NativeReaction> reactions;
    std::string trailer_strings;
    std::string trailer_text;
    bool active = false;

    void begin()
    {
        fixed = NativeScanHeader{};
        reactions.clear();
        trailer_strings.clear();
        trailer_text.clear();
        active = true;
    }

    void finish(ScanHeader& h)
    {
        h.scan_number = fixed.scan_number;
        h.controller_type = fixed.controller_type;
        h.controller_number = fixed.controller_number;
        h.ms_order = static_cast<MsOrder>(fixed.ms_order);
        h.polarity = static_cast<Polarity>(fixed.polarity);
        h.analyzer = fixed.analyzer;
        h.ionization = fixed.ionization;
        h.centroid = fixed.centroid != 0;
        h.peak_source = static_cast<PeakSource>(fixed.peak_source);
        h.retention_time = fixed.retention_time;
        h.tic = fixed.tic;
        h.base_peak_mass = fixed.base_peak_mass;
        h.base_peak_intensity = fixed.base_peak_intensity;
        h.low_mass = fixed.low_mass;
        h.high_mass = fixed.high_mass;
        h.low_wavelength = fixed.low_wavelength;
        h.high_wavelength = fixed.high_wavelength;
        h.reactions.resize(reactions.size());
        for (std::size_t i = 0; i < reactions.size(); ++i)
        {
            const auto& n = reactions[i];
            auto& r = h.reactions[i];
            r.precursor_mass = n.precursor_mass;
            r.isolation_width = n.isolation_width;
            r.isolation_offset = n.isolation_offset;
            r.collision_energy = n.collision_energy;
            r.first_precursor_mass = n.first_precursor_mass;
            r.last_precursor_mass = n.last_precursor_mass;
            r.activation = n.activation;
            r.collision_energy_valid = (n.flags & 1) != 0;
            r.precursor_range_valid = (n.flags & 2) != 0;
            r.multiple_activation = (n.flags & 4) != 0;
        }
        unpack_strings(trailer_strings, h.trailer_strings);
        unpack_strings(trailer_text, h.trailer_text);
        active = false;
    }
};

template <typename T>
void* grow(std::vector<T>& v, std::int64_t count)
{
    const std::size_t old = v.size();
    v.resize(old + static_cast<std::size_t>(count));
    return v.data() + old;
}

void clear_header(ScanHeader& h)
{
    h.filter.clear();
    h.trailer_doubles.clear();
    h.trailer_ints.clear();
    h.trailer_bools.clear();
}

/// Writes one scan into a reusable Spectrum.
struct SpectrumSink
{
    Spectrum& out;
    ScanStage stage;

    void* alloc(std::int32_t kind, std::int64_t count)
    {
        auto& h = out.header;
        switch (kind)
        {
        case kHeader:
            stage.begin();
            clear_header(h);
            out.mz.clear(); out.intensities.clear(); out.charges.clear();
            out.noise_mz.clear(); out.noise_intensities.clear(); out.noise_baselines.clear();
            return &stage.fixed;
        case kFilter: h.filter.resize(static_cast<std::size_t>(count)); return h.filter.data();
        case kReactions: return grow(stage.reactions, count);
        case kMz: return grow(out.mz, count);
        case kIntensities: return grow(out.intensities, count);
        case kCharges: return grow(out.charges, count);
        case kNoiseMz: return grow(out.noise_mz, count);
        case kNoiseIntensities: return grow(out.noise_intensities, count);
        case kNoiseBaselines: return grow(out.noise_baselines, count);
        case kTrailerDoubles: return grow(h.trailer_doubles, count);
        case kTrailerInts: return grow(h.trailer_ints, count);
        case kTrailerBools: return grow(h.trailer_bools, count);
        case kTrailerStrings: stage.trailer_strings.resize(static_cast<std::size_t>(count)); return stage.trailer_strings.data();
        case kTrailerText: stage.trailer_text.resize(static_cast<std::size_t>(count)); return stage.trailer_text.data();
        default: return nullptr;
        }
    }

    void finish()
    {
        if (stage.active) stage.finish(out.header);
    }
};

/// Appends scans to a packed ScanBatch.
struct BatchSink
{
    ScanBatch& out;
    ScanStage stage;

    void close_scan()
    {
        if (stage.active) stage.finish(out.headers.back());
    }

    void* alloc(std::int32_t kind, std::int64_t count)
    {
        switch (kind)
        {
        case kHeader:
            close_scan();
            out.headers.emplace_back();
            out.peak_offsets.push_back(out.mz.size());
            out.charge_offsets.push_back(out.charges.size());
            out.noise_offsets.push_back(out.noise_mz.size());
            stage.begin();
            return &stage.fixed;
        case kFilter:
        {
            auto& f = out.headers.back().filter;
            f.resize(static_cast<std::size_t>(count));
            return f.data();
        }
        case kReactions: return grow(stage.reactions, count);
        case kMz: return grow(out.mz, count);
        case kIntensities: return grow(out.intensities, count);
        case kCharges: return grow(out.charges, count);
        case kNoiseMz: return grow(out.noise_mz, count);
        case kNoiseIntensities: return grow(out.noise_intensities, count);
        case kNoiseBaselines: return grow(out.noise_baselines, count);
        case kTrailerDoubles: return grow(out.headers.back().trailer_doubles, count);
        case kTrailerInts: return grow(out.headers.back().trailer_ints, count);
        case kTrailerBools: return grow(out.headers.back().trailer_bools, count);
        case kTrailerStrings: stage.trailer_strings.resize(static_cast<std::size_t>(count)); return stage.trailer_strings.data();
        case kTrailerText: stage.trailer_text.resize(static_cast<std::size_t>(count)); return stage.trailer_text.data();
        default: return nullptr;
        }
    }

    void finish()
    {
        close_scan();
        out.peak_offsets.push_back(out.mz.size());
        out.charge_offsets.push_back(out.charges.size());
        out.noise_offsets.push_back(out.noise_mz.size());
    }
};

struct LayoutSink
{
    std::vector<NativeTrailerLabel> labels;
    std::string names;

    void* alloc(std::int32_t kind, std::int64_t count)
    {
        if (kind == kLayoutLabels) return grow(labels, count);
        if (kind == kLayoutNames) { names.resize(static_cast<std::size_t>(count)); return names.data(); }
        return nullptr;
    }
};

/// The C callback handed to managed code. Never throws (null signals failure).
template <typename Sink>
void* sink_alloc(void* ctx, std::int32_t kind, std::int64_t count) noexcept
{
    if (count < 0) return nullptr;
    try { return static_cast<Sink*>(ctx)->alloc(kind, count); }
    catch (...) { return nullptr; }
}

void check_read_result(int result, const char* context)
{
    if (result == -6) throw bridge_error(std::string(context) + ": out of memory");
    check_int_result(result, context);
}

const char* const kMsOrderNames[] = {"Ng", "Nl", "Par", "Any", "Ms", "Ms2", "Ms3", "Ms4", "Ms5", "Ms6", "Ms7", "Ms8", "Ms9", "Ms10"};
const char* const kAnalyzerNames[] = {"MassAnalyzerITMS", "MassAnalyzerTQMS", "MassAnalyzerSQMS", "MassAnalyzerTOFMS",
    "MassAnalyzerFTMS", "MassAnalyzerSector", "Any", "MassAnalyzerASTMS"};
const char* const kIonizationNames[] = {"ElectronImpact", "ChemicalIonization", "FastAtomBombardment", "ElectroSpray",
    "AtmosphericPressureChemicalIonization", "NanoSpray", "ThermoSpray", "FieldDesorption",
    "MatrixAssistedLaserDesorptionIonization", "GlowDischarge", "Any", "PaperSprayIonization", "CardNanoSprayIonization",
    "IonizationMode1", "IonizationMode2", "IonizationMode3", "IonizationMode4", "IonizationMode5", "IonizationMode6",
    "IonizationMode7", "IonizationMode8", "IonizationMode9", "IonModeBeyondKnown"};
const char* const kActivationNames[] = {"CollisionInducedDissociation", "MultiPhotonDissociation", "ElectronCaptureDissociation",
    "PQD", "ElectronTransferDissociation", "HigherEnergyCollisionalDissociation", "Any", "SAactivation",
    "ProtonTransferReaction", "NegativeElectronTransferDissociation", "NegativeProtonTransferReaction",
    "UltraVioletPhotoDissociation", "ElectronInducedDissociation", "ElectronEnergy", "Infrared", "ModeD", "ModeE", "ModeF",
    "ModeG", "ModeH", "ModeI", "ModeJ", "ModeK", "ModeL", "ModeM", "ModeN", "ModeO", "ModeP", "ModeQ", "ModeR", "ModeS",
    "ModeT", "ModeU", "ModeV", "ModeW", "ModeX", "ModeY", "ModeZ", "LastActivation"};

template <std::size_t N>
std::string enum_name(const char* const (&names)[N], int value, int offset = 0)
{
    const int index = value + offset;
    if (index >= 0 && index < static_cast<int>(N)) return names[index];
    return std::to_string(value);
}

} // anonymous namespace

// ================================================================
//  Free functions
// ================================================================

std::string ms_order_name(MsOrder order) { return enum_name(kMsOrderNames, static_cast<int>(order), 3); }
std::string mass_analyzer_name(int analyzer) { return enum_name(kAnalyzerNames, analyzer); }
std::string ionization_mode_name(int ionization) { return enum_name(kIonizationNames, ionization); }
std::string activation_type_name(int activation) { return enum_name(kActivationNames, activation); }

namespace
{
template <typename T>
const T& trailer_slot(const TrailerLayout& layout, int label_index, TrailerKind kind, const std::vector<T>& values)
{
    if (label_index < 0 || static_cast<std::size_t>(label_index) >= layout.labels.size())
        throw std::out_of_range("trailer label index out of range");
    const auto& label = layout.labels[static_cast<std::size_t>(label_index)];
    if (label.kind != kind) throw std::out_of_range("trailer '" + label.label + "' has a different kind");
    if (label.slot < 0 || static_cast<std::size_t>(label.slot) >= values.size())
        throw std::out_of_range("trailer '" + label.label + "' was not read");
    return values[static_cast<std::size_t>(label.slot)];
}
} // namespace

double ScanHeader::trailer_double(const TrailerLayout& layout, int label_index) const
{
    return trailer_slot(layout, label_index, TrailerKind::Double, trailer_doubles);
}

std::int64_t ScanHeader::trailer_int(const TrailerLayout& layout, int label_index) const
{
    return trailer_slot(layout, label_index, TrailerKind::Int, trailer_ints);
}

bool ScanHeader::trailer_bool(const TrailerLayout& layout, int label_index) const
{
    return trailer_slot(layout, label_index, TrailerKind::Bool, trailer_bools) != 0;
}

const std::string& ScanHeader::trailer_string(const TrailerLayout& layout, int label_index) const
{
    return trailer_slot(layout, label_index, TrailerKind::String, trailer_strings);
}

double ScanHeader::trailer_as_double(const TrailerLayout& layout, int label_index) const
{
    if (label_index < 0 || static_cast<std::size_t>(label_index) >= layout.labels.size())
        throw std::out_of_range("trailer label index out of range");
    switch (layout.labels[static_cast<std::size_t>(label_index)].kind)
    {
    case TrailerKind::Double: return trailer_double(layout, label_index);
    case TrailerKind::Int: return static_cast<double>(trailer_int(layout, label_index));
    case TrailerKind::Bool: return trailer_bool(layout, label_index) ? 1.0 : 0.0;
    default:
    {
        const std::string& text = trailer_string(layout, label_index);
        const char* begin = text.c_str();
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) return std::numeric_limits<double>::quiet_NaN();
        return value;
    }
    }
}

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

    auto fn = rt.api().GetScanCount;
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

    auto fn = rt.api().OpenRawFile;
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
            auto fn = api().CloseRawFile;
            fn(handle_);
        }
        catch (...) {}
        handle_ = 0;
    }
}

RawFile::RawFile(int handle) noexcept : handle_(handle)
{
}

RawFile::RawFile(RawFile&& other) noexcept
    : handle_(other.handle_), layout_(std::move(other.layout_)), layout_loaded_(other.layout_loaded_)
{
    other.handle_ = 0;
    other.layout_loaded_ = false;
}

RawFile& RawFile::operator=(RawFile&& other) noexcept
{
    if (this != &other)
    {
        if (handle_ > 0)
        {
            try
            {
                auto fn = api().CloseRawFile;
                fn(handle_);
            }
            catch (...) {}
        }
        handle_ = other.handle_;
        layout_ = std::move(other.layout_);
        layout_loaded_ = other.layout_loaded_;
        other.handle_ = 0;
        other.layout_loaded_ = false;
    }
    return *this;
}

// -- Run header ----------------------------------------------------

int RawFile::scan_count() const
{
    auto fn = api().H_GetScanCount;
    int r = fn(handle_);
    check_int_result(r, "scan_count");
    return r;
}

int RawFile::first_scan_number() const
{
    auto fn = api().H_GetFirstScanNumber;
    int r = fn(handle_);
    check_int_result(r, "first_scan_number");
    return r;
}

int RawFile::last_scan_number() const
{
    auto fn = api().H_GetLastScanNumber;
    int r = fn(handle_);
    check_int_result(r, "last_scan_number");
    return r;
}

double RawFile::start_time() const
{
    auto fn = api().H_GetStartTime;
    double r = fn(handle_);
    check_double_result(r, "start_time");
    return r;
}

double RawFile::end_time() const
{
    auto fn = api().H_GetEndTime;
    double r = fn(handle_);
    check_double_result(r, "end_time");
    return r;
}

double RawFile::low_mass() const
{
    auto fn = api().H_GetLowMass;
    double r = fn(handle_);
    check_double_result(r, "low_mass");
    return r;
}

double RawFile::high_mass() const
{
    auto fn = api().H_GetHighMass;
    double r = fn(handle_);
    check_double_result(r, "high_mass");
    return r;
}

bool RawFile::has_ms_data() const
{
    auto fn = api().H_HasMSData;
    int r = fn(handle_);
    check_int_result(r, "has_ms_data");
    return r != 0;
}

int RawFile::ms_instrument_count() const
{
    auto fn = api().H_GetMSInstrumentCount;
    int r = fn(handle_);
    check_int_result(r, "ms_instrument_count");
    return r;
}

// -- Instrument info -----------------------------------------------

std::string RawFile::instrument_model() const
{
    auto fn = api().H_GetInstrumentModel;
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_name() const
{
    auto fn = api().H_GetInstrumentName;
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_serial_number() const
{
    auto fn = api().H_GetInstrumentSerialNumber;
    return call_string_fn(fn, handle_);
}

std::string RawFile::instrument_software_version() const
{
    auto fn = api().H_GetInstrumentSoftwareVersion;
    return call_string_fn(fn, handle_);
}

// -- File header / sample info -------------------------------------

std::string RawFile::creation_date() const
{
    auto fn = api().H_GetCreationDate;
    return call_string_fn(fn, handle_);
}

std::string RawFile::file_description() const
{
    auto fn = api().H_GetFileDescription;
    return call_string_fn(fn, handle_);
}

int RawFile::file_revision() const
{
    auto fn = api().H_GetFileRevision;
    int r = fn(handle_);
    check_int_result(r, "file_revision");
    return r;
}

std::string RawFile::sample_name() const
{
    auto fn = api().H_GetSampleName;
    return call_string_fn(fn, handle_);
}

// -- Scan-level scalar queries -------------------------------------

double RawFile::retention_time(int scan_number) const
{
    auto fn = api().H_GetRetentionTime;
    double r = fn(handle_, scan_number);
    check_double_result(r, "retention_time");
    return r;
}

int RawFile::ms_level(int scan_number) const
{
    auto fn = api().H_GetMSLevel;
    int r = fn(handle_, scan_number);
    check_int_result(r, "ms_level");
    return r;
}

bool RawFile::is_centroid_scan(int scan_number) const
{
    auto fn = api().H_IsCentroidScan;
    int r = fn(handle_, scan_number);
    check_int_result(r, "is_centroid_scan");
    return r != 0;
}

std::string RawFile::scan_filter(int scan_number) const
{
    auto fn = api().H_GetScanFilter;
    return call_string_fn(fn, handle_, scan_number);
}

int RawFile::polarity(int scan_number) const
{
    auto fn = api().H_GetPolarity;
    int r = fn(handle_, scan_number);
    check_int_result(r, "polarity");
    return r;
}

std::string RawFile::mass_analyzer_type(int scan_number) const
{
    auto fn = api().H_GetMassAnalyzerType;
    return call_string_fn(fn, handle_, scan_number);
}

std::string RawFile::ionization_mode(int scan_number) const
{
    auto fn = api().H_GetIonizationMode;
    return call_string_fn(fn, handle_, scan_number);
}

// -- Single-pass scan reading --------------------------------------

void RawFile::load_layout() const
{
    LayoutSink sink;
    const int n = api().H_GetTrailerLayout(handle_, &sink_alloc<LayoutSink>, &sink);
    check_read_result(n, "trailer_layout");
    TrailerLayout layout;
    std::vector<std::string> names;
    unpack_strings(sink.names, names);
    if (names.size() != sink.labels.size()) throw bridge_error("trailer_layout: inconsistent label data");
    layout.labels.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        auto& l = layout.labels[i];
        l.label = std::move(names[i]);
        l.kind = static_cast<TrailerKind>(sink.labels[i].kind);
        l.slot = sink.labels[i].slot;
        l.vendor_type = sink.labels[i].vendor_type;
        l.selected = sink.labels[i].selected != 0;
        switch (l.kind)
        {
        case TrailerKind::Double: ++layout.double_count; break;
        case TrailerKind::Int: ++layout.int_count; break;
        case TrailerKind::Bool: ++layout.bool_count; break;
        default: ++layout.string_count; break;
        }
    }
    layout_ = std::move(layout);
    layout_loaded_ = true;
}

const TrailerLayout& RawFile::trailer_layout() const
{
    if (!layout_loaded_) load_layout();
    return layout_;
}

void RawFile::set_trailer_selection(const std::vector<std::string>& labels)
{
    std::string packed;
    for (const auto& label : labels)
    {
        if (label.empty()) continue;
        packed += label;
        packed.push_back('\0');
    }
    check_int_result(api().H_SetTrailerSelection(handle_, packed.empty() ? nullptr : packed.data(),
        static_cast<int>(packed.size())), "set_trailer_selection");
    layout_loaded_ = false;
}

ScanHeader RawFile::scan_header(int scan_number, const ReadOptions& options) const
{
    ReadOptions header_only = options;
    header_only.peaks = false;
    header_only.charges = false;
    header_only.noise = false;
    Spectrum spectrum;
    read_scan(scan_number, spectrum, header_only);
    return std::move(spectrum.header);
}

Spectrum RawFile::read_scan(int scan_number, const ReadOptions& options) const
{
    Spectrum spectrum;
    read_scan(scan_number, spectrum, options);
    return spectrum;
}

void RawFile::read_scan(int scan_number, Spectrum& out, const ReadOptions& options) const
{
    SpectrumSink sink{out, {}};
    const int r = api().H_ReadScans(handle_, scan_number, scan_number, read_flags(options),
        &sink_alloc<SpectrumSink>, &sink);
    check_read_result(r, "read_scan");
    sink.finish();
}

ScanBatch RawFile::read_scans(int first_scan, int last_scan, const ReadOptions& options) const
{
    ScanBatch batch;
    read_scans(first_scan, last_scan, batch, options);
    return batch;
}

void RawFile::read_scans(int first_scan, int last_scan, ScanBatch& out, const ReadOptions& options) const
{
    out.headers.clear();
    out.mz.clear(); out.intensities.clear(); out.peak_offsets.clear();
    out.charges.clear(); out.charge_offsets.clear();
    out.noise_mz.clear(); out.noise_intensities.clear(); out.noise_baselines.clear(); out.noise_offsets.clear();
    if (last_scan >= first_scan)
    {
        const auto n = static_cast<std::size_t>(last_scan) - static_cast<std::size_t>(first_scan) + 1;
        out.headers.reserve(n);
        out.peak_offsets.reserve(n + 1);
        out.charge_offsets.reserve(n + 1);
        out.noise_offsets.reserve(n + 1);
    }
    BatchSink sink{out, {}};
    const int r = api().H_ReadScans(handle_, first_scan, last_scan, read_flags(options),
        &sink_alloc<BatchSink>, &sink);
    if (r < 0)
    {
        out.headers.clear();
        out.mz.clear(); out.intensities.clear(); out.peak_offsets.clear();
        out.charges.clear(); out.charge_offsets.clear();
        out.noise_mz.clear(); out.noise_intensities.clear(); out.noise_baselines.clear(); out.noise_offsets.clear();
        check_read_result(r, "read_scans");
    }
    sink.finish();
}

SpectrumData RawFile::spectrum_data(int scan_number, bool centroid) const
{
    ReadOptions options;
    options.centroid = centroid;
    options.trailers = false;
    Spectrum spectrum;
    read_scan(scan_number, spectrum, options);
    return SpectrumData{std::move(spectrum.mz), std::move(spectrum.intensities)};
}

RawFile RawFile::thread_accessor() const
{
    const int handle = api().H_CreateThreadAccessor(handle_);
    check_int_result(handle, "thread_accessor");
    return RawFile(handle);
}

// -- Precursor / reaction ------------------------------------------

double RawFile::precursor_mass(int scan_number) const
{
    auto fn = api().H_GetPrecursorMass;
    double r = fn(handle_, scan_number);
    check_double_result(r, "precursor_mass");
    return r;
}

int RawFile::precursor_charge(int scan_number) const
{
    auto fn = api().H_GetPrecursorCharge;
    int r = fn(handle_, scan_number);
    check_int_result(r, "precursor_charge");
    return r;
}

double RawFile::collision_energy(int scan_number) const
{
    auto fn = api().H_GetCollisionEnergy;
    double r = fn(handle_, scan_number);
    check_double_result(r, "collision_energy");
    return r;
}

std::string RawFile::activation_type(int scan_number) const
{
    auto fn = api().H_GetActivationType;
    return call_string_fn(fn, handle_, scan_number);
}

double RawFile::isolation_width(int scan_number) const
{
    auto fn = api().H_GetIsolationWidth;
    double r = fn(handle_, scan_number);
    check_double_result(r, "isolation_width");
    return r;
}

// -- Scan statistics -----------------------------------------------

double RawFile::base_peak_mass(int scan_number) const
{
    auto fn = api().H_GetBasePeakMass;
    double r = fn(handle_, scan_number);
    check_double_result(r, "base_peak_mass");
    return r;
}

double RawFile::base_peak_intensity(int scan_number) const
{
    auto fn = api().H_GetBasePeakIntensity;
    double r = fn(handle_, scan_number);
    check_double_result(r, "base_peak_intensity");
    return r;
}

double RawFile::tic(int scan_number) const
{
    auto fn = api().H_GetTIC;
    double r = fn(handle_, scan_number);
    check_double_result(r, "tic");
    return r;
}

// -- Chromatograms -------------------------------------------------

ChromatogramData RawFile::chromatogram_data() const
{
    auto fn_len = api().H_GetChromatogramLength;
    int len = fn_len(handle_);
    check_int_result(len, "chromatogram_data");
    if (len == 0) return {};

    ChromatogramData result;
    result.times.resize(static_cast<std::size_t>(len));
    result.intensities.resize(static_cast<std::size_t>(len));

    auto fn_data = api().H_GetChromatogramData;
    int written = fn_data(handle_, result.times.data(), result.intensities.data(), len);
    if (written < 0) check_int_result(written, "chromatogram_data");
    result.times.resize(static_cast<std::size_t>(written));
    result.intensities.resize(static_cast<std::size_t>(written));
    return result;
}

int RawFile::xic_length(double mz_start, double mz_end,
                         double rt_start_minutes, double rt_end_minutes) const
{
    auto fn = api().H_GetXicLength;
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

    auto fn = api().H_GetXicData;
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
    auto fn = api().H_GetTrailerExtraCount;
    int r = fn(handle_, scan_number);
    check_int_result(r, "trailer_extra_count");
    return r;
}

std::vector<std::string> RawFile::trailer_extra_labels(int scan_number) const
{
    auto fn = api().H_GetTrailerExtraLabels;

    // First call: get total needed size.
    int needed = fn(handle_, scan_number, nullptr, 0);
    check_int_result(needed, "metadata query");
    if (needed == 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    const int written = fn(handle_, scan_number, buf.data(), static_cast<int>(buf.size()));
    check_int_result(written, "trailer labels");
    if (written != needed) throw bridge_error("Trailer labels changed during query");

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
    auto fn = api().H_GetTrailerExtraValue;
    auto key_utf8 = make_utf8(key);

    int needed = fn(handle_, scan_number, key_utf8.data(), nullptr, 0);
    check_int_result(needed, "metadata query");
    if (needed == 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    const int written = fn(handle_, scan_number, key_utf8.data(), buf.data(), static_cast<int>(buf.size()));
    check_int_result(written, "trailer value");
    if (written > needed) throw bridge_error("Trailer value changed during query");
    return std::string(buf.data());
}

// -- Instrument methods --------------------------------------------

int RawFile::instrument_method_count() const
{
    auto fn = api().H_GetInstrumentMethodCount;
    int r = fn(handle_);
    check_int_result(r, "instrument_method_count");
    return r;
}

std::string RawFile::instrument_method(int index) const
{
    auto fn = api().H_GetInstrumentMethod;
    int needed = fn(handle_, index, nullptr, 0);
    check_int_result(needed, "metadata query");
    if (needed == 0) return {};

    std::vector<char> buf(static_cast<std::size_t>(needed) + 1, '\0');
    const int written = fn(handle_, index, buf.data(), static_cast<int>(buf.size()));
    check_int_result(written, "instrument method");
    if (written > needed) throw bridge_error("Instrument method changed during query");
    return std::string(buf.data());
}

std::string RawFile::file_metadata_json(bool include_methods) const
{
    auto fn = api().H_GetFileMetadata;
    return call_string_fn(fn, handle_, include_methods ? 1 : 0);
}

std::string RawFile::file_sha1() const
{
    // One call: the hex digest always fits, so the file is hashed exactly once.
    std::array<char, 128> buf{};
    const int n = api().H_GetFileSha1(handle_, buf.data(), static_cast<int>(buf.size()));
    check_int_result(n, "file_sha1");
    if (static_cast<std::size_t>(n) >= buf.size()) throw bridge_error("file_sha1: unexpected digest length");
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

std::string RawFile::scan_metadata_json(int scan_number) const
{
    auto fn = api().H_GetScanMetadata;
    return call_string_fn(fn, handle_, scan_number);
}

std::vector<double> RawFile::spectrum_auxiliary_array(int scan_number, int array_kind) const
{
    auto fn = api().H_GetSpectrumAuxiliaryArray;
    int count = fn(handle_, scan_number, array_kind, nullptr, 0);
    check_int_result(count, "spectrum auxiliary array");
    if (count == 0) return {};
    std::vector<double> result(static_cast<std::size_t>(count));
    int written = fn(handle_, scan_number, array_kind, result.data(), count);
    check_int_result(written, "spectrum auxiliary array");
    if (written != count) throw bridge_error("Auxiliary array changed during query");
    return result;
}

std::string RawFile::detector_chromatograms_json() const
{
    auto fn = api().H_GetDetectorChromatograms;
    return call_string_fn(fn, handle_);
}

int RawFile::instrument_count(int device_type) const
{
    auto fn = api().H_GetInstrumentCount;
    int count = fn(handle_, device_type);
    check_int_result(count, "instrument count");
    return count;
}

void RawFile::select_instrument(int device_type, int instrument_number)
{
    auto fn = api().H_SelectInstrument;
    check_int_result(fn(handle_, device_type, instrument_number), "select instrument");
    layout_loaded_ = false;
}

} // namespace openms::thermo_bridge
