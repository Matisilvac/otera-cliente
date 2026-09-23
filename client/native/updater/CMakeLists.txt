cmake_minimum_required(VERSION 3.20)
project(otera_updater CXX)

# Dependencies come from vcpkg.json next to this file (baseline pinned to the
# one the game binaries use). IXWebSocket is built on mbedTLS on every system,
# so TLS always trusts the same pinned cacert.pem, not the OS store.
find_package(ixwebsocket CONFIG REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# Tests build a second binary with a local origin and key (a header that defines
# OTERA_UPDATER_ORIGIN and OTERA_UPDATER_PUBLIC_KEY). The shipped one never has
# it; verify_release.py checks that through `otera-updater version`.
set(OTERA_UPDATER_TEST_CONFIG "" CACHE FILEPATH "Test-only origin and key header")

add_executable(otera-updater otera_updater.cpp)
target_compile_features(otera-updater PRIVATE cxx_std_17)
target_link_libraries(otera-updater PRIVATE ixwebsocket::ixwebsocket MbedTLS::mbedtls nlohmann_json::nlohmann_json ZLIB::ZLIB)
if(OTERA_UPDATER_TEST_CONFIG)
  target_compile_definitions(otera-updater PRIVATE OTERA_UPDATER_TEST_CONFIG="${OTERA_UPDATER_TEST_CONFIG}")
endif()

if(WIN32)
  # No console window, static CRT: a single file that runs on a clean Windows.
  set_property(TARGET otera-updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_link_options(otera-updater PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
  target_link_libraries(otera-updater PRIVATE ws2_32 crypt32 bcrypt shlwapi)
elseif(APPLE)
  target_link_libraries(otera-updater PRIVATE "-framework Security" "-framework CoreFoundation")
else()
  target_link_libraries(otera-updater PRIVATE pthread)
endif()
