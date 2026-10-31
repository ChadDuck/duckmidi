cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/delivery-time.cmake")

# Deliveries go to build/duckmidi-vVERSION-TYPE, replacing an earlier delivery of the same version
# and type. Files are staged first, so a failed install leaves the earlier delivery in place.
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Use the CMake deliver target from a configured build.")
endif()

if(NOT DELIVERY_TYPE MATCHES "^(Full|Core)$")
  message(FATAL_ERROR "Deliveries need DELIVERY_TYPE=Full or Core.")
endif()

unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP EPOCH "%s" UTC)
duck_delivery_stamp(STAMP "${EPOCH}")
file(REAL_PATH "${SOURCE_DIR}/build" BUILD_ROOT)
set(STAGING "${BUILD_ROOT}/development/delivery-staging-${STAMP}-v${VERSION}")
set(DESTINATION "${BUILD_ROOT}/duckmidi-v${VERSION}-${DELIVERY_TYPE}")

foreach(PATH_TO_CHECK STAGING DESTINATION)
  cmake_path(IS_PREFIX BUILD_ROOT "${${PATH_TO_CHECK}}" NORMALIZE INSIDE_BUILD)

  if(NOT INSIDE_BUILD OR (PATH_TO_CHECK STREQUAL "STAGING" AND EXISTS "${STAGING}"))
    message(FATAL_ERROR "Delivery paths must be new locations inside build/: ${${PATH_TO_CHECK}}")
  endif()
endforeach()

if(EXISTS "${DESTINATION}" OR IS_SYMLINK "${DESTINATION}")
  file(REAL_PATH "${DESTINATION}" RESOLVED_DESTINATION)

  if(IS_SYMLINK "${DESTINATION}" OR NOT RESOLVED_DESTINATION STREQUAL DESTINATION)
    message(FATAL_ERROR "Refusing to replace a redirected delivery folder: ${DESTINATION}")
  endif()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}"
  --prefix "${STAGING}" --strip RESULT_VARIABLE STATUS)

if(NOT STATUS EQUAL 0)
  message(FATAL_ERROR "Package installation failed; staged files remain under build/development/.")
endif()

if(EXISTS "${DESTINATION}")
  file(REMOVE_RECURSE "${DESTINATION}")

  if(EXISTS "${DESTINATION}")
    message(FATAL_ERROR "Could not remove the previous delivery: ${DESTINATION}")
  endif()
endif()

file(RENAME "${STAGING}" "${DESTINATION}" NO_REPLACE RESULT STATUS)

if(NOT STATUS STREQUAL "0")
  message(FATAL_ERROR "Could not publish the new delivery: ${STATUS}")
endif()

message(STATUS "Delivered ${DESTINATION}")
