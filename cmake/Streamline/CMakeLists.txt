cmake_minimum_required(VERSION 3.21)

add_library(Streamline INTERFACE)

target_include_directories(
  Streamline INTERFACE ${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include)

if(MSVC)
  target_compile_options(Streamline INTERFACE /wd5103)
endif()

if(CMAKE_HOST_UNIX)
  # sl_pcl.h's __cplusplus==202302L check for std::to_underlying is unreliable under this
  # toolchain; patched here (NVIDIA's repo, not our fork) to always use the portable fallback.
  set(_sl_pcl_h "${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include/sl_pcl.h")
  file(READ "${_sl_pcl_h}" _sl_pcl_h_content)
  # Normalize to LF first so the REPLACE below matches regardless of the file's checked-out line endings.
  string(REPLACE "\r\n" "\n" _sl_pcl_h_content "${_sl_pcl_h_content}")
  string(
    REPLACE
    "// c++23 has to_underlying implementation\n#if __cplusplus == 202302L\nusing to_underlying = std::to_underlying;\n#else\n// Return \`enum class\` member as value of underlying type (i.e. an int).  Basically same as:\n// static_cast<std::underlying_type_t<decltype(value)>>(value);\n// See c++23s std::to_underlying()\ntemplate<class T>\nconstexpr auto to_underlying(T value)\n{\n    return std::underlying_type_t<T>(value);\n}\n#endif"
    "// Portable to_underlying fallback (patched at configure time by\n// cmake/Streamline/CMakeLists.txt); identical to std::to_underlying.\ntemplate<class T>\nconstexpr auto to_underlying(T value)\n{\n    return std::underlying_type_t<T>(value);\n}"
    _sl_pcl_h_content
    "${_sl_pcl_h_content}"
  )
  # string(REPLACE) is a silent no-op if the search text isn't found;
  # fail loud rather than silently ship the unpatched, broken form.
  if(NOT _sl_pcl_h_content MATCHES "Portable to_underlying fallback")
    message(FATAL_ERROR "${_sl_pcl_h}: expected to_underlying patch text not found; Streamline-DX12 may have changed upstream.")
  endif()
  string(REPLACE "\n" "\r\n" _sl_pcl_h_content "${_sl_pcl_h_content}")
  file(WRITE "${_sl_pcl_h}" "${_sl_pcl_h_content}")
endif()
