#!/usr/bin/env bash
# ==============================================================================
# make-appimage.sh — Build a Via AppImage locally
#
# Downloads linuxdeploy + Qt plugin to build/tools/ if not already present,
# then builds a Release binary and packages it into an AppImage.
#
# Prerequisites:
#   - All build dependencies installed (Qt6, FUSE3, etc.)
#   - A Qt installation whose plugin tree includes QtSvg and XCB support
#   - A complete runtime dependency closure for the Qt XCB platform plugin
#   - A working C++ compiler and CMake
#
# Output:
#   Via-<version>-<arch>.AppImage in the project root
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-appimage"
TOOLS_DIR="${BUILD_DIR}/tools"
APPDIR="${BUILD_DIR}/AppDir"

LINUXDEPLOY="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT="${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

# --- Helpers ------------------------------------------------------------------

info()  { echo -e "\033[1;34m==>\033[0m $*"; }
error() { echo -e "\033[1;31mERROR:\033[0m $*" >&2; }

check_command() {
    if ! command -v "$1" &>/dev/null; then
        error "Required command '$1' not found. Please install it."
        exit 1
    fi
}

download_tool() {
    local url="$1"
    local dest="$2"

    if [[ -f "${dest}" ]]; then
        info "$(basename "${dest}") already present, skipping download."
        return 0
    fi

    info "Downloading $(basename "${dest}")..."
    mkdir -p "$(dirname "${dest}")"
    if ! wget -q --show-progress -O "${dest}" "${url}"; then
        error "Failed to download $(basename "${dest}") from ${url}"
        rm -f "${dest}"
        exit 1
    fi
    chmod +x "${dest}"
}

read_resolved_version() {
    local build_dir="$1"
    local version_file="${build_dir}/via-version.txt"

    if [[ ! -f "${version_file}" ]]; then
        error "Resolved version file not found at ${version_file}"
        exit 1
    fi

    tr -d '[:space:]' < "${version_file}"
}

resolve_qmake() {
    if [[ -n "${QMAKE:-}" && -x "${QMAKE}" ]]; then
        printf '%s\n' "${QMAKE}"
        return 0
    fi

    for candidate in /usr/lib/qt6/bin/qmake /usr/lib/x86_64-linux-gnu/qt6/bin/qmake; do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    command -v qmake6 2>/dev/null || command -v qmake 2>/dev/null || true
}

print_plugin_dir_contents() {
    local label="$1"
    local dir_path="$2"

    info "Qt plugin directory (${label}): ${dir_path}"
    if [[ ! -d "${dir_path}" ]]; then
        echo "  (missing)"
        return
    fi

    find "${dir_path}" -maxdepth 1 -type f -printf '  %f\n' | sort
}

print_qt_prereq_hint() {
    cat >&2 <<'EOF'
This AppImage packaging flow requires a Qt installation with QtSvg plugins and the XCB platform plugin.
On Ubuntu/Debian, the missing pieces are commonly provided by packages such as qt6-svg-dev and the libxcb/libxkbcommon-x11 runtime packages.
If you are using a custom Qt install, make sure qmake points at the same Qt tree that contains those plugins.
EOF
}

print_xcb_dependency_hint() {
    cat >&2 <<'EOF'
The Qt XCB platform plugin is present, but one or more of its host runtime dependencies are missing.
Install the matching runtime libraries for your distro before packaging. On Ubuntu/Debian, this is commonly a mix of libxkbcommon-x11-0, libxcb-cursor0, and related libxcb-* packages.
EOF
}

preflight_qt_packaging() {
    local plugin_root
    local required_plugins
    local -a missing_plugins=()
    local -a missing_xcb_deps=()
    local ldd_output

    QMAKE="$(resolve_qmake)"
    if [[ -z "${QMAKE}" ]]; then
        error "Cannot find qmake. Make sure your Qt installation is available before packaging."
        print_qt_prereq_hint
        exit 1
    fi

    plugin_root="$("${QMAKE}" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    if [[ -z "${plugin_root}" || ! -d "${plugin_root}" ]]; then
        error "qmake did not resolve a usable Qt plugin root."
        printf 'Resolved QMAKE: %s\n' "${QMAKE}" >&2
        print_qt_prereq_hint
        exit 1
    fi

    info "Resolved QMAKE: ${QMAKE}"
    info "Qt plugin root: ${plugin_root}"
    print_plugin_dir_contents "iconengines" "${plugin_root}/iconengines"
    print_plugin_dir_contents "imageformats" "${plugin_root}/imageformats"
    print_plugin_dir_contents "platforms" "${plugin_root}/platforms"

    required_plugins=$'iconengines/libqsvgicon.so\nimageformats/libqsvg.so\nplatforms/libqxcb.so'
    while IFS= read -r relative_path; do
        [[ -z "${relative_path}" ]] && continue
        if [[ ! -f "${plugin_root}/${relative_path}" ]]; then
            missing_plugins+=("${plugin_root}/${relative_path}")
        fi
    done <<< "${required_plugins}"

    if (( ${#missing_plugins[@]} > 0 )); then
        error "Missing required Qt plugins for AppImage packaging:"
        printf '  - %s\n' "${missing_plugins[@]}" >&2
        print_qt_prereq_hint
        exit 1
    fi

    ldd_output="$(ldd "${plugin_root}/platforms/libqxcb.so" || true)"
    while IFS= read -r line; do
        if [[ "${line}" == *"not found"* ]]; then
            missing_xcb_deps+=("$(awk '{print $1}' <<< "${line}")")
        fi
    done <<< "${ldd_output}"

    if (( ${#missing_xcb_deps[@]} > 0 )); then
        error "Qt platform plugin libqxcb.so has unresolved runtime dependencies:"
        printf '  - %s\n' "${missing_xcb_deps[@]}" >&2
        print_xcb_dependency_hint
        exit 1
    fi

    info "Qt packaging preflight passed."
}

# --- Preflight checks --------------------------------------------------------

check_command cmake
check_command wget
check_command ldd

preflight_qt_packaging

# --- Download linuxdeploy tools -----------------------------------------------

info "Ensuring linuxdeploy tools are available..."
download_tool "${LINUXDEPLOY_URL}" "${LINUXDEPLOY}"
download_tool "${LINUXDEPLOY_QT_URL}" "${LINUXDEPLOY_QT}"

# --- Build --------------------------------------------------------------------

info "Configuring Release build..."
cmake_args=(
    -B "${BUILD_DIR}"
    -S "${PROJECT_DIR}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/usr
    -DBUILD_TESTS=OFF
)

if [[ -n "${VIA_VERSION_OVERRIDE:-}" ]]; then
    cmake_args+=("-DVIA_VERSION_OVERRIDE=${VIA_VERSION_OVERRIDE}")
fi

cmake "${cmake_args[@]}"

APP_VERSION="$(read_resolved_version "${BUILD_DIR}")"
APPIMAGE_ARCH="$(uname -m)"
TARGET_APPIMAGE="Via-${APP_VERSION}-${APPIMAGE_ARCH}.AppImage"

info "Building..."
cmake --build "${BUILD_DIR}" --parallel

# --- Install to AppDir -------------------------------------------------------

info "Installing to AppDir..."
rm -rf "${APPDIR}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}"

# Ensure icon and desktop file are in place (belt-and-suspenders)
mkdir -p "${APPDIR}/usr/share/icons/hicolor/scalable/apps"
cp "${PROJECT_DIR}/res/icons/via.svg" \
   "${APPDIR}/usr/share/icons/hicolor/scalable/apps/via.svg"

mkdir -p "${APPDIR}/usr/share/applications"
cp "${PROJECT_DIR}/res/via.desktop" \
   "${APPDIR}/usr/share/applications/via.desktop"

# --- Create AppImage ----------------------------------------------------------

info "Creating AppImage..."

export QMAKE
export EXTRA_QT_PLUGINS="svg;"

# linuxdeploy tools are themselves AppImages that need FUSE to mount.
# Setting this env var tells them to extract-and-run instead, which
# works everywhere (containers, systems without fuse, etc.).
export APPIMAGE_EXTRACT_AND_RUN=1

cd "${PROJECT_DIR}"
rm -f "${TARGET_APPIMAGE}"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --output appimage

# Rename to the resolved versioned name
GENERATED="$(find . -maxdepth 1 -type f -name 'Via*.AppImage' -printf '%T@ %f\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
if [[ -z "${GENERATED}" ]]; then
    error "linuxdeploy did not produce an AppImage"
    exit 1
fi

if [[ "${GENERATED}" != "${TARGET_APPIMAGE}" ]]; then
    mv "${GENERATED}" "${TARGET_APPIMAGE}"
fi

info "Done! AppImage created: ${PROJECT_DIR}/${TARGET_APPIMAGE}"
