cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED OPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD)
  message(FATAL_ERROR "OPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD is required")
endif()
if(NOT DEFINED OPENMS_THERMO_BRIDGE_VENDOR_DIR)
  message(FATAL_ERROR "OPENMS_THERMO_BRIDGE_VENDOR_DIR is required")
endif()
if(NOT DEFINED OPENMS_THERMO_BRIDGE_THERMO_COMMIT)
  message(FATAL_ERROR "OPENMS_THERMO_BRIDGE_THERMO_COMMIT is required")
endif()
if(NOT DEFINED OPENMS_THERMO_BRIDGE_THERMO_VERSION)
  message(FATAL_ERROR "OPENMS_THERMO_BRIDGE_THERMO_VERSION is required")
endif()

file(MAKE_DIRECTORY "${OPENMS_THERMO_BRIDGE_VENDOR_DIR}")

set(package_names
  "thermofisher.commoncore.backgroundsubtraction.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
  "thermofisher.commoncore.data.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
  "thermofisher.commoncore.massprecisionestimator.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
  "thermofisher.commoncore.randomaccessreaderplugin.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
  "thermofisher.commoncore.rawfilereader.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg")

foreach(package_name IN LISTS package_names)
  set(package_path "${OPENMS_THERMO_BRIDGE_VENDOR_DIR}/${package_name}")
  if(EXISTS "${package_path}")
    continue()
  endif()

  if(NOT OPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD)
    message(FATAL_ERROR
      "Required vendor package is missing: ${package_path}\n"
      "Provide the Thermo .nupkg files manually or configure with -DOPENMS_THERMO_BRIDGE_ENABLE_VENDOR_DOWNLOAD=ON.")
  endif()

  set(package_url "https://raw.githubusercontent.com/thermofisherlsms/RawFileReader/${OPENMS_THERMO_BRIDGE_THERMO_COMMIT}/Libs/NetCore/Net8/${package_name}")
  message(STATUS "Downloading ${package_name}")
  file(DOWNLOAD "${package_url}" "${package_path}" STATUS download_status TLS_VERIFY ON)
  list(GET download_status 0 download_code)
  list(GET download_status 1 download_message)
  if(NOT download_code EQUAL 0)
    file(REMOVE "${package_path}")
    message(FATAL_ERROR "Failed to download ${package_name}: ${download_message}")
  endif()
endforeach()
