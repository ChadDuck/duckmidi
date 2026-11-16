add_library(duck_db SHARED filesystem.c repository.c runtime.c history_api.c package.c
  "${PROJECT_SOURCE_DIR}/vendor/sqlite-3.50.4/sqlite3.c")
target_compile_features(duck_db PRIVATE c_std_17)
target_compile_definitions(duck_db PRIVATE DES_BUILD SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED)
target_include_directories(duck_db PRIVATE "${PROJECT_SOURCE_DIR}/vendor/sqlite-3.50.4")
target_link_libraries(duck_db PUBLIC duck_common PRIVATE SDL3::SDL3)
set_target_properties(duck_db PROPERTIES PREFIX "" OUTPUT_NAME db
  C_VISIBILITY_PRESET hidden)
if(APPLE)
  set_target_properties(duck_db PROPERTIES INSTALL_NAME_DIR "@rpath" MACOSX_RPATH ON)
endif()
if(MINGW)
  target_link_options(duck_db PRIVATE -static-libgcc)
elseif(NOT WIN32)
  target_link_libraries(duck_db PRIVATE m ${CMAKE_DL_LIBS})
endif()
