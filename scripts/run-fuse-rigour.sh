#!/usr/bin/env bash
# ==============================================================================
# run-fuse-rigour.sh — Local FUSE rigour / regression test script
#
# Exercises the Via FUSE filesystem with patterns that replicate known issues:
#   - Repeated readdir of root to detect duplicate entries
#   - Concurrent stat + open (thumbnail-style) access patterns
#   - Rapid directory navigation (enter subfolder → back to root)
#   - File access under load to trigger download timeouts
#   - pjdfstest POSIX compliance suite (optional)
#
# Designed to be run locally against a live Via FUSE mount. Combines the
# Dolphin-specific regression tests with the full pjdfstest POSIX suite.
#
# Prerequisites:
#   - Via built with BUILD_TESTS=ON (via_fuse_harness binary available)
#   - FUSE3 installed and /dev/fuse accessible
#   - For --skip-harness: an already-mounted Via FUSE filesystem
#   - For pjdfstest: gcc, make, cmake, perl (auto-built if not present)
#   - For harness mode: VIA_CLIENT_ID, VIA_CLIENT_SECRET, VIA_REFRESH_TOKEN
#
# Usage:
#   ./scripts/run-fuse-rigour.sh [--mount-point /path] [--iterations N]
#                                [--skip-harness] [--verbose]
#                                [--pjdfstest] [--skip-pjdfstest]
#                                [--pjdfstest-dir /path] [--pjdfstest-skip GROUPS]
#
# Options:
#   --mount-point DIR       Where to mount (default: /tmp/via-fuse-rigour)
#   --harness PATH          Path to via_fuse_harness (default: build/via_fuse_harness)
#   --iterations N          Number of readdir stress iterations (default: 20)
#   --skip-harness          Assume mount is already active (e.g. running Via app)
#   --verbose               Print every readdir listing for diff inspection
#   --pjdfstest             Run pjdfstest POSIX compliance suite (auto-builds)
#   --skip-pjdfstest        Skip pjdfstest even if available (default behavior)
#   --pjdfstest-dir PATH    Path to existing pjdfstest build (skip clone/build)
#   --pjdfstest-skip GROUPS Comma-separated pjdfstest groups to skip
#                           (default: link,symlink,chown,chmod,chflags,chgrp)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────

MOUNT_POINT="${VIA_MOUNT_POINT:-/tmp/via-fuse-rigour}"
HARNESS_BIN="${PROJECT_DIR}/build/via_fuse_harness"
ITERATIONS=20
SKIP_HARNESS=false
VERBOSE=false
HARNESS_PID=""
READY_FILE="${MOUNT_POINT}/../.via-fuse-ready"
RESULTS_DIR="${PROJECT_DIR}/build/rigour-results"
PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0

# pjdfstest options
RUN_PJDFSTEST=false
PJDFSTEST_DIR="${PJDFSTEST_DIR:-}"
PJDFSTEST_CLONE="/tmp/pjdfstest"
DEFAULT_PJDFSTEST_SKIP="link,symlink,chown,chmod,chflags,chgrp"
PJDFSTEST_SKIP="${PJDFSTEST_SKIP:-${DEFAULT_PJDFSTEST_SKIP}}"

# ── Helpers ───────────────────────────────────────────────────────────────────

info()    { echo -e "\033[1;34m==>\033[0m $*"; }
success() { echo -e "\033[1;32m  ✓\033[0m $*"; }
fail()    { echo -e "\033[1;31m  ✗\033[0m $*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
warn()    { echo -e "\033[1;33m  ⚠\033[0m $*"; WARN_COUNT=$((WARN_COUNT + 1)); }
pass()    { echo -e "\033[1;32m  ✓\033[0m $*"; PASS_COUNT=$((PASS_COUNT + 1)); }

cleanup() {
    local exit_code=$?
    info "Cleaning up..."

    if [[ "${SKIP_HARNESS}" == false && -n "${HARNESS_PID}" ]] && kill -0 "${HARNESS_PID}" 2>/dev/null; then
        info "Sending SIGTERM to FUSE harness (PID ${HARNESS_PID})"
        kill -TERM "${HARNESS_PID}" 2>/dev/null || true
        for i in $(seq 1 20); do
            if ! kill -0 "${HARNESS_PID}" 2>/dev/null; then break; fi
            sleep 0.5
        done
        if kill -0 "${HARNESS_PID}" 2>/dev/null; then
            warn "Harness did not exit gracefully, sending SIGKILL"
            kill -9 "${HARNESS_PID}" 2>/dev/null || true
        fi
    fi

    if [[ "${SKIP_HARNESS}" == false ]] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        warn "Mount point still active, forcing unmount"
        fusermount3 -u -z "${MOUNT_POINT}" 2>/dev/null || \
        fusermount  -u -z "${MOUNT_POINT}" 2>/dev/null || true
    fi

    rm -f "${READY_FILE}" 2>/dev/null || true

    if [[ ${exit_code} -ne 0 ]]; then
        echo ""
        echo -e "\033[1;31mScript exited with code ${exit_code}\033[0m"
    fi
    exit ${exit_code}
}

trap cleanup EXIT

# ── Parse arguments ───────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mount-point) MOUNT_POINT="$2"; shift 2 ;;
        --harness)     HARNESS_BIN="$2"; shift 2 ;;
        --iterations)  ITERATIONS="$2";  shift 2 ;;
        --skip-harness) SKIP_HARNESS=true; shift ;;
        --verbose)     VERBOSE=true; shift ;;
        --pjdfstest)   RUN_PJDFSTEST=true; shift ;;
        --skip-pjdfstest) RUN_PJDFSTEST=false; shift ;;
        --pjdfstest-dir) PJDFSTEST_DIR="$2"; RUN_PJDFSTEST=true; shift 2 ;;
        --pjdfstest-skip) PJDFSTEST_SKIP="$2"; shift 2 ;;
        -h|--help)
            head -40 "$0" | tail -35
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# ── Validate prerequisites ───────────────────────────────────────────────────

if [[ "${SKIP_HARNESS}" == false ]]; then
    if [[ -z "${VIA_CLIENT_ID:-}" || -z "${VIA_CLIENT_SECRET:-}" || -z "${VIA_REFRESH_TOKEN:-}" ]]; then
        echo "ERROR: Required env vars not set: VIA_CLIENT_ID, VIA_CLIENT_SECRET, VIA_REFRESH_TOKEN" >&2
        echo "  Use --skip-harness if you already have a running Via mount." >&2
        exit 1
    fi

    if [[ ! -f "${HARNESS_BIN}" ]]; then
        echo "ERROR: FUSE test harness not found at: ${HARNESS_BIN}" >&2
        echo "  Build with: cmake -B build -DBUILD_TESTS=ON && cmake --build build" >&2
        exit 1
    fi

    if [[ ! -e /dev/fuse ]]; then
        echo "ERROR: /dev/fuse not available." >&2
        exit 1
    fi
fi

mkdir -p "${RESULTS_DIR}"

# ── Step 1: Start FUSE test harness (unless --skip-harness) ─────────────────

if [[ "${SKIP_HARNESS}" == false ]]; then
    info "Starting FUSE test harness..."
    info "  Mount point: ${MOUNT_POINT}"

    mkdir -p "${MOUNT_POINT}"
    export VIA_MOUNT_POINT="${MOUNT_POINT}"
    export QT_QPA_PLATFORM=offscreen

    rm -f "${READY_FILE}"

    "${HARNESS_BIN}" &
    HARNESS_PID=$!
    info "  Harness PID: ${HARNESS_PID}"

    info "Waiting for FUSE mount to become ready..."
    TIMEOUT=60
    ELAPSED=0
    while [[ ${ELAPSED} -lt ${TIMEOUT} ]]; do
        if [[ -f "${READY_FILE}" ]] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
            success "FUSE filesystem mounted and ready (${ELAPSED}s)"
            break
        fi
        if ! kill -0 "${HARNESS_PID}" 2>/dev/null; then
            echo "ERROR: FUSE harness exited prematurely" >&2
            wait "${HARNESS_PID}" || true
            HARNESS_PID=""
            exit 1
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
    done

    if [[ ${ELAPSED} -ge ${TIMEOUT} ]]; then
        echo "ERROR: Timed out waiting for FUSE mount (${TIMEOUT}s)" >&2
        exit 1
    fi
else
    info "Skipping harness startup (--skip-harness)"
    if ! [[ -d "${MOUNT_POINT}" ]]; then
        echo "ERROR: Mount point ${MOUNT_POINT} does not exist" >&2
        exit 1
    fi
    info "Using existing mount at: ${MOUNT_POINT}"
fi

# Give a moment for the MetadataRefreshWorker to do its initial sync
sleep 2

# ==============================================================================
# TEST SUITE
# ==============================================================================

echo ""
echo "================================================================"
echo "  Via FUSE Rigour Test Suite"
echo "  Mount: ${MOUNT_POINT}"
echo "  Iterations: ${ITERATIONS}"
echo "================================================================"
echo ""

# ── Test 1: Baseline readdir ─────────────────────────────────────────────────

info "Test 1: Baseline readdir — capture reference listing"

BASELINE_FILE="${RESULTS_DIR}/baseline.txt"
ls -1 "${MOUNT_POINT}" | sort > "${BASELINE_FILE}" 2>/dev/null
BASELINE_COUNT=$(wc -l < "${BASELINE_FILE}")

if [[ ${BASELINE_COUNT} -eq 0 ]]; then
    warn "Root directory is empty — tests will be limited"
else
    pass "Baseline captured: ${BASELINE_COUNT} entries"
fi

if [[ "${VERBOSE}" == true ]]; then
    echo "  Baseline entries:"
    sed 's/^/    /' "${BASELINE_FILE}"
fi

# ── Test 2: Repeated readdir — check for duplicate entries ───────────────────

info "Test 2: Repeated readdir (${ITERATIONS}x) — check for duplicates"

DUPLICATES_FOUND=false
for i in $(seq 1 "${ITERATIONS}"); do
    ITER_FILE="${RESULTS_DIR}/readdir_iter_${i}.txt"
    ls -1 "${MOUNT_POINT}" > "${ITER_FILE}" 2>/dev/null

    # Check for duplicate lines (same filename appearing twice)
    DUPS=$(sort "${ITER_FILE}" | uniq -d)
    if [[ -n "${DUPS}" ]]; then
        fail "Iteration ${i}: Duplicate entries detected!"
        echo "  Duplicates:"
        echo "${DUPS}" | sed 's/^/    /'
        DUPLICATES_FOUND=true
    fi

    # Check entry count stability
    ITER_COUNT=$(wc -l < "${ITER_FILE}")
    if [[ ${ITER_COUNT} -ne ${BASELINE_COUNT} ]]; then
        warn "Iteration ${i}: Entry count changed (baseline=${BASELINE_COUNT}, now=${ITER_COUNT})"
        diff "${BASELINE_FILE}" <(sort "${ITER_FILE}") || true
    fi
done

if [[ "${DUPLICATES_FOUND}" == false ]]; then
    pass "No duplicate entries across ${ITERATIONS} readdir iterations"
fi

# ── Test 3: stat every entry — check for ENOENT on listed files ──────────────

info "Test 3: stat() every entry from baseline listing"

STAT_ERRORS=0
while IFS= read -r entry; do
    if ! stat "${MOUNT_POINT}/${entry}" >/dev/null 2>&1; then
        fail "stat failed for: ${entry}"
        STAT_ERRORS=$((STAT_ERRORS + 1))
    fi
done < "${BASELINE_FILE}"

if [[ ${STAT_ERRORS} -eq 0 ]]; then
    pass "All ${BASELINE_COUNT} entries stat'd successfully"
fi

# ── Test 4: Thumbnail-style access (stat + partial read) ─────────────────────

info "Test 4: Thumbnail-style access — stat + head on regular files"

THUMB_ERRORS=0
THUMB_TESTED=0
while IFS= read -r entry; do
    FPATH="${MOUNT_POINT}/${entry}"
    if [[ -f "${FPATH}" ]]; then
        THUMB_TESTED=$((THUMB_TESTED + 1))
        # Mimics what Dolphin/KIO does: stat then read first 64KB for thumbnail
        if ! head -c 65536 "${FPATH}" > /dev/null 2>&1; then
            warn "Thumbnail read failed for: ${entry}"
            THUMB_ERRORS=$((THUMB_ERRORS + 1))
        fi
    fi
done < "${BASELINE_FILE}"

if [[ ${THUMB_TESTED} -eq 0 ]]; then
    warn "No regular files in root to test thumbnail access"
elif [[ ${THUMB_ERRORS} -eq 0 ]]; then
    pass "All ${THUMB_TESTED} regular files read successfully"
else
    warn "${THUMB_ERRORS}/${THUMB_TESTED} thumbnail reads failed"
fi

# ── Test 5: Navigate in/out of subdirectories then recheck root ──────────────

info "Test 5: Directory navigation — enter subfolders then recheck root"

# Find subdirectories
SUBDIRS=()
while IFS= read -r entry; do
    if [[ -d "${MOUNT_POINT}/${entry}" ]]; then
        SUBDIRS+=("${entry}")
    fi
done < "${BASELINE_FILE}"

NAV_ISSUES=0
if [[ ${#SUBDIRS[@]} -eq 0 ]]; then
    warn "No subdirectories in root to test navigation"
else
    for subdir in "${SUBDIRS[@]}"; do
        # Enter subdirectory (triggers readdir + getattr)
        ls "${MOUNT_POINT}/${subdir}" > /dev/null 2>&1 || true

        # Return to root and check for duplicates
        AFTER_NAV="${RESULTS_DIR}/after_nav_${subdir//\//_}.txt"
        ls -1 "${MOUNT_POINT}" > "${AFTER_NAV}" 2>/dev/null

        DUPS=$(sort "${AFTER_NAV}" | uniq -d)
        if [[ -n "${DUPS}" ]]; then
            fail "Duplicates after navigating into '${subdir}':"
            echo "${DUPS}" | sed 's/^/    /'
            NAV_ISSUES=$((NAV_ISSUES + 1))
        fi

        NAV_COUNT=$(wc -l < "${AFTER_NAV}")
        if [[ ${NAV_COUNT} -ne ${BASELINE_COUNT} ]]; then
            warn "Entry count changed after visiting '${subdir}' (baseline=${BASELINE_COUNT}, now=${NAV_COUNT})"
            NAV_ISSUES=$((NAV_ISSUES + 1))
        fi
    done

    if [[ ${NAV_ISSUES} -eq 0 ]]; then
        pass "Root listing stable after navigating ${#SUBDIRS[@]} subdirectories"
    fi
fi

# ── Test 6: Rapid repeated readdir (simulates Dolphin rapid back/forward) ────

info "Test 6: Rapid-fire readdir (burst of ${ITERATIONS} ls calls)"

# Fire ls commands as fast as possible, collect outputs
for i in $(seq 1 "${ITERATIONS}"); do
    ls -1 "${MOUNT_POINT}" > "${RESULTS_DIR}/rapid_${i}.txt" 2>/dev/null &
done
wait

RAPID_ISSUES=0
for i in $(seq 1 "${ITERATIONS}"); do
    RAPID_FILE="${RESULTS_DIR}/rapid_${i}.txt"
    DUPS=$(sort "${RAPID_FILE}" | uniq -d)
    if [[ -n "${DUPS}" ]]; then
        fail "Rapid readdir ${i}: Duplicate entries!"
        echo "${DUPS}" | sed 's/^/    /'
        RAPID_ISSUES=$((RAPID_ISSUES + 1))
    fi
done

if [[ ${RAPID_ISSUES} -eq 0 ]]; then
    pass "No duplicates in rapid-fire readdir burst"
fi

# ── Test 7: readdir + concurrent file access (simulates thumbnail gen) ───────

info "Test 7: readdir during concurrent file reads"

CONCURRENT_ISSUES=0
for i in $(seq 1 5); do
    # Start background file reads to simulate thumbnail generation
    while IFS= read -r entry; do
        FPATH="${MOUNT_POINT}/${entry}"
        if [[ -f "${FPATH}" ]]; then
            head -c 4096 "${FPATH}" > /dev/null 2>&1 &
        fi
    done < "${BASELINE_FILE}"

    # While reads are in flight, do a readdir
    CONCURRENT_FILE="${RESULTS_DIR}/concurrent_${i}.txt"
    ls -1 "${MOUNT_POINT}" > "${CONCURRENT_FILE}" 2>/dev/null

    wait  # Wait for all background reads

    DUPS=$(sort "${CONCURRENT_FILE}" | uniq -d)
    if [[ -n "${DUPS}" ]]; then
        fail "Concurrent access ${i}: Duplicate entries!"
        echo "${DUPS}" | sed 's/^/    /'
        CONCURRENT_ISSUES=$((CONCURRENT_ISSUES + 1))
    fi

    CONC_COUNT=$(wc -l < "${CONCURRENT_FILE}")
    if [[ ${CONC_COUNT} -ne ${BASELINE_COUNT} ]]; then
        warn "Entry count changed during concurrent access ${i} (baseline=${BASELINE_COUNT}, now=${CONC_COUNT})"
    fi
done

if [[ ${CONCURRENT_ISSUES} -eq 0 ]]; then
    pass "No duplicates during concurrent file reads"
fi

# ── Test 8: Check for inode stability across readdir calls ───────────────────

info "Test 8: Inode stability across readdir calls"

INODE_FILE1="${RESULTS_DIR}/inodes_1.txt"
INODE_FILE2="${RESULTS_DIR}/inodes_2.txt"

ls -1i "${MOUNT_POINT}" | sort -k2 > "${INODE_FILE1}" 2>/dev/null
sleep 1
ls -1i "${MOUNT_POINT}" | sort -k2 > "${INODE_FILE2}" 2>/dev/null

INODE_DIFF=$(diff "${INODE_FILE1}" "${INODE_FILE2}" || true)
if [[ -z "${INODE_DIFF}" ]]; then
    pass "Inode numbers stable across readdir calls"
else
    warn "Inode numbers changed between readdir calls (may cause Dolphin confusion)"
    if [[ "${VERBOSE}" == true ]]; then
        echo "${INODE_DIFF}" | head -20
    fi
fi

# ── Test 9: Long-duration readdir stability (accumulation check) ─────────────

info "Test 9: Long-duration stability — readdir every 5s for ~60s"

LONG_ISSUES=0
LONG_ITERATIONS=12
FIRST_LISTING="${RESULTS_DIR}/long_1.txt"
ls -1 "${MOUNT_POINT}" | sort > "${FIRST_LISTING}" 2>/dev/null

for i in $(seq 2 "${LONG_ITERATIONS}"); do
    sleep 5
    CURRENT="${RESULTS_DIR}/long_${i}.txt"
    ls -1 "${MOUNT_POINT}" | sort > "${CURRENT}" 2>/dev/null

    # Check for duplicates within single listing
    DUPS=$(sort "${CURRENT}" | uniq -d)
    if [[ -n "${DUPS}" ]]; then
        fail "Long-duration iteration ${i}: Duplicate entries!"
        echo "${DUPS}" | sed 's/^/    /'
        LONG_ISSUES=$((LONG_ISSUES + 1))
    fi

    # Check consistency with first listing
    LISTING_DIFF=$(diff "${FIRST_LISTING}" "${CURRENT}" || true)
    if [[ -n "${LISTING_DIFF}" && "${VERBOSE}" == true ]]; then
        echo "  Listing drift at iteration ${i}:"
        echo "${LISTING_DIFF}" | head -10 | sed 's/^/    /'
    fi
done

if [[ ${LONG_ISSUES} -eq 0 ]]; then
    pass "No duplicates over ~60s monitoring period"
fi

# ── Test 10: getattr for non-existent file (negative lookup) ─────────────────

info "Test 10: Negative lookup — stat non-existent file"

if stat "${MOUNT_POINT}/__nonexistent_file_rigour_test__" >/dev/null 2>&1; then
    fail "stat succeeded for non-existent file (should return ENOENT)"
else
    pass "Non-existent file correctly returns ENOENT"
fi

# ── Test 11: Write and read-back verification ────────────────────────────────

info "Test 11: Write + read-back consistency"

WRITE_TEST_DIR="${MOUNT_POINT}/.rigour-test-$$"
WRITE_OK=true

if mkdir -p "${WRITE_TEST_DIR}" 2>/dev/null; then
    # Write a known payload and read it back
    PAYLOAD="Via rigour test $(date +%s) $$"
    WRITE_FILE="${WRITE_TEST_DIR}/test-write.txt"

    if echo "${PAYLOAD}" > "${WRITE_FILE}" 2>/dev/null; then
        READBACK=$(cat "${WRITE_FILE}" 2>/dev/null) || READBACK=""
        if [[ "${READBACK}" == "${PAYLOAD}" ]]; then
            pass "Write and read-back match"
        else
            fail "Read-back mismatch: wrote '${PAYLOAD}', got '${READBACK}'"
            WRITE_OK=false
        fi
        rm -f "${WRITE_FILE}" 2>/dev/null || true
    else
        warn "Write failed (may be read-only mount or API error)"
    fi

    rmdir "${WRITE_TEST_DIR}" 2>/dev/null || true
else
    warn "Cannot create test directory (mount may be read-only)"
fi

# ==============================================================================
# PJDFSTEST POSIX COMPLIANCE SUITE
# ==============================================================================

if [[ "${RUN_PJDFSTEST}" == true ]]; then
    echo ""
    echo "================================================================"
    echo "  pjdfstest POSIX Compliance Suite"
    echo "================================================================"
    echo ""

    # ── Build pjdfstest if needed ─────────────────────────────────────

    PJDFSTEST_BIN=""

    if [[ -n "${PJDFSTEST_DIR}" && -f "${PJDFSTEST_DIR}/pjdfstest" ]]; then
        info "Using existing pjdfstest at ${PJDFSTEST_DIR}"
        PJDFSTEST_BIN="${PJDFSTEST_DIR}/pjdfstest"
    else
        # Check build tools
        PJDFSTEST_MISSING_TOOLS=false
        for cmd in gcc make cmake perl; do
            if ! command -v "${cmd}" &>/dev/null; then
                warn "pjdfstest requires '${cmd}' but it is not installed"
                PJDFSTEST_MISSING_TOOLS=true
            fi
        done

        if [[ "${PJDFSTEST_MISSING_TOOLS}" == true ]]; then
            fail "Cannot build pjdfstest — missing build tools (gcc, make, cmake, perl)"
        else
            info "Cloning and building pjdfstest..."
            if [[ -d "${PJDFSTEST_CLONE}" ]]; then
                rm -rf "${PJDFSTEST_CLONE}"
            fi

            if git clone --depth 1 https://github.com/pjd/pjdfstest.git "${PJDFSTEST_CLONE}" 2>/dev/null; then
                cmake -B "${PJDFSTEST_CLONE}/build" -S "${PJDFSTEST_CLONE}" >/dev/null 2>&1
                if cmake --build "${PJDFSTEST_CLONE}/build" --parallel >/dev/null 2>&1; then
                    PJDFSTEST_DIR="${PJDFSTEST_CLONE}/build"
                    PJDFSTEST_BIN="${PJDFSTEST_DIR}/pjdfstest"
                    pass "pjdfstest built successfully"
                else
                    fail "pjdfstest build failed"
                fi
            else
                fail "Failed to clone pjdfstest repository"
            fi
        fi
    fi

    if [[ -n "${PJDFSTEST_BIN}" && -x "${PJDFSTEST_BIN}" ]]; then
        # ── Create test working directory inside the mount ────────────

        PJD_TEST_DIR="${MOUNT_POINT}/.pjdfstest-workdir-$$"
        mkdir -p "${PJD_TEST_DIR}" 2>/dev/null || {
            fail "Failed to create pjdfstest working directory inside FUSE mount"
            PJDFSTEST_BIN=""  # skip the rest
        }
    fi

    if [[ -n "${PJDFSTEST_BIN}" && -x "${PJDFSTEST_BIN}" ]]; then
        info "Running pjdfstest in ${PJD_TEST_DIR}"
        info "Skipping test groups: ${PJDFSTEST_SKIP}"

        # Find test scripts directory
        PJDFSTEST_TESTS_DIR="${PJDFSTEST_DIR}/../tests"
        if [[ ! -d "${PJDFSTEST_TESTS_DIR}" ]]; then
            PJDFSTEST_TESTS_DIR="${PJDFSTEST_CLONE}/tests"
        fi

        if [[ ! -d "${PJDFSTEST_TESTS_DIR}" ]]; then
            fail "Cannot find pjdfstest test scripts directory"
        else
            # Convert skip list to array
            IFS=',' read -ra PJD_SKIP_ARRAY <<< "${PJDFSTEST_SKIP}"

            # Discover test groups
            PJD_TEST_GROUPS=()
            for dir in "${PJDFSTEST_TESTS_DIR}"/*/; do
                [[ -d "${dir}" ]] || continue
                group_name="$(basename "${dir}")"
                skip=false
                for skip_item in "${PJD_SKIP_ARRAY[@]}"; do
                    if [[ "${group_name}" == "${skip_item}" ]]; then
                        skip=true
                        break
                    fi
                done
                if [[ "${skip}" == false ]]; then
                    PJD_TEST_GROUPS+=("${group_name}")
                else
                    info "  Skipping group: ${group_name}"
                fi
            done

            info "Test groups: ${PJD_TEST_GROUPS[*]}"

            PJD_RESULTS_LOG="${RESULTS_DIR}/pjdfstest-results.log"
            : > "${PJD_RESULTS_LOG}"
            PJD_OVERALL_EXIT=0

            pushd "${PJD_TEST_DIR}" > /dev/null

            for group in "${PJD_TEST_GROUPS[@]}"; do
                group_dir="${PJDFSTEST_TESTS_DIR}/${group}"
                [[ -d "${group_dir}" ]] || continue

                # Find .t test files
                shopt -s nullglob
                test_files=("${group_dir}"/*.t)
                shopt -u nullglob
                if [[ ${#test_files[@]} -eq 0 ]]; then
                    continue
                fi

                info "  Running group: ${group} (${#test_files[@]} tests)"

                if command -v prove &>/dev/null; then
                    prove -f "${group_dir}"/*.t \
                        :: "${PJDFSTEST_BIN}" \
                        2>&1 | tee -a "${PJD_RESULTS_LOG}" || PJD_OVERALL_EXIT=1
                else
                    for t_file in "${test_files[@]}"; do
                        perl "${t_file}" "${PJDFSTEST_BIN}" \
                            2>&1 | tee -a "${PJD_RESULTS_LOG}" || PJD_OVERALL_EXIT=1
                    done
                fi
            done

            popd > /dev/null

            # Parse TAP results
            PJD_PASS=$(grep -c "^ok " "${PJD_RESULTS_LOG}" 2>/dev/null || echo 0)
            PJD_FAIL=$(grep -c "^not ok " "${PJD_RESULTS_LOG}" 2>/dev/null || echo 0)

            echo ""
            echo "  pjdfstest: ${PJD_PASS} passed, ${PJD_FAIL} failed"
            echo "  Results log: ${PJD_RESULTS_LOG}"

            if [[ ${PJD_OVERALL_EXIT} -eq 0 ]]; then
                pass "pjdfstest POSIX suite passed"
            else
                # Count pjdfstest failures as warnings, not hard fails,
                # since many POSIX edge cases don't apply to cloud FUSE.
                warn "pjdfstest had ${PJD_FAIL} failures (see log for details)"
            fi
        fi

        # Clean up test directory
        rm -rf "${PJD_TEST_DIR}" 2>/dev/null || true
    fi
else
    if [[ "${VERBOSE}" == true ]]; then
        info "pjdfstest skipped (use --pjdfstest to enable)"
    fi
fi

# ==============================================================================
# SUMMARY
# ==============================================================================

echo ""
echo "================================================================"
echo "  Results Summary"
echo "================================================================"
echo ""
echo -e "  \033[1;32mPassed:\033[0m  ${PASS_COUNT}"
echo -e "  \033[1;31mFailed:\033[0m  ${FAIL_COUNT}"
echo -e "  \033[1;33mWarnings:\033[0m ${WARN_COUNT}"
echo ""
echo "  Detailed output saved to: ${RESULTS_DIR}/"
echo ""

if [[ ${FAIL_COUNT} -gt 0 ]]; then
    echo -e "\033[1;31mSome tests FAILED. See output above for details.\033[0m"
    exit 1
else
    echo -e "\033[1;32mAll tests passed.\033[0m"
    exit 0
fi
