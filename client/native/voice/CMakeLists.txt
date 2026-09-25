cmake_minimum_required(VERSION 3.20)
project(otera_voice C CXX)

# Dependencies come from vcpkg.json next to this file, with the same baseline as
# the game and the updater. miniaudio is a single header: capture and playback on
# every system (Core Audio, WASAPI, PulseAudio/ALSA) without linking anything.
# speexdsp does echo cancellation, noise suppression and the speech probability
# the gate uses; Opus is the codec. mbedTLS encrypts the UDP session with the
# relay (ChaCha20-Poly1305; the updater already builds it, libsodium's port needs
# autotools); IXWebSocket (no TLS: it only listens on 127.0.0.1) talks to the game.
find_package(Threads REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# The speexdsp port ships no CMake config, and Windows runners have no pkg-config.
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_echo.h REQUIRED)
find_library(SPEEXDSP_LIBRARY NAMES speexdsp libspeexdsp REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

add_library(voice_core STATIC voice_dsp.cpp voice_engine.cpp voice_net.cpp miniaudio_impl.c)
target_include_directories(voice_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${MINIAUDIO_INCLUDE_DIR} ${SPEEXDSP_INCLUDE_DIR})
target_compile_features(voice_core PUBLIC cxx_std_17)
target_link_libraries(voice_core PUBLIC Opus::opus ${SPEEXDSP_LIBRARY} MbedTLS::mbedcrypto Threads::Threads ${CMAKE_DL_LIBS})

add_executable(otera-voice otera_voice.cpp voice_serve.cpp)
target_link_libraries(otera-voice PRIVATE voice_core ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json)

if(WIN32)
  set_property(TARGET voice_core otera-voice PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_definitions(voice_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(voice_core PUBLIC ws2_32 bcrypt)
  # The game launches it: no console window. wmain gets the arguments as UTF-16, so
  # an install folder with accents in the user name arrives intact.
  target_link_options(otera-voice PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
elseif(APPLE)
  target_link_libraries(voice_core PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
else()
  # miniaudio opens PulseAudio/ALSA with dlopen at run time; it only needs libm here.
  target_link_libraries(voice_core PUBLIC m)
endif()
