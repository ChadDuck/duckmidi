if(WIN32)
  set(FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/vendor/ffmpeg-9.0")
  set(FFMPEG_COMPONENTS avcodec avformat avutil swresample)
  set(FFMPEG_DLL_NAMES avcodec-63 avformat-63 avutil-61 swresample-7)
  foreach(INDEX RANGE 0 3)
    list(GET FFMPEG_COMPONENTS ${INDEX} COMPONENT)
    list(GET FFMPEG_DLL_NAMES ${INDEX} DLL)
    add_library(FFmpeg::${COMPONENT} SHARED IMPORTED)
    set_target_properties(FFmpeg::${COMPONENT} PROPERTIES
      IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/lib${COMPONENT}.dll.a"
      IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/${DLL}.dll"
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include")
    list(APPEND FFMPEG_LIBRARIES FFmpeg::${COMPONENT})
    list(APPEND FFMPEG_DLLS "${FFMPEG_ROOT}/bin/${DLL}.dll")
  endforeach()
else()
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec>=61.19 libavformat>=61 libavutil>=59 libswresample>=5)
  set(FFMPEG_LIBRARIES PkgConfig::FFMPEG)
endif()
