set(DIRECTXTEX_TAG oct2025)

# The patch reroutes the BC6H shader compile through `wine cmd /c`, so a
# native Windows build must not get it.
if(CMAKE_HOST_UNIX)
    set(DIRECTXTEX_PATCHES PATCHES wine-shader-compile.patch)
else()
    set(DIRECTXTEX_PATCHES "")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Microsoft/DirectXTex
    REF ${DIRECTXTEX_TAG}
    SHA512 8adca6e50dc5da91d2be0c9a644a3372f0c134ec80d71260d72dca79b2422d5eccae844b1b5d0eb4f335548730eb3b1faad4ba7e228f865c7688b60915e70efc
    HEAD_REF main
    ${DIRECTXTEX_PATCHES}
)

# DirectXTex compiles its own BC6H-encode shaders at build time; reuse the same fxc2-under-Wine
# stand-in as the directxtk port (duplicated -- vcpkg ports can't share build logic).
if(CMAKE_HOST_UNIX)
    file(WRITE "${SOURCE_PATH}/DirectXTex/Shaders/wine-compile-shaders.sh" "#!/bin/sh
set -u
CompileShadersOutput=\"$1\"
FxcTool=\"$2\"
shift 2
find \"$CompileShadersOutput\" -type f -name '*.inc' -delete 2>/dev/null || true
\"${CMAKE_COMMAND}\" -E env CompileShadersOutput=\"$CompileShadersOutput\" WINEDEBUG=-all LegacyShaderCompiler=\"$FxcTool\" wine cmd /c CompileShaders.cmd \"$@\" > \"$CompileShadersOutput/compileshaders.log\" 2>&1
if grep -q \"Got an error\" \"$CompileShadersOutput/compileshaders.log\"; then
    echo \"fxc2 reported shader compilation error(s); see $CompileShadersOutput/compileshaders.log\" >&2
    exit 1
fi
expected_count=\$(grep -c '^\"' \"$CompileShadersOutput/compileshaders.log\" 2>/dev/null || true)
[ -n \"\$expected_count\" ] || expected_count=0
inc_count=\$(find \"$CompileShadersOutput\" -type f -name '*.inc' 2>/dev/null | wc -l)
if [ \"\$expected_count\" -eq 0 ] || [ \"\$inc_count\" -ne \"\$expected_count\" ]; then
    echo \"Shader compilation produced \$inc_count .inc file(s), expected \$expected_count; see $CompileShadersOutput/compileshaders.log\" >&2
    exit 1
fi
exit 0
")
endif()

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        dx11 BUILD_DX11
        dx12 BUILD_DX12
        jpeg ENABLE_LIBJPEG_SUPPORT
        openexr ENABLE_OPENEXR_SUPPORT
        png ENABLE_LIBPNG_SUPPORT
        spectre ENABLE_SPECTRE_MITIGATION
        tools BUILD_TOOLS
)

set(EXTRA_OPTIONS -DBUILD_SAMPLE=OFF)

if(VCPKG_TARGET_IS_WINDOWS AND NOT (VCPKG_TARGET_IS_XBOX OR VCPKG_TARGET_IS_MINGW) AND NOT "dx12" IN_LIST FEATURES)
  list(APPEND EXTRA_OPTIONS "-DCMAKE_DISABLE_FIND_PACKAGE_directx-headers=TRUE")
endif()

if(CMAKE_HOST_UNIX AND ("dx11" IN_LIST FEATURES))
    find_program(FXC2_MINGW_CLANGXX NAMES x86_64-w64-mingw32-clang++ HINTS "$ENV{LLVM_MINGW_BIN}")
    if(NOT FXC2_MINGW_CLANGXX)
        message(FATAL_ERROR "${PORT}: cross-compiling from a Linux host needs an llvm-mingw toolchain (x86_64-w64-mingw32-clang++) on PATH, or pointed at via the LLVM_MINGW_BIN environment variable, to build the fxc2 shader-compiler stand-in. See https://github.com/WasabiIceCream/fxc2.")
    endif()

    vcpkg_from_github(
        OUT_SOURCE_PATH FXC2_SOURCE_PATH
        REPO WasabiIceCream/fxc2
        REF v1.0.0
        SHA512 1d5d67157983058e0bbad3f5fca4c56caa46f29b12c0cdd3efa6a3efc7b3bd5321b4dcbb1c2cdac17569d9271146a8eef61225fb4469f41123584f52178d643e
        HEAD_REF master
    )

    set(FXC2_EXE "${CURRENT_BUILDTREES_DIR}/fxc2.exe")
    execute_process(
        COMMAND "${FXC2_MINGW_CLANGXX}" -static "${FXC2_SOURCE_PATH}/fxc2.cpp" -o "${FXC2_EXE}"
        RESULT_VARIABLE FXC2_BUILD_RESULT
    )
    if(NOT FXC2_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "${PORT}: failed to build fxc2.exe from ${FXC2_SOURCE_PATH}")
    endif()
    file(COPY "${FXC2_SOURCE_PATH}/d3dcompiler_47.dll" DESTINATION "${CURRENT_BUILDTREES_DIR}")

    list(APPEND EXTRA_OPTIONS "-DDIRECTX_FXC_TOOL=${FXC2_EXE}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${FEATURE_OPTIONS} ${EXTRA_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH share/directxtex)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
