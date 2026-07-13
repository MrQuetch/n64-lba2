set(AIL_WIIU_SOURCES
    SOUND_WIIU_BACKEND.CPP
    STREAM_WIIU.CPP
    ${CMAKE_CURRENT_SOURCE_DIR}/../SDL/stb_vorbis.c
)
if(MVIDEO_BACKEND STREQUAL "smacker")
  list(APPEND AIL_WIIU_SOURCES VIDEO_AUDIO_WIIU.CPP)
  # SOUND_WIIU_BACKEND.CPP gates the VideoAudio_MixInto callback hook on this.
  target_compile_definitions(ail PRIVATE USE_MVIDEO_SMACKER)
endif()
target_sources(ail PRIVATE ${AIL_WIIU_SOURCES})
target_compile_definitions(ail PUBLIC USE_SOUND_WIIU)
# stb_vorbis.h lives in the SDL backend dir; shared on purpose.
target_include_directories(ail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../SDL)
