# speech: the plugin and its tests, built on their own (the engine build takes the plugin
# from plugins/speech instead). Needs a krkrz_dev checkout for tp_stub and ncbind:
#
#   cmake -B build -DKRKRZ_DEV_DIR=/path/to/krkrz_dev && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(speech_repo C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SPEECH_PLUGIN "Build the plugin (needs krkrz_dev)" ON)

set(KRKRZ_DEV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../krkrz_dev CACHE PATH "A krkrz_dev checkout (for tp_stub and ncbind)")
set(TPSTUB_DIR ${KRKRZ_DEV_DIR}/src/plugins/tp_stub CACHE PATH "Path to tp_stub directory")
set(NCBIND_DIR ${KRKRZ_DEV_DIR}/src/plugins/ncbind CACHE PATH "Path to ncbind directory")
set(KRKRZ_VARIANT SDL CACHE STRING "The engine's build variant (SDL, WIN, LIB)")

if(SPEECH_PLUGIN)
    if(NOT EXISTS ${TPSTUB_DIR}/tp_stub.h)
        message(FATAL_ERROR "No tp_stub at ${TPSTUB_DIR}: set KRKRZ_DEV_DIR to a krkrz_dev checkout, or -DSPEECH_PLUGIN=OFF")
    endif()
    add_subdirectory(plugins/speech)
endif()

# The parts with no engine in them, and their tests
enable_testing()
add_library(speech_core STATIC plugins/speech/common.cpp plugins/speech/speech_posix.cpp)
target_include_directories(speech_core PUBLIC plugins/speech)
add_executable(speech_test tests/speech_test.cpp)
target_link_libraries(speech_test PRIVATE speech_core)
add_test(NAME speech_test COMMAND speech_test)
