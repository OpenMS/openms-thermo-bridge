include(CMakeParseArguments)

function(openms_thermo_bridge_vendor_package_names out_var)
  set(${out_var}
    "thermofisher.commoncore.backgroundsubtraction.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
    "thermofisher.commoncore.data.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
    "thermofisher.commoncore.massprecisionestimator.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
    "thermofisher.commoncore.randomaccessreaderplugin.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
    "thermofisher.commoncore.rawfilereader.${OPENMS_THERMO_BRIDGE_THERMO_VERSION}.nupkg"
    PARENT_SCOPE)
endfunction()

function(openms_thermo_bridge_copy_runtime_files)
  set(options)
  set(one_value_args TARGET MANAGED_DIR)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

  if(NOT ARG_TARGET)
    message(FATAL_ERROR "openms_thermo_bridge_copy_runtime_files requires TARGET")
  endif()
  if(NOT ARG_MANAGED_DIR)
    set(ARG_MANAGED_DIR "${OpenMSThermoBridge_MANAGED_DIR}")
  endif()
  if(NOT ARG_MANAGED_DIR)
    message(FATAL_ERROR "openms_thermo_bridge_copy_runtime_files requires MANAGED_DIR or OpenMSThermoBridge_MANAGED_DIR")
  endif()

  set(_openms_thermo_bridge_runtime_commands
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "$<TARGET_FILE_DIR:${ARG_TARGET}>/managed"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${ARG_TARGET}>/managed"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ARG_MANAGED_DIR}" "$<TARGET_FILE_DIR:${ARG_TARGET}>/managed")
  if(DotNetHost_RUNTIME_LIBRARY)
    list(APPEND _openms_thermo_bridge_runtime_commands
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DotNetHost_RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${ARG_TARGET}>")
  endif()

  add_custom_command(TARGET ${ARG_TARGET} POST_BUILD
    ${_openms_thermo_bridge_runtime_commands}
    COMMENT "Copying managed bridge runtime files for ${ARG_TARGET}"
    VERBATIM)
endfunction()
