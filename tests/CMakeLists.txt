cmake_minimum_required(VERSION 3.10)
project(dawn_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Parent directory contains the source files
set(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Find required packages
find_package(CURL REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)
find_package(OpenSSL REQUIRED)

include_directories(${PARENT_DIR})
include_directories(${CURL_INCLUDE_DIRS})
include_directories(${JSONC_INCLUDE_DIRS})

# Test: SSE Parser
add_executable(test_sse_parser
               test_sse_parser.c
               ${PARENT_DIR}/sse_parser.c
               ${PARENT_DIR}/logging.c)

target_link_libraries(test_sse_parser
                      ${JSONC_LIBRARIES})

# Test: Sentence Buffer
add_executable(test_sentence_buffer
               test_sentence_buffer.c
               ${PARENT_DIR}/sentence_buffer.c
               ${PARENT_DIR}/logging.c)

target_link_libraries(test_sentence_buffer
                      ${JSONC_LIBRARIES})

# Test: End-to-End Streaming
add_executable(test_streaming
               test_streaming.c
               ${PARENT_DIR}/llm_interface.c
               ${PARENT_DIR}/llm_openai.c
               ${PARENT_DIR}/llm_claude.c
               ${PARENT_DIR}/llm_streaming.c
               ${PARENT_DIR}/sentence_buffer.c
               ${PARENT_DIR}/sse_parser.c
               ${PARENT_DIR}/logging.c)

target_link_libraries(test_streaming
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      OpenSSL::SSL
                      OpenSSL::Crypto)

# Test: ASR Benchmark
# Find CUDA libraries (required by Vosk)
set(CUDA_TOOLKIT_ROOT_DIR "/usr/local/cuda-12.6")
find_library(CUDA_LIBRARIES NAMES cuda HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUDART_LIBRARIES NAMES cudart HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUSPARSE_LIBRARIES NAMES cusparse HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUBLAS_LIBRARIES NAMES cublas HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CUSOLVER_LIBRARIES NAMES cusolver HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
find_library(CURAND_LIBRARIES NAMES curand HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)

add_executable(asr_benchmark
               asr_benchmark.c
               ${PARENT_DIR}/asr_interface.c
               ${PARENT_DIR}/asr_vosk.c
               ${PARENT_DIR}/asr_whisper.c
               ${PARENT_DIR}/logging.c)

target_include_directories(asr_benchmark PRIVATE
                           ${PARENT_DIR}/whisper.cpp/include)

target_link_libraries(asr_benchmark
                      vosk
                      whisper
                      sndfile
                      ${JSONC_LIBRARIES}
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES})
