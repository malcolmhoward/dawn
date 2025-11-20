cmake_minimum_required(VERSION 3.10)

# For native CUDA code
set(CUDA_TOOLKIT_ROOT_DIR "/usr/local/cuda-12.6")
project(dawn LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the version number in the header file directly
set(VERSION_NUMBER "1.0.0")

# Try to get the current Git SHA
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_SHA_RESULT
)

# Check if the command was successful
if(NOT GIT_SHA_RESULT EQUAL 0)
    set(GIT_SHA "unknown")
endif()

# Pass the Git SHA to the compiler
add_definitions(-DGIT_SHA=\"${GIT_SHA}\")

# HTTP connections (AI and such)
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

# Local config, MQTT commmands, AI APIs
pkg_check_modules(JSONC REQUIRED json-c)
include_directories(${JSONC_INCLUDE_DIRS})

# MQTT
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
include_directories(${MOSQUITTO_INCLUDE_DIRS})

# Audio file playback
pkg_check_modules(FLAC REQUIRED flac)
include_directories(${FLAC_INCLUDE_DIRS})

# Base64 Encoding
find_package(OpenSSL REQUIRED)
include_directories(${OpenSSL_INCLUDE_DIR})

# Find the required CUDA libraries and add them to the link libraries
find_library(CUDA_LIBRARIES NAMES cuda HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUDART_LIBRARIES NAMES cudart HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUSPARSE_LIBRARIES NAMES cusparse HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUBLAS_LIBRARIES NAMES cublas HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUSOLVER_LIBRARIES NAMES cusolver HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CURAND_LIBRARIES NAMES curand HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)

# Wave file handling
#find_package(PkgConfig QUIET)
#if(PKG_CONFIG_FOUND)
#    pkg_check_modules(LIBSNDFILE_PKGCONF sndfile)
#endif(PKG_CONFIG_FOUND)

# Text to Speech
add_library(piper STATIC piper.cpp)
target_link_libraries(piper onnxruntime)

# Audio Backend Options - choose one
option(USE_ALSA "Use ALSA for all audio (capture and playback)" ON)

# Audio dependencies based on backend selection
if(USE_ALSA)
    # ALSA-only mode (no PulseAudio dependency)
    message(STATUS "Audio backend: ALSA (fully embedded mode)")
else()
    # PulseAudio mode (desktop/server systems)
    find_package(PulseAudio REQUIRED)
    include_directories(${PULSEAUDIO_INCLUDE_DIRS})
    message(STATUS "Audio backend: PulseAudio (desktop mode)")
endif()

# ASR Engine Options
option(ENABLE_VOSK "Enable Vosk ASR engine (legacy support)" OFF)

# Whisper ASR (disable examples, tests, and server for DAWN integration)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(whisper.cpp)

link_directories(${CMAKE_SOURCE_DIR})

# Base source files (always compiled)
set(DAWN_SOURCES
    asr_interface.c
    asr_whisper.c
    audio_capture_thread.c
    audio_utils.c
    dawn.c
    dawn_network_audio.c
    dawn_server.c
    dawn_wav_utils.c
    flac_playback.c
    llm_command_parser.c
    llm_interface.c
    llm_openai.c
    llm_claude.c
    llm_streaming.c
    logging.c
    mic_passthrough.c
    mosquitto_comms.c
    ring_buffer.c
    sentence_buffer.c
    sse_parser.c
    text_to_command_nuevo.c
    text_to_speech.cpp
    vad_silero.c
    word_to_number.c
)

# Define ALSA_DEVICE for conditional compilation
if(USE_ALSA)
    add_definitions(-DALSA_DEVICE)
endif()

# Conditionally add Vosk support
if(ENABLE_VOSK)
    list(APPEND DAWN_SOURCES asr_vosk.c)
    add_definitions(-DENABLE_VOSK)
    message(STATUS "Vosk ASR support: ENABLED")
else()
    message(STATUS "Vosk ASR support: DISABLED (Whisper only)")
endif()

add_executable(dawn ${DAWN_SOURCES})

target_link_libraries(dawn
                      piper
                      piper_phonemize
                      espeak-ng
                      onnxruntime
                      pthread
                      sndfile
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      ${SPDLOG_LIBRARIES}
                      whisper
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES}
                      ${MOSQUITTO_LIBRARIES}
                      ${FLAC_LIBRARIES}
                      OpenSSL::SSL
                      OpenSSL::Crypto)

# Link appropriate audio library based on backend selection
if(USE_ALSA)
    target_link_libraries(dawn asound)
else()
    target_link_libraries(dawn pulse-simple pulse)
endif()

# Conditionally link Vosk library
if(ENABLE_VOSK)
    target_link_libraries(dawn vosk)
endif()

target_include_directories(piper PUBLIC
                           /usr/local/include/piper-phonemize
                           ${SPDLOG_INCLUDE_DIRS}
                           /usr/local/include/onnxruntime/)

set(SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/en_GB-alba-medium.onnx
    ${CMAKE_SOURCE_DIR}/en_GB-alba-medium.onnx.json
)

set(DESTINATION_DIR
    ${CMAKE_BINARY_DIR}
)

file(COPY ${SOURCE_FILES} DESTINATION ${DESTINATION_DIR})

# Conditionally create Vosk model symlink
if(ENABLE_VOSK)
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/vosk-model-en-us-0.22 ${CMAKE_BINARY_DIR}/model)
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/commands_config_nuevo.json ${CMAKE_BINARY_DIR}/commands_config_nuevo.json)

# Optional: Build tests (enable with -DBUILD_TESTS=ON)
option(BUILD_TESTS "Build test programs" OFF)
if(BUILD_TESTS)
   add_subdirectory(tests)
endif()

