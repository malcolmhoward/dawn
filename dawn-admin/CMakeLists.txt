# dawn-admin CLI tool
# Administrative command-line interface for Dawn daemon management

cmake_minimum_required(VERSION 3.16)

# Source files
set(DAWN_ADMIN_SOURCES
    main.c
    socket_client.c
    password_prompt.c
)

# Create executable
add_executable(dawn-admin ${DAWN_ADMIN_SOURCES})

# Include directories
target_include_directories(dawn-admin PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Link libraries
# Note: SODIUM variables are set in parent CMakeLists.txt when ENABLE_AUTH is ON
target_link_libraries(dawn-admin pthread ${SODIUM_LIBRARIES})

# Install to sbin (administrative tools)
install(TARGETS dawn-admin
    RUNTIME DESTINATION sbin
)

# Set C standard
set_target_properties(dawn-admin PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
