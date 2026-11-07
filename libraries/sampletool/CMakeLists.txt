add_library(sampletool MODULE sampletool.c)
target_compile_definitions(sampletool PRIVATE DME_BUILD_PLUGIN)
target_link_libraries(sampletool PRIVATE duckmedia SDL3::SDL3)
set_target_properties(sampletool PROPERTIES PREFIX "" OUTPUT_NAME "Samples"
  INSTALL_RPATH_USE_LINK_PATH TRUE C_VISIBILITY_PRESET hidden)
if(MINGW)
  target_link_options(sampletool PRIVATE -static-libgcc)
endif()
