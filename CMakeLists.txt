cmake_minimum_required(VERSION 3.24)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  message(FATAL_ERROR "AuRunning currently supports CMake 3.24-3.x; use cmake<4")
endif()

project(au_running VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "AuRunning v1 supports Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ixwebsocket
  URL https://codeload.github.com/machinezone/IXWebSocket/zip/038e0bfc34547fa1539af4c9fc86d0bb2b8d259f
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
  nlohmann_json
  URL https://codeload.github.com/nlohmann/json/zip/722c03495f9978eb727f480b6ea0742f652e06a9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
  abseil
  URL https://codeload.github.com/abseil/abseil-cpp/zip/edbf66288b7e21c33b39512affc593ff7c3e18fd
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
  webrtc_apm
  URL https://codeload.github.com/jiemojiemo/webrtc-audio-processing-cmake/zip/2d34106f2ab170ebbdf557b8ad646e906887289f
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

add_compile_definitions(WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
FetchContent_MakeAvailable(ixwebsocket nlohmann_json)
FetchContent_GetProperties(abseil)
if(NOT abseil_POPULATED)
  FetchContent_Populate(abseil)
endif()
FetchContent_GetProperties(webrtc_apm)
if(NOT webrtc_apm_POPULATED)
  FetchContent_Populate(webrtc_apm)
  file(COPY "${abseil_SOURCE_DIR}/" DESTINATION "${webrtc_apm_SOURCE_DIR}/abseil-cpp")
endif()
set(WEBRTC_RTC_BASE_CMAKE "${webrtc_apm_SOURCE_DIR}/webrtc/rtc_base/CMakeLists.txt")
file(READ "${WEBRTC_RTC_BASE_CMAKE}" WEBRTC_RTC_BASE_CONTENT)
string(REPLACE
  "if(NOT WIN32)\n    list(REMOVE_ITEM RTC_BASE_SRC \"\${CMAKE_CURRENT_SOURCE_DIR}/synchronization/rw_lock_win.cc\")\nendif()"
  "if(WIN32)\n    list(REMOVE_ITEM RTC_BASE_SRC \"\${CMAKE_CURRENT_SOURCE_DIR}/synchronization/rw_lock_posix.cc\")\nelse()\n    list(REMOVE_ITEM RTC_BASE_SRC \"\${CMAKE_CURRENT_SOURCE_DIR}/synchronization/rw_lock_win.cc\")\nendif()"
  WEBRTC_RTC_BASE_CONTENT
  "${WEBRTC_RTC_BASE_CONTENT}"
)
file(WRITE "${WEBRTC_RTC_BASE_CMAKE}" "${WEBRTC_RTC_BASE_CONTENT}")
set(WEBRTC_COMMON_AUDIO_CMAKE "${webrtc_apm_SOURCE_DIR}/webrtc/common_audio/CMakeLists.txt")
file(READ "${WEBRTC_COMMON_AUDIO_CMAKE}" WEBRTC_COMMON_AUDIO_CONTENT)
string(REPLACE
  "CMAKE_SYSTEM_PROCESSOR MATCHES \"(x86)|(x86_64)|(i686)\""
  "CMAKE_SYSTEM_PROCESSOR MATCHES \"(x86)|(x86_64)|(i686)|(AMD64)|(amd64)\""
  WEBRTC_COMMON_AUDIO_CONTENT
  "${WEBRTC_COMMON_AUDIO_CONTENT}"
)
file(WRITE "${WEBRTC_COMMON_AUDIO_CMAKE}" "${WEBRTC_COMMON_AUDIO_CONTENT}")
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64)|(amd64)|(x64)")
  set(have_avx2 TRUE)
endif()
add_subdirectory("${webrtc_apm_SOURCE_DIR}" "${webrtc_apm_BINARY_DIR}" EXCLUDE_FROM_ALL)

add_executable(au-running src/main.cpp)
target_link_libraries(au-running PRIVATE
  ixwebsocket
  nlohmann_json::nlohmann_json
  webrtc_apm_wrapper
  avrt
  ole32
  uuid
  winmm
)
if(MSVC)
  target_sources(webrtc_apm_wrapper PRIVATE
    "${webrtc_apm_SOURCE_DIR}/webrtc/common_audio/fir_filter_avx2.cc"
  )
  target_compile_options(webrtc_apm_wrapper PRIVATE /arch:AVX2)
endif()
target_include_directories(au-running PRIVATE
  "${webrtc_apm_SOURCE_DIR}/webrtc"
  "${webrtc_apm_SOURCE_DIR}/abseil-cpp"
)
target_compile_options(au-running PRIVATE /W4 /permissive- /EHsc)

install(TARGETS au-running RUNTIME DESTINATION bin)
install(FILES configs/config.json DESTINATION configs)
