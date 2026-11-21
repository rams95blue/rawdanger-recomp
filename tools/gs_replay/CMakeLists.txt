# gs_replay: renders GS captures (.gsr) offline with ps2recomp's GS frontend + CPU backend.
#   cmake -S tools/gs_replay -B work/gs_replay_build && cmake --build work/gs_replay_build --config Release
cmake_minimum_required(VERSION 3.20)
project(gs_replay CXX)
set(CMAKE_CXX_STANDARD 20)
set(PS2X ${CMAKE_CURRENT_LIST_DIR}/../../ps2recomp/ps2xRuntime)
add_executable(gs_replay
    gs_replay.cpp
    ${PS2X}/src/lib/gs/gs_frontend.cpp
    ${PS2X}/src/lib/gs/gs_cpu_backend.cpp
    ${PS2X}/src/lib/gs/ps2_gs_memory.cpp)
target_include_directories(gs_replay PRIVATE ${PS2X}/include ${PS2X}/src/lib ${CMAKE_CURRENT_LIST_DIR}/../../ps2recomp/ps2xIOP/include)
if(MSVC)
    target_compile_options(gs_replay PRIVATE /arch:AVX2 /O2)
    set_property(TARGET gs_replay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    target_compile_options(gs_replay PRIVATE -msse4.1 -O2)
endif()
