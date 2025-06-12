add_executable(TinyGB WIN32 main.c minigb_apu/minigb_apu.c)
target_compile_definitions(TinyGB PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)
