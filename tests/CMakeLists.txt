if(NOT WIN32)
  message(FATAL_ERROR "The desktop regression fixtures currently require Windows. The core CTest suite runs independently.")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(TEST_BIN "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(TEST_LIBRARIES duckversion duckcore duckgui duckmidi_io duckmidi_input fluidsynth duckmedia ducksamplesynth winhttp)

function(test_dependencies TARGET)
  target_include_directories(${TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/${IMGUI}" "${PROJECT_SOURCE_DIR}/${IMGUI}/backends" "${ICON_OUTPUT}")
  target_link_libraries(${TARGET} PRIVATE ${TEST_LIBRARIES})
  if(MSVC)
    target_compile_options(${TARGET} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${TARGET} PRIVATE -UNDEBUG)
  endif()
endfunction()

get_target_property(EDITOR_SOURCES duckmidi SOURCES)
list(FILTER EDITOR_SOURCES EXCLUDE REGEX "(src/main\\.c|src/audio\\.c|tools/app\\.rc)$")
list(TRANSFORM EDITOR_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")
add_library(editor_test_objects OBJECT ${EDITOR_SOURCES})
test_dependencies(editor_test_objects)
foreach(AUDIO audio recording_feedback_audio event_controls_audio visualizers_audio)
  if(AUDIO STREQUAL "audio")
    set(AUDIO_SOURCE "${PROJECT_SOURCE_DIR}/src/audio.c")
  else()
    set(AUDIO_SOURCE "${AUDIO}.c")
  endif()
  add_library(${AUDIO}_test_objects OBJECT ${AUDIO_SOURCE})
  test_dependencies(${AUDIO}_test_objects)
endforeach()

add_library(editor_mouse_test_objects OBJECT editor_mouse_test.cpp)
test_dependencies(editor_mouse_test_objects)
foreach(SPECIAL source_workflow pattern_workflow recording_feedback visualizers)
  add_library(${SPECIAL}_mouse_test_objects OBJECT editor_mouse_test.cpp)
  string(TOUPPER "${SPECIAL}" DEFINE)
  target_compile_definitions(${SPECIAL}_mouse_test_objects PRIVATE "DM_TEST_${DEFINE}")
  test_dependencies(${SPECIAL}_mouse_test_objects)
endforeach()

function(desktop_test NAME AUDIO)
  add_executable(${NAME} ${NAME}.c $<TARGET_OBJECTS:editor_test_objects>)
  test_dependencies(${NAME})
  set(MOUSE editor_mouse_test_objects)
  foreach(SPECIAL source_workflow pattern_workflow recording_feedback visualizers)
    if(NAME STREQUAL "${SPECIAL}_test")
      set(MOUSE ${SPECIAL}_mouse_test_objects)
    endif()
  endforeach()
  target_sources(${NAME} PRIVATE $<TARGET_OBJECTS:${MOUSE}> $<TARGET_OBJECTS:${AUDIO}_test_objects>)
  set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_BIN}" LINKER_LANGUAGE CXX)
  if(MINGW)
    target_link_options(${NAME} PRIVATE -static-libgcc -static-libstdc++)
  endif()
  add_dependencies(${NAME} duckmidi)
  add_custom_command(TARGET ${NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${FLUID_DLLS} ${FFMPEG_DLLS} "$<TARGET_FILE:SDL3::SDL3>" "${TEST_BIN}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BIN}/plugins"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:m64export>" "${TEST_BIN}/plugins/"
    COMMAND_EXPAND_LISTS VERBATIM)
  if(EXISTS "${COMPILER_BIN}/libwinpthread-1.dll")
    add_custom_command(TARGET ${NAME} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${COMPILER_BIN}/libwinpthread-1.dll" "${TEST_BIN}" VERBATIM)
  endif()
  # MANUAL builds the target without registering it with CTest.
  if(NOT "MANUAL" IN_LIST ARGN)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      LABELS desktop RUN_SERIAL TRUE FIXTURES_REQUIRED soundfont_fixtures)
  endif()
endfunction()

foreach(NAME editor recording window transport_shortcuts dialog edit_gestures ports_editor
    workspace_navigation selection_history automation_transfer lfo_shapes event_clipboard
    source_workflow channel_row live_edit_editor interface event_lanes midi_inspector export_plugins backlog)
  desktop_test(${NAME}_test audio)
endforeach()
set_tests_properties(dialog_test PROPERTIES DISABLED TRUE)
foreach(NAME recording_feedback pattern_workflow channel_routing toolbar_followup todo_b todo_c todo_d
    frame_performance)
  desktop_test(${NAME}_test recording_feedback_audio)
endforeach()
desktop_test(event_controls_test event_controls_audio)
desktop_test(midi_recording_test recording_feedback_audio)
add_library(midi_input_performance_audio_test_objects OBJECT midi_input_performance_audio.c)
test_dependencies(midi_input_performance_audio_test_objects)
desktop_test(midi_input_performance_test midi_input_performance_audio MANUAL)
desktop_test(visualizers_test visualizers_audio)
desktop_test(samples_editor_test audio)
target_sources(samples_editor_test PRIVATE samples_editor_ui.cpp)
set_tests_properties(samples_editor_test PROPERTIES FIXTURES_REQUIRED
  "soundfont_fixtures;sample_import_audio")
add_custom_command(TARGET samples_editor_test POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:sampletool>" "${TEST_BIN}/plugins/"
  VERBATIM)
target_sources(export_plugins_test PRIVATE export_plugins_ui.cpp native_repository_fixture.c)
target_sources(todo_b_test PRIVATE todo_b_ui.cpp)
target_sources(todo_c_test PRIVATE todo_c_ui.cpp)

foreach(NAME audio_test audio_loop_test live_edit_audio_test midi_input_audio_test)
  add_executable(${NAME} ${NAME}.c)
  if(NAME STREQUAL "audio_test")
    target_sources(${NAME} PRIVATE $<TARGET_OBJECTS:audio_test_objects>)
  endif()
  test_dependencies(${NAME})
  set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_BIN}" LINKER_LANGUAGE CXX)
  if(MINGW)
    target_link_options(${NAME} PRIVATE -static-libgcc -static-libstdc++)
  endif()
  add_dependencies(${NAME} editor_test)
  add_test(NAME ${NAME} COMMAND ${NAME})
  set_tests_properties(${NAME} PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS audio RUN_SERIAL TRUE FIXTURES_REQUIRED soundfont_fixtures)
endforeach()

add_test(NAME generate_soundfont COMMAND "${Python3_EXECUTABLE}" tools/generate_test_soundfont.py)
add_test(NAME generate_programs COMMAND "${Python3_EXECUTABLE}" tools/generate_test_soundfont.py --all-programs)
set_tests_properties(generate_soundfont generate_programs PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" FIXTURES_SETUP soundfont_fixtures RUN_SERIAL TRUE)

set(REGISTRY_FIXTURES "${PROJECT_SOURCE_DIR}/build/development/export-registry-fixtures")
foreach(VARIANT fixture bad-abi bad-schema missing-entry allocation-failure z-duplicate)
  add_library(plugin_${VARIANT} MODULE export_plugin_fixture.c)
  set_target_properties(plugin_${VARIANT} PROPERTIES PREFIX "" OUTPUT_NAME "${VARIANT}"
    LIBRARY_OUTPUT_DIRECTORY "${REGISTRY_FIXTURES}" RUNTIME_OUTPUT_DIRECTORY "${REGISTRY_FIXTURES}")
  if(MINGW)
    target_link_options(plugin_${VARIANT} PRIVATE -static-libgcc)
  endif()
endforeach()
target_compile_definitions(plugin_bad-abi PRIVATE FIXTURE_ABI=5)
target_compile_definitions(plugin_bad-schema PRIVATE FIXTURE_BAD_UI)
target_compile_definitions(plugin_missing-entry PRIVATE FIXTURE_MISSING_ENTRY)
target_compile_definitions(plugin_allocation-failure PRIVATE FIXTURE_CREATE_FAIL)
target_compile_definitions(plugin_z-duplicate PRIVATE FIXTURE_DUPLICATE)
add_library(plugin_tool_fixture MODULE tool_plugin_fixture.c)
set_target_properties(plugin_tool_fixture PROPERTIES PREFIX "" OUTPUT_NAME "sample-tool-fixture"
  LIBRARY_OUTPUT_DIRECTORY "${REGISTRY_FIXTURES}" RUNTIME_OUTPUT_DIRECTORY "${REGISTRY_FIXTURES}")
add_custom_target(export_registry_fixtures
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BIN}/plugins"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:duck_export_support>" "${TEST_BIN}/plugins/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:duck_export_support>" "${REGISTRY_FIXTURES}/"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:m64export>" "${REGISTRY_FIXTURES}/m64.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:plugin_fixture>" "${TEST_BIN}/plugins/fixture.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:ootexport>" "${TEST_BIN}/plugins/"
  DEPENDS m64export ootexport plugin_fixture plugin_tool_fixture plugin_bad-abi plugin_bad-schema plugin_missing-entry
    plugin_allocation-failure plugin_z-duplicate VERBATIM)
add_dependencies(export_plugins_test export_registry_fixtures)
add_executable(export_registry_test export_registry_test.c "${PROJECT_SOURCE_DIR}/src/export_plugins.c")
test_dependencies(export_registry_test)
set_target_properties(export_registry_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_BIN}")
add_dependencies(export_registry_test editor_test export_registry_fixtures)
add_test(NAME export_registry_test COMMAND export_registry_test)
set_tests_properties(export_registry_test PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS plugins RUN_SERIAL TRUE)

# Renders the user guide's screenshots; run through tools/update-guide-images.ps1, not CTest.
add_executable(guide_captures "${PROJECT_SOURCE_DIR}/tools/guide-captures.cpp"
  $<TARGET_OBJECTS:editor_test_objects> $<TARGET_OBJECTS:audio_test_objects>)
test_dependencies(guide_captures)
set_target_properties(guide_captures PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_BIN}"
  LINKER_LANGUAGE CXX)
if(MINGW)
  target_link_options(guide_captures PRIVATE -static-libgcc -static-libstdc++)
endif()
add_dependencies(guide_captures editor_test)
