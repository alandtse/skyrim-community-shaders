# C++ unit tests for plugin utility code.
#
# Catch2-based. Disjoint from tests/shaders (which tests HLSL via
# ShaderTestFramework). Add test sources here for any plugin code that
# doesn't require a live DirectX device or ImGui context.

cmake_minimum_required(VERSION 3.31)

option(BUILD_CPP_TESTS "Build C++ unit tests for plugin utilities" ON)

if(NOT BUILD_CPP_TESTS)
    message(STATUS "C++ unit tests disabled")
    return()
endif()

message(STATUS "Configuring C++ unit tests...")

include(FetchContent)

# Reused by tests/shaders if that target is also enabled. FetchContent dedupes
# by name, so the same Catch2 source tree serves both.
FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.11.0
)
FetchContent_MakeAvailable(Catch2)

# MSVC 14.50+ (VS 2026) ICE workaround copied from tests/shaders.
if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
    if(TARGET Catch2)
        target_compile_options(Catch2 PRIVATE /Od)
    endif()
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
# SphericalHarmonics uses the float2/3/4 = DirectX::SimpleMath::* aliases.
find_package(directxtk CONFIG REQUIRED)
# Input.h uses magic_enum for device enum handling.
find_package(magic_enum CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
# test_generationclaim.cpp uses <thread>/<latch>; MSVC links this in
# implicitly, but Threads::Threads keeps the target portable to other toolchains.
find_package(Threads REQUIRED)

add_executable(cpp_tests
    test_main.cpp
    test_bootsnapshot.cpp
    test_subrect.cpp
    test_perfutils.cpp
    test_restartsettings.cpp
    test_sphericalharmonics.cpp
    test_input.cpp
    test_stringutils.cpp
    test_isl_radiusmath.cpp
    test_shadowcaster_math.cpp
    test_atlas_allocator.cpp
    test_llf_sanitize.cpp
    test_hdr_png_metadata.cpp
    test_cacheinvalidation.cpp
    test_contenthash.cpp
    test_shadercachemanifest.cpp
    test_shadercacheclear.cpp
    test_generationclaim.cpp
    test_settingspatch.cpp
    test_weatheridparser.cpp
    test_effectsourcecompatibility.cpp
    # Stub I18n::Get so units-under-test that call T() (Subrect's DrawEditor)
    # link without pulling the translation loader into the test binary.
    i18n_stub.cpp
    # Compile the unit-under-test directly into the test binary so we don't
    # depend on the plugin DLL build (which pulls in FFX/Streamline/etc.).
    "${CMAKE_SOURCE_DIR}/src/Utils/Subrect.cpp"
    "${CMAKE_SOURCE_DIR}/src/Utils/SphericalHarmonics.cpp"
    "${CMAKE_SOURCE_DIR}/src/Utils/HdrPngMetadata.cpp"
    "${CMAKE_SOURCE_DIR}/src/Utils/SettingsKeys.cpp"
    "${CMAKE_SOURCE_DIR}/src/Features/Effects11/WeatherIDParser.cpp"
    "${CMAKE_SOURCE_DIR}/src/Features/Effects11/EffectSourceCompatibility.cpp"
)

# The plugin defines the float2/3/4 = SimpleMath aliases in its PCH, which the
# test target does not use. Force-include a tiny prelude so SphericalHarmonics
# (and its .cpp) see those aliases without the heavy RE/SKSE PCH.
if(MSVC)
    target_compile_options(cpp_tests PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/test_prelude.h")
else()
    target_compile_options(cpp_tests PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/test_prelude.h")
endif()

set_property(TARGET cpp_tests PROPERTY CXX_STANDARD 23)
set_property(TARGET cpp_tests PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(cpp_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(cpp_tests PRIVATE
    Catch2::Catch2
    nlohmann_json::nlohmann_json
    imgui::imgui
    Microsoft::DirectXTK
    magic_enum::magic_enum
    xxHash::xxhash
    Threads::Threads
)

# windows.h (pulled in via d3d11.h) defines min/max macros that break the
# std::min<T>/std::max<T> calls in Subrect.cpp. The plugin's PCH suppresses
# this implicitly; the test target needs it explicitly.
target_compile_definitions(cpp_tests PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

enable_testing()
add_test(
    NAME CppUtilTests
    COMMAND cpp_tests --reporter compact
    WORKING_DIRECTORY $<TARGET_FILE_DIR:cpp_tests>
)
set_tests_properties(CppUtilTests PROPERTIES
    TIMEOUT 60
    LABELS "Cpp;UnitTests"
)

message(STATUS "C++ unit tests configured successfully")
