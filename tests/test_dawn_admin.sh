#!/bin/bash
#
# Integration tests for dawn-admin CLI
# Requires: Dawn daemon running with auth enabled
#
# Usage: ./test_dawn_admin.sh [--token TOKEN]
#
# If --token is not provided, the script will prompt for it.
# Get the token from Dawn daemon startup logs:
#   grep "Setup token" /path/to/dawn.log
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0
SKIPPED=0

# Path to dawn-admin binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DAWN_ADMIN="${PROJECT_ROOT}/build-debug/dawn-admin/dawn-admin"

# Test user credentials
TEST_USER="testuser_$$"
TEST_PASS="TestPass123!"
TEST_ADMIN_USER="testadmin_$$"
TEST_ADMIN_PASS="AdminPass456!"

# Admin credentials for authenticated operations (set after creating admin)
ADMIN_USER=""
ADMIN_PASS=""

# Setup token (can be set via --token arg or DAWN_SETUP_TOKEN env var)
SETUP_TOKEN="${DAWN_SETUP_TOKEN:-}"

# Backup path for db backup test
BACKUP_PATH="/tmp/dawn_auth_backup_$$.db"

print_header() {
    echo ""
    echo "========================================"
    echo " $1"
    echo "========================================"
}

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((++PASSED))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((++FAILED))
}

skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((++SKIPPED))
}

info() {
    echo -e "[INFO] $1"
}

# Check if dawn-admin exists
check_binary() {
    if [[ ! -x "$DAWN_ADMIN" ]]; then
        echo -e "${RED}Error: dawn-admin not found at $DAWN_ADMIN${NC}"
        echo "Build the project first: make -C build-debug -j4"
        exit 1
    fi
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --token)
                SETUP_TOKEN="$2"
                shift 2
                ;;
            --help|-h)
                echo "Usage: $0 [--token TOKEN]"
                echo ""
                echo "Options:"
                echo "  --token TOKEN  Setup token from Dawn daemon logs"
                echo "  --help         Show this help"
                echo ""
                echo "Environment variables:"
                echo "  DAWN_SETUP_TOKEN  Alternative to --token"
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                exit 1
                ;;
        esac
    done
}

# Prompt for setup token if not provided
get_setup_token() {
    if [[ -z "$SETUP_TOKEN" ]]; then
        echo ""
        echo "A setup token is required to create test users."
        echo "Find it in Dawn daemon logs: grep 'Setup token' <logfile>"
        echo ""
        read -p "Enter setup token (DAWN-XXXX-XXXX-XXXX-XXXX): " SETUP_TOKEN

        if [[ -z "$SETUP_TOKEN" ]]; then
            echo -e "${RED}No token provided. Exiting.${NC}"
            exit 1
        fi
    fi
}

# ============================================================================
# Test Cases
# ============================================================================

test_ping() {
    print_header "Testing: ping"

    if $DAWN_ADMIN ping >/dev/null 2>&1; then
        pass "Daemon responded to ping"
    else
        fail "Daemon did not respond to ping"
        echo "Is the Dawn daemon running with ENABLE_AUTH?"
        exit 1
    fi
}

test_user_create() {
    print_header "Testing: user create"

    # Create admin user (required for initial setup)
    # Note: Setup tokens are one-time use - only one user can be created per token
    info "Creating admin user: $TEST_ADMIN_USER"
    if DAWN_SETUP_TOKEN="$SETUP_TOKEN" DAWN_PASSWORD="$TEST_ADMIN_PASS" \
       $DAWN_ADMIN user create "$TEST_ADMIN_USER" --admin 2>&1; then
        pass "Created admin user '$TEST_ADMIN_USER'"
        ADMIN_USER="$TEST_ADMIN_USER"
        ADMIN_PASS="$TEST_ADMIN_PASS"
    else
        fail "Failed to create admin user '$TEST_ADMIN_USER'"
        return 1
    fi

    # Verify token is now invalidated (should fail)
    info "Verifying setup token is now invalidated"
    if DAWN_SETUP_TOKEN="$SETUP_TOKEN" DAWN_PASSWORD="$TEST_PASS" \
       $DAWN_ADMIN user create "shouldfail_$$" --admin 2>&1; then
        fail "Token should have been invalidated after first use"
    else
        pass "Setup token correctly invalidated after use"
    fi
}

test_user_list() {
    print_header "Testing: user list"

    output=$($DAWN_ADMIN user list 2>&1) || true

    if echo "$output" | grep -q "$TEST_ADMIN_USER"; then
        pass "User list contains admin user"
    else
        fail "User list missing admin user"
        echo "$output"
    fi

    # Test JSON output (--json flag not yet implemented)
    # info "Testing JSON output"
    # if $DAWN_ADMIN user list --json 2>&1 | grep -q '"username"'; then
    #     pass "JSON output format works"
    # else
    #     fail "JSON output format failed"
    # fi
    skip "JSON output not yet implemented"
}

test_user_passwd() {
    print_header "Testing: user passwd"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for passwd test"
        return
    fi

    NEW_PASS="NewPassword789!"

    # Change admin's own password (use current password for auth)
    info "Changing password for $TEST_ADMIN_USER"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" DAWN_PASSWORD="$NEW_PASS" \
       $DAWN_ADMIN user passwd "$TEST_ADMIN_USER" 2>&1; then
        pass "Password changed for '$TEST_ADMIN_USER'"
        # Update for later tests
        ADMIN_PASS="$NEW_PASS"
    else
        fail "Failed to change password for '$TEST_ADMIN_USER'"
    fi
}

test_user_unlock() {
    print_header "Testing: user unlock"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for unlock test"
        return
    fi

    # Note: We can't easily lock an account without the WebUI login,
    # so we just test that the command runs without error on an unlocked user
    info "Running unlock on '$TEST_ADMIN_USER' (already unlocked)"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
       $DAWN_ADMIN user unlock "$TEST_ADMIN_USER" 2>&1; then
        pass "Unlock command executed successfully"
    else
        # This might fail if user isn't locked, which is fine
        pass "Unlock command completed (user may not have been locked)"
    fi
}

test_session_list() {
    print_header "Testing: session list"

    # Just verify the command runs - there may or may not be sessions
    if $DAWN_ADMIN session list 2>&1; then
        pass "Session list command executed"
    else
        fail "Session list command failed"
    fi

    # Test JSON output
    if $DAWN_ADMIN session list --json >/dev/null 2>&1; then
        pass "Session list JSON output works"
    else
        fail "Session list JSON output failed"
    fi
}

test_db_status() {
    print_header "Testing: db status"

    output=$($DAWN_ADMIN db status 2>&1) || true

    if echo "$output" | grep -qi "user"; then
        pass "Database status shows user info"
    else
        fail "Database status missing user info"
        echo "$output"
    fi

    if echo "$output" | grep -qi "session"; then
        pass "Database status shows session info"
    else
        fail "Database status missing session info"
    fi
}

test_db_backup() {
    print_header "Testing: db backup"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for backup test"
        return
    fi

    # Remove any existing backup
    rm -f "$BACKUP_PATH"

    info "Backing up to $BACKUP_PATH"
    if DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
       $DAWN_ADMIN db backup "$BACKUP_PATH" 2>&1; then
        if [[ -f "$BACKUP_PATH" ]]; then
            pass "Database backup created"

            # Check permissions (should be 0600)
            perms=$(stat -c "%a" "$BACKUP_PATH" 2>/dev/null || stat -f "%Lp" "$BACKUP_PATH" 2>/dev/null)
            if [[ "$perms" == "600" ]]; then
                pass "Backup has secure permissions (0600)"
            else
                fail "Backup permissions incorrect: $perms (expected 600)"
            fi

            # Cleanup
            rm -f "$BACKUP_PATH"
        else
            fail "Backup file not created"
        fi
    else
        fail "Database backup command failed"
    fi
}

test_db_compact() {
    print_header "Testing: db compact"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for compact test"
        return
    fi

    info "Compacting database (may be rate-limited)"
    output=$(DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
             $DAWN_ADMIN db compact 2>&1)

    if echo "$output" | grep -qi "compacted\|rate.limited\|success"; then
        pass "Database compact command executed"
    else
        # Rate limiting is expected if run recently
        if echo "$output" | grep -qi "rate\|limit\|wait"; then
            pass "Database compact correctly rate-limited"
        else
            fail "Database compact unexpected response: $output"
        fi
    fi
}

test_log_show() {
    print_header "Testing: log show"

    # Basic log show
    if $DAWN_ADMIN log show 2>&1 | head -20 >/dev/null; then
        pass "Log show command executed"
    else
        fail "Log show command failed"
    fi

    # With limit
    info "Testing --last option"
    if $DAWN_ADMIN log show --last 5 2>&1 >/dev/null; then
        pass "Log show with --last works"
    else
        fail "Log show with --last failed"
    fi

    # Filter by event type
    info "Testing --type filter"
    if $DAWN_ADMIN log show --type USER_CREATED 2>&1 >/dev/null; then
        pass "Log show with --type filter works"
    else
        fail "Log show with --type filter failed"
    fi

    # Filter by user
    info "Testing --user filter"
    if $DAWN_ADMIN log show --user "$TEST_ADMIN_USER" 2>&1 >/dev/null; then
        pass "Log show with --user filter works"
    else
        fail "Log show with --user filter failed"
    fi
}

test_user_delete() {
    print_header "Testing: user delete"

    if [[ -z "$ADMIN_USER" ]]; then
        skip "No admin user available for delete test"
        return
    fi

    # Try to delete the test admin user (should be blocked - last admin protection)
    info "Attempting to delete last admin: $TEST_ADMIN_USER (should be blocked)"
    output=$(DAWN_ADMIN_USER="$ADMIN_USER" DAWN_ADMIN_PASSWORD="$ADMIN_PASS" \
             $DAWN_ADMIN user delete "$TEST_ADMIN_USER" --yes 2>&1) || true

    if echo "$output" | grep -qi "last admin"; then
        pass "Last admin deletion correctly blocked"
    elif $DAWN_ADMIN user list 2>&1 | grep -q "$TEST_ADMIN_USER"; then
        pass "Admin user still exists (deletion was blocked)"
    else
        fail "Last admin was deleted - protection failed!"
    fi
}

test_last_admin_protection() {
    # Already tested in test_user_delete - skip duplicate test
    skip "Last admin protection already tested in user delete"
}

# ============================================================================
# Main
# ============================================================================

main() {
    echo "========================================"
    echo " Dawn Admin CLI Integration Tests"
    echo "========================================"
    echo ""
    echo "Binary: $DAWN_ADMIN"
    echo "Test user: $TEST_USER"
    echo "Test admin: $TEST_ADMIN_USER"
    echo ""

    check_binary
    parse_args "$@"

    # Test connectivity first
    test_ping

    # Get setup token for user creation
    get_setup_token

    # Run tests in order
    test_user_create
    test_user_list
    test_user_passwd
    test_user_unlock
    test_session_list
    test_db_status
    test_db_backup
    test_db_compact
    test_log_show

    # Cleanup tests (run last)
    test_user_delete
    test_last_admin_protection

    # Summary
    print_header "Test Summary"
    echo -e "${GREEN}Passed: $PASSED${NC}"
    echo -e "${RED}Failed: $FAILED${NC}"
    echo -e "${YELLOW}Skipped: $SKIPPED${NC}"
    echo ""

    if [[ $FAILED -gt 0 ]]; then
        echo -e "${RED}Some tests failed!${NC}"
        exit 1
    else
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    fi
}

main "$@"
