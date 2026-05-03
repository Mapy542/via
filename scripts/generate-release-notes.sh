#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE=""
CURRENT_VERSION=""
CURRENT_TAG=""

usage() {
    cat <<'EOF'
Usage: generate-release-notes.sh <debian|github> [--version <x.y.z>] [--tag <vX.Y.Z>]

Outputs commit-based release notes derived from git history.
EOF
}

in_git_repo() {
    git -C "${PROJECT_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1
}

read_version() {
    tr -d '[:space:]' < "${PROJECT_DIR}/VERSION"
}

resolve_previous_tag() {
    local tag

    if ! in_git_repo; then
        return 0
    fi

    while IFS= read -r tag; do
        if [[ ! "${tag}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
            continue
        fi

        if [[ "${tag}" == "${CURRENT_TAG}" ]]; then
            continue
        fi

        printf '%s\n' "${tag}"
        return 0
    done < <(git -C "${PROJECT_DIR}" tag --sort=-version:refname)
}

collect_subjects() {
    local previous_tag="$1"
    local range=()

    if ! in_git_repo; then
        return 0
    fi

    if [[ -n "${previous_tag}" ]]; then
        range=("${previous_tag}..HEAD")
    fi

    git -C "${PROJECT_DIR}" log --no-merges --format=%s "${range[@]}" | sed '/^[[:space:]]*$/d'
}

emit_debian_notes() {
    local previous_tag="$1"
    local subject
    local found=0

    while IFS= read -r subject; do
        found=1
        printf '  * %s\n' "${subject}"
    done < <(collect_subjects "${previous_tag}")

    if [[ "${found}" -eq 0 ]]; then
        printf '  * Build release package.\n'
    fi
}

emit_github_notes() {
    local previous_tag="$1"
    local subject
    local found=0

    if [[ -n "${previous_tag}" ]]; then
        printf '## Changes since %s\n' "${previous_tag}"
    else
        printf '## Changes\n'
    fi

    while IFS= read -r subject; do
        found=1
        printf -- '- %s\n' "${subject}"
    done < <(collect_subjects "${previous_tag}")

    if [[ "${found}" -eq 0 ]]; then
        printf -- '- Release %s\n' "${CURRENT_VERSION}"
    fi

    printf '\n## Artifacts\n'
    printf -- '- AppImage: `Via-%s-x86_64.AppImage`, `Via-%s-aarch64.AppImage`\n' \
        "${CURRENT_VERSION}" "${CURRENT_VERSION}"
    printf -- '- Debian: `via_%s-1_amd64.deb`, `via_%s-1_arm64.deb`\n' \
        "${CURRENT_VERSION}" "${CURRENT_VERSION}"
}

while (($# > 0)); do
    case "$1" in
        debian|github)
            MODE="$1"
            shift
            ;;
        --version)
            CURRENT_VERSION="$2"
            shift 2
            ;;
        --tag)
            CURRENT_TAG="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${MODE}" ]]; then
    usage >&2
    exit 1
fi

if [[ -z "${CURRENT_VERSION}" ]]; then
    CURRENT_VERSION="$(read_version)"
fi

if [[ -z "${CURRENT_TAG}" ]]; then
    CURRENT_TAG="v${CURRENT_VERSION}"
fi

PREVIOUS_TAG="$(resolve_previous_tag)"

case "${MODE}" in
    debian)
        emit_debian_notes "${PREVIOUS_TAG}"
        ;;
    github)
        emit_github_notes "${PREVIOUS_TAG}"
        ;;
esac