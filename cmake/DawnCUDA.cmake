# DawnCUDA.cmake - Shared CUDA auto-detection for daemon and satellite builds
#
# Sets the following variables:
#   DAWN_CUDA_FOUND          - TRUE if CUDA toolkit and libraries were found
#   DAWN_CUDA_TOOLKIT_DIR    - Path to CUDA toolkit root
#   CUDA_LIBRARIES           - cuda library
#   CUDART_LIBRARIES         - cudart library
#   CUSPARSE_LIBRARIES       - cusparse library
#   CUBLAS_LIBRARIES         - cublas library
#   CUSOLVER_LIBRARIES       - cusolver library
#   CURAND_LIBRARIES         - curand library
#
# Options:
#   ENABLE_CUDA              - Set to OFF to disable CUDA detection (default: ON)
#   DAWN_CUDA_SKIP_RPI       - Set to ON to skip detection on Raspberry Pi (default: ON)

if(DEFINED _DAWN_CUDA_INCLUDED)
    return()
endif()
set(_DAWN_CUDA_INCLUDED TRUE)

option(ENABLE_CUDA "Enable CUDA GPU acceleration (auto-detected)" ON)

# Allow parent to signal RPi platform
if(NOT DEFINED DAWN_CUDA_SKIP_RPI)
    set(DAWN_CUDA_SKIP_RPI ON)
endif()

set(DAWN_CUDA_FOUND FALSE)

if(ENABLE_CUDA AND NOT (DAWN_CUDA_SKIP_RPI AND PLATFORM STREQUAL "RPI"))
    # Find CUDA toolkit
    find_path(DAWN_CUDA_TOOLKIT_DIR
        NAMES bin/nvcc
        HINTS /usr/local/cuda-12.6 /usr/local/cuda /opt/cuda
        NO_DEFAULT_PATH)
    if(NOT DAWN_CUDA_TOOLKIT_DIR)
        # Fallback: check if nvcc is on PATH
        find_program(_DAWN_NVCC_EXE nvcc)
        if(_DAWN_NVCC_EXE)
            get_filename_component(DAWN_CUDA_TOOLKIT_DIR "${_DAWN_NVCC_EXE}" DIRECTORY)
            get_filename_component(DAWN_CUDA_TOOLKIT_DIR "${DAWN_CUDA_TOOLKIT_DIR}" DIRECTORY)
        endif()
        unset(_DAWN_NVCC_EXE CACHE)
    endif()

    if(DAWN_CUDA_TOOLKIT_DIR)
        # Determine arch-specific library path
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(_CUDA_ARCH_SUFFIX "aarch64-linux")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
            set(_CUDA_ARCH_SUFFIX "x86_64-linux")
        else()
            set(_CUDA_ARCH_SUFFIX "${CMAKE_SYSTEM_PROCESSOR}-linux")
        endif()

        set(_CUDA_LIB_HINTS
            ${DAWN_CUDA_TOOLKIT_DIR}/targets/${_CUDA_ARCH_SUFFIX}/lib
            ${DAWN_CUDA_TOOLKIT_DIR}/lib64)

        find_library(CUDA_LIBRARIES NAMES cuda HINTS ${_CUDA_LIB_HINTS})
        find_library(CUDART_LIBRARIES NAMES cudart HINTS ${_CUDA_LIB_HINTS})
        find_library(CUSPARSE_LIBRARIES NAMES cusparse HINTS ${_CUDA_LIB_HINTS})
        find_library(CUBLAS_LIBRARIES NAMES cublas HINTS ${_CUDA_LIB_HINTS})
        find_library(CUSOLVER_LIBRARIES NAMES cusolver HINTS ${_CUDA_LIB_HINTS})
        find_library(CURAND_LIBRARIES NAMES curand HINTS ${_CUDA_LIB_HINTS})

        if(CUDA_LIBRARIES AND CUDART_LIBRARIES)
            set(DAWN_CUDA_FOUND TRUE)
            message(STATUS "CUDA support: ENABLED (toolkit: ${DAWN_CUDA_TOOLKIT_DIR})")
        else()
            message(STATUS "CUDA support: DISABLED (toolkit found but libraries missing)")
        endif()

        unset(_CUDA_ARCH_SUFFIX)
        unset(_CUDA_LIB_HINTS)
    else()
        message(STATUS "CUDA support: DISABLED (no CUDA toolkit found)")
    endif()
else()
    if(PLATFORM STREQUAL "RPI")
        message(STATUS "CUDA support: DISABLED (Raspberry Pi)")
    else()
        message(STATUS "CUDA support: DISABLED (-DENABLE_CUDA=OFF)")
    endif()
endif()
