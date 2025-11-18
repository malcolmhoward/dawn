#!/usr/bin/env bash

###############################################################################
# format_code.sh - Automated code formatting for C/C++ projects
#
# This script recursively formats C/C++ source files using clang-format
# with the project's style configuration.
#
# Usage:
#   ./format_code.sh [directory]
#
# If no directory is specified, formats current directory.
###############################################################################

set -e  # Exit on error

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
CLANG_FORMAT="clang-format"
CONFIG_FILE=".clang-format"

# File extensions to format
C_EXTENSIONS=("c" "h")
CPP_EXTENSIONS=("cpp" "hpp" "cc" "cxx" "hh" "hxx")

# Directories and files to exclude (add more as needed)
EXCLUDE_DIRS=(
   ".git"
   "build"
   "cmake-build-debug"
   "cmake-build-release"
   ".vscode"
   ".idea"
   "node_modules"
   "vendor"
   "third_party"
   "external"
   "utf8"
   "whisper.cpp"
)

EXCLUDE_FILES=(
   "json.hpp"  # Third-party library
   "piper.c"
   "piper.h"
   "utf8.h"
   "vosk_api.h"
)

###############################################################################
# Functions
###############################################################################

print_usage() {
   cat << USAGE
Usage: $0 [OPTIONS] [DIRECTORY]

Format C/C++ source code using clang-format according to project style.

OPTIONS:
   -h, --help          Show this help message
   -n, --dry-run       Show what would be formatted without making changes
   -v, --verbose       Show detailed output
   -c, --config FILE   Use alternate config file (default: .clang-format)
   --check             Check if files are formatted (exit 1 if not)

EXAMPLES:
   $0                  Format all files in current directory
   $0 src/             Format all files in src/ directory
   $0 --dry-run        Show what would be changed without formatting
   $0 --check          Check if code is properly formatted (CI mode)

USAGE
}

log_info() {
   echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
   echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
   echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
   echo -e "${RED}[ERROR]${NC} $1"
}

check_clang_format() {
   if ! command -v "$CLANG_FORMAT" &> /dev/null; then
      log_error "clang-format not found. Please install it:"
      echo "  Ubuntu/Debian: sudo apt-get install clang-format"
      echo "  macOS:         brew install clang-format"
      echo "  Fedora:        sudo dnf install clang-tools-extra"
      exit 1
   fi
   
   local version
   version=$($CLANG_FORMAT --version | grep -oP '\d+\.\d+' | head -1)
   log_info "Using clang-format version $version"
}

should_exclude_dir() {
   local dir=$1
   local basename
   basename=$(basename "$dir")
   
   for exclude in "${EXCLUDE_DIRS[@]}"; do
      if [[ "$basename" == "$exclude" ]]; then
         return 0
      fi
   done
   return 1
}

should_exclude_file() {
   local file=$1
   local basename
   basename=$(basename "$file")
   
   for exclude in "${EXCLUDE_FILES[@]}"; do
      if [[ "$basename" == "$exclude" ]]; then
         return 0
      fi
   done
   return 1
}

is_c_file() {
   local file=$1
   local ext="${file##*.}"
   
   for e in "${C_EXTENSIONS[@]}"; do
      if [[ "$ext" == "$e" ]]; then
         return 0
      fi
   done
   return 1
}

is_cpp_file() {
   local file=$1
   local ext="${file##*.}"
   
   for e in "${CPP_EXTENSIONS[@]}"; do
      if [[ "$ext" == "$e" ]]; then
         return 0
      fi
   done
   return 1
}

format_file() {
   local file=$1
   local dry_run=$2
   local check_mode=$3
   local verbose=$4
   
   if should_exclude_file "$file"; then
      [[ $verbose -eq 1 ]] && log_warning "Skipping excluded file: $file"
      return 0
   fi
   
   if [[ $check_mode -eq 1 ]]; then
      # Check mode: see if file needs formatting
      if ! $CLANG_FORMAT --style=file:"$CONFIG_FILE" --dry-run --Werror "$file" &> /dev/null; then
         log_error "Not formatted: $file"
         return 1
      fi
      [[ $verbose -eq 1 ]] && log_success "OK: $file"
      return 0
   fi
   
   if [[ $dry_run -eq 1 ]]; then
      # Dry run: show what would change
      if ! $CLANG_FORMAT --style=file:"$CONFIG_FILE" --dry-run --Werror "$file" &> /dev/null; then
         log_info "Would format: $file"
      fi
   else
      # Actually format the file
      if $CLANG_FORMAT --style=file:"$CONFIG_FILE" -i "$file"; then
         [[ $verbose -eq 1 ]] && log_success "Formatted: $file"
      else
         log_error "Failed to format: $file"
         return 1
      fi
   fi
   
   return 0
}

find_and_format() {
   local target_dir=$1
   local dry_run=$2
   local check_mode=$3
   local verbose=$4
   
   local count=0
   local failed=0
   
   # Build find command to exclude directories
   local exclude_opts=()
   for dir in "${EXCLUDE_DIRS[@]}"; do
      exclude_opts+=(-path "*/$dir" -prune -o)
   done
   
   # Find all C/C++ files
   while IFS= read -r -d '' file; do
      if is_c_file "$file" || is_cpp_file "$file"; then
         if format_file "$file" "$dry_run" "$check_mode" "$verbose"; then
            ((count++))
         else
            ((failed++))
         fi
      fi
   done < <(find "$target_dir" "${exclude_opts[@]}" -type f -print0)
   
   echo ""
   if [[ $check_mode -eq 1 ]]; then
      if [[ $failed -eq 0 ]]; then
         log_success "All $count files are properly formatted"
         return 0
      else
         log_error "$failed of $count files need formatting"
         return 1
      fi
   elif [[ $dry_run -eq 1 ]]; then
      log_info "Dry run complete. Checked $count files."
   else
      if [[ $failed -eq 0 ]]; then
         log_success "Successfully formatted $count files"
      else
         log_error "Failed to format $failed of $count files"
         return 1
      fi
   fi
}

###############################################################################
# Main
###############################################################################

main() {
   local target_dir="."
   local dry_run=0
   local check_mode=0
   local verbose=0
   
   # Parse arguments
   while [[ $# -gt 0 ]]; do
      case $1 in
         -h|--help)
            print_usage
            exit 0
            ;;
         -n|--dry-run)
            dry_run=1
            shift
            ;;
         --check)
            check_mode=1
            shift
            ;;
         -v|--verbose)
            verbose=1
            shift
            ;;
         -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
         -*)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
         *)
            target_dir="$1"
            shift
            ;;
      esac
   done
   
   # Check if target directory exists
   if [[ ! -d "$target_dir" ]]; then
      log_error "Directory not found: $target_dir"
      exit 1
   fi
   
   # Check if config file exists
   if [[ ! -f "$CONFIG_FILE" ]]; then
      log_error "Config file not found: $CONFIG_FILE"
      log_info "Create a .clang-format file or specify one with --config"
      exit 1
   fi
   
   # Check for clang-format
   check_clang_format
   
   # Show what we're doing
   if [[ $dry_run -eq 1 ]]; then
      log_info "DRY RUN: Checking what would be formatted in: $target_dir"
   elif [[ $check_mode -eq 1 ]]; then
      log_info "CHECK MODE: Verifying formatting in: $target_dir"
   else
      log_info "Formatting C/C++ files in: $target_dir"
   fi
   
   log_info "Using config: $CONFIG_FILE"
   echo ""
   
   # Do the work
   if find_and_format "$target_dir" "$dry_run" "$check_mode" "$verbose"; then
      exit 0
   else
      exit 1
   fi
}

# Run main
main "$@"
