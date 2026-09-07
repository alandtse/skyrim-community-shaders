set(STREAMLINE_RUNTIME_VERSION "2.12.0")
set(STREAMLINE_RUNTIME_ARCHIVE_SHA256
    "F5C0A3D870707DDDC3570FB4BCD3655CF48A8A68C3A9D342910CFA21B77DCF48"
)
set(STREAMLINE_RUNTIME_ARCHIVE_URL
    "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${STREAMLINE_RUNTIME_VERSION}/streamline-sdk-v${STREAMLINE_RUNTIME_VERSION}.zip"
)
set(STREAMLINE_RUNTIME_ROOT "${CMAKE_CURRENT_BINARY_DIR}/streamline-runtime")
set(STREAMLINE_RUNTIME_ARCHIVE
    "${STREAMLINE_RUNTIME_ROOT}/streamline-sdk-v${STREAMLINE_RUNTIME_VERSION}.zip"
)
set(STREAMLINE_RUNTIME_EXTRACT_ROOT "${STREAMLINE_RUNTIME_ROOT}/sdk")
set(STREAMLINE_RUNTIME_EXTRACT_STAMP
    "${STREAMLINE_RUNTIME_EXTRACT_ROOT}/.archive-sha256"
)
set(STREAMLINE_RUNTIME_RELATIVE_DIRECTORY "Shaders/Upscaling/Streamline")
set(STREAMLINE_RUNTIME_DIRECTORY
    "${STREAMLINE_RUNTIME_ROOT}/payload/${STREAMLINE_RUNTIME_RELATIVE_DIRECTORY}"
)
# Upscaling::streamlineDX12 loads its own interposer from a separate plugin
# directory; the interposer/plugin DLLs are API-agnostic, so this reuses the
# same downloaded archive rather than a second SDK.
set(STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY "Shaders/Upscaling/StreamlineDX12")
set(STREAMLINE_RUNTIME_DX12_DIRECTORY
    "${STREAMLINE_RUNTIME_ROOT}/payload/${STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY}"
)
file(MAKE_DIRECTORY "${STREAMLINE_RUNTIME_ROOT}")
file(MAKE_DIRECTORY "${STREAMLINE_RUNTIME_DIRECTORY}")
file(MAKE_DIRECTORY "${STREAMLINE_RUNTIME_DX12_DIRECTORY}")

set(_streamline_download_status 0 "cached")
if(EXISTS "${STREAMLINE_RUNTIME_ARCHIVE}")
    file(SHA256 "${STREAMLINE_RUNTIME_ARCHIVE}" _streamline_existing_sha256)
endif()
if(NOT _streamline_existing_sha256 STREQUAL "${STREAMLINE_RUNTIME_ARCHIVE_SHA256}")
    file(
        DOWNLOAD "${STREAMLINE_RUNTIME_ARCHIVE_URL}"
        "${STREAMLINE_RUNTIME_ARCHIVE}"
        EXPECTED_HASH "SHA256=${STREAMLINE_RUNTIME_ARCHIVE_SHA256}"
        STATUS _streamline_download_status
        TLS_VERIFY ON
        TIMEOUT 600
        INACTIVITY_TIMEOUT 60
    )
endif()
list(GET _streamline_download_status 0 _streamline_download_code)
list(GET _streamline_download_status 1 _streamline_download_message)
if(NOT _streamline_download_code EQUAL 0)
    file(REMOVE "${STREAMLINE_RUNTIME_ARCHIVE}")
    message(
        FATAL_ERROR
        "Failed to download Streamline ${STREAMLINE_RUNTIME_VERSION}: ${_streamline_download_message}"
    )
endif()

set(_streamline_extract_required ON)
if(EXISTS "${STREAMLINE_RUNTIME_EXTRACT_STAMP}")
    file(READ "${STREAMLINE_RUNTIME_EXTRACT_STAMP}" _streamline_extracted_hash)
    string(STRIP "${_streamline_extracted_hash}" _streamline_extracted_hash)
    if(_streamline_extracted_hash STREQUAL STREAMLINE_RUNTIME_ARCHIVE_SHA256)
        set(_streamline_extract_required OFF)
    endif()
endif()

if(_streamline_extract_required)
    file(REMOVE_RECURSE "${STREAMLINE_RUNTIME_EXTRACT_ROOT}")
    file(MAKE_DIRECTORY "${STREAMLINE_RUNTIME_EXTRACT_ROOT}")
    file(
        ARCHIVE_EXTRACT
        INPUT "${STREAMLINE_RUNTIME_ARCHIVE}"
        DESTINATION "${STREAMLINE_RUNTIME_EXTRACT_ROOT}"
    )
    file(
        WRITE "${STREAMLINE_RUNTIME_EXTRACT_STAMP}"
        "${STREAMLINE_RUNTIME_ARCHIVE_SHA256}\n"
    )
endif()

file(
    GLOB_RECURSE _streamline_archive_files
    LIST_DIRECTORIES FALSE
    "${STREAMLINE_RUNTIME_EXTRACT_ROOT}/*"
)

function(stage_streamline_runtime _filename _directory _out_var)
    set(_production_matches "")
    foreach(_candidate IN LISTS _streamline_archive_files)
        get_filename_component(_candidate_name "${_candidate}" NAME)
        if(NOT _candidate_name STREQUAL _filename)
            continue()
        endif()

        get_filename_component(_candidate_directory "${_candidate}" DIRECTORY)
        get_filename_component(
            _candidate_directory_name
            "${_candidate_directory}"
            NAME
        )
        get_filename_component(
            _candidate_parent
            "${_candidate_directory}"
            DIRECTORY
        )
        get_filename_component(
            _candidate_parent_name
            "${_candidate_parent}"
            NAME
        )
        if(
            _candidate_directory_name STREQUAL "x64"
            AND _candidate_parent_name STREQUAL "bin"
        )
            list(APPEND _production_matches "${_candidate}")
        endif()
    endforeach()

    list(LENGTH _production_matches _production_match_count)
    if(NOT _production_match_count EQUAL 1)
        message(
            FATAL_ERROR
            "Expected one production ${_filename} in Streamline ${STREAMLINE_RUNTIME_VERSION}, found ${_production_match_count}"
        )
    endif()

    list(GET _production_matches 0 _source)
    set(_destination "${_directory}/${_filename}")
    file(COPY_FILE "${_source}" "${_destination}" ONLY_IF_DIFFERENT)
    set(${_out_var}
        ${${_out_var}}
        "${_destination}"
        PARENT_SCOPE
    )
endfunction()

set(STREAMLINE_RUNTIME_FILES "")
stage_streamline_runtime(nvngx_dlss.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)
stage_streamline_runtime(sl.common.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)
stage_streamline_runtime(sl.dlss.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)
stage_streamline_runtime(sl.interposer.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)
stage_streamline_runtime(sl.pcl.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)
stage_streamline_runtime(sl.reflex.dll "${STREAMLINE_RUNTIME_DIRECTORY}" STREAMLINE_RUNTIME_FILES)

register_feature_payload(
    Upscaling
    FILES ${STREAMLINE_RUNTIME_FILES}
    DESTINATION "${STREAMLINE_RUNTIME_RELATIVE_DIRECTORY}"
)

# streamlineDX12 needs the same core plugins plus DLSS-G (frame generation).
set(STREAMLINE_RUNTIME_DX12_FILES "")
stage_streamline_runtime(nvngx_dlss.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(nvngx_dlssg.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.common.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.dlss.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.dlss_g.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.interposer.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.pcl.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)
stage_streamline_runtime(sl.reflex.dll "${STREAMLINE_RUNTIME_DX12_DIRECTORY}" STREAMLINE_RUNTIME_DX12_FILES)

register_feature_payload(
    Upscaling
    FILES ${STREAMLINE_RUNTIME_DX12_FILES}
    DESTINATION "${STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY}"
)
