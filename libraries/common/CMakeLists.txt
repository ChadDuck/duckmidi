# Utilities shared by the history database, the N64 exporter library and the exporters, compiled
# into each library that uses them and never exported from a plugin.
add_library(duck_common STATIC common.c map.c paths.c sha256.c)
target_compile_features(duck_common PRIVATE c_std_17)
set_target_properties(duck_common PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
