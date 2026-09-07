cmake_minimum_required(VERSION 3.21)

project(OpenMSThermoBridge VERSION 0.3.0 LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(OPENMS_THERMO_BRIDGE_BUILD_CLI "Build the thermo_host CLI executable." ON)
option(OPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD "Download Thermo vendor packages during the build." OFF)
set(OPENMS_THERMO_BRIDGE_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/thermo-feed" CACHE PATH "Directory containing the Thermo vendor .nupkg files.")
set(OPENMS_THERMO_BRIDGE_THERMO_COMMIT "80963674b5c10e58236da63023ad6fa0264bbb00" CACHE STRING "RawFileReader commit used for vendor package snapshots.")
set(OPENMS_THERMO_BRIDGE_THERMO_VERSION "8.0.37" CACHE STRING "Thermo RawFileReader package version.")
set(OPENMS_THERMO_BRIDGE_SAMPLE_RAW_URL "https://raw.githubusercontent.com/HegemanLab/small-thermo-raw-file-examples/cfac6a25b44913273dd458d5f5db9b20b0c1107d/ginkgotoxin-ms-positive.raw" CACHE STRING "Public sample RAW file used by the test suite.")
option(OPENMS_THERMO_BRIDGE_DOWNLOAD_TEST_DATA "Download public sample RAW data when BUILD_TESTING is enabled." ON)
set(OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR "" CACHE PATH "Path to a pre-built managed directory (skips dotnet publish).")
option(OPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED "Automatically download pre-built managed artifacts from GitHub Releases." OFF)

if(NOT OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR AND NOT OPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED)
  find_program(DOTNET_EXECUTABLE NAMES dotnet
    HINTS
      "/opt/homebrew/opt/dotnet/bin"
      "/opt/homebrew/bin"
    REQUIRED)
  execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" --list-sdks
    RESULT_VARIABLE OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_RESULT
    OUTPUT_VARIABLE OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_OUTPUT
    ERROR_VARIABLE OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_RESULT EQUAL 0
     OR OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_OUTPUT MATCHES "^[ \t\r\n]*$")
    message(FATAL_ERROR
      "OpenMSThermoBridge: building ThermoWrapperManaged.csproj locally requires a .NET SDK because CMake runs "
      "'${DOTNET_EXECUTABLE} publish'. The detected dotnet executable did not report any installed SDKs via "
      "'${DOTNET_EXECUTABLE} --list-sdks'.\n"
      "Install a matching .NET SDK (for example .NET 8), or use "
      "-DOPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR=/path/to/prebuilt or "
      "-DOPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED=ON to skip the local managed publish step.\n"
      "Even with pre-built managed artifacts, the native bridge still needs the platform-specific nethost headers "
      "and library, which are usually provided by the .NET SDK/host pack rather than a runtime-only install.\n"
      "dotnet --list-sdks stderr: ${OPENMS_THERMO_BRIDGE_DOTNET_SDK_LIST_ERROR}")
  endif()
else()
  find_program(DOTNET_EXECUTABLE NAMES dotnet)
endif()
find_package(DotNetHost REQUIRED)

set(OPENMS_THERMO_BRIDGE_MANAGED_DIR "${CMAKE_CURRENT_BINARY_DIR}/openms_thermo_bridge_managed")

# ── Pre-built managed artifact support ───────────────────────────────────────
# Task 3: auto-download pre-built managed artifacts from GitHub Releases.
if(OPENMS_THERMO_BRIDGE_DOWNLOAD_PREBUILT_MANAGED AND NOT OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR)
  # Determine platform identifier for the release asset.
  if(WIN32)
    set(_openms_thermo_bridge_prebuilt_platform "win-x64")
  elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
      set(_openms_thermo_bridge_prebuilt_platform "osx-arm64")
    else()
      set(_openms_thermo_bridge_prebuilt_platform "osx-x64")
    endif()
  else()
    set(_openms_thermo_bridge_prebuilt_platform "linux-x64")
  endif()

  set(_openms_thermo_bridge_prebuilt_tag "v${PROJECT_VERSION}")
  set(_openms_thermo_bridge_prebuilt_zip_name
      "openms-thermo-bridge-managed-${_openms_thermo_bridge_prebuilt_platform}-${_openms_thermo_bridge_prebuilt_tag}.zip")
  set(_openms_thermo_bridge_prebuilt_url
      "https://github.com/OpenMS/openms-thermo-bridge/releases/download/${_openms_thermo_bridge_prebuilt_tag}/${_openms_thermo_bridge_prebuilt_zip_name}")
  set(_openms_thermo_bridge_prebuilt_zip
      "${CMAKE_CURRENT_BINARY_DIR}/prebuilt-managed/${_openms_thermo_bridge_prebuilt_zip_name}")
  set(_openms_thermo_bridge_prebuilt_extract_dir
      "${CMAKE_CURRENT_BINARY_DIR}/prebuilt-managed/extracted")

  message(STATUS "OpenMSThermoBridge: Downloading pre-built managed artifact from ${_openms_thermo_bridge_prebuilt_url}")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/prebuilt-managed")
  if(NOT EXISTS "${_openms_thermo_bridge_prebuilt_zip}")
    file(DOWNLOAD
      "${_openms_thermo_bridge_prebuilt_url}"
      "${_openms_thermo_bridge_prebuilt_zip}"
      STATUS _openms_thermo_bridge_download_status
      TLS_VERIFY ON)
    list(GET _openms_thermo_bridge_download_status 0 _openms_thermo_bridge_download_code)
    list(GET _openms_thermo_bridge_download_status 1 _openms_thermo_bridge_download_message)
    if(NOT _openms_thermo_bridge_download_code EQUAL 0)
      message(FATAL_ERROR
        "OpenMSThermoBridge: Failed to download pre-built managed artifact: ${_openms_thermo_bridge_download_message}\n"
        "URL: ${_openms_thermo_bridge_prebuilt_url}\n"
        "Either provide -DOPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR=/path/to/extracted or ensure a .NET 8 SDK is "
        "available so CMake can build the managed bridge itself.")
    endif()
  else()
    message(STATUS "OpenMSThermoBridge: Using cached pre-built artifact at ${_openms_thermo_bridge_prebuilt_zip}")
  endif()

  file(MAKE_DIRECTORY "${_openms_thermo_bridge_prebuilt_extract_dir}")
  if(NOT EXISTS "${_openms_thermo_bridge_prebuilt_extract_dir}/ThermoWrapperManaged.dll")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${_openms_thermo_bridge_prebuilt_zip}"
      WORKING_DIRECTORY "${_openms_thermo_bridge_prebuilt_extract_dir}"
      RESULT_VARIABLE _openms_thermo_bridge_extract_result)
    if(NOT _openms_thermo_bridge_extract_result EQUAL 0)
      message(FATAL_ERROR "OpenMSThermoBridge: Failed to extract pre-built managed artifact.")
    endif()
  endif()

  set(OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR "${_openms_thermo_bridge_prebuilt_extract_dir}"
      CACHE PATH "Path to a pre-built managed directory (skips dotnet publish)." FORCE)
  message(STATUS "OpenMSThermoBridge: Using downloaded pre-built managed artifacts at ${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}")
endif()

# Task 2: use a manually supplied or auto-downloaded pre-built managed directory.
if(OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR)
  if(NOT EXISTS "${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}/ThermoWrapperManaged.dll")
    message(FATAL_ERROR
      "OpenMSThermoBridge: OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR is set to "
      "'${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}' but ThermoWrapperManaged.dll was not found there.")
  endif()
  if(NOT EXISTS "${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}/ThermoWrapperManaged.runtimeconfig.json")
    message(FATAL_ERROR
      "OpenMSThermoBridge: OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR is set to "
      "'${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}' but ThermoWrapperManaged.runtimeconfig.json was not found there.")
  endif()
  set(OPENMS_THERMO_BRIDGE_MANAGED_DIR "${OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR}")
  message(STATUS "OpenMSThermoBridge: Using pre-built managed directory: ${OPENMS_THERMO_BRIDGE_MANAGED_DIR}")
endif()
# ── End pre-built managed artifact support ────────────────────────────────────
set(OPENMS_THERMO_BRIDGE_TEST_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/testdata")
set(OPENMS_THERMO_BRIDGE_NUGET_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridge.NuGet.config")
set(OpenMSThermoBridge_MANAGED_DIR "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}" CACHE INTERNAL "Managed runtime directory for OpenMSThermoBridge.")

if(NOT OPENMS_THERMO_BRIDGE_PREBUILT_MANAGED_DIR)
  set(OPENMS_THERMO_BRIDGE_MANAGED_FILES
      "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}/ThermoWrapperManaged.dll"
      "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}/ThermoWrapperManaged.runtimeconfig.json")

  file(TO_CMAKE_PATH "${OPENMS_THERMO_BRIDGE_VENDOR_DIR}" OPENMS_THERMO_BRIDGE_VENDOR_DIR_CMAKE_PATH)
  file(CONFIGURE
    OUTPUT "${OPENMS_THERMO_BRIDGE_NUGET_CONFIG}"
    CONTENT "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<configuration>\n  <packageSources>\n    <clear />\n    <add key=\"openms-thermo-bridge-vendor\" value=\"@OPENMS_THERMO_BRIDGE_VENDOR_DIR_CMAKE_PATH@\" />\n    <add key=\"nuget.org\" value=\"https://api.nuget.org/v3/index.json\" />\n  </packageSources>\n</configuration>\n"
    @ONLY)

  include(OpenMSThermoBridgeFunctions)

  openms_thermo_bridge_vendor_package_names(OPENMS_THERMO_BRIDGE_VENDOR_PACKAGE_NAMES)
  set(OPENMS_THERMO_BRIDGE_VENDOR_PACKAGE_FILES)
  foreach(package_name IN LISTS OPENMS_THERMO_BRIDGE_VENDOR_PACKAGE_NAMES)
    list(APPEND OPENMS_THERMO_BRIDGE_VENDOR_PACKAGE_FILES "${OPENMS_THERMO_BRIDGE_VENDOR_DIR}/${package_name}")
  endforeach()

  add_custom_target(openms_thermo_bridge_vendor_packages
    COMMAND "${CMAKE_COMMAND}"
            -DOPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD=${OPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD}
            -DOPENMS_THERMO_BRIDGE_VENDOR_DIR=${OPENMS_THERMO_BRIDGE_VENDOR_DIR}
            -DOPENMS_THERMO_BRIDGE_THERMO_COMMIT=${OPENMS_THERMO_BRIDGE_THERMO_COMMIT}
            -DOPENMS_THERMO_BRIDGE_THERMO_VERSION=${OPENMS_THERMO_BRIDGE_THERMO_VERSION}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadThermoPackages.cmake"
    BYPRODUCTS ${OPENMS_THERMO_BRIDGE_VENDOR_PACKAGE_FILES}
    COMMENT "Preparing Thermo vendor packages"
    VERBATIM)

  add_custom_command(
    OUTPUT ${OPENMS_THERMO_BRIDGE_MANAGED_FILES}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}"
    COMMAND "${DOTNET_EXECUTABLE}" publish "${CMAKE_CURRENT_SOURCE_DIR}/ThermoWrapperManaged.csproj"
            -c Release
            --nologo
            --self-contained false
            -o "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}"
            --configfile "${OPENMS_THERMO_BRIDGE_NUGET_CONFIG}"
    DEPENDS openms_thermo_bridge_vendor_packages
            "${CMAKE_CURRENT_SOURCE_DIR}/ThermoWrapperManaged.csproj"
            "${CMAKE_CURRENT_SOURCE_DIR}/ThermoWrapperManaged.cs"
            "${CMAKE_CURRENT_SOURCE_DIR}/ThermoMetadata.cs"
            "${OPENMS_THERMO_BRIDGE_NUGET_CONFIG}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Publishing managed Thermo bridge"
    VERBATIM)

  add_custom_target(openms_thermo_bridge_managed ALL DEPENDS ${OPENMS_THERMO_BRIDGE_MANAGED_FILES})
else()
  include(OpenMSThermoBridgeFunctions)
  add_custom_target(openms_thermo_bridge_managed ALL)
endif()

add_library(openms_thermo_bridge src/thermo_bridge.cpp src/cv_mapping.cpp)
add_library(OpenMSThermoBridge::openms_thermo_bridge ALIAS openms_thermo_bridge)

# Generate a standard export header so the public API is visible from shared
# library builds regardless of the parent project's visibility preset.
include(GenerateExportHeader)
generate_export_header(openms_thermo_bridge
  EXPORT_MACRO_NAME     OPENMS_THERMO_BRIDGE_EXPORT
  EXPORT_FILE_NAME      openms_thermo_bridge/openms_thermo_bridge_export.h
  BASE_NAME             OPENMS_THERMO_BRIDGE)

# Opt-in to hiding symbols by default so only OPENMS_THERMO_BRIDGE_EXPORT
# symbols are exported.  Callers that set CMAKE_CXX_VISIBILITY_PRESET=hidden
# (e.g. OpenMS) do not need to override the target's visibility manually.
set_target_properties(openms_thermo_bridge PROPERTIES
  CXX_VISIBILITY_PRESET     hidden
  VISIBILITY_INLINES_HIDDEN ON)

target_compile_features(openms_thermo_bridge PUBLIC cxx_std_17)
target_include_directories(openms_thermo_bridge
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(openms_thermo_bridge PRIVATE DotNetHost::nethost)
if(NOT WIN32)
  target_link_libraries(openms_thermo_bridge PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(openms_thermo_bridge openms_thermo_bridge_managed)

if(OPENMS_THERMO_BRIDGE_BUILD_CLI)
  add_executable(thermo_host src/thermo_host_main.cpp)
  target_link_libraries(thermo_host PRIVATE openms_thermo_bridge)
  openms_thermo_bridge_copy_runtime_files(TARGET thermo_host)
  install(TARGETS thermo_host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG QUIET)
  if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)
  endif()

  file(MAKE_DIRECTORY "${OPENMS_THERMO_BRIDGE_TEST_DATA_DIR}")
  set(OPENMS_THERMO_BRIDGE_SAMPLE_RAW_FILE "${OPENMS_THERMO_BRIDGE_TEST_DATA_DIR}/ginkgotoxin-ms-positive.raw")
  if(OPENMS_THERMO_BRIDGE_DOWNLOAD_TEST_DATA)
    file(DOWNLOAD
      "${OPENMS_THERMO_BRIDGE_SAMPLE_RAW_URL}"
      "${OPENMS_THERMO_BRIDGE_SAMPLE_RAW_FILE}"
      STATUS OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_STATUS
      TLS_VERIFY ON)
    list(GET OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_STATUS 0 OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_CODE)
    list(GET OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_STATUS 1 OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_MESSAGE)
    if(NOT OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_CODE EQUAL 0)
      message(FATAL_ERROR "Failed to download sample RAW file: ${OPENMS_THERMO_BRIDGE_SAMPLE_DOWNLOAD_MESSAGE}")
    endif()
  endif()

  add_executable(thermo_bridge_tests tests/test_thermo_bridge.cpp)
  target_compile_features(thermo_bridge_tests PRIVATE cxx_std_17)
  target_link_libraries(thermo_bridge_tests PRIVATE openms_thermo_bridge Catch2::Catch2WithMain)
  target_compile_definitions(thermo_bridge_tests PRIVATE
    OPENMS_THERMO_BRIDGE_TEST_SAMPLE_RAW="$<IF:$<BOOL:${OPENMS_THERMO_BRIDGE_DOWNLOAD_TEST_DATA}>,${OPENMS_THERMO_BRIDGE_SAMPLE_RAW_FILE},>"
    OPENMS_THERMO_BRIDGE_TEST_EXPECTED_SCAN_COUNT=90)
  openms_thermo_bridge_copy_runtime_files(TARGET thermo_bridge_tests)

  add_executable(cv_mapping_tests tests/test_cv_mapping.cpp)
  target_compile_features(cv_mapping_tests PRIVATE cxx_std_17)
  target_link_libraries(cv_mapping_tests PRIVATE openms_thermo_bridge Catch2::Catch2WithMain)

  include(Catch)
  catch_discover_tests(thermo_bridge_tests)
  catch_discover_tests(cv_mapping_tests)
endif()

install(TARGETS openms_thermo_bridge EXPORT OpenMSThermoBridgeTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openms_thermo_bridge/openms_thermo_bridge_export.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/openms_thermo_bridge")
install(DIRECTORY "${OPENMS_THERMO_BRIDGE_MANAGED_DIR}/" DESTINATION "${CMAKE_INSTALL_LIBDIR}/openms_thermo_bridge/managed")

install(EXPORT OpenMSThermoBridgeTargets
  FILE OpenMSThermoBridgeTargets.cmake
  NAMESPACE OpenMSThermoBridge::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OpenMSThermoBridge")

set(OPENMS_THERMO_BRIDGE_CONFIG_MANAGED_DIR "\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/openms_thermo_bridge/managed")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenMSThermoBridgeHelpers.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeHelpers.cmake"
  @ONLY)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenMSThermoBridgeConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OpenMSThermoBridge")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeConfigVersion.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/OpenMSThermoBridgeHelpers.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindDotNetHost.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OpenMSThermoBridge")
