# DawnTools.cmake - Tool Configuration and Compile-time Exclusion
#
# This module provides CMake options for enabling/disabling individual tools.
# When a tool is disabled, its source files are not compiled and the
# DAWN_ENABLE_<TOOL>_TOOL define is not set, allowing compile-time exclusion.
#
# Usage in CMakeLists.txt:
#   include(cmake/DawnTools.cmake)
#   # TOOL_SOURCES variable will contain enabled tool source files
#
# To disable a tool at configure time:
#   cmake -DDAWN_ENABLE_SHUTDOWN_TOOL=OFF ..

# =============================================================================
# Tool Enable/Disable Options
# =============================================================================

# Plugin tools (can be disabled)
option(DAWN_ENABLE_SHUTDOWN_TOOL "Enable shutdown command tool" ON)
option(DAWN_ENABLE_MUSIC_TOOL "Enable music playback tool" ON)
option(DAWN_ENABLE_CALCULATOR_TOOL "Enable calculator tool" ON)
option(DAWN_ENABLE_WEATHER_TOOL "Enable weather service tool" ON)
option(DAWN_ENABLE_SEARCH_TOOL "Enable web search tool" ON)
option(DAWN_ENABLE_URL_TOOL "Enable URL fetcher tool" ON)
option(DAWN_ENABLE_SMARTTHINGS_TOOL "Enable SmartThings integration" ON)
option(DAWN_ENABLE_MEMORY_TOOL "Enable memory/recall system" ON)
option(DAWN_ENABLE_DATETIME_TOOL "Enable date/time tools" ON)
option(DAWN_ENABLE_VOLUME_TOOL "Enable volume control tool" ON)
option(DAWN_ENABLE_LLM_STATUS_TOOL "Enable LLM status/switch tool" ON)
option(DAWN_ENABLE_SWITCH_LLM_TOOL "Enable switch_llm meta-tool" ON)
option(DAWN_ENABLE_RESET_CONVERSATION_TOOL "Enable reset_conversation tool" ON)
option(DAWN_ENABLE_VIEWING_TOOL "Enable viewing/vision tool (MQTT)" ON)
option(DAWN_ENABLE_HUD_TOOLS "Enable HUD control tools (MQTT)" ON)
option(DAWN_ENABLE_AUDIO_TOOLS "Enable voice amplifier and audio device tools" ON)
option(DAWN_ENABLE_SCHEDULER_TOOL "Enable scheduler/timer/alarm/reminder tool" ON)

# =============================================================================
# Tool Registry (always included)
# =============================================================================

set(TOOL_REGISTRY_SOURCES
    src/tools/tool_registry.c
)

# =============================================================================
# Conditional Source Files and Definitions
# =============================================================================

set(TOOL_SOURCES ${TOOL_REGISTRY_SOURCES})

# Shutdown Tool
if(DAWN_ENABLE_SHUTDOWN_TOOL)
    add_definitions(-DDAWN_ENABLE_SHUTDOWN_TOOL)
    list(APPEND TOOL_SOURCES src/tools/shutdown_tool.c)
    message(STATUS "DAWN: Shutdown tool ENABLED")
else()
    message(STATUS "DAWN: Shutdown tool DISABLED")
endif()

# Music Tool
if(DAWN_ENABLE_MUSIC_TOOL)
    add_definitions(-DDAWN_ENABLE_MUSIC_TOOL)
    list(APPEND TOOL_SOURCES src/tools/music_tool.c)
    message(STATUS "DAWN: Music tool ENABLED")
else()
    message(STATUS "DAWN: Music tool DISABLED")
endif()

# Calculator Tool
if(DAWN_ENABLE_CALCULATOR_TOOL)
    add_definitions(-DDAWN_ENABLE_CALCULATOR_TOOL)
    # Note: calculator.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/calculator_tool.c)
    message(STATUS "DAWN: Calculator tool ENABLED")
else()
    message(STATUS "DAWN: Calculator tool DISABLED")
endif()

# Weather Tool
if(DAWN_ENABLE_WEATHER_TOOL)
    add_definitions(-DDAWN_ENABLE_WEATHER_TOOL)
    # Note: weather_service.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/weather_tool.c)
    message(STATUS "DAWN: Weather tool ENABLED")
else()
    message(STATUS "DAWN: Weather tool DISABLED")
endif()

# Search Tool
if(DAWN_ENABLE_SEARCH_TOOL)
    add_definitions(-DDAWN_ENABLE_SEARCH_TOOL)
    # Note: web_search.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/search_tool.c)
    message(STATUS "DAWN: Search tool ENABLED")
else()
    message(STATUS "DAWN: Search tool DISABLED")
endif()

# URL Fetcher Tool
if(DAWN_ENABLE_URL_TOOL)
    add_definitions(-DDAWN_ENABLE_URL_TOOL)
    # Note: url_fetcher.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/url_tool.c)
    message(STATUS "DAWN: URL fetcher tool ENABLED")
else()
    message(STATUS "DAWN: URL fetcher tool DISABLED")
endif()

# SmartThings Tool
if(DAWN_ENABLE_SMARTTHINGS_TOOL)
    add_definitions(-DDAWN_ENABLE_SMARTTHINGS_TOOL)
    # Note: smartthings_service.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/smartthings_tool.c)
    message(STATUS "DAWN: SmartThings tool ENABLED")
else()
    message(STATUS "DAWN: SmartThings tool DISABLED")
endif()

# Memory Tool
if(DAWN_ENABLE_MEMORY_TOOL)
    add_definitions(-DDAWN_ENABLE_MEMORY_TOOL)
    list(APPEND TOOL_SOURCES src/tools/memory_tool.c)
    message(STATUS "DAWN: Memory tool ENABLED")
else()
    message(STATUS "DAWN: Memory tool DISABLED")
endif()

# DateTime Tools (date and time)
if(DAWN_ENABLE_DATETIME_TOOL)
    add_definitions(-DDAWN_ENABLE_DATETIME_TOOL)
    list(APPEND TOOL_SOURCES src/tools/datetime_tool.c)
    message(STATUS "DAWN: DateTime tools ENABLED")
else()
    message(STATUS "DAWN: DateTime tools DISABLED")
endif()

# Volume Tool
if(DAWN_ENABLE_VOLUME_TOOL)
    add_definitions(-DDAWN_ENABLE_VOLUME_TOOL)
    list(APPEND TOOL_SOURCES src/tools/volume_tool.c)
    message(STATUS "DAWN: Volume tool ENABLED")
else()
    message(STATUS "DAWN: Volume tool DISABLED")
endif()

# LLM Status Tool
if(DAWN_ENABLE_LLM_STATUS_TOOL)
    add_definitions(-DDAWN_ENABLE_LLM_STATUS_TOOL)
    list(APPEND TOOL_SOURCES src/tools/llm_status_tool.c)
    message(STATUS "DAWN: LLM Status tool ENABLED")
else()
    message(STATUS "DAWN: LLM Status tool DISABLED")
endif()

# Switch LLM Tool
if(DAWN_ENABLE_SWITCH_LLM_TOOL)
    add_definitions(-DDAWN_ENABLE_SWITCH_LLM_TOOL)
    list(APPEND TOOL_SOURCES src/tools/switch_llm_tool.c)
    message(STATUS "DAWN: Switch LLM tool ENABLED")
else()
    message(STATUS "DAWN: Switch LLM tool DISABLED")
endif()

# Reset Conversation Tool
if(DAWN_ENABLE_RESET_CONVERSATION_TOOL)
    add_definitions(-DDAWN_ENABLE_RESET_CONVERSATION_TOOL)
    list(APPEND TOOL_SOURCES src/tools/reset_conversation_tool.c)
    message(STATUS "DAWN: Reset Conversation tool ENABLED")
else()
    message(STATUS "DAWN: Reset Conversation tool DISABLED")
endif()

# Viewing Tool (MQTT-based vision)
if(DAWN_ENABLE_VIEWING_TOOL)
    add_definitions(-DDAWN_ENABLE_VIEWING_TOOL)
    list(APPEND TOOL_SOURCES src/tools/viewing_tool.c)
    message(STATUS "DAWN: Viewing tool ENABLED")
else()
    message(STATUS "DAWN: Viewing tool DISABLED")
endif()

# HUD Tools (MQTT-based: hud_control, hud_mode, faceplate, recording, visual_offset)
if(DAWN_ENABLE_HUD_TOOLS)
    add_definitions(-DDAWN_ENABLE_HUD_TOOLS)
    list(APPEND TOOL_SOURCES src/tools/hud_tools.c)
    message(STATUS "DAWN: HUD tools ENABLED")
else()
    message(STATUS "DAWN: HUD tools DISABLED")
endif()

# Audio Tools (voice_amplifier, audio_device)
if(DAWN_ENABLE_AUDIO_TOOLS)
    add_definitions(-DDAWN_ENABLE_AUDIO_TOOLS)
    list(APPEND TOOL_SOURCES src/tools/audio_tools.c)
    message(STATUS "DAWN: Audio tools ENABLED")
else()
    message(STATUS "DAWN: Audio tools DISABLED")
endif()

# Scheduler Tool
if(DAWN_ENABLE_SCHEDULER_TOOL)
    add_definitions(-DDAWN_ENABLE_SCHEDULER_TOOL)
    list(APPEND TOOL_SOURCES src/tools/scheduler_tool.c)
    message(STATUS "DAWN: Scheduler tool ENABLED")
else()
    message(STATUS "DAWN: Scheduler tool DISABLED")
endif()

# =============================================================================
# Summary
# =============================================================================

list(LENGTH TOOL_SOURCES TOOL_COUNT)
message(STATUS "DAWN: ${TOOL_COUNT} tool source files configured")
