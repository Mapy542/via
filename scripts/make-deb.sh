#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_DIR}/build-deb"
STAGE_ROOT="${BUILD_ROOT}/source"
ARTIFACTS_DIR="${BUILD_ROOT}/artifacts"
VERSION_FILE="${PROJECT_DIR}/VERSION"
CHANGELOG_TEMPLATE="${PROJECT_DIR}/debian/changelog.in"
RELEASE_NOTES_GENERATOR="${SCRIPT_DIR}/generate-release-notes.sh"
DEBIAN_REVISION="1"

info()  { echo -e "\033[1;34m==>\033[0m $*"; }
error() { echo -e "\033[1;31mERROR:\033[0m $*" >&2; }

check_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error "Required command '$1' not found. Please install it."
        exit 1
    fi
}

read_version() {
    if [[ ! -f "${VERSION_FILE}" ]]; then
        error "Missing VERSION file at ${VERSION_FILE}"
        exit 1
    fi

    local version
    version="$(tr -d '[:space:]' < "${VERSION_FILE}")"
    if [[ -z "${version}" ]]; then
        error "VERSION is empty"
        exit 1
    fi

    if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
        error "Unsupported VERSION '${version}'. Expected 3 or 4 numeric components."
        exit 1
    fi

    printf '%s\n' "${version}"
}

check_maintainer_placeholders() {
    local file
    local matches=()

    for file in \
        "${PROJECT_DIR}/debian/control" \
        "${PROJECT_DIR}/debian/copyright" \
        "${PROJECT_DIR}/debian/changelog.in"; do
        if grep -n "MAINTAINER" "${file}" >/dev/null 2>&1; then
            matches+=("${file}")
        fi
    done

    if (( ${#matches[@]} > 0 )); then
        error "Replace the MAINTAINER placeholders in the Debian metadata before building a package."
        printf 'Placeholder files:\n' >&2
        printf '  %s\n' "${matches[@]}" >&2
        exit 1
    fi
}

stage_source_tree() {
    local stage_dir="$1"

    rm -rf "${stage_dir}"
    mkdir -p "${stage_dir}"

    info "Staging source tree..."
    (
        cd "${PROJECT_DIR}"
        tar \
            --exclude='./.git' \
            --exclude='./build' \
            --exclude='./build-appimage' \
            --exclude='./build-deb' \
            --exclude='./AppDir' \
            --exclude='./*.AppImage' \
            --exclude='./*.deb' \
            --exclude='./*.changes' \
            --exclude='./*.buildinfo' \
            --exclude='./*.dsc' \
            --exclude='./*.tar.*' \
            -cf - .
    ) | tar -xf - -C "${stage_dir}"
}

generate_changelog() {
    local app_version="$1"
    local deb_version="$2"
    local stage_dir="$3"
    local changelog_date
    local changelog_body
    local line

    changelog_date="$(LC_ALL=C date -R)"
    changelog_body="$(bash "${RELEASE_NOTES_GENERATOR}" debian --version "${app_version}")"

    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line//@DEB_VERSION@/${deb_version}}"
        line="${line//@CHANGELOG_DATE@/${changelog_date}}"

        if [[ "${line}" == *"@CHANGELOG_BODY@"* ]]; then
            printf '%s\n' "${line//@CHANGELOG_BODY@/${changelog_body}}"
        else
            printf '%s\n' "${line}"
        fi
    done < "${CHANGELOG_TEMPLATE}" > "${stage_dir}/debian/changelog"
}

collect_artifacts() {
    local package_version="$1"
    local deb_arch="$2"
    local package_dir="$3"
    local changes_path="${package_dir}/via_${package_version}_${deb_arch}.changes"
    local buildinfo_path="${package_dir}/via_${package_version}_${deb_arch}.buildinfo"
    local artifact_name
    local -a referenced_artifacts=()

    rm -rf "${ARTIFACTS_DIR}"
    mkdir -p "${ARTIFACTS_DIR}"

    cp "${changes_path}" "${ARTIFACTS_DIR}/"
    cp "${buildinfo_path}" "${ARTIFACTS_DIR}/"

    mapfile -t referenced_artifacts < <(
        awk '
            /^Files:/ { in_files = 1; next }
            in_files && /^[^[:space:]]/ { exit }
            in_files && NF >= 5 { print $NF }
        ' "${changes_path}"
    )

    if (( ${#referenced_artifacts[@]} == 0 )); then
        error "No package artifacts listed in ${changes_path}"
        exit 1
    fi

    for artifact_name in "${referenced_artifacts[@]}"; do
        if [[ ! -f "${package_dir}/${artifact_name}" ]]; then
            error "${changes_path} references missing artifact ${artifact_name}"
            exit 1
        fi

        if [[ "${artifact_name}" == "$(basename "${buildinfo_path}")" ]]; then
            continue
        fi

        cp "${package_dir}/${artifact_name}" "${ARTIFACTS_DIR}/"
    done
}

APP_VERSION="$(read_version)"
DEB_VERSION="${APP_VERSION}-${DEBIAN_REVISION}"
STAGE_DIR="${STAGE_ROOT}/via-${DEB_VERSION}"

check_maintainer_placeholders

check_command tar
check_command desktop-file-validate
check_command dpkg-architecture
check_command dpkg-buildpackage
check_command dpkg-deb
check_command lintian

DEB_ARCH="$(dpkg-architecture -qDEB_HOST_ARCH)"

mkdir -p "${STAGE_ROOT}"
stage_source_tree "${STAGE_DIR}"
generate_changelog "${APP_VERSION}" "${DEB_VERSION}" "${STAGE_DIR}"

info "Validating desktop entry..."
desktop-file-validate "${STAGE_DIR}/res/via.desktop"

info "Building Debian package ${DEB_VERSION} for ${DEB_ARCH}..."
(
    cd "${STAGE_DIR}"
    dpkg-buildpackage -us -uc -b
)

collect_artifacts "${DEB_VERSION}" "${DEB_ARCH}" "${STAGE_ROOT}"

DEB_PATH="${ARTIFACTS_DIR}/via_${DEB_VERSION}_${DEB_ARCH}.deb"
CHANGES_PATH="${ARTIFACTS_DIR}/via_${DEB_VERSION}_${DEB_ARCH}.changes"

info "Running lintian..."
lintian --fail-on error "${CHANGES_PATH}"

info "Inspecting package metadata..."
dpkg-deb -I "${DEB_PATH}"

info "Inspecting package contents..."
dpkg-deb -c "${DEB_PATH}"

info "Done! Debian package artifacts created in ${ARTIFACTS_DIR}"