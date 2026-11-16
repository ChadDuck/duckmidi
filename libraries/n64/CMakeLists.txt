# What the SM64 and OoT exporters share because of how the N64 audio engine works: port layout,
# mapping rows, controller curves and game-specific sample preparation. bake/ is the generic
# sample baking it calls into.
add_library(n64 STATIC mapping.c sample.c choke.c effects.c ports.c curves.c resample.c
  bake/bake.c bake/resample.c)
target_compile_features(n64 PRIVATE c_std_17)
target_link_libraries(n64 PUBLIC duck_common)
set_target_properties(n64 PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(NOT WIN32)
  target_link_libraries(n64 PUBLIC m)
endif()
