add_library(duck_export_support SHARED filesystem.c map.c repository.c sha256.c source_json.c source_xml.cpp source_make.c mapping.c sample.c choke.c curves.c effects.c bake.c resample.c recipe.c runtime.c history_api.c package.c
  "${PROJECT_SOURCE_DIR}/vendor/yyjson-0.12.0/yyjson.c"
  "${PROJECT_SOURCE_DIR}/vendor/pugixml-1.15/pugixml.cpp"
  "${PROJECT_SOURCE_DIR}/vendor/sqlite-3.50.4/sqlite3.c")
target_compile_features(duck_export_support PRIVATE c_std_17)
target_compile_definitions(duck_export_support PRIVATE DES_BUILD SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED)
target_include_directories(duck_export_support PRIVATE "${PROJECT_SOURCE_DIR}/vendor/sqlite-3.50.4"
  "${PROJECT_SOURCE_DIR}/vendor/yyjson-0.12.0" "${PROJECT_SOURCE_DIR}/vendor/pugixml-1.15")
target_link_libraries(duck_export_support PRIVATE SDL3::SDL3)
set_target_properties(duck_export_support PROPERTIES PREFIX "" OUTPUT_NAME db
  C_VISIBILITY_PRESET hidden)
if(APPLE)
  set_target_properties(duck_export_support PROPERTIES INSTALL_NAME_DIR "@rpath" MACOSX_RPATH ON)
endif()
if(MINGW)
  target_link_options(duck_export_support PRIVATE -static-libgcc -static-libstdc++)
elseif(NOT WIN32)
  target_link_libraries(duck_export_support PRIVATE m ${CMAKE_DL_LIBS})
endif()
