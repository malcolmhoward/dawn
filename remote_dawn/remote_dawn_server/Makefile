# Voice Assistant Server - Production Build System
# Standalone voice assistant with complete Audio->Text->LLM->TTS pipeline
# Features: GPU acceleration (CUDA), real-time processing, ESP32 compatibility

# === Compiler Configuration ===
CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -std=c99 -pthread -O2 -g -D_POSIX_C_SOURCE=200809L
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread -O2 -g
LDFLAGS = -pthread -L. -Wl,-rpath,.

# === Dependencies ===
# CUDA Libraries (for GPU acceleration)
CUDA_PATH ?= /usr/local/cuda
CUDA_LDFLAGS = -L$(CUDA_PATH)/lib64 -L$(CUDA_PATH)/lib
CUDA_LIBS = -lcudart -lcublas -lcurand -lcusparse -lcusolver -lcuda

# TTS Dependencies (Piper)
PIPER_LIBS = -lpiper_phonemize -lonnxruntime -lspdlog -lespeak-ng
PIPER_INCLUDES = -I/usr/local/include/piper-phonemize \
                 -I/usr/local/include/onnxruntime \
                 -I/usr/local/include/spdlog

# Speech Recognition (Vosk)
VOSK_LIBS = -lvosk

# LLM Communication
LLM_LIBS = -lcurl -ljson-c

# All libraries
ALL_LIBS = $(LDFLAGS) $(CUDA_LDFLAGS) $(VOSK_LIBS) $(LLM_LIBS) $(PIPER_LIBS) $(CUDA_LIBS)

# === Source Files ===
# Main application
MAIN_SOURCES = voice_assistant_server.c

# Core server components  
SERVER_SOURCES = dawn_server.c dawn_network_audio.c logging.c
SERVER_OBJECTS = $(SERVER_SOURCES:.c=.o)

# TTS components (C++)
TTS_SOURCES = dawn_tts_wrapper.cpp piper.cpp  
TTS_OBJECTS = $(TTS_SOURCES:.cpp=.o)

# All objects
MAIN_OBJECTS = $(MAIN_SOURCES:.c=.o)
ALL_OBJECTS = $(MAIN_OBJECTS) $(SERVER_OBJECTS) $(TTS_OBJECTS)

# === Executables ===
TARGET = voice_assistant_server
HEADERS = dawn_server.h dawn_network_audio.h dawn_tts_wrapper.h logging.h

# === Default Target ===
all: $(TARGET)

# === Build Main Executable ===
$(TARGET): $(ALL_OBJECTS)
	@echo "Linking voice assistant server..."
	$(CXX) $(ALL_OBJECTS) -o $@ $(ALL_LIBS)
	@echo "Voice assistant server built successfully"

# === Object File Compilation ===
# C source files
%.o: %.c $(HEADERS)
	@echo "Compiling C: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# C++ source files  
%.o: %.cpp
	@echo "Compiling C++: $<"
	$(CXX) $(CXXFLAGS) $(PIPER_INCLUDES) -c $< -o $@

# === Verification ===
check-deps:
	@echo "Checking dependencies..."
	@ldconfig -p | grep -q piper_phonemize || echo "WARNING: piper_phonemize not found"
	@ldconfig -p | grep -q onnxruntime || echo "WARNING: onnxruntime not found"
	@ldconfig -p | grep -q libvosk || echo "WARNING: libvosk not found"
	@ldconfig -p | grep -q libcurl || echo "WARNING: libcurl not found"
	@ldconfig -p | grep -q json-c || echo "WARNING: json-c not found"
	@test -f "libvosk.so" || echo "WARNING: Local libvosk.so not found"
	@echo "Checking CUDA (optional for GPU acceleration)..."
	@test -d "$(CUDA_PATH)" && echo "INFO: CUDA found at $(CUDA_PATH)" || echo "INFO: CUDA not found (CPU-only mode)"
	@echo "Dependency check complete"

# === Testing ===
test: $(TARGET) check-deps
	@echo "Starting voice assistant server..."
	@echo "Make sure:"
	@echo "  - LLM server running at http://127.0.0.1:8080"  
	@echo "  - Vosk model at ../../vosk-model-en-us-0.22"
	@echo "  - Piper model at ../../en_GB-alba-medium.onnx[.json]"
	./$(TARGET)

# === Development Builds ===
debug: clean
	$(MAKE) CFLAGS="$(CFLAGS) -O0 -g3 -DDEBUG" CXXFLAGS="$(CXXFLAGS) -O0 -g3 -DDEBUG" all
	@echo "Debug build complete"

release: clean  
	$(MAKE) CFLAGS="$(CFLAGS) -O3 -DNDEBUG" CXXFLAGS="$(CXXFLAGS) -O3 -DNDEBUG" all
	@echo "Release build complete"

# === Maintenance ===
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(ALL_OBJECTS) $(TARGET)
	@echo "Clean complete"

clean-all: clean
	rm -f *.o *.a *.so core.*

# === Installation ===
install: $(TARGET)
	@echo "Installing voice assistant server..."
	sudo cp $(TARGET) /usr/local/bin/
	@echo "Installation complete"

uninstall:
	sudo rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstall complete"

# === Information ===
info:
	@echo "Voice Assistant Server Build System"
	@echo "Target: $(TARGET)"
	@echo "Sources: $(MAIN_SOURCES) $(SERVER_SOURCES) $(TTS_SOURCES)"
	@echo "Compiler: $(CC) / $(CXX)"
	@echo "CUDA Path: $(CUDA_PATH)"
	@echo "Libraries: Piper TTS, Vosk ASR, CUDA (GPU acceleration)"

help:
	@echo "Voice Assistant Server Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all          Build voice assistant server (default)"
	@echo "  test         Build and run with dependency check"
	@echo "  debug        Build with debug symbols"
	@echo "  release      Build optimized release version"
	@echo "  clean        Remove build artifacts"
	@echo "  install      Install to /usr/local/bin"
	@echo "  check-deps   Verify dependencies"
	@echo "  info         Show build information"
	@echo ""
	@echo "Requirements:"
	@echo "  - Piper TTS libraries and models"
	@echo "  - Vosk speech recognition"
	@echo "  - LLM server (llama.cpp)"
	@echo "  - ESP32 client for audio I/O"
	@echo "  - CUDA libraries (optional, for GPU acceleration)"

# === Phony Targets ===
.PHONY: all clean clean-all debug release test install uninstall check-deps info help
