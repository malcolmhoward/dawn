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
