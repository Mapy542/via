#!/usr/bin/env bash
# ==============================================================================
# make-appimage.sh — Build a Via AppImage locally
#
# Downloads linuxdeploy + Qt plugin to build/tools/ if not already present,
# then builds a Release binary and packages it into an AppImage.
#
# Prerequisites:
#   - All build dependencies installed (Qt6, FUSE3, etc.)
#   - A working C++ compiler and CMake
#
# Output:
#   Via-x86_64.AppImage in the project root
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

# --- Preflight checks --------------------------------------------------------

check_command cmake
check_command wget

# --- Download linuxdeploy tools -----------------------------------------------

info "Ensuring linuxdeploy tools are available..."
download_tool "${LINUXDEPLOY_URL}" "${LINUXDEPLOY}"
download_tool "${LINUXDEPLOY_QT_URL}" "${LINUXDEPLOY_QT}"

# --- Build --------------------------------------------------------------------

info "Configuring Release build..."
cmake -B "${BUILD_DIR}" \
    -S "${PROJECT_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTS=OFF

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

# Find qmake — try qt6 locations first, then fall back to PATH
QMAKE=""
for candidate in /usr/lib/qt6/bin/qmake /usr/lib/x86_64-linux-gnu/qt6/bin/qmake; do
    if [[ -x "${candidate}" ]]; then
        QMAKE="${candidate}"
        break
    fi
done

if [[ -z "${QMAKE}" ]]; then
    QMAKE="$(command -v qmake6 2>/dev/null || command -v qmake 2>/dev/null || true)"
fi

if [[ -z "${QMAKE}" ]]; then
    error "Cannot find qmake. Make sure Qt6 development packages are installed."
    exit 1
fi

export QMAKE
export EXTRA_QT_PLUGINS="svg;"

# linuxdeploy tools are themselves AppImages that need FUSE to mount.
# Setting this env var tells them to extract-and-run instead, which
# works everywhere (containers, systems without fuse, etc.).
export APPIMAGE_EXTRACT_AND_RUN=1

cd "${PROJECT_DIR}"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --output appimage

# Rename to a consistent name
GENERATED=$(ls -1t Via*.AppImage 2>/dev/null | head -n1)
if [[ -n "${GENERATED}" && "${GENERATED}" != "Via-x86_64.AppImage" ]]; then
    mv "${GENERATED}" "Via-x86_64.AppImage"
fi

info "Done! AppImage created: ${PROJECT_DIR}/Via-x86_64.AppImage"
