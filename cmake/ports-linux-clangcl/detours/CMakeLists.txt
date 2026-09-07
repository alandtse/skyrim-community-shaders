cmake_minimum_required(VERSION 3.20)
project(detours LANGUAGES CXX)

# Mirrors src/Makefile's OBJS list exactly (all target-processor disassembler
# variants are always built into one lib upstream, not selected per-triplet).
add_library(
    detours STATIC
    src/creatwth.cpp
    src/detours.cpp
    src/disasm.cpp
    src/image.cpp
    src/modules.cpp
    src/disolx86.cpp
    src/disolx64.cpp
    src/disolia64.cpp
    src/disolarm.cpp
    src/disolarm64.cpp
)
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x501)
target_include_directories(detours PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

include(GNUInstallDirs)
install(TARGETS detours ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/detours.h src/detver.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/detours)
