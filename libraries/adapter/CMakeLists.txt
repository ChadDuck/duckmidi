# What the bundled exporters' repository adapters compile in: the client over the runtime's
# DmeHistoryApi, recipe records, and byte-preserving edits of a checkout's JSON, XML and
# Makefiles. Static, so an exporter loads without plugins/db.
add_library(duck_adapter STATIC client.c recipe.c source_json.c source_make.c source_xml.cpp
  "${PROJECT_SOURCE_DIR}/vendor/yyjson-0.12.0/yyjson.c"
  "${PROJECT_SOURCE_DIR}/vendor/pugixml-1.15/pugixml.cpp")
target_compile_features(duck_adapter PRIVATE c_std_17)
target_include_directories(duck_adapter PRIVATE "${PROJECT_SOURCE_DIR}/vendor/yyjson-0.12.0"
  "${PROJECT_SOURCE_DIR}/vendor/pugixml-1.15")
target_link_libraries(duck_adapter PUBLIC duck_common)
set_target_properties(duck_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
if(MINGW)
  target_link_options(duck_adapter INTERFACE -static-libgcc -static-libstdc++)
endif()
