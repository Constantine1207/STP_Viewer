include_guard(GLOBAL)
# This file can be included before finding the application dependencies.
set(STP_DEPS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../.deps/install" CACHE PATH "Pinned native dependencies")
list(PREPEND CMAKE_PREFIX_PATH "${STP_DEPS_PREFIX}")
find_package(ZLIB REQUIRED)
find_path(STP_XLSXWRITER_INCLUDE_DIR NAMES xlsxwriter.h HINTS "${STP_DEPS_PREFIX}/include" REQUIRED)
find_library(STP_XLSXWRITER_LIBRARY NAMES xlsxwriter libxlsxwriter HINTS "${STP_DEPS_PREFIX}/lib" REQUIRED)
if(NOT TARGET STP::xlsxwriter)
  add_library(STP::xlsxwriter UNKNOWN IMPORTED GLOBAL)
  set_target_properties(STP::xlsxwriter PROPERTIES
    IMPORTED_LOCATION "${STP_XLSXWRITER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${STP_XLSXWRITER_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB")
endif()
