#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "PortAudio::portaudio" for configuration "Release"
set_property(TARGET PortAudio::portaudio APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(PortAudio::portaudio PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/libportaudio.dll.a"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/libportaudio.dll"
  )

list(APPEND _cmake_import_check_targets PortAudio::portaudio )
list(APPEND _cmake_import_check_files_for_PortAudio::portaudio "${_IMPORT_PREFIX}/lib/libportaudio.dll.a" "${_IMPORT_PREFIX}/bin/libportaudio.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
