#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libopenmpt::libopenmpt" for configuration "Debug"
set_property(TARGET libopenmpt::libopenmpt APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(libopenmpt::libopenmpt PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/openmpt.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_DEBUG "MPG123::libmpg123;Vorbis::vorbisfile;Vorbis::vorbis"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/openmpt.dll"
  )

list(APPEND _cmake_import_check_targets libopenmpt::libopenmpt )
list(APPEND _cmake_import_check_files_for_libopenmpt::libopenmpt "${_IMPORT_PREFIX}/debug/lib/openmpt.lib" "${_IMPORT_PREFIX}/debug/bin/openmpt.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
