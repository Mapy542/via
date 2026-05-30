#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_binary="${1:-$repo_root/build/test_SyncDatabase}"

if [[ ! -x "$test_binary" ]]; then
    echo "SyncDatabase test binary not found or not executable: $test_binary" >&2
    exit 1
fi

readonly -a excluded_tests=(
    testInitialize_RejectsNewerVersion
    testInitialize_RequiresResetForLegacySchema
    testInitialize_RequiresExplicitDiscardForDirtyLegacySchema
    testFuseOperations_DatabaseClosed_Graceful
    testClearFuseRepresentationState_ReturnsFalseOnClosedDb
)

mapfile -t all_tests < <("$test_binary" -functions 2>/dev/null | sed 's/()$//' | grep '^test')

selected_tests=()
for test_name in "${all_tests[@]}"; do
    skip_test=false
    for excluded_name in "${excluded_tests[@]}"; do
        if [[ "$test_name" == "$excluded_name" ]]; then
            skip_test=true
            break
        fi
    done

    if [[ "$skip_test" == false ]]; then
        selected_tests+=("$test_name")
    fi
done

if [[ ${#selected_tests[@]} -eq 0 ]]; then
    echo "No SyncDatabase tests selected for fatal-warning validation." >&2
    exit 1
fi

cd "$repo_root"
env QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" QT_FATAL_WARNINGS=1 \
    "$test_binary" -txt "${selected_tests[@]}"