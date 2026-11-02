# Writes the version of this build: VERSION when one was set for the build, otherwise the build
# time in UTC as YYYY.MM.DD.HHMM. Run by the duckmidi_version target before every build; files are
# rewritten only when the version changes.
#   OUTPUT_DIR  receives duckmidi_version.h (DM_APP_VERSION) and version.txt
#   README_IN   optional; written to OUTPUT_DIR/README.txt with @DUCKMIDI_VERSION@ replaced
if(NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "Use the duckmidi_version target from a configured build.")
endif()

set(DUCKMIDI_VERSION "${VERSION}")

if(DUCKMIDI_VERSION STREQUAL "")
  string(TIMESTAMP DUCKMIDI_VERSION "%Y.%m.%d.%H%M" UTC)
endif()

file(CONFIGURE OUTPUT "${OUTPUT_DIR}/duckmidi_version.h"
  CONTENT "#define DM_APP_VERSION \"@DUCKMIDI_VERSION@\"\n" @ONLY)
file(CONFIGURE OUTPUT "${OUTPUT_DIR}/version.txt" CONTENT "@DUCKMIDI_VERSION@" @ONLY)

if(DEFINED README_IN)
  configure_file("${README_IN}" "${OUTPUT_DIR}/README.txt" @ONLY)
endif()
