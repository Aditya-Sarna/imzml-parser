#!/usr/bin/env bash
# =============================================================================
# build_on_vps.sh  —  Run as root on the VPS after rsync
# =============================================================================
set -euo pipefail

APP_DIR="/opt/imzml"
MINIFORGE=/opt/miniforge3
export PATH="$MINIFORGE/bin:$PATH"

echo "==> Building imzml_server on Linux"
cd "$APP_DIR/src"

# Activate the conda env so the cmake can find OpenMS headers
source "$MINIFORGE/etc/profile.d/conda.sh"
conda activate openms_env

CONDA_ENV="$MINIFORGE/envs/openms_env"
BUILD="$APP_DIR/build"

mkdir -p "$BUILD"
cd "$BUILD"

cmake "$APP_DIR/src" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DOPENMS_CONDA_ENV="$CONDA_ENV" \
    -DCMAKE_INSTALL_PREFIX="$APP_DIR" \
    2>&1

cmake --build . --target imzml_server -j"$(nproc)"

echo ""
echo "==> Build done: $BUILD/bin/imzml_server"
ls -lh "$BUILD/bin/imzml_server"
