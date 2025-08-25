# DAWN Audio Protocol Echo Server Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -O2 -g
LDFLAGS = -pthread

# Source files
SERVER_SOURCES = dawn_server.c
TEST_SOURCES = test_dawn_server.c
HEADERS = dawn_server.h

# Object files
SERVER_OBJECTS = $(SERVER_SOURCES:.c=.o)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)

# Executables
TEST_EXECUTABLE = test_dawn_server
SERVER_LIB = libdawn_server.a

# Default target
all: $(TEST_EXECUTABLE) $(SERVER_LIB)

# Test executable (links with server code)
$(TEST_EXECUTABLE): $(TEST_OBJECTS) $(SERVER_OBJECTS)
	$(CC) $(TEST_OBJECTS) $(SERVER_OBJECTS) -o $@ $(LDFLAGS)
	@echo "Built test executable: $@"

# Static library for integration into other projects
$(SERVER_LIB): $(SERVER_OBJECTS)
	ar rcs $@ $(SERVER_OBJECTS)
	@echo "Built static library: $@"

# Object file compilation
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(SERVER_OBJECTS) $(TEST_OBJECTS) $(TEST_EXECUTABLE) $(SERVER_LIB)
	@echo "Cleaned build artifacts"

# Install headers and library (optional)
install: $(SERVER_LIB)
	@echo "Installing DAWN server library..."
	sudo mkdir -p /usr/local/include/dawn
	sudo cp $(HEADERS) /usr/local/include/dawn/
	sudo cp $(SERVER_LIB) /usr/local/lib/
	sudo ldconfig
	@echo "Installation complete"

# Uninstall
uninstall:
	@echo "Uninstalling DAWN server library..."
	sudo rm -rf /usr/local/include/dawn
	sudo rm -f /usr/local/lib/$(SERVER_LIB)
	sudo ldconfig
	@echo "Uninstallation complete"

# Development targets
debug: CFLAGS += -DDEBUG -g3
debug: $(TEST_EXECUTABLE)

release: CFLAGS += -DNDEBUG -O3
release: $(TEST_EXECUTABLE) $(SERVER_LIB)

# Test the server
test: $(TEST_EXECUTABLE)
	@echo "Starting DAWN echo server test..."
	@echo "Note: Use Ctrl+C to stop the server"
	./$(TEST_EXECUTABLE)

# Check for common issues
check:
	@echo "Checking for potential issues..."
	cppcheck --enable=all --std=c99 $(SERVER_SOURCES) $(TEST_SOURCES) 2>/dev/null || echo "cppcheck not available"
	@echo "Check complete"

# Help target
help:
	@echo "DAWN Audio Protocol Echo Server Build System"
	@echo "============================================"
	@echo ""
	@echo "Available targets:"
	@echo "  all         - Build test executable and static library (default)"
	@echo "  clean       - Remove all build artifacts"
	@echo "  debug       - Build with debug symbols and DEBUG flag"
	@echo "  release     - Build optimized release version"
	@echo "  test        - Build and run the test server"
	@echo "  install     - Install library and headers to system"
	@echo "  uninstall   - Remove installed files from system"
	@echo "  check       - Run static analysis (requires cppcheck)"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make              # Build everything"
	@echo "  make test         # Build and run test server"
	@echo "  make debug        # Build with debug info"
	@echo "  make clean        # Clean up build files"

# Force rebuild
.PHONY: all clean install uninstall debug release test check help

# Compiler and system info
info:
	@echo "Build Configuration:"
	@echo "  CC: $(CC)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  System: $(shell uname -s)"
	@echo "  Architecture: $(shell uname -m)"
