#.rst:
# FindNvAPI
# ---------
# Finds the NVIDIA NVAPI library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::NvAPI - The NVAPI library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  find_path(NVAPI_INCLUDE_DIR NAMES nvapi.h
                              HINTS ${DEPENDS_PATH}/include
                              ${${CORE_SYSTEM_NAME}_SEARCH_CONFIG})

  find_library(NVAPI_LIBRARY NAMES nvapi64
                             HINTS ${DEPENDS_PATH}/lib
                             ${${CORE_SYSTEM_NAME}_SEARCH_CONFIG})

  if(NOT VERBOSE_FIND)
    set(${CMAKE_FIND_PACKAGE_NAME}_FIND_QUIETLY TRUE)
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(NvAPI
                                    REQUIRED_VARS NVAPI_LIBRARY NVAPI_INCLUDE_DIR)

  if(NvAPI_FOUND)
    add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} UNKNOWN IMPORTED)
    set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
                                                                     IMPORTED_LOCATION "${NVAPI_LIBRARY}"
                                                                     INTERFACE_INCLUDE_DIRECTORIES "${NVAPI_INCLUDE_DIR}"
                                                                     INTERFACE_COMPILE_DEFINITIONS HAVE_NVAPI)
  endif()
endif()
