set(FFX_RUNTIME_SDK_COMMIT "60f4ea81909200d8542eca14dccb2628b763a9a3")
set(FFX_RUNTIME_BASE_URL
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FFX_RUNTIME_SDK_COMMIT}/Kits/FidelityFX/signedbin"
)
set(FFX_RUNTIME_FEATURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ffx-runtime")
set(FFX_RUNTIME_SHADER_ROOT "${FFX_RUNTIME_FEATURE_ROOT}/Shaders")
set(FFX_RUNTIME_RELATIVE_DIRECTORY "Shaders/Upscaling/FidelityFX")
set(FFX_RUNTIME_DIRECTORY
    "${FFX_RUNTIME_FEATURE_ROOT}/${FFX_RUNTIME_RELATIVE_DIRECTORY}"
)
file(MAKE_DIRECTORY "${FFX_RUNTIME_DIRECTORY}")

function(download_ffx_runtime _filename _sha256)
    set(_destination "${FFX_RUNTIME_DIRECTORY}/${_filename}")
    set(_download_status 0 "cached")
    if(EXISTS "${_destination}")
        file(SHA256 "${_destination}" _existing_sha256)
    endif()
    if(NOT _existing_sha256 STREQUAL _sha256)
        file(
            DOWNLOAD "${FFX_RUNTIME_BASE_URL}/${_filename}"
            "${_destination}"
            EXPECTED_HASH "SHA256=${_sha256}"
            STATUS _download_status
            TLS_VERIFY ON
            TIMEOUT 120
            INACTIVITY_TIMEOUT 20
        )
    endif()
    list(GET _download_status 0 _status_code)
    list(GET _download_status 1 _status_message)
    if(NOT _status_code EQUAL 0)
        file(REMOVE "${_destination}")
        message(
            FATAL_ERROR
            "Failed to download ${_filename}: ${_status_message}"
        )
    endif()
    set(FFX_RUNTIME_FILES ${FFX_RUNTIME_FILES} "${_destination}" PARENT_SCOPE)
endfunction()

set(FFX_RUNTIME_FILES "")
download_ffx_runtime(
    amd_fidelityfx_framegeneration_dx12.dll
    02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18
)
download_ffx_runtime(
    amd_fidelityfx_loader_dx12.dll
    E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA
)
download_ffx_runtime(
    amd_fidelityfx_upscaler_dx12.dll
    D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46
)

register_feature_payload(
    Upscaling
    FILES ${FFX_RUNTIME_FILES}
    DESTINATION "${FFX_RUNTIME_RELATIVE_DIRECTORY}"
)
