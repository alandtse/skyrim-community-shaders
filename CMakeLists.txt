cmake_minimum_required(VERSION 3.21)

project(
	CommunityShaders
	VERSION 0.8.7
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ########################################################################################################################
# ## Build options
# ########################################################################################################################
message("Options:")
option(AUTO_PLUGIN_DEPLOYMENT "Copy the build output and addons to env:CommunityShadersOutputDir." OFF)
option(ZIP_TO_DIST "Zip the base mod and addons to their own 7z file in dist." ON)
option(AIO_ZIP_TO_DIST "Zip the base mod and addons to a AIO 7z file in dist." ON)
option(TRACY_SUPPORT "Enable support for tracy profiler" OFF)
message("\tAuto plugin deployment: ${AUTO_PLUGIN_DEPLOYMENT}")
message("\tZip to dist: ${ZIP_TO_DIST}")
message("\tAIO Zip to dist: ${AIO_ZIP_TO_DIST}")
message("\tTracy profiler: ${TRACY_SUPPORT}")

# #######################################################################################################################
# # Add CMake features
# #######################################################################################################################
include(XSEPlugin)

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
find_package(Tracy CONFIG REQUIRED)
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/Streamline)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
	"$<$<BOOL:${TRACY_SUPPORT}>:TRACY_SUPPORT>"
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${BSHOSHANY_THREAD_POOL_INCLUDE_DIRS}
	${CLIB_UTIL_INCLUDE_DIRS}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	debug ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Debug/detours.lib
	optimized ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Release/detours.lib
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
	Tracy::TracyClient
	Streamline
)

# https://gitlab.kitware.com/cmake/cmake/-/issues/24922#note_1371990
if(MSVC_VERSION GREATER_EQUAL 1936 AND MSVC_IDE) # 17.6+
	# When using /std:c++latest, "Build ISO C++23 Standard Library Modules" defaults to "Yes".
	# Default to "No" instead.
	#
	# As of CMake 3.26.4, there isn't a way to control this property
	# (https://gitlab.kitware.com/cmake/cmake/-/issues/24922),
	# We'll use the MSBuild project system instead
	# (https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-file-structure)
	file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/Directory.Build.props" CONTENT [==[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <BuildStlModules>false</BuildStlModules>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]==] @ONLY)
endif()

# #######################################################################################################################
# # Feature version detection
# #######################################################################################################################
file(GLOB_RECURSE FEATURE_CONFIG_FILES
	LIST_DIRECTORIES false
	CONFIGURE_DEPENDS
	"features/*/Shaders/Features/*.ini"
)

foreach(FEATURE_PATH ${FEATURE_CONFIG_FILES})
	get_filename_component(FEATURE ${FEATURE_PATH} NAME_WE)
	file(READ "${FEATURE_PATH}" CONFIG_VALUE)
	string(REGEX MATCH "Version = ([0-9]*)-([0-9]*)-([0-9]*)" _ ${CONFIG_VALUE})
	set(ver_major ${CMAKE_MATCH_1})
	set(ver_minor ${CMAKE_MATCH_2})
	set(ver_patch ${CMAKE_MATCH_3})
	list(APPEND FEATURE_VERSIONS \t\t{\ \"${FEATURE}\"sv,\ {${ver_major},${ver_minor},${ver_patch}}\ })
endforeach()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FEATURE_CONFIG_FILES}")

string(REPLACE ";" ",\n" FEATURE_VERSIONS "${FEATURE_VERSIONS}")

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/FeatureVersions.h.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/FeatureVersions.h
	@ONLY
)

target_sources(
	"${PROJECT_NAME}"
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/cmake/FeatureVersions.h
)

# #######################################################################################################################
# # clang-format
# #######################################################################################################################

find_program(CLANG_FORMAT_PATH clang-format)
if(CLANG_FORMAT_PATH)
	add_custom_target(FORMAT_CODE
		COMMAND ${CLANG_FORMAT_PATH} -i -style=file ${CPP_SOURCES};${HLSL_FILES}
		COMMENT "Running clang format for cpp and hlsl files"
	)
endif()

# #######################################################################################################################
# # Automatic deployment
# #######################################################################################################################
file(GLOB FEATURE_PATHS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/features/*)
string(TIMESTAMP UTC_NOW "%Y-%m-%dT%H-%MZ" UTC)
if(AUTO_PLUGIN_DEPLOYMENT OR AIO_ZIP_TO_DIST)
	set(AIO_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio")
	message("Copying package folder with dll/pdb with all features to ${AIO_DIR}")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E remove_directory "${AIO_DIR}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/SKSE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package "${AIO_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${FEATURE_PATHS} "${AIO_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${AIO_DIR}/SKSE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${AIO_DIR}/SKSE/Plugins/"
	)

	# Generate timestamps for shader files
	file(GLOB_RECURSE HLSL_FILES "${CMAKE_SOURCE_DIR}/package/*.hlsl*" "${FEATURE_PATHS}/*.hlsl*")
	file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shader_timestamps)

	foreach(file IN LISTS HLSL_FILES)
		get_filename_component(FILE_NAME ${file} NAME)
		set(TIMESTAMP_FILE "${CMAKE_BINARY_DIR}/shader_timestamps/${FILE_NAME}.timestamp")
		file(TIMESTAMP ${file} FILE_TIMESTAMP)
		file(WRITE "${TIMESTAMP_FILE}" "${FILE_TIMESTAMP}")
	endforeach()

	# Create a stamp file to track timestamp changes
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/shaders_timestamp.stamp
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package "${AIO_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${FEATURE_PATHS} "${AIO_DIR}"
		COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/shaders_timestamp.stamp
		DEPENDS ${HLSL_FILES}
	)
endif()

# Automatic deployment to CommunityShaders output directory.
if(AUTO_PLUGIN_DEPLOYMENT)
	foreach(DEPLOY_TARGET $ENV{CommunityShadersOutputDir})
		message("Copying AIO to ${DEPLOY_TARGET}")

		# Copy AIO directory
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory ${AIO_DIR} "${DEPLOY_TARGET}"
		)

		# Copy Shaders with timestamp
		string(MD5 DEPLOY_TARGET_HASH ${DEPLOY_TARGET})
		set(TARGET_TIMESTAMP_FILE "${CMAKE_BINARY_DIR}/shader_timestamps/${DEPLOY_TARGET_HASH}_timestamp.stamp")

		add_custom_command(
			OUTPUT ${TARGET_TIMESTAMP_FILE}
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${AIO_DIR}/Shaders" "${DEPLOY_TARGET}/Shaders"
			COMMAND ${CMAKE_COMMAND} -E touch ${TARGET_TIMESTAMP_FILE}
			DEPENDS ${CMAKE_BINARY_DIR}/shaders_timestamp.stamp
		)

		list(APPEND DEPLOY_TARGET_TIMESTAMP_FILES ${TARGET_TIMESTAMP_FILE})
	endforeach()

	add_custom_target(COPY_SHADERS ALL
		DEPENDS ${DEPLOY_TARGET_TIMESTAMP_FILES}
	)

	if(NOT DEFINED ENV{CommunityShadersOutputDir})
		message("When using AUTO_PLUGIN_DEPLOYMENT option, you need to set environment variable 'CommunityShadersOutputDir'")
	endif()
endif()

# Zip base CommunityShaders and all addons as their own 7z in dist folder
if(ZIP_TO_DIST)
	set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E remove_directory "${ZIP_DIR}" ${CMAKE_SOURCE_DIR}/dist
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ZIP_DIR}/SKSE/Plugins" ${CMAKE_SOURCE_DIR}/dist
	)

	message("Copying base CommunityShader into ${ZIP_DIR}.")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package "${ZIP_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/"
	)

	set(TARGET_ZIP "${PROJECT_NAME}-${UTC_NOW}.7z")
	message("Zipping ${ZIP_DIR} to ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP}")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP} --format=7zip -- .
		WORKING_DIRECTORY ${ZIP_DIR}
	)

	foreach(FEATURE_PATH ${FEATURE_PATHS})
		get_filename_component(FEATURE ${FEATURE_PATH} NAME)
		message("Zipping ${FEATURE_PATH} to ${CMAKE_SOURCE_DIR}/dist/${FEATURE}-${UTC_NOW}.7z")
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${FEATURE}-${UTC_NOW}.7z --format=7zip -- .
			WORKING_DIRECTORY ${FEATURE_PATH}
		)
	endforeach()
endif()

# Create a AIO zip for easier testing
if(AIO_ZIP_TO_DIST)
	if(NOT ZIP_TO_DIST)
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_SOURCE_DIR}/dist
			COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
		)
	endif()

	set(TARGET_AIO_ZIP "${PROJECT_NAME}_AIO-${UTC_NOW}.7z")
	message("Zipping ${AIO_DIR} to ${CMAKE_SOURCE_DIR}/dist/${TARGET_AIO_ZIP}")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${TARGET_AIO_ZIP} --format=7zip -- .
		WORKING_DIRECTORY ${AIO_DIR}
	)
endif()
