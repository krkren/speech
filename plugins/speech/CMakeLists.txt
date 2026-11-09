# The speech plugin. Built by the engine when this folder is in TVP_PLUGIN_FOLDERS (listed
# after krkrz_dev's plugins, whose tp_stub and ncbind it uses), or on its own through the
# CMakeLists.txt at the top of this repository.
set(TPSTUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tp_stub CACHE PATH "Path to tp_stub directory")
include(${TPSTUB_DIR}/krkrz.cmake)

# each backend's file builds only on its own platform (#if), the Objective-C++ one on Apple's
set(SPEECH_SOURCES main.cpp common.cpp speech_posix.cpp speech_windows.cpp speech_android.cpp speech_web.cpp)
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND SPEECH_SOURCES speech_apple.mm)
endif()

krkrz_plugin(speech NCBIND SOURCES ${SPEECH_SOURCES})
target_compile_features(speech PRIVATE cxx_std_17)
if(APPLE)
    set_source_files_properties(speech_apple.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(speech PRIVATE "-framework AVFoundation" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(speech PRIVATE ole32)
endif()
