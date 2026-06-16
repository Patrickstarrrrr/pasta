#
# FindCUDD.cmake - Locate CUDD either via a packaged config or by header+lib
#

include(GNUInstallDirs)

# Try upstream/system CONFIG package first.
find_package(CUDD CONFIG QUIET HINTS ${CUDD_HOME} $ENV{CUDD_HOME} ${CUDD_DIR} $ENV{CUDD_DIR})
if(CUDD_FOUND)
  if(NOT CUDD_FIND_QUIETLY)
    message(STATUS "Found upstream/system CUDD package (CUDDConfig.cmake)")
  endif()
else()
  if(NOT CUDD_FIND_QUIETLY)
    message(STATUS "Failed to find upstream/system CUDD package; reverting to manual search")
  endif()

  find_library(
    CUDD_LIBRARY_DIR
    NAMES cudd libcudd
    HINTS ${CUDD_HOME} $ENV{CUDD_HOME} ${CUDD_DIR} $ENV{CUDD_DIR}
    PATH_SUFFIXES lib ${CMAKE_INSTALL_LIBDIR}
  )

  find_path(
    CUDD_INCLUDE_DIR
    NAMES cudd.h cuddObj.hh
    HINTS ${CUDD_HOME} $ENV{CUDD_HOME} ${CUDD_DIR} $ENV{CUDD_DIR}
    PATH_SUFFIXES include ${CMAKE_INSTALL_INCLUDEDIR}
  )

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(
    CUDD
    REQUIRED_VARS CUDD_INCLUDE_DIR CUDD_LIBRARY_DIR
  )

  if(CUDD_FOUND)
    add_library(cudd::libcudd UNKNOWN IMPORTED)
    set_target_properties(cudd::libcudd PROPERTIES IMPORTED_LOCATION "${CUDD_LIBRARY_DIR}")
    set_target_properties(cudd::libcudd PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CUDD_INCLUDE_DIR}")
    message(STATUS "Manually found CUDD (libs: ${CUDD_LIBRARY_DIR}; headers: ${CUDD_INCLUDE_DIR})")
    set(CUDD_LIBRARIES cudd::libcudd)
  elseif(CUDD_FIND_REQUIRED)
    message(FATAL_ERROR "Failed to find CUDD library files/public headers!")
  elseif(NOT CUDD_FIND_QUIETLY)
    message(WARNING "Failed to manually find CUDD libraries/headers")
  endif()
endif()
