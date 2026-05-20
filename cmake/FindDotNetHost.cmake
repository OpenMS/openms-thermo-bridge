include(FindPackageHandleStandardArgs)

set(_dotnet_host_rid)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(_dotnet_host_rid win-arm64)
  else()
    set(_dotnet_host_rid win-x64)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(CMAKE_OSX_ARCHITECTURES MATCHES "(^|;)x86_64(;|$)")
    set(_dotnet_host_rid osx-x64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_dotnet_host_rid osx-arm64)
  else()
    set(_dotnet_host_rid osx-x64)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_dotnet_host_rid linux-arm64)
  else()
    set(_dotnet_host_rid linux-x64)
  endif()
endif()

if(NOT _dotnet_host_rid)
  message(FATAL_ERROR "Unsupported platform for DotNetHost: ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR OR OPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED)
  set(_dotnet_host_requirements_message
    "OpenMSThermoBridge can reuse pre-built managed artifacts, but compiling the native bridge still requires the "
    ".NET nethost headers and library for ${_dotnet_host_rid}. Those files usually come from the platform-specific "
    ".NET SDK/host pack rather than a runtime-only install.")
else()
  set(_dotnet_host_requirements_message
    "OpenMSThermoBridge builds ThermoWrapperManaged.csproj locally with 'dotnet publish' and also needs the .NET "
    "nethost headers and library for ${_dotnet_host_rid}. Install a matching .NET SDK so both requirements are "
    "available.")
endif()

set(_dotnet_host_roots)
if(DEFINED ENV{DOTNET_ROOT} AND NOT "$ENV{DOTNET_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{DOTNET_ROOT}" _dotnet_root_path)
  list(APPEND _dotnet_host_roots "${_dotnet_root_path}")
endif()
if(WIN32 AND DEFINED ENV{DOTNET_ROOT_x86} AND NOT "$ENV{DOTNET_ROOT_x86}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{DOTNET_ROOT_x86}" _dotnet_root_x86_path)
  list(APPEND _dotnet_host_roots "${_dotnet_root_x86_path}")
endif()
foreach(candidate IN ITEMS
    "/usr/share/dotnet"
    "/usr/local/share/dotnet"
    "/opt/homebrew/share/dotnet"
    "/opt/homebrew/opt/dotnet/libexec"
    "C:/Program Files/dotnet"
    "C:/Program Files (x86)/dotnet")
  file(TO_CMAKE_PATH "${candidate}" candidate_path)
  list(APPEND _dotnet_host_roots "${candidate_path}")
endforeach()
list(REMOVE_DUPLICATES _dotnet_host_roots)

set(_dotnet_host_candidates)
foreach(root IN LISTS _dotnet_host_roots)
  file(GLOB _root_candidates LIST_DIRECTORIES TRUE "${root}/packs/Microsoft.NETCore.App.Host.${_dotnet_host_rid}/*/runtimes/${_dotnet_host_rid}/native")
  if(_root_candidates)
    list(APPEND _dotnet_host_candidates ${_root_candidates})
  endif()
endforeach()
list(SORT _dotnet_host_candidates COMPARE NATURAL ORDER DESCENDING)

set(DotNetHost_INCLUDE_DIR)
set(DotNetHost_LIBRARY)
set(DotNetHost_RUNTIME_LIBRARY)
foreach(candidate IN LISTS _dotnet_host_candidates)
  if(EXISTS "${candidate}/nethost.h")
    if(WIN32)
      set(_candidate_library "${candidate}/nethost.lib")
      set(_candidate_runtime "${candidate}/nethost.dll")
    elseif(APPLE)
      set(_candidate_library "${candidate}/libnethost.a")
      if(NOT EXISTS "${_candidate_library}")
        set(_candidate_library "${candidate}/libnethost.dylib")
      endif()
      set(_candidate_runtime "${candidate}/libnethost.dylib")
    else()
      set(_candidate_library "${candidate}/libnethost.a")
      if(NOT EXISTS "${_candidate_library}")
        set(_candidate_library "${candidate}/libnethost.so")
      endif()
      set(_candidate_runtime "${candidate}/libnethost.so")
    endif()

    if(EXISTS "${_candidate_library}")
      set(DotNetHost_INCLUDE_DIR "${candidate}")
      set(DotNetHost_LIBRARY "${_candidate_library}")
      if(EXISTS "${_candidate_runtime}" AND NOT DotNetHost_LIBRARY MATCHES "\\.(a|lib)$")
        set(DotNetHost_RUNTIME_LIBRARY "${_candidate_runtime}")
      elseif(WIN32 AND EXISTS "${_candidate_runtime}")
        set(DotNetHost_RUNTIME_LIBRARY "${_candidate_runtime}")
      else()
        unset(DotNetHost_RUNTIME_LIBRARY)
      endif()
      break()
    endif()
  endif()
endforeach()

find_package_handle_standard_args(DotNetHost
  REQUIRED_VARS DotNetHost_INCLUDE_DIR DotNetHost_LIBRARY
  FAIL_MESSAGE [=[
Could not locate the .NET nethost SDK pack for ${_dotnet_host_rid}.
${_dotnet_host_requirements_message}
Install a .NET SDK that provides the host pack for ${_dotnet_host_rid}
and ensure DOTNET_ROOT points to the installation.
]=])

if(DotNetHost_FOUND AND NOT TARGET DotNetHost::nethost)
  if(WIN32 AND DotNetHost_RUNTIME_LIBRARY)
    add_library(DotNetHost::nethost SHARED IMPORTED)
    set_target_properties(DotNetHost::nethost PROPERTIES
      IMPORTED_IMPLIB "${DotNetHost_LIBRARY}"
      IMPORTED_LOCATION "${DotNetHost_RUNTIME_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${DotNetHost_INCLUDE_DIR}")
  else()
    add_library(DotNetHost::nethost UNKNOWN IMPORTED)
    set_target_properties(DotNetHost::nethost PROPERTIES
      IMPORTED_LOCATION "${DotNetHost_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${DotNetHost_INCLUDE_DIR}")
  endif()
endif()
