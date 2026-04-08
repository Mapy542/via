#!/usr/bin/env bash
# ==============================================================================
# run-pjdfstest.sh — Run pjdfstest POSIX compliance suite against Via's FUSE fs
#
# This script:
#   1. Builds pjdfstest from source (cloned into /tmp)
#   2. Starts the Via FUSE test harness (headless mount)
#   3. Runs the pjdfstest suite against the mounted filesystem
#   4. Collects results and exits with appropriate code
#   5. Cleans up (unmount + remove temp files)
#
# Prerequisites:
#   - Via built with BUILD_TESTS=ON (via_fuse_harness binary available)
#   - FUSE3 installed and /dev/fuse accessible
#   - Environment variables set:
#       VIA_CLIENT_ID, VIA_CLIENT_SECRET, VIA_REFRESH_TOKEN
#   - Build tools: gcc, make, cmake
#   - Perl (for pjdfstest's prove runner)
#
# Usage:
#   ./scripts/run-pjdfstest.sh [--mount-point /path] [--harness /path/to/via_fuse_harness]
#
# Environment variables:
#   VIA_CLIENT_ID       OAuth 2.0 client ID
#   VIA_CLIENT_SECRET   OAuth 2.0 client secret
#   VIA_REFRESH_TOKEN   OAuth 2.0 refresh token
#   VIA_MOUNT_POINT     Mount point override (default: /tmp/via-fuse-test)
#   VIA_CACHE_SIZE_MB   Cache size in MB (default: 500)
#   PJDFSTEST_DIR       Path to existing pjdfstest build (skip clone/build)
#   SKIP_TESTS          Comma-separated pjdfstest groups to skip (e.g., "link,symlink")
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────

MOUNT_POINT="${VIA_MOUNT_POINT:-/tmp/via-fuse-test}"
HARNESS_BIN="${PROJECT_DIR}/build/via_fuse_harness"
PJDFSTEST_DIR="${PJDFSTEST_DIR:-}"
PJDFSTEST_CLONE="/tmp/pjdfstest"
RESULTS_DIR="${PROJECT_DIR}/build/e2e-results"
HARNESS_PID=""
READY_FILE="${MOUNT_POINT}/../.via-fuse-ready"

# Tests to skip — Google Drive FUSE doesn't support hard links, symlinks,
# or POSIX permission bits / ownership changes (everything runs as the
# mounting user). These are expected failures.
DEFAULT_SKIP="link,symlink,chown,chmod,chflags,chgrp"
SKIP_TESTS="${SKIP_TESTS:-${DEFAULT_SKIP}}"

# ── Helpers ───────────────────────────────────────────────────────────────────

info()    { echo -e "\033[1;34m==>\033[0m $*"; }
success() { echo -e "\033[1;32m==>\033[0m $*"; }
warn()    { echo -e "\033[1;33mWARN:\033[0m $*"; }
error()   { echo -e "\033[1;31mERROR:\033[0m $*" >&2; }

cleanup() {
    local exit_code=$?
    info "Cleaning up..."

    # Stop the harness
    if [[ -n "${HARNESS_PID}" ]] && kill -0 "${HARNESS_PID}" 2>/dev/null; then
        info "Sending SIGTERM to FUSE harness (PID ${HARNESS_PID})"
        kill -TERM "${HARNESS_PID}" 2>/dev/null || true
        # Wait up to 10 seconds for graceful shutdown
        for i in $(seq 1 20); do
            if ! kill -0 "${HARNESS_PID}" 2>/dev/null; then
                break
            fi
            sleep 0.5
        done
        # Force kill if still running
        if kill -0 "${HARNESS_PID}" 2>/dev/null; then
            warn "Harness did not exit gracefully, sending SIGKILL"
            kill -9 "${HARNESS_PID}" 2>/dev/null || true
        fi
    fi

    # Ensure unmounted
    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        warn "Mount point still active, forcing unmount"
        fusermount3 -u -z "${MOUNT_POINT}" 2>/dev/null || \
        fusermount -u -z "${MOUNT_POINT}" 2>/dev/null || true
    fi

    rm -f "${READY_FILE}"

    if [[ ${exit_code} -ne 0 ]]; then
        error "Script exited with code ${exit_code}"
    fi
    exit ${exit_code}
}

trap cleanup EXIT

# ── Parse arguments ───────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mount-point)
            MOUNT_POINT="$2"
            shift 2
            ;;
        --harness)
            HARNESS_BIN="$2"
            shift 2
            ;;
        --pjdfstest-dir)
            PJDFSTEST_DIR="$2"
            shift 2
            ;;
        --skip)
            SKIP_TESTS="$2"
            shift 2
            ;;
        -h|--help)
            head -30 "$0" | tail -25
            exit 0
            ;;
        *)
            error "Unknown argument: $1"
            exit 1
            ;;
    esac
done

# ── Validate prerequisites ───────────────────────────────────────────────────

if [[ -z "${VIA_CLIENT_ID:-}" || -z "${VIA_CLIENT_SECRET:-}" || -z "${VIA_REFRESH_TOKEN:-}" ]]; then
    error "Required environment variables not set:"
    error "  VIA_CLIENT_ID, VIA_CLIENT_SECRET, VIA_REFRESH_TOKEN"
    exit 1
fi

if [[ ! -f "${HARNESS_BIN}" ]]; then
    error "FUSE test harness not found at: ${HARNESS_BIN}"
    error "Build with: cmake -B build -DBUILD_TESTS=ON && cmake --build build"
    exit 1
fi

if [[ ! -e /dev/fuse ]]; then
    error "/dev/fuse not available. Install fuse3 and ensure the module is loaded."
    exit 1
fi

for cmd in perl make cmake; do
    if ! command -v "${cmd}" &>/dev/null; then
        error "Required command '${cmd}' not found"
        exit 1
    fi
done

# ── Step 1: Build pjdfstest ──────────────────────────────────────────────────

if [[ -n "${PJDFSTEST_DIR}" && -f "${PJDFSTEST_DIR}/pjdfstest" ]]; then
    info "Using existing pjdfstest at ${PJDFSTEST_DIR}"
else
    info "Cloning and building pjdfstest..."
    if [[ -d "${PJDFSTEST_CLONE}" ]]; then
        rm -rf "${PJDFSTEST_CLONE}"
    fi
    git clone --depth 1 https://github.com/pjd/pjdfstest.git "${PJDFSTEST_CLONE}"
    cmake -B "${PJDFSTEST_CLONE}/build" -S "${PJDFSTEST_CLONE}"
    cmake --build "${PJDFSTEST_CLONE}/build" --parallel
    PJDFSTEST_DIR="${PJDFSTEST_CLONE}/build"
    info "pjdfstest built at ${PJDFSTEST_DIR}"
fi

PJDFSTEST_BIN="${PJDFSTEST_DIR}/pjdfstest"
if [[ ! -x "${PJDFSTEST_BIN}" ]]; then
    error "pjdfstest binary not found or not executable at ${PJDFSTEST_BIN}"
    exit 1
fi

# ── Step 2: Start the FUSE test harness ──────────────────────────────────────

info "Starting FUSE test harness..."
info "  Mount point: ${MOUNT_POINT}"

export VIA_MOUNT_POINT="${MOUNT_POINT}"
export QT_QPA_PLATFORM=offscreen

rm -f "${READY_FILE}"

"${HARNESS_BIN}" &
HARNESS_PID=$!
info "  Harness PID: ${HARNESS_PID}"

# Wait for mount to become ready (up to 60 seconds)
info "Waiting for FUSE mount to become ready..."
TIMEOUT=60
ELAPSED=0
while [[ ${ELAPSED} -lt ${TIMEOUT} ]]; do
    if [[ -f "${READY_FILE}" ]] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        success "FUSE filesystem mounted and ready (${ELAPSED}s)"
        break
    fi

    # Check harness hasn't crashed
    if ! kill -0 "${HARNESS_PID}" 2>/dev/null; then
        error "FUSE harness exited prematurely"
        wait "${HARNESS_PID}" || true
        HARNESS_PID=""
        exit 1
    fi

    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

if [[ ${ELAPSED} -ge ${TIMEOUT} ]]; then
    error "Timed out waiting for FUSE mount (${TIMEOUT}s)"
    exit 1
fi

# ── Step 3: Run pjdfstest ────────────────────────────────────────────────────

mkdir -p "${RESULTS_DIR}"

# Create a test working directory inside the FUSE mount
TEST_DIR="${MOUNT_POINT}/pjdfstest-workdir"
mkdir -p "${TEST_DIR}" || {
    error "Failed to create test directory inside FUSE mount"
    exit 1
}

info "Running pjdfstest against ${TEST_DIR}"
info "  Skipping test groups: ${SKIP_TESTS}"

cd "${TEST_DIR}"

# Build the list of test directories to run (exclude skipped groups)
PJDFSTEST_TESTS_DIR="${PJDFSTEST_DIR}/../tests"
if [[ ! -d "${PJDFSTEST_TESTS_DIR}" ]]; then
    # Try alternate layout (some builds put tests/ at top level)
    PJDFSTEST_TESTS_DIR="${PJDFSTEST_CLONE}/tests"
fi

if [[ ! -d "${PJDFSTEST_TESTS_DIR}" ]]; then
    error "Cannot find pjdfstest test scripts directory"
    exit 1
fi

# Convert skip list to array
IFS=',' read -ra SKIP_ARRAY <<< "${SKIP_TESTS}"

# Discover test groups
TEST_GROUPS=()
for dir in "${PJDFSTEST_TESTS_DIR}"/*/; do
    group_name="$(basename "${dir}")"
    skip=false
    for skip_item in "${SKIP_ARRAY[@]}"; do
        if [[ "${group_name}" == "${skip_item}" ]]; then
            skip=true
            break
        fi
    done
    if [[ "${skip}" == false ]]; then
        TEST_GROUPS+=("${group_name}")
    else
        warn "Skipping test group: ${group_name}"
    fi
done

info "Test groups to run: ${TEST_GROUPS[*]}"

# Run tests using prove (Perl TAP harness) if available, else run directly
OVERALL_EXIT=0
RESULTS_LOG="${RESULTS_DIR}/pjdfstest-results.log"
: > "${RESULTS_LOG}"

for group in "${TEST_GROUPS[@]}"; do
    group_dir="${PJDFSTEST_TESTS_DIR}/${group}"
    if [[ ! -d "${group_dir}" ]]; then
        continue
    fi

    info "Running test group: ${group}"

    # Find .t test files
    test_files=("${group_dir}"/*.t 2>/dev/null) || true
    if [[ ${#test_files[@]} -eq 0 || ! -f "${test_files[0]}" ]]; then
        warn "  No .t files in ${group}, skipping"
        continue
    fi

    if command -v prove &>/dev/null; then
        prove -f "${group_dir}"/*.t \
            :: "${PJDFSTEST_BIN}" \
            2>&1 | tee -a "${RESULTS_LOG}" || OVERALL_EXIT=1
    else
        # Fallback: run each .t file directly
        for t_file in "${group_dir}"/*.t; do
            info "  Running $(basename "${t_file}")"
            perl "${t_file}" "${PJDFSTEST_BIN}" \
                2>&1 | tee -a "${RESULTS_LOG}" || OVERALL_EXIT=1
        done
    fi
done

# ── Step 4: Summary ──────────────────────────────────────────────────────────

info "Results saved to ${RESULTS_LOG}"

# Count pass/fail from TAP output
PASS_COUNT=$(grep -c "^ok " "${RESULTS_LOG}" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -c "^not ok " "${RESULTS_LOG}" 2>/dev/null || echo 0)

echo ""
echo "============================================="
echo "  pjdfstest Results Summary"
echo "============================================="
echo "  Passed:  ${PASS_COUNT}"
echo "  Failed:  ${FAIL_COUNT}"
echo "  Skipped: ${SKIP_TESTS}"
echo "============================================="
echo ""

if [[ ${OVERALL_EXIT} -ne 0 ]]; then
    warn "Some tests failed. Review ${RESULTS_LOG} for details."
else
    success "All executed tests passed!"
fi

# ── Step 5: Cleanup test directory ───────────────────────────────────────────

info "Removing test working directory..."
rm -rf "${TEST_DIR}" 2>/dev/null || true

exit ${OVERALL_EXIT}
