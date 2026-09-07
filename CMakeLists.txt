cmake_minimum_required(VERSION 4.2)
cmake_policy(SET CMP0116 NEW)
set(CMAKE_POLICY_WARNING_CMP0116 OFF)

# devbench bridge — declared before project() so the vcpkg toolchain (loaded at project())
# sees the manifest-feature selection. The devbench-api dependency lives behind the
# `devbench-bridge` vcpkg feature, so -DDEVBENCH_BRIDGE=OFF actually drops it (not just the
# find_package/link/define below).
option(
    DEVBENCH_BRIDGE
    "Register the plugin's tools into the devbench test bench (requires the devbench-api port; runtime no-op when no devbench host is present)"
    ON
)
if(DEVBENCH_BRIDGE)
    list(APPEND VCPKG_MANIFEST_FEATURES "devbench-bridge")
endif()

# Unlike DEVBENCH_BRIDGE above, the vcpkg `effects11` port is unconditional (vcpkg.json), not a manifest feature.
option(
    ENABLE_EFFECTS11
    "Enable the Effects11 feature (ENB-style post-processing framework)."
    ON
)

project(
    # gersemi: ignore
    CommunityShaders
    VERSION 2.11.0
    LANGUAGES CXX
)

# default install path
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set_property(
        CACHE CMAKE_INSTALL_PREFIX
        PROPERTY VALUE "${CMAKE_CURRENT_BINARY_DIR}/aio"
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ########################################################################################################################
# ## Build options
# ########################################################################################################################
message("Options:")
option(
    AUTO_PLUGIN_DEPLOYMENT
    "Copy the build output and addons to env:CommunityShadersOutputDir."
    OFF
)
option(
    ZIP_TO_DIST
    "Zip the base mod and addons to their own 7z file in dist."
    ON
)
option(
    AIO_ZIP_TO_DIST
    "Zip the base mod and addons to a AIO 7z file in dist."
    ON
)
option(TRACY_SUPPORT "Enable support for tracy profiler" OFF)
option(
    BUILD_SHADER_TESTS
    "Build shader unit tests (runs automatically before packaging)"
    ON
)
option(
    AIO_INCLUDE_NON_AUTOUPLOAD
    "Include features whose .ini has autoupload != true in the AIO. Off by default — the AIO ships only release-ready features. Turn on for local dev builds that want every feature regardless of release status."
    OFF
)
message("\tAuto plugin deployment: ${AUTO_PLUGIN_DEPLOYMENT}")
message("\tZip to dist: ${ZIP_TO_DIST}")
message("\tAIO Zip to dist: ${AIO_ZIP_TO_DIST}")
message("\tTracy profiler: ${TRACY_SUPPORT}")
message("\tShader tests: ${BUILD_SHADER_TESTS}")
message("\tAIO include non-autoupload features: ${AIO_INCLUDE_NON_AUTOUPLOAD}")
message("\tEffects11: ${ENABLE_EFFECTS11}")

# #######################################################################################################################
# # Build version info from git
# #######################################################################################################################
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_DESCRIBE "unknown")
endif()
message("\tBuild version: ${GIT_DESCRIBE}")

# #######################################################################################################################
# # Add CMake features
# #######################################################################################################################
include(XSEPlugin)

# add_cxx_files() globs all of src/*.cpp|.h unconditionally; when Effects11 is
# off, drop its sources here rather than adding per-feature filtering to that
# shared helper, so it never reaches a shipped binary.
if(NOT ENABLE_EFFECTS11)
    get_target_property(_cs_all_sources "${PROJECT_NAME}" SOURCES)
    list(FILTER _cs_all_sources EXCLUDE REGEX "/src/Features/Effects11")
    set_target_properties(
        "${PROJECT_NAME}"
        PROPERTIES SOURCES "${_cs_all_sources}"
    )
    unset(_cs_all_sources)
endif()

# #######################################################################################################################
# # Find dependencies
# #######################################################################################################################
find_path(BSHOSHANY_THREAD_POOL_INCLUDE_DIRS "BS_thread_pool.hpp")
find_package(magic_enum CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")
find_package(pystring CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
find_package(unordered_dense CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_path(EXPRTK_INCLUDE_DIRS "exprtk.hpp" REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)

if(DEVBENCH_BRIDGE)
    find_package(devbench-api CONFIG REQUIRED)
endif()
if(ENABLE_EFFECTS11)
    find_package(effects11 CONFIG REQUIRED)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/Streamline)

find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")
find_library(DETOURS_LIBRARY detours REQUIRED)
include(FidelityFX-SDK)

target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE "$<$<BOOL:${TRACY_SUPPORT}>:TRACY_SUPPORT>"
)

file(
    GLOB FEATURE_SHADER_DIRS
    RELATIVE "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/features/*/Shaders"
)

foreach(_dir IN LISTS FEATURE_SHADER_DIRS)
    target_include_directories(
        ${PROJECT_NAME}
        PRIVATE "${CMAKE_SOURCE_DIR}/${_dir}"
    )
endforeach()

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${BSHOSHANY_THREAD_POOL_INCLUDE_DIRS}
        ${CLIB_UTIL_INCLUDE_DIRS}
        "${CMAKE_SOURCE_DIR}/package/Shaders"
        "${CMAKE_SOURCE_DIR}/extern/stb_image_write_hdr_png"
        ${DETOURS_INCLUDE_DIRS}
        ${EXPRTK_INCLUDE_DIRS}
)

# ImGuiVRHelper client API — fetched from upstream so the dependency and its
# pinned version are explicit instead of a vendored copy. Configures only the
# helper's api/ CMake target (SOURCE_SUBDIR) and skips its submodules. The
# INTERFACE target compiles the client-side handshake stub
# (GetImGuiVRHelperInterface001 — one SKSE messaging Dispatch on first call,
# cached vtable thereafter, no link dependency on the helper plugin; resolves at
# runtime via SKSE messaging) into SCS and exposes the SDK headers, inheriting
# our ImGui / DX11 / json. Override with
# -DFETCHCONTENT_SOURCE_DIR_IMGUIVRHELPER=<path> for local helper development.
include(FetchContent)
FetchContent_Declare(
    ImGuiVRHelper
    GIT_REPOSITORY https://github.com/alandtse/imgui-vr-helper.git
    GIT_TAG v1.7.0
    GIT_SUBMODULES "" SOURCE_SUBDIR api
)
FetchContent_MakeAvailable(ImGuiVRHelper)
target_link_libraries(${PROJECT_NAME} PRIVATE ImGuiVRHelper::api)

# stb_image_write impl (incl. ReShade's vendored HDR-PNG extension) builds as C, like
# upstream's deps/stb_impl.c: the extension passes unsigned char* where stb's
# encode_png_line takes signed char* (legal in C, an error in C++). Not picked up by the
# src/*.cpp glob, so add it explicitly; skip the C++ PCH and silence warnings for this
# third-party TU (the target builds with /W4 /WX). Keeps the vendored header pristine.
# The top-level project() enables only CXX; enable C explicitly for this .c TU.
enable_language(C)
set(STB_IMAGE_WRITE_IMPL "${CMAKE_SOURCE_DIR}/src/Utils/StbImageWriteImpl.c")
target_sources(${PROJECT_NAME} PRIVATE "${STB_IMAGE_WRITE_IMPL}")
set_source_files_properties(
    "${STB_IMAGE_WRITE_IMPL}"
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS
            "$<$<OR:$<C_COMPILER_ID:MSVC>,$<C_COMPILER_ID:Clang>>:/W0>"
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        Microsoft::CppWinRT
        magic_enum::magic_enum
        xbyak::xbyak
        nlohmann_json::nlohmann_json
        imgui::imgui
        EASTL
        Microsoft::DirectXTK
        Microsoft::DirectXTex
        pystring::pystring
        unordered_dense::unordered_dense
        efsw::efsw
        xxHash::xxhash
        Tracy::TracyClient
        Streamline
        d3d12.lib
        Microsoft::DirectX-Headers
        ${DETOURS_LIBRARY}
        windowsapp
        windowscodecs
)

# devbench bridge: opt-out via -DDEVBENCH_BRIDGE=OFF. When off, the port isn't required
# and DevBenchBridge.cpp compiles to an empty Install().
if(DEVBENCH_BRIDGE)
    target_link_libraries(${PROJECT_NAME} PRIVATE DevBench::API)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEVBENCH_BRIDGE_ENABLED)
endif()

if(ENABLE_EFFECTS11)
    target_link_libraries(${PROJECT_NAME} PRIVATE Microsoft::Effects11)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_EFFECTS11)
endif()

if(MSVC)
    # Large translation units (e.g. LightLimitFix/ShadowCasterUI.cpp pulling the
    # full feature + editor headers) exceed the default COFF section limit; /bigobj.
    target_compile_options(${PROJECT_NAME} PRIVATE "/bigobj")
endif()

# https://gitlab.kitware.com/cmake/cmake/-/issues/24922#note_1371990
if(MSVC_VERSION GREATER_EQUAL 1936 AND MSVC_IDE) # 17.6+
    # When using /std:c++latest, "Build ISO C++23 Standard Library Modules" defaults to "Yes".
    # Default to "No" instead.
    #
    # As of CMake 3.26.4, there isn't a way to control this property
    # (https://gitlab.kitware.com/cmake/cmake/-/issues/24922),
    # We'll use the MSBuild project system instead
    # (https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-file-structure)
    file(
        CONFIGURE
        OUTPUT "${CMAKE_BINARY_DIR}/Directory.Build.props"
        CONTENT
            [==[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <BuildStlModules>false</BuildStlModules>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]==]
        @ONLY
    )
endif()

# #######################################################################################################################
# # Feature version detection
# #######################################################################################################################
file(
    GLOB_RECURSE FEATURE_CONFIG_FILES
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "features/*/Shaders/Features/*.ini"
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FeatureStaging.cmake)

foreach(FEATURE_PATH ${FEATURE_CONFIG_FILES})
    get_filename_component(FEATURE ${FEATURE_PATH} NAME_WE)
    file(READ "${FEATURE_PATH}" CONFIG_VALUE)
    string(STRIP "${CONFIG_VALUE}" CONFIG_VALUE)
    if(CONFIG_VALUE)
        string(
            REGEX MATCH "[Vv]ersion = ([0-9]+)-([0-9]+)-([0-9]+)"
            _
            "${CONFIG_VALUE}"
        )
        if(
            DEFINED CMAKE_MATCH_1
            AND DEFINED CMAKE_MATCH_2
            AND DEFINED CMAKE_MATCH_3
        )
            set(ver_major ${CMAKE_MATCH_1})
            set(ver_minor ${CMAKE_MATCH_2})
            set(ver_patch ${CMAKE_MATCH_3})
            list(
                APPEND FEATURE_VERSIONS
                "\t\t{\"${FEATURE}\"sv, {${ver_major},${ver_minor},${ver_patch}}}"
            )
        else()
            message(
                WARNING
                "Feature config file '${FEATURE_PATH}' does not contain a valid version string. Skipping."
            )
        endif()
    else()
        message(
            WARNING
            "Feature config file '${FEATURE_PATH}' is empty or contains only whitespace. Skipping version detection for this feature."
        )
    endif()

    # Detect release stage from Alpha/Beta flags (cmake/FeatureStaging.cmake,
    # shared with the standalone CI emitter so there is one implementation).
    feature_ini_stage("${CONFIG_VALUE}" _STAGE_IS_ALPHA _STAGE_IS_BETA)
    if(_STAGE_IS_ALPHA)
        list(APPEND FEATURE_ALPHA_NAMES "\t\t\"${FEATURE}\"sv")
    elseif(_STAGE_IS_BETA)
        list(APPEND FEATURE_BETA_NAMES "\t\t\"${FEATURE}\"sv")
    endif()

    # Detect the Unreleased flag, which gates UI visibility independently of the
    # release stage above. Same line anchoring and CMAKE_MATCH_2 guard.
    set(_STAGE_UNRELEASED "")
    string(
        REGEX MATCH "(^|[\r\n])[ \t]*[Uu]nreleased[ \t]*=[ \t]*([A-Za-z0-9]+)"
        _STAGE_UNRELEASED_MATCH
        "${CONFIG_VALUE}"
    )
    if(_STAGE_UNRELEASED_MATCH)
        string(TOLOWER "${CMAKE_MATCH_2}" _STAGE_UNRELEASED)
    endif()
    if(_STAGE_UNRELEASED MATCHES "^(true|1|yes|on)$")
        list(APPEND FEATURE_UNRELEASED_NAMES "\t\t\"${FEATURE}\"sv")
    endif()

    # Detect core features by checking for the CORE marker file in the
    # feature root (features/<Folder>/CORE). FEATURE_PATH is
    # features/<Folder>/Shaders/Features/<ShortName>.ini, so the feature
    # root is three directories up.
    get_filename_component(_FEATURE_DIR "${FEATURE_PATH}" DIRECTORY)
    get_filename_component(_FEATURE_DIR "${_FEATURE_DIR}" DIRECTORY)
    get_filename_component(_FEATURE_DIR "${_FEATURE_DIR}" DIRECTORY)
    if(EXISTS "${_FEATURE_DIR}/CORE")
        list(APPEND FEATURE_CORE_NAMES "\t\t\"${FEATURE}\"sv")
    endif()
endforeach()

set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${FEATURE_CONFIG_FILES}"
)

string(REPLACE ";" ",\n" FEATURE_VERSIONS "${FEATURE_VERSIONS}")
string(REPLACE ";" ",\n" FEATURE_CORE_NAMES "${FEATURE_CORE_NAMES}")
string(REPLACE ";" ",\n" FEATURE_ALPHA_NAMES "${FEATURE_ALPHA_NAMES}")
string(REPLACE ";" ",\n" FEATURE_BETA_NAMES "${FEATURE_BETA_NAMES}")
string(REPLACE ";" ",\n" FEATURE_UNRELEASED_NAMES "${FEATURE_UNRELEASED_NAMES}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FeatureVersions.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/FeatureVersions.h
    @ONLY
)

target_sources(
    "${PROJECT_NAME}"
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cmake/FeatureVersions.h
)

# #######################################################################################################################
# # Theme presets
# #######################################################################################################################
set(THEMES_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/package/SKSE/Plugins/CommunityShaders/Themes"
)
file(GLOB THEME_JSON_FILES CONFIGURE_DEPENDS "${THEMES_DIR}/*.json")
set(THEME_PRESET_NAMES_LIST "")
foreach(f IN LISTS THEME_JSON_FILES)
    get_filename_component(name "${f}" NAME_WE)
    list(APPEND THEME_PRESET_NAMES_LIST "${name}")
endforeach()
list(SORT THEME_PRESET_NAMES_LIST)
list(LENGTH THEME_PRESET_NAMES_LIST THEME_PRESET_COUNT)
set(THEME_PRESET_NAMES "")
foreach(p IN LISTS THEME_PRESET_NAMES_LIST)
    set(THEME_PRESET_NAMES "${THEME_PRESET_NAMES}\t\t\"${p}\",\n")
endforeach()
string(REGEX REPLACE ",\n$" "" THEME_PRESET_NAMES "${THEME_PRESET_NAMES}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThemePresets.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/ThemePresets.h
    @ONLY
)

target_sources(
    "${PROJECT_NAME}"
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cmake/ThemePresets.h
)

# #######################################################################################################################
# # clang-format
# #######################################################################################################################

find_program(CLANG_FORMAT_PATH clang-format)
if(CLANG_FORMAT_PATH)
    add_custom_target(
        FORMAT_CODE
        COMMAND ${CLANG_FORMAT_PATH} -i -style=file ${CPP_SOURCES};${HLSL_FILES}
        COMMENT "Running clang format for cpp and hlsl files"
    )
endif()

# #######################################################################################################################
# # HLSL additional include directories for VS intellisense
# #######################################################################################################################

set(HLSL_INCLUDE_DIRS ${FEATURE_SHADER_DIRS} "package/Shaders")

set(HLSL_INCLUDE_JSON "")
foreach(dir IN LISTS HLSL_INCLUDE_DIRS)
    if(HLSL_INCLUDE_JSON STREQUAL "")
        set(HLSL_INCLUDE_JSON "    \"${dir}\"")
    else()
        set(HLSL_INCLUDE_JSON "${HLSL_INCLUDE_JSON},\n    \"${dir}\"")
    endif()
endforeach()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shadertoolsconfig.json.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/shadertoolsconfig.json"
    @ONLY
)

# #######################################################################################################################
# # Shader validation config generation
# #######################################################################################################################

# Add target to generate shader validation configuration files
# This requires hlslkit and valid Skyrim installations with recent log files
find_program(POWERSHELL_PATH pwsh powershell)
if(POWERSHELL_PATH)
    add_custom_target(
        generate_shader_configs
        COMMAND
            ${POWERSHELL_PATH} -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/.github/configs/generate-shader-configs.ps1"
            -OutputDir "${CMAKE_SOURCE_DIR}/.github/configs"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT
            "Generating shader validation configuration files from Skyrim log files"
    )
endif()

# #######################################################################################################################
# # Automatic deployment
# #######################################################################################################################
file(GLOB FEATURE_PATHS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/features/*)
# Drop empty leftover feature dirs (e.g. after a rename) so they do not create
# Package-* targets or get copied into AIO / install trees as empty folders.
set(_valid_feature_paths "")
foreach(_fpath IN LISTS FEATURE_PATHS)
    if(IS_DIRECTORY "${_fpath}")
        file(GLOB_RECURSE _feat_files LIST_DIRECTORIES FALSE "${_fpath}/*")
        if(_feat_files)
            list(APPEND _valid_feature_paths "${_fpath}")
        else()
            get_filename_component(_feat_name "${_fpath}" NAME)
            message(STATUS "Skipping empty feature directory: ${_feat_name}")
        endif()
    endif()
endforeach()
set(FEATURE_PATHS ${_valid_feature_paths})
unset(_valid_feature_paths)
unset(_feat_files)

# Effects11's sources aren't compiled in when ENABLE_EFFECTS11 is off (see the
# source filter above), so staging its ini/shaders to a local dev deploy only
# produces an orphaned-ini warning at runtime, never a working feature.
set(AIO_LOCAL_FEATURE_PATHS ${FEATURE_PATHS})
if(NOT ENABLE_EFFECTS11)
    list(FILTER AIO_LOCAL_FEATURE_PATHS EXCLUDE REGEX "/Effects11$")
endif()

string(TIMESTAMP UTC_NOW "%Y-%m-%dT%H-%MZ" UTC)

# Set AIO directory path used by multiple targets below
set(AIO_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio")

# feature_in_aio(): TRUE if the feature ships in the AIO (CORE, or its `.ini`
# sets autoupload/aio truthy). Shared with the standalone partition emitter and
# the CI consistency guard so all three agree on what the AIO contains.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AioPartition.cmake)

# Partition features into AIO-included / -excluded sets.
# AIO_INCLUDE_NON_AUTOUPLOAD=ON includes everything (local dev override) --
# except a feature whose sources aren't even compiled into this binary (see
# the ENABLE_EFFECTS11 source filter above): deploying its ini/shaders when
# the DLL has no matching Feature class just produces an orphaned-ini warning
# at runtime, not a working feature.
set(AIO_INCLUDED_FEATURE_PATHS "")
set(AIO_EXCLUDED_FEATURE_PATHS "")
foreach(_fpath IN LISTS FEATURE_PATHS)
    if(NOT IS_DIRECTORY "${_fpath}")
        continue()
    endif()
    if(NOT ENABLE_EFFECTS11 AND _fpath MATCHES "/Effects11$")
        list(APPEND AIO_EXCLUDED_FEATURE_PATHS "${_fpath}")
    elseif(AIO_INCLUDE_NON_AUTOUPLOAD)
        list(APPEND AIO_INCLUDED_FEATURE_PATHS "${_fpath}")
    else()
        feature_in_aio("${_fpath}" _ok)
        if(_ok)
            list(APPEND AIO_INCLUDED_FEATURE_PATHS "${_fpath}")
        else()
            list(APPEND AIO_EXCLUDED_FEATURE_PATHS "${_fpath}")
        endif()
    endif()
endforeach()
list(LENGTH AIO_INCLUDED_FEATURE_PATHS _aio_in_count)
list(LENGTH AIO_EXCLUDED_FEATURE_PATHS _aio_out_count)
message("\tAIO features included: ${_aio_in_count}")

# Build the paths (relative to AIO root) that the archive step deletes
# from the stage. We derive them from each feature's on-disk structure
# because the source folder, inner shader dir, and .ini basename can
# all differ (e.g. IBL ships ImageBasedLighting.ini under
# Shaders/Features, with shader code under Shaders/IBL/).
set(AIO_EXCLUDED_FEATURE_NAMES "")
set(AIO_EXCLUDED_STAGE_RELPATHS "")
foreach(_fpath IN LISTS AIO_EXCLUDED_FEATURE_PATHS)
    get_filename_component(_fname "${_fpath}" NAME)
    list(APPEND AIO_EXCLUDED_FEATURE_NAMES "${_fname}")

    if(EXISTS "${_fpath}/Shaders")
        file(
            GLOB _shader_subdirs
            LIST_DIRECTORIES TRUE
            RELATIVE "${_fpath}/Shaders"
            "${_fpath}/Shaders/*"
        )
        foreach(_sd IN LISTS _shader_subdirs)
            if(_sd STREQUAL "Features")
                continue()
            endif()
            if(IS_DIRECTORY "${_fpath}/Shaders/${_sd}")
                list(APPEND AIO_EXCLUDED_STAGE_RELPATHS "Shaders/${_sd}")
            endif()
        endforeach()
    endif()

    if(EXISTS "${_fpath}/Shaders/Features")
        file(
            GLOB _ini_files
            RELATIVE "${_fpath}/Shaders"
            "${_fpath}/Shaders/Features/*.ini"
        )
        foreach(_ini IN LISTS _ini_files)
            list(APPEND AIO_EXCLUDED_STAGE_RELPATHS "Shaders/${_ini}")
        endforeach()
    endif()

    # Strip the feature's non-shader staged assets (.esp/.esl, meshes/, etc.).
    # DESTINATION "." maps feature-root-relative paths straight to the AIO root;
    # enumerate files (not dirs) so shared trees like meshes/ survive for others.
    file(GLOB_RECURSE _nonshader_files RELATIVE "${_fpath}" "${_fpath}/*")
    foreach(_nf IN LISTS _nonshader_files)
        if(_nf MATCHES "^Shaders/" OR _nf STREQUAL "CORE")
            continue()
        endif()
        list(APPEND AIO_EXCLUDED_STAGE_RELPATHS "${_nf}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES AIO_EXCLUDED_STAGE_RELPATHS)

if(_aio_out_count GREATER 0)
    message(
        "\tAIO features excluded from release archive (autoupload != true, set AIO_INCLUDE_NON_AUTOUPLOAD=ON to override): ${_aio_out_count}"
    )
    foreach(_fname IN LISTS AIO_EXCLUDED_FEATURE_NAMES)
        message("\t\t- ${_fname}")
    endforeach()
    message(
        "\tNote: excluded features still build and validate; they are stripped only at AIO archive time."
    )
endif()

# Robocopy wrapper for Windows incremental file copy (used by deployment targets).
# We invoke through `cmd /c "<absolute-path>"` rather than the bare wrapper path because
# modern Windows refuses to execute scripts from the current directory without an explicit
# `.\` prefix, and CMake strips the absolute path on COMMAND args that live inside
# CMAKE_BINARY_DIR (the custom-build CWD). Treating the wrapper as an argument to cmd
# keeps the absolute path intact in the generated MSBuild command.
if(WIN32)
    set(ROBOCOPY_WRAPPER_PATH "${CMAKE_BINARY_DIR}/robocopy_wrapper.cmd")
    file(
        WRITE ${ROBOCOPY_WRAPPER_PATH}
        "@echo off\r\nrem Robocopy wrapper: forwards all args to robocopy and normalizes exit codes\r\nrobocopy %*\r\nset rc=%ERRORLEVEL%\r\nif %rc% GEQ 8 exit /b %rc%\r\nexit /b 0\r\n"
    )
    set(ROBOCOPY_WRAPPER cmd /c "${ROBOCOPY_WRAPPER_PATH}")
endif()

# #######################################################################################################################
# # CMake install() infrastructure for manual packaging
# #######################################################################################################################

# Append a '/' to install contents but not the directory itself. Uses
# the full FEATURE_PATHS (not the autoupload filter) so cross-feature
# shader #includes still resolve at build time.
set(FEATURE_PATHS_SLASH ${FEATURE_PATHS})
list(TRANSFORM FEATURE_PATHS_SLASH APPEND /)

# Install logic for AIO package
# To copy AIO package at a folder do `${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${AIO_DIR}`
# REMOVE_RECURSE below wipes the ENTIRE --prefix dir first, so it must only
# ever target the isolated AIO_DIR staging folder, never a live game install
# (e.g. --prefix pointed at Data/) -- the guard below enforces this.
install(
    CODE
        "
    if(NOT \"\${CMAKE_INSTALL_PREFIX}\" STREQUAL \"${AIO_DIR}\")
        message(FATAL_ERROR
            \"Refusing to install: --prefix must be \\\"${AIO_DIR}\\\" \"
            \"(got \\\"\${CMAKE_INSTALL_PREFIX}\\\"). This target recursively \"
            \"deletes its entire --prefix directory before staging the AIO \"
            \"package, so it must never be pointed at a live game install. \"
            \"Use BuildRelease.bat <PRESET>-WITH-AUTO-DEPLOYMENT or the \"
            \"DEPLOY_ALL/COPY_SHADERS targets to deploy to a game folder.\")
    endif()
    file(REMOVE_RECURSE \"\${CMAKE_INSTALL_PREFIX}\")
"
)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION SKSE/Plugins COMPONENT SKSE)
install(
    FILES $<TARGET_PDB_FILE:${PROJECT_NAME}>
    DESTINATION SKSE/Plugins
    COMPONENT SKSE
)
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/package/ ${FEATURE_PATHS_SLASH}
    DESTINATION .
    COMPONENT Shaders
    PATTERN "Tests" EXCLUDE
)
include(FeaturePackaging)
include(FidelityFX-Runtime)
include(Streamline-Runtime)
install(CODE "file(REMOVE \${CMAKE_INSTALL_PREFIX}/Core)" COMPONENT Shaders)

# #######################################################################################################################
# # Automatic AIO preparation (incremental copy system)
# #######################################################################################################################

if(AUTO_PLUGIN_DEPLOYMENT OR AIO_ZIP_TO_DIST)
    message("Preparing AIO package in ${AIO_DIR}")

    function(append_copy_if_different _cmds _srcs _src_base _dst_base)
        foreach(_src IN LISTS ${_srcs})
            file(RELATIVE_PATH _rel "${_src_base}" "${_src}")
            set(_dst "${_dst_base}/${_rel}")
            get_filename_component(_dst_dir "${_dst}" DIRECTORY)
            list(
                APPEND ${_cmds}
                COMMAND
                ${CMAKE_COMMAND}
                -E
                make_directory
                "${_dst_dir}"
                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${_src}"
                "${_dst}"
            )
        endforeach()
        set(${_cmds} ${${_cmds}} PARENT_SCOPE)
    endfunction()

    # Prepare AIO only when sources change. Gather package + feature files as
    # inputs so the prepare step runs only when something actually changed.
    # Shader files are intentionally excluded - copy_shaders.stamp owns those,
    # and including them here causes a race where two parallel custom-build
    # rules call cmake -E copy_if_different on the same destination file
    # (Permission denied on Windows when one process has it open for write).
    file(
        GLOB_RECURSE _AIO_PACKAGE_FILES
        LIST_DIRECTORIES FALSE
        "${CMAKE_SOURCE_DIR}/package/*"
    )
    list(FILTER _AIO_PACKAGE_FILES EXCLUDE REGEX "/Tests/")
    list(FILTER _AIO_PACKAGE_FILES EXCLUDE REGEX "/Shaders/")
    foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
        file(GLOB_RECURSE _tmp LIST_DIRECTORIES FALSE "${_fpath}/*")
        list(FILTER _tmp EXCLUDE REGEX "/Shaders/")
        list(APPEND _AIO_PACKAGE_FILES ${_tmp})
    endforeach()

    # Prepare AIO by copying files only when different. This avoids updating
    # timestamps for unchanged files and prevents downstream incremental
    # deploys from copying everything every build.
    set(_prepare_aio_cmds)
    # Written to a file, not a `-D` list: Ninja mis-quotes an escaped-semicolon
    # list with space-containing entries, truncating it to one garbled entry.
    set(_feature_paths_list_file
        "${CMAKE_CURRENT_BINARY_DIR}/feature_paths.txt"
    )
    set(_aio_source_paths
        ${AIO_LOCAL_FEATURE_PATHS}
        "${FFX_RUNTIME_FEATURE_ROOT}"
    )
    string(REPLACE ";" "\n" _feature_paths_file_content "${_aio_source_paths}")
    file(WRITE "${_feature_paths_list_file}" "${_feature_paths_file_content}")

    # Ensure SKSE/Plugins dir exists
    # Note: DLL and PDB are copied via POST_BUILD command to avoid race conditions
    list(
        APPEND _prepare_aio_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        "${AIO_DIR}/SKSE/Plugins"
    )
    list(
        APPEND _prepare_aio_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -DMODE=AIO
        -DAIO_DIR="${AIO_DIR}"
        -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
        -DFEATURE_PATHS_FILE="${_feature_paths_list_file}"
        -P
        "${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake"
    )

    # Copy package files (exclude test files from production packages, and
    # exclude shaders since copy_shaders.stamp owns those - see input-tracking
    # comment above for the race-condition rationale).
    file(
        GLOB_RECURSE _AIO_PACKAGE_SOURCE_FILES
        LIST_DIRECTORIES FALSE
        "${CMAKE_SOURCE_DIR}/package/*"
    )
    list(FILTER _AIO_PACKAGE_SOURCE_FILES EXCLUDE REGEX "/Tests/")
    list(FILTER _AIO_PACKAGE_SOURCE_FILES EXCLUDE REGEX "/Shaders/")
    append_copy_if_different(_prepare_aio_cmds _AIO_PACKAGE_SOURCE_FILES "${CMAKE_SOURCE_DIR}/package" "${AIO_DIR}")

    # Copy feature folders (files only; copy_shaders.stamp owns Shaders/
    # to avoid racing). All features copied — autoupload filter applies
    # only at archive time.
    #
    # The CORE marker is excluded from the per-feature copy: every feature
    # that has one would write to the same `${AIO_DIR}/CORE` path, and the
    # trailing `remove` below deletes it anyway. When MSBuild runs the copy
    # commands in parallel two features can race for the write and one
    # fails with "Permission denied", taking PREPARE_AIO down with it.
    foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
        if(EXISTS "${_fpath}")
            file(
                GLOB_RECURSE _feature_files
                LIST_DIRECTORIES FALSE
                "${_fpath}/*"
            )
            list(FILTER _feature_files EXCLUDE REGEX "/Shaders/")
            list(FILTER _feature_files EXCLUDE REGEX "/CORE$")
            append_copy_if_different(_prepare_aio_cmds _feature_files "${_fpath}" "${AIO_DIR}")
        endif()
    endforeach()

    # Remove CORE from AIO if it's left over from a previous build that pre-
    # dated the per-feature exclusion above. Cheap, idempotent.
    list(
        APPEND _prepare_aio_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        remove
        "${AIO_DIR}/CORE"
    )
    list(
        APPEND _prepare_aio_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        touch
        ${CMAKE_CURRENT_BINARY_DIR}/prepare_aio.stamp
    )

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/prepare_aio.stamp
            ${_prepare_aio_cmds}
        DEPENDS
            ${_AIO_PACKAGE_FILES}
            ${PROJECT_NAME}
            ${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake
    )

    # If AIO was manually (or programmatically) deleted, the stamp is stale —
    # clear it so cmake --build re-runs PREPARE_AIO to recreate the directory.
    if(NOT EXISTS "${AIO_DIR}")
        file(
            REMOVE
            "${CMAKE_CURRENT_BINARY_DIR}/prepare_aio.stamp"
            "${CMAKE_CURRENT_BINARY_DIR}/copy_shaders.stamp"
        )
    endif()

    add_custom_target(
        PREPARE_AIO
        ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/prepare_aio.stamp
    )

    # Copy DLL and PDB using POST_BUILD to avoid race conditions with file locking
    # This ensures the linker has fully released the files before we attempt to copy them
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/SKSE/Plugins"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${AIO_DIR}/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
            "${AIO_DIR}/SKSE/Plugins/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Copying built DLL and PDB to AIO package"
        VERBATIM
    )

    # Only copy shaders when HLSL files change; copy individually so unchanged
    # files do not get their timestamps updated.
    file(
        GLOB_RECURSE _package_shaders
        LIST_DIRECTORIES FALSE
        "${CMAKE_SOURCE_DIR}/package/Shaders/*"
    )
    # Exclude test files from production packages
    list(FILTER _package_shaders EXCLUDE REGEX "/Tests/")
    set(_shader_copy_cmds)

    # All (locally deployed) features' shaders are copied — cross-feature
    # #includes need the full tree at compile time (e.g. DistantTree.hlsl
    # pulls IBL/IBL.hlsli). Effects11 is excluded via AIO_LOCAL_FEATURE_PATHS
    # when ENABLE_EFFECTS11 is off, since nothing else includes its shaders.
    # Written to a file rather than passed as a `-D` list, same reason as
    # FEATURE_PATHS_FILE above (Ninja mis-quotes it).
    set(_feature_shader_paths "${FFX_RUNTIME_SHADER_ROOT}")
    foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
        if(EXISTS "${_fpath}/Shaders")
            list(APPEND _feature_shader_paths "${_fpath}/Shaders")
        endif()
    endforeach()
    set(_feature_shader_paths_list_file
        "${CMAKE_CURRENT_BINARY_DIR}/feature_shader_paths.txt"
    )
    string(
        REPLACE ";"
        "\n"
        _feature_shader_paths_file_content
        "${_feature_shader_paths}"
    )
    file(
        WRITE "${_feature_shader_paths_list_file}"
        "${_feature_shader_paths_file_content}"
    )

    list(
        APPEND _shader_copy_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -DMODE=SHADERS
        -DAIO_DIR="${AIO_DIR}"
        -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
        -DFEATURE_SHADER_PATHS_FILE="${_feature_shader_paths_list_file}"
        -P
        "${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake"
    )

    append_copy_if_different(_shader_copy_cmds _package_shaders "${CMAKE_SOURCE_DIR}/package/Shaders" "${AIO_DIR}/Shaders")
    append_copy_if_different(_shader_copy_cmds FFX_RUNTIME_FILES "${FFX_RUNTIME_SHADER_ROOT}" "${AIO_DIR}/Shaders")
    # Streamline-Runtime.cmake's payload is a build-tree dir, not a FEATURE_PATH, so
    # the glob below never discovers it -- wire it in explicitly, same as FFX above.
    append_copy_if_different(_shader_copy_cmds STREAMLINE_RUNTIME_FILES "${STREAMLINE_RUNTIME_DIRECTORY}" "${AIO_DIR}/${STREAMLINE_RUNTIME_RELATIVE_DIRECTORY}")
    append_copy_if_different(_shader_copy_cmds STREAMLINE_RUNTIME_DX12_FILES "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" "${AIO_DIR}/${STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY}")

    # Accumulated below and added to DEPENDS -- HLSL_FILES alone missed non-shader
    # feature payload (e.g. bundled DLLs), silently pinning stale binaries forever.
    set(_all_feature_shader_files)
    foreach(_fpath IN LISTS AIO_LOCAL_FEATURE_PATHS)
        if(EXISTS "${_fpath}/Shaders")
            file(
                GLOB_RECURSE _feat_shaders
                LIST_DIRECTORIES FALSE
                "${_fpath}/Shaders/*"
            )
            list(FILTER _feat_shaders EXCLUDE REGEX "/Tests/")
            append_copy_if_different(_shader_copy_cmds _feat_shaders "${_fpath}/Shaders" "${AIO_DIR}/Shaders")
            list(APPEND _all_feature_shader_files ${_feat_shaders})
        endif()
    endforeach()

    add_custom_command(
        OUTPUT copy_shaders.stamp
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders"
            ${_shader_copy_cmds}
        COMMAND ${CMAKE_COMMAND} -E touch copy_shaders.stamp
        DEPENDS
            ${HLSL_FILES}
            ${FFX_RUNTIME_FILES}
            ${STREAMLINE_RUNTIME_FILES}
            ${STREAMLINE_RUNTIME_DX12_FILES}
            ${_package_shaders}
            ${_all_feature_shader_files}
            ${CMAKE_SOURCE_DIR}/cmake/CleanupStaleEntries.cmake
        COMMENT "Copying changed shaders into AIO/Shaders"
    )

    # Standalone target for preparing shaders for CI validation
    # This allows shader validation to run without waiting for the full build
    add_custom_target(
        prepare_shaders
        DEPENDS copy_shaders.stamp
        COMMENT "Preparing shaders for validation"
    )

    # Incremental local validation: compile only the shaders affected by the
    # current working-tree diff (vs HEAD). Mirrors the CI incremental path but
    # derives the change set from git instead of a precomputed list. Override
    # the config with -DVALIDATE_CHANGED_CONFIG=... (defaults to Flatrim).
    set(VALIDATE_CHANGED_CONFIG
        "${CMAKE_SOURCE_DIR}/.github/configs/shader-validation.yaml"
        CACHE FILEPATH
        "hlslkit config used by the validate_changed target"
    )
    add_custom_target(
        validate_changed
        COMMAND
            ${CMAKE_COMMAND} -E env python
            "${CMAKE_SOURCE_DIR}/tools/validate_changed_shaders.py" --repo-root
            "${CMAKE_SOURCE_DIR}" -- hlslkit-compile --shader-dir
            "${AIO_DIR}/Shaders" --output-dir "${CMAKE_BINARY_DIR}/ShaderCache"
            --config "${VALIDATE_CHANGED_CONFIG}" --max-warnings 0
            --suppress-warnings X1519
        DEPENDS prepare_shaders
        USES_TERMINAL
        COMMENT "Validating only shaders affected by working-tree changes"
    )
endif()

# Automatic deployment to CommunityShaders output directory.
if(AUTO_PLUGIN_DEPLOYMENT)
    # On git HEAD change (branch switch/commit), wipe non-shader AIO content
    # and its deploy stamp so the still-timestamp-based robocopy hop below
    # re-copies with fresh timestamps. Shaders/ is excluded: its deploy hop
    # (SyncShaderDeploy.cmake) compares content, not mtime, so wiping it here
    # would only force needless in-game shader recompiles on every switch.
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_head
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_head)
        set(_git_head_stamp "${CMAKE_BINARY_DIR}/git-head.stamp")
        set(_prev_git_head "")
        if(EXISTS "${_git_head_stamp}")
            file(READ "${_git_head_stamp}" _prev_git_head)
            string(STRIP "${_prev_git_head}" _prev_git_head)
        endif()
        if(NOT "${_git_head}" STREQUAL "${_prev_git_head}")
            message(
                STATUS
                "Git HEAD changed (${_prev_git_head} -> ${_git_head}): clearing non-shader AIO content and deploy stamp for full redeploy"
            )
            # Delete everything under AIO_DIR except Shaders (see rationale
            # above). The DLL is always rebuilt fresh by the C++ compile step
            # so it doesn't need special handling.
            file(GLOB _aio_top_entries "${AIO_DIR}/*")
            foreach(_entry IN LISTS _aio_top_entries)
                get_filename_component(_entry_name "${_entry}" NAME)
                if(NOT _entry_name STREQUAL "Shaders")
                    file(REMOVE_RECURSE "${_entry}")
                endif()
            endforeach()
            # Clear prepare_aio.stamp (repopulates what was just deleted;
            # PREPARE_AIO never touches Shaders/) and the non-shader deploy
            # stamp. copy_shaders.stamp is left alone since Shaders/ wasn't touched.
            file(
                GLOB _build_stamps
                "${CMAKE_BINARY_DIR}/*_deploy.stamp"
                "${CMAKE_BINARY_DIR}/prepare_aio.stamp"
            )
            foreach(_stamp IN LISTS _build_stamps)
                file(REMOVE "${_stamp}")
            endforeach()
        endif()
        file(WRITE "${_git_head_stamp}" "${_git_head}")
    endif()

    set(DEPLOY_TARGET_HASHES)
    if(WIN32)
        foreach(DEPLOY_TARGET $ENV{CommunityShadersOutputDir})
            message("Deploying AIO to ${DEPLOY_TARGET} (incremental)")

            # Ensure destination root exists
            add_custom_command(
                TARGET ${PROJECT_NAME}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_TARGET}"
            )

            string(MD5 DEPLOY_TARGET_HASH ${DEPLOY_TARGET})

            # Incremental copy (non-shaders) - produce a stamp so the COPY_SHADERS
            # target can depend on both shader and non-shader deploy steps.
            add_custom_command(
                OUTPUT ${DEPLOY_TARGET_HASH}_deploy.stamp
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_TARGET}"
                # No /XO: a previous manual or external deploy can leave dest
                # files with mtime > source. /XO then skips them even when the
                # source file changed (different size and/or content), so build
                # output silently diverges from what runs in-game. Without /XO,
                # robocopy's default name+size+timestamp delta catches both
                # newer-source and size-mismatch cases. The git-HEAD-change
                # block above handles bulk-redeploy when checking out a new
                # branch; this command handles the per-build incremental.
                COMMAND
                    ${ROBOCOPY_WRAPPER} "${AIO_DIR}" "${DEPLOY_TARGET}" "/E"
                    "/XD" "${AIO_DIR}/Shaders" "/COPY:DAT" "/R:1" "/W:1" "/NFL"
                    "/NDL" "/NJH" "/NJS"
                COMMAND
                    ${CMAKE_COMMAND} -E touch ${DEPLOY_TARGET_HASH}_deploy.stamp
                # Depend on the PREPARE_AIO target AND its stamp file: the
                # target-level dependency stops the VS generator duplicating
                # PREPARE_AIO's producing rule into DEPLOY_ALL.vcxproj (two
                # copies race writing the same AIO tree); the stamp file adds
                # the mtime edge so this rule re-runs when staging changes
                # (target-only DEPENDS is order-only and never re-triggers).
                # Also depend on prepare_shaders: robocopy still enumerates
                # the /XD-excluded Shaders dir, which can race its writers.
                DEPENDS
                    PREPARE_AIO
                    prepare_shaders
                    ${CMAKE_CURRENT_BINARY_DIR}/prepare_aio.stamp
                COMMENT
                    "Incremental deploy (excluding Shaders) to ${DEPLOY_TARGET} (robocopy-wrapper)"
            )

            # Ensure plugin DLL/PDB are copied directly to the target SKSE/Plugins
            # folder in case robocopy rules do not copy them as expected.
            add_custom_command(
                OUTPUT ${DEPLOY_TARGET_HASH}_plugin.stamp
                COMMAND
                    ${CMAKE_COMMAND} -E make_directory
                    "${DEPLOY_TARGET}/SKSE/Plugins"
                COMMAND
                    ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${PROJECT_NAME}>
                    "${DEPLOY_TARGET}/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
                COMMAND
                    ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_PDB_FILE:${PROJECT_NAME}>
                    "${DEPLOY_TARGET}/SKSE/Plugins/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
                COMMAND
                    ${CMAKE_COMMAND} -E touch ${DEPLOY_TARGET_HASH}_plugin.stamp
                # See PREPARE_AIO target-name rationale above; the target-only
                # DEPENDS is order-only, so also depend on the built file
                # itself for the mtime edge that re-triggers on content change.
                DEPENDS
                    PREPARE_AIO
                    ${PROJECT_NAME}
                    $<TARGET_FILE:${PROJECT_NAME}>
                COMMENT "Copy plugin DLL/PDB to ${DEPLOY_TARGET}/SKSE/Plugins"
            )

            list(APPEND DEPLOY_TARGET_HASHES ${DEPLOY_TARGET_HASH}_plugin.stamp)

            # Incremental shader-only copy for fast dev iteration
            # Depends ONLY on copy_shaders.stamp (no DLL, no PREPARE_AIO)
            # Content-based, not robocopy: an unchanged shader keeps its
            # existing destination mtime, so the disk cache and FileWatcher
            # never see a false "source newer" event.
            add_custom_command(
                OUTPUT ${DEPLOY_TARGET_HASH}_shaders_only.stamp
                COMMAND
                    ${CMAKE_COMMAND} -DSRC_DIR="${AIO_DIR}/Shaders"
                    -DDST_DIR="${DEPLOY_TARGET}/Shaders" -P
                    "${CMAKE_SOURCE_DIR}/cmake/SyncShaderDeploy.cmake"
                COMMAND
                    ${CMAKE_COMMAND} -E touch
                    ${DEPLOY_TARGET_HASH}_shaders_only.stamp
                # See the PREPARE_AIO target+stamp rationale above.
                DEPENDS
                    prepare_shaders
                    ${CMAKE_CURRENT_BINARY_DIR}/copy_shaders.stamp
                    ${CMAKE_SOURCE_DIR}/cmake/SyncShaderDeploy.cmake
                COMMENT "Fast shader-only deploy to ${DEPLOY_TARGET}/Shaders"
            )

            list(
                APPEND SHADER_ONLY_HASHES
                ${DEPLOY_TARGET_HASH}_shaders_only.stamp
            )

            # Full shader copy for packaging (includes PREPARE_AIO dependency)
            # This ensures DLL is built and all AIO files are ready. Same
            # content-based sync as the shaders-only stamp above.
            add_custom_command(
                OUTPUT ${DEPLOY_TARGET_HASH}_shaders_full.stamp
                COMMAND
                    ${CMAKE_COMMAND} -DSRC_DIR="${AIO_DIR}/Shaders"
                    -DDST_DIR="${DEPLOY_TARGET}/Shaders" -P
                    "${CMAKE_SOURCE_DIR}/cmake/SyncShaderDeploy.cmake"
                COMMAND
                    ${CMAKE_COMMAND} -E touch
                    ${DEPLOY_TARGET_HASH}_shaders_full.stamp
                # Depend on copy_shaders.stamp's real source files, not the
                # stamp itself: referencing that OUTPUT made the VS generator
                # duplicate its producing rule into DEPLOY_ALL.vcxproj, racing
                # the real copy in prepare_shaders.vcxproj. Also chained after
                # this target's own _deploy.stamp for the same reason.
                DEPENDS
                    prepare_shaders
                    PREPARE_AIO
                    ${HLSL_FILES}
                    ${FFX_RUNTIME_FILES}
                    ${STREAMLINE_RUNTIME_FILES}
                    ${STREAMLINE_RUNTIME_DX12_FILES}
                    ${_package_shaders}
                    ${_all_feature_shader_files}
                    ${CMAKE_SOURCE_DIR}/cmake/SyncShaderDeploy.cmake
                    ${CMAKE_CURRENT_BINARY_DIR}/${DEPLOY_TARGET_HASH}_deploy.stamp
                COMMENT
                    "Full shader deploy to ${DEPLOY_TARGET}/Shaders (with PREPARE_AIO)"
            )

            list(APPEND DEPLOY_TARGET_HASHES ${DEPLOY_TARGET_HASH}_deploy.stamp)
            list(
                APPEND DEPLOY_TARGET_HASHES
                ${DEPLOY_TARGET_HASH}_shaders_full.stamp
            )
        endforeach()
    else()
        # AUTO_PLUGIN_DEPLOYMENT is enabled but the host is not Windows. Do
        # not attempt to deploy to local Skyrim directories on non-Windows
        # systems; instead provide a minimal COPY_SHADERS target so CI jobs
        # that only prepare shaders still work.
        message(
            WARNING
            "AUTO_PLUGIN_DEPLOYMENT is enabled but not supported on this platform; skipping deployment to CommunityShadersOutputDir"
        )
    endif()

    # Lightweight target for fast shader dev iteration
    # Deploys ONLY shaders to game directory (no DLL build, no tests)
    add_custom_target(
        COPY_SHADERS
        DEPENDS ${SHADER_ONLY_HASHES}
        COMMENT "Fast shader-only deploy to game directory (no DLL, no tests)"
    )

    # Full deployment target for packaging/CI
    # Builds DLL, prepares AIO, deploys everything, runs tests
    # Depend on the prepare_shaders target (not its copy_shaders.stamp
    # output file): the per-deploy-target stamps above already order
    # themselves after prepare_shaders/PREPARE_AIO by target name, and
    # listing the raw file here too would make the VS generator duplicate
    # the shader-copy rule into DEPLOY_ALL.vcxproj, letting that duplicate
    # race the real one in prepare_shaders.vcxproj for the same AIO tree.
    add_custom_target(
        DEPLOY_ALL
        ALL
        DEPENDS prepare_shaders ${DEPLOY_TARGET_HASHES}
        COMMENT "Full deployment: DLL + shaders + all files to game directory"
    )
endif()

if(NOT DEFINED ENV{CommunityShadersOutputDir})
    message(
        "When using AUTO_PLUGIN_DEPLOYMENT option, you need to set environment variable 'CommunityShadersOutputDir'"
    )
endif()

find_program(
    SEVENZ_EXECUTABLE
    NAMES 7z 7za
    PATHS "C:/Program Files/7-Zip" "C:/Program Files (x86)/7-Zip"
    DOC "7-Zip archiver for compressed packages"
)

# Zip base CommunityShaders and all addons as their own 7z in dist folder
if(ZIP_TO_DIST)
    set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
    message("Copying base CommunityShader into ${ZIP_DIR}.")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E remove_directory "${ZIP_DIR}"
            ${CMAKE_SOURCE_DIR}/dist
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${ZIP_DIR}/SKSE/Plugins"
            ${CMAKE_SOURCE_DIR}/dist
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package
            "${ZIP_DIR}"
        COMMAND
            ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
            "${ZIP_DIR}/SKSE/Plugins/"
        COMMAND
            ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}>
            "${ZIP_DIR}/SKSE/Plugins/"
    )
    foreach(FEATURE_PATH ${FEATURE_PATHS})
        if(EXISTS "${FEATURE_PATH}/CORE")
            add_custom_command(
                TARGET ${PROJECT_NAME}
                POST_BUILD
                COMMAND
                    ${CMAKE_COMMAND} -E copy_directory ${FEATURE_PATH}
                    "${ZIP_DIR}"
            )
        endif()
    endforeach()
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove "${ZIP_DIR}/CORE"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${ZIP_DIR}/Shaders/Tests"
    )

    set(TARGET_ZIP "${PROJECT_NAME}-${UTC_NOW}.7z")
    message("Zipping ${ZIP_DIR} to ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP}")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP}
            --format=7zip -- .
        WORKING_DIRECTORY ${ZIP_DIR}
    )

    foreach(FEATURE_PATH ${FEATURE_PATHS})
        if(EXISTS "${FEATURE_PATH}/CORE")
            continue()
        endif()
        get_filename_component(FEATURE ${FEATURE_PATH} NAME)
        message(
            "Zipping ${FEATURE_PATH} to ${CMAKE_SOURCE_DIR}/dist/${FEATURE}-${UTC_NOW}.7z"
        )
        set(FEATURE_ZIP_STAGE
            "${CMAKE_CURRENT_BINARY_DIR}/feature_zip/${FEATURE}"
        )
        feature_package_commands(
            FEATURE_ZIP_COMMANDS
            "${FEATURE}"
            "${FEATURE_PATH}"
            "${FEATURE_ZIP_STAGE}"
            "${CMAKE_SOURCE_DIR}/dist/${FEATURE}-${UTC_NOW}.7z"
            cf
        )
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            ${FEATURE_ZIP_COMMANDS}
        )
    endforeach()
endif()

# Create a AIO zip for easier testing
if(AIO_ZIP_TO_DIST)
    if(NOT ZIP_TO_DIST)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E remove_directory ${CMAKE_SOURCE_DIR}/dist
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
        )
    endif()

    # Create a stamp-producing custom command for the AIO archive so CMake
    # only rebuilds the archive when its inputs change. The archive filename
    # keeps the UTC timestamp as before, but the command writes a stable
    # stamp file that CMake can track as OUTPUT.
    if(SEVENZ_EXECUTABLE)
        set(TARGET_AIO_ZIP "${PROJECT_NAME}_AIO-${UTC_NOW}.7z")
    else()
        set(TARGET_AIO_ZIP "${PROJECT_NAME}_AIO-${UTC_NOW}.zip")
    endif()
    set(AIO_ARCHIVE "${CMAKE_SOURCE_DIR}/dist/${TARGET_AIO_ZIP}")
    set(AIO_ZIP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/aio_package.stamp")
    set(AIO_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio_stage")

    message("Zipping ${AIO_DIR} to ${AIO_ARCHIVE}")

    # Fast path: tar AIO_DIR directly. Slow path (excluded features):
    # copy → stage, strip excluded relpaths, tar the stage. AIO_DIR stays
    # complete so AUTO_PLUGIN_DEPLOYMENT delivers the full tree. Prefers a
    # real 7z binary (smaller, faster archives); falls back to CMake's
    # built-in tar-as-zip when 7z isn't installed.
    set(_aio_zip_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        "${CMAKE_SOURCE_DIR}/dist"
    )
    set(_aio_archive_source_dir "${AIO_DIR}")
    if(AIO_EXCLUDED_FEATURE_NAMES)
        list(
            APPEND _aio_zip_cmds
            COMMAND
            ${CMAKE_COMMAND}
            -E
            rm
            -rf
            "${AIO_STAGE_DIR}"
            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_directory
            "${AIO_DIR}"
            "${AIO_STAGE_DIR}"
        )
        foreach(_relpath IN LISTS AIO_EXCLUDED_STAGE_RELPATHS)
            list(
                APPEND _aio_zip_cmds
                COMMAND
                ${CMAKE_COMMAND}
                -E
                rm
                -rf
                "${AIO_STAGE_DIR}/${_relpath}"
            )
        endforeach()
        set(_aio_archive_source_dir "${AIO_STAGE_DIR}")
    endif()
    if(SEVENZ_EXECUTABLE)
        list(
            APPEND _aio_zip_cmds
            COMMAND
            ${CMAKE_COMMAND}
            -E
            rm
            -f
            "${AIO_ARCHIVE}"
            COMMAND
            ${CMAKE_COMMAND}
            -E
            chdir
            "${_aio_archive_source_dir}"
            "${SEVENZ_EXECUTABLE}"
            a
            -t7z
            -mx1
            -mmt=on
            "${AIO_ARCHIVE}"
            *
        )
    else()
        message(WARNING "7z not found; AIO archive will use uncompressed zip")
        list(
            APPEND _aio_zip_cmds
            COMMAND
            ${CMAKE_COMMAND}
            -E
            chdir
            "${_aio_archive_source_dir}"
            ${CMAKE_COMMAND}
            -E
            tar
            cf
            "${AIO_ARCHIVE}"
            --format=zip
            --
            .
        )
    endif()
    list(
        APPEND _aio_zip_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        touch
        ${AIO_ZIP_STAMP}
    )

    add_custom_command(
        OUTPUT ${AIO_ZIP_STAMP} ${_aio_zip_cmds}
        DEPENDS PREPARE_AIO ${CMAKE_CURRENT_BINARY_DIR}/copy_shaders.stamp
        COMMENT "Creating AIO archive ${AIO_ARCHIVE}"
    )

    add_custom_target(AIO_ZIP_PACKAGE ALL DEPENDS ${AIO_ZIP_STAMP})
endif()

if(NOT DEFINED ENV{CommunityShadersOutputDir})
    message(
        "When using AUTO_PLUGIN_DEPLOYMENT option, you need to set environment variable 'CommunityShadersOutputDir'"
    )
endif()

# #######################################################################################################################
# # Manual packaging targets (Package-XXX)
# #######################################################################################################################

set(DIST_PATH "${CMAKE_SOURCE_DIR}/dist")
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")

set(CORE_PACKAGE "${DIST_PATH}/${PROJECT_NAME}-${UTC_NOW}.7z")

# CORE_SOURCES = all content copied to the AIO directory + the SKSE plugin dll
file(
    GLOB_RECURSE CORE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/package/*"
)
# Add SKSE plugin dll as dependency (use target-file generator expression so CMake
# knows the actual output path of the target at build time)
list(APPEND CORE_SOURCES "$<TARGET_FILE:${PROJECT_NAME}>")

set(CORE_FEATURE_PATHS "${CMAKE_SOURCE_DIR}/package/")

foreach(FEATURE_PATH ${FEATURE_PATHS})
    if(EXISTS "${FEATURE_PATH}/CORE")
        list(APPEND CORE_FEATURE_PATHS "${FEATURE_PATH}/")
        file(GLOB_RECURSE FEATURE_SOURCES CONFIGURE_DEPENDS "${FEATURE_PATH}/*")
        list(APPEND CORE_SOURCES ${FEATURE_SOURCES})
    endif()
endforeach()

# Core package
set(FEATURE_PATH "${CMAKE_BINARY_DIR}/Core")
file(MAKE_DIRECTORY ${FEATURE_PATH})
add_custom_command(
    OUTPUT ${CORE_PACKAGE}
    DEPENDS ${CORE_SOURCES}
    COMMAND
        ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${FEATURE_PATH}
        --component SKSE
    COMMAND
        ${CMAKE_COMMAND} -E copy_directory ${CORE_FEATURE_PATHS} ${FEATURE_PATH}
    COMMAND ${CMAKE_COMMAND} -E rm -f -- ${FEATURE_PATH}/Core
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${FEATURE_PATH}/Shaders/Tests"
    COMMAND ${CMAKE_COMMAND} -E tar cfv ${CORE_PACKAGE} --format=7zip -- .
    WORKING_DIRECTORY ${FEATURE_PATH}
    COMMENT "Creating Core zip package"
)
add_custom_target("Package-Core" DEPENDS ${CORE_PACKAGE})

# Feature packages
foreach(FEATURE_PATH ${FEATURE_PATHS})
    if(EXISTS "${FEATURE_PATH}/CORE")
        continue()
    endif()

    list(APPEND CORE_FEATURE_PATHS "${FEATURE_PATH}/")
    file(GLOB_RECURSE FEATURE_SOURCES CONFIGURE_DEPENDS "${FEATURE_PATH}/*")
    list(APPEND CORE_SOURCES ${FEATURE_SOURCES})

    get_filename_component(FEATURE ${FEATURE_PATH} NAME)
    set(FEATURE_PACKAGE "${DIST_PATH}/${FEATURE}-${UTC_NOW}.7z")

    set(FEATURE_PACKAGE_STAGE
        "${CMAKE_CURRENT_BINARY_DIR}/feature_package/${FEATURE}"
    )
    feature_package_commands(
        FEATURE_PACKAGE_COMMANDS
        "${FEATURE}"
        "${FEATURE_PATH}"
        "${FEATURE_PACKAGE_STAGE}"
        "${FEATURE_PACKAGE}"
        cfv
    )
    feature_payload_dependencies("${FEATURE}" FEATURE_PACKAGE_DEPENDENCIES)
    add_custom_command(
        OUTPUT ${FEATURE_PACKAGE} ${FEATURE_PACKAGE_COMMANDS}
        DEPENDS ${FEATURE_SOURCES} ${FEATURE_PACKAGE_DEPENDENCIES}
        COMMENT "Creating ${FEATURE} zip package"
    )

    string(REPLACE " " "" FEATURE ${FEATURE})
    string(REPLACE "-" "" FEATURE ${FEATURE})
    add_custom_target("Package-${FEATURE}" DEPENDS ${FEATURE_PACKAGE})
endforeach()

# AIO Folder target
add_custom_command(
    OUTPUT ${AIO_DIR}/SKSE/Plugins/${PROJECT_NAME}.dll
    DEPENDS ${CORE_SOURCES} ${FFX_RUNTIME_FILES}
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${AIO_DIR}
    COMMENT "Installing to AIO folder"
)
add_custom_target("AIO" DEPENDS ${AIO_DIR}/SKSE/Plugins/${PROJECT_NAME}.dll)

# Manual AIO package target. Strips non-autoupload features at archive
# time (mirroring AIO_ZIP_PACKAGE) so the manual path produces the same
# release-ready bundle as the automated path. Prefers a real 7z binary;
# falls back to CMake's built-in tar-as-zip when 7z isn't installed.
if(SEVENZ_EXECUTABLE)
    set(AIO_PACKAGE "${DIST_PATH}/${PROJECT_NAME}_AIO-${UTC_NOW}.7z")
else()
    set(AIO_PACKAGE "${DIST_PATH}/${PROJECT_NAME}_AIO-${UTC_NOW}.zip")
endif()
set(AIO_MANUAL_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio_manual_stage")
set(_aio_manual_cmds
    COMMAND
    ${CMAKE_COMMAND}
    -E
    make_directory
    ${AIO_DIR}
    COMMAND
    ${CMAKE_COMMAND}
    --install
    ${CMAKE_BINARY_DIR}
    --prefix
    ${AIO_DIR}
)
set(_aio_manual_source_dir "${AIO_DIR}")
if(AIO_EXCLUDED_FEATURE_NAMES)
    list(
        APPEND _aio_manual_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        rm
        -rf
        "${AIO_MANUAL_STAGE_DIR}"
        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_directory
        "${AIO_DIR}"
        "${AIO_MANUAL_STAGE_DIR}"
    )
    foreach(_relpath IN LISTS AIO_EXCLUDED_STAGE_RELPATHS)
        list(
            APPEND _aio_manual_cmds
            COMMAND
            ${CMAKE_COMMAND}
            -E
            rm
            -rf
            "${AIO_MANUAL_STAGE_DIR}/${_relpath}"
        )
    endforeach()
    set(_aio_manual_source_dir "${AIO_MANUAL_STAGE_DIR}")
endif()
if(SEVENZ_EXECUTABLE)
    list(
        APPEND _aio_manual_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        rm
        -f
        "${AIO_PACKAGE}"
        COMMAND
        ${CMAKE_COMMAND}
        -E
        chdir
        "${_aio_manual_source_dir}"
        "${SEVENZ_EXECUTABLE}"
        a
        -t7z
        -mx1
        -mmt=on
        "${AIO_PACKAGE}"
        *
    )
else()
    list(
        APPEND _aio_manual_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        chdir
        "${_aio_manual_source_dir}"
        ${CMAKE_COMMAND}
        -E
        tar
        cfv
        ${AIO_PACKAGE}
        --format=zip
        --
        .
    )
endif()

add_custom_command(
    OUTPUT ${AIO_PACKAGE}
    DEPENDS ${CORE_SOURCES} ${FFX_RUNTIME_FILES} ${_aio_manual_cmds}
    COMMENT "Creating AIO zip package (manual)"
)
add_custom_target("Package-AIO-Manual" DEPENDS ${AIO_PACKAGE})

# #######################################################################################################################
# # Shader Unit Tests
# #######################################################################################################################
if(BUILD_SHADER_TESTS)
    message(STATUS "Adding shader tests subdirectory")
    enable_testing() # Enable CTest integration for shader tests
    add_subdirectory(tests/shaders)
endif()

# C++ unit tests for plugin utility code (separate from HLSL shader tests).
# Gated on its own flag so it can be enabled/disabled independently.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests/cpp)
    if(TARGET cpp_tests)
        add_custom_target(
            run_cpp_tests
            COMMAND $<TARGET_FILE:cpp_tests> --reporter compact
            DEPENDS cpp_tests
            WORKING_DIRECTORY $<TARGET_FILE_DIR:cpp_tests>
            COMMENT "Running C++ unit tests..."
            VERBATIM
        )
    endif()
endif()

if(BUILD_SHADER_TESTS)
    # Add a custom target that runs the shader tests
    # Users can run this manually with: cmake --build <build-dir> --target run_shader_tests
    # Runs the test executable directly (not via CTest) to show discovery count
    add_custom_target(
        run_shader_tests
        COMMAND $<TARGET_FILE:shader_tests> --reporter compact
        DEPENDS shader_tests
        WORKING_DIRECTORY $<TARGET_FILE_DIR:shader_tests>
        COMMENT "Running shader unit tests..."
        VERBATIM
    )

    # Make all package targets depend on shader tests passing
    add_dependencies("Package-Core" run_shader_tests)
    add_dependencies("Package-AIO-Manual" run_shader_tests)
    foreach(FEATURE_PATH ${FEATURE_PATHS})
        get_filename_component(FEATURE ${FEATURE_PATH} NAME)
        string(REPLACE " " "" FEATURE ${FEATURE})
        string(REPLACE "-" "" FEATURE ${FEATURE})
        if(TARGET "Package-${FEATURE}")
            add_dependencies("Package-${FEATURE}" run_shader_tests)
        endif()
    endforeach()

    # Make shader deployment targets depend on tests passing
    if(TARGET prepare_shaders)
        add_dependencies(prepare_shaders run_shader_tests)
    endif()
    if(TARGET DEPLOY_ALL)
        add_dependencies(DEPLOY_ALL run_shader_tests)
    endif()

    message(
        STATUS
        "Package and shader deployment targets will automatically run shader tests before deploying"
    )
endif()

message("*************************************************************")
message("Open Shaders configuration complete (fork of Community Shaders)")
message(
    "To prepare a package of AIO (.7z when 7-Zip is found), Core, or Features"
)
message("  Build cmake targets:")
message("    - Package-Core: Core package")
message("    - Package-AIO-Manual: AIO package (manual)")
message("    - Package-<Feature>: Individual feature packages")
message("  Or use cmake --install for custom deployment:")
message("    cmake --install ./build/ALL --prefix <TARGET_DIR>")
if(BUILD_SHADER_TESTS)
    message("To run shader tests manually:")
    message(
        "  cmake --build build/<preset> --config <Debug|Release> --target run_shader_tests"
    )
endif()
message("Try switching to build preset 'Dev' for faster iteration time")
message("*************************************************************")
