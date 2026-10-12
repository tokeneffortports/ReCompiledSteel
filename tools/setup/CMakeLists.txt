# The setup program: RealSteelSetup.exe (Windows) / realsteel-setup (Linux).
# Standalone - no ReXGlue dependency - so it also builds on its own:
#   cmake -S tools/setup -B tools/setup/build && cmake --build tools/setup/build
cmake_minimum_required(VERSION 3.25)
project(realsteel_setup LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RS_SETUP_SOURCES stfs.cpp install.cpp setup_common.cpp)
if(WIN32)
    add_executable(realsteel_setup WIN32 ${RS_SETUP_SOURCES} setup_win.cpp)
    set_target_properties(realsteel_setup PROPERTIES OUTPUT_NAME "RealSteelSetup")
    target_compile_definitions(realsteel_setup PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(realsteel_setup PRIVATE comctl32 ole32 shell32 user32 gdi32 uuid)
else()
    add_executable(realsteel_setup ${RS_SETUP_SOURCES} setup_posix.cpp)
    set_target_properties(realsteel_setup PROPERTIES OUTPUT_NAME "realsteel-setup")
endif()
