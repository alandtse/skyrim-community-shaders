vcpkg_download_distfile(ARCHIVE
    URLS "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/${VERSION}"
    FILENAME "cppwinrt.${VERSION}.zip"
    SHA512 ADF9EC7059A58B3E0EB0057DE52900692F58305AEE8BA708D265D273A81127978BEB9BF2599B00855B61B725D4E6EB06206B66897EAEAEF1AEC83948D60BC293
)

vcpkg_extract_source_archive(
    src
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(CPPWINRT_ARCH win32)
else()
    set(CPPWINRT_ARCH ${VCPKG_TARGET_ARCHITECTURE})
endif()

set(CPPWINRT_TOOL "${src}/bin/cppwinrt.exe")

# `-base` needs no Windows SDK .winmd metadata (xwin stages none); this repo only uses the
# header-only winrt::com_ptr it provides. `-input local` needs %WinDir%\System32\WinMetadata.
find_program(WINE_EXECUTABLE NAMES wine)
if(NOT WINE_EXECUTABLE)
    message(FATAL_ERROR "${PORT}: cross-compiling from a Linux/macOS host needs `wine` on PATH to run cppwinrt.exe (base-headers-only generation, no Windows SDK required).")
endif()

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include")

# cppwinrt.exe treats a leading "/" as a Windows option switch, rejecting a raw Unix path;
# `winepath -w` converts it to the "Z:\..." form the tool accepts (Wine never translates argv).
find_program(WINEPATH_EXECUTABLE NAMES winepath)
if(NOT WINEPATH_EXECUTABLE)
    message(FATAL_ERROR "${PORT}: `winepath` (ships alongside `wine`) not found on PATH.")
endif()
vcpkg_execute_required_process(
    COMMAND "${WINEPATH_EXECUTABLE}" -w "${CURRENT_PACKAGES_DIR}/include"
    OUTPUT_VARIABLE CPPWINRT_OUTPUT_WINPATH_RAW
    WORKING_DIRECTORY "${CURRENT_PACKAGES_DIR}"
    LOGNAME "cppwinrt-winepath-${TARGET_TRIPLET}"
)
string(STRIP "${CPPWINRT_OUTPUT_WINPATH_RAW}" CPPWINRT_OUTPUT_WINPATH)

message(STATUS "Generating C++/WinRT base headers (-base, no Windows SDK)")
vcpkg_execute_required_process(
    COMMAND "${WINE_EXECUTABLE}" "${CPPWINRT_TOOL}" -base -output "${CPPWINRT_OUTPUT_WINPATH}" -verbose
    WORKING_DIRECTORY "${CURRENT_PACKAGES_DIR}"
    LOGNAME "cppwinrt-generate-${TARGET_TRIPLET}"
)

# cppwinrt.exe emits lowercase `#include <directxmath.h>`; a case-sensitive filesystem
# resolves it to xwin's SDK copy instead of vcpkg's, duplicate-defining every XM_* constant.
file(GLOB CPPWINRT_GENERATED_HEADERS "${CURRENT_PACKAGES_DIR}/include/winrt/base.h" "${CURRENT_PACKAGES_DIR}/include/winrt/winrt.ixx")
foreach(header IN LISTS CPPWINRT_GENERATED_HEADERS)
    file(READ "${header}" CPPWINRT_HEADER_CONTENTS)
    string(REPLACE "#include <directxmath.h>" "#include <DirectXMath.h>" CPPWINRT_HEADER_CONTENTS "${CPPWINRT_HEADER_CONTENTS}")
    file(WRITE "${header}" "${CPPWINRT_HEADER_CONTENTS}")
endforeach()

set(CPPWINRT_LIB "${src}/build/native/lib/${CPPWINRT_ARCH}/cppwinrt_fast_forwarder.lib")
file(INSTALL "${CPPWINRT_LIB}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL "${CPPWINRT_LIB}" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
endif()
file(INSTALL "${CPPWINRT_TOOL}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/cppwinrt")

set(tool_path "tools/cppwinrt/cppwinrt.exe")
set(lib_name "cppwinrt_fast_forwarder.lib")

configure_file("${CMAKE_CURRENT_LIST_DIR}/cppwinrt-config.cmake.in"
  "${CURRENT_PACKAGES_DIR}/share/${PORT}/${PORT}-config.cmake"
  @ONLY)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${src}/LICENSE")
