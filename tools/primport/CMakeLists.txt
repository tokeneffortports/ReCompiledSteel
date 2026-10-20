# Fighter import from other Yuke's Xbox 360 games (tools/primport). Standalone
# C++20, no ReXGlue dependency; the setup program links the same sources.
#   cmake -S tools/primport -B tools/primport/build && cmake --build tools/primport/build
cmake_minimum_required(VERSION 3.25)
project(realsteel_primport LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RS_PRIMPORT_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/primport_core.cpp
    ${CMAKE_CURRENT_LIST_DIR}/primport_model.cpp
    ${CMAKE_CURRENT_LIST_DIR}/primport_icons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/primport_motion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/primport.cpp)

add_executable(rs_primport ${CMAKE_CURRENT_LIST_DIR}/cli.cpp ${RS_PRIMPORT_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/../setup/stfs.cpp)
set_target_properties(rs_primport PROPERTIES OUTPUT_NAME "rs-primport")
if(MSVC)
    target_compile_definitions(rs_primport PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
