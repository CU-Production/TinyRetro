set(LIBMGBA_ONLY ON)
set(M_CORE_GB OFF)
include(FetchContent)
FetchContent_Declare(mgba
    GIT_REPOSITORY https://github.com/mgba-emu/mgba
    GIT_TAG 0.10.5
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(mgba)

add_executable(TinyGBA WIN32 main.c)
target_link_libraries(TinyGBA mgba)
