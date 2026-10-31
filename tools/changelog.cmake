# Generates the packaged changelog from CHANGELOG.md in the project root. The current version must
# have its own "## VERSION" section with a numbered list, so every build records what changed.
set(CHANGELOG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md")
set(CHANGELOG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/changelog/CHANGELOG.md")

if(NOT EXISTS "${CHANGELOG_SOURCE}")
  message(FATAL_ERROR "CHANGELOG.md is missing from the project root.")
endif()

file(READ "${CHANGELOG_SOURCE}" CHANGELOG_TEXT)
string(REPLACE "\r" "" CHANGELOG_TEXT "${CHANGELOG_TEXT}")
string(FIND "${CHANGELOG_TEXT}" "\n## ${DUCKMIDI_DELIVERY_VERSION}\n\n1. " CHANGELOG_SECTION)

if(CHANGELOG_SECTION EQUAL -1)
  message(FATAL_ERROR "CHANGELOG.md needs a \"## ${DUCKMIDI_DELIVERY_VERSION}\" section with a numbered list.")
endif()

# configure_file reruns CMake whenever the source changes, so each build carries the latest record.
configure_file("${CHANGELOG_SOURCE}" "${CHANGELOG_OUTPUT}" COPYONLY)
install(FILES "${CHANGELOG_OUTPUT}" DESTINATION changelog)
