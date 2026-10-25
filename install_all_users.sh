#!/usr/bin/env bash
# Step 3 of logbook.md: install the visualization tools for all users.
# Reuses what was downloaded/built in ~/.local/opt (step 1).
#
#   sudo bash install_all_users.sh
#
# Result:
#   /usr/local/cuda-13.2 (+ /usr/local/cuda symlink)   CUDA toolkit
#   /opt/viztools/{optix-9.1.0,sdl3,entt-4.0.0,imgui-1.93.0,usd-26.11}
#   /etc/profile.d/viztools.sh                          environment for login shells
#   /etc/ld.so.conf.d/viztools.conf                     runtime library paths
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo bash $0" >&2
    exit 1
fi

USER_HOME=$(getent passwd "${SUDO_USER:-pablofigueroa}" | cut -d: -f6)
LOCAL=$USER_HOME/.local/opt
SRC=$LOCAL/src
DEST=/opt/viztools

echo "==> CUDA 13.2 toolkit -> /usr/local/cuda-13.2"
if [[ ! -x /usr/local/cuda-13.2/bin/nvcc ]]; then
    TMPDIR=$SRC bash "$SRC/cuda_13.2.1_linux.run" --nox11 -- --silent --toolkit --no-man-page --no-drm --override
fi

mkdir -p "$DEST"

echo "==> OptiX 9.1.0 headers"
rm -rf "$DEST/optix-9.1.0"
cp -r "$LOCAL/optix-9.1.0" "$DEST/optix-9.1.0"

echo "==> Dear ImGui 1.93.0 sources + backends"
rm -rf "$DEST/imgui-1.93.0"
cp -r "$LOCAL/imgui-1.93.0" "$DEST/imgui-1.93.0"

echo "==> SDL3 3.4.18"
cmake --install "$SRC/SDL3/build" --prefix "$DEST/sdl3" > /dev/null

echo "==> EnTT 4.0.0"
cmake --install "$SRC/entt/build" --prefix "$DEST/entt-4.0.0" > /dev/null

echo "==> OpenUSD 26.11.1"
rm -rf "$DEST/usd-26.11"
cp -r "$LOCAL/usd-26.11" "$DEST/usd-26.11"

# Point text files (cmake/pkg-config) that mention the per-user location to /opt/viztools
grep -rlI "$LOCAL" "$DEST" | xargs -r sed -i "s#$LOCAL#$DEST#g"

chown -R root:root "$DEST"
chmod -R a+rX "$DEST"

echo "==> /etc/ld.so.conf.d/viztools.conf"
cat > /etc/ld.so.conf.d/viztools.conf <<EOF
/usr/local/cuda-13.2/lib64
$DEST/sdl3/lib
$DEST/usd-26.11/lib
EOF
ldconfig

echo "==> /etc/profile.d/viztools.sh"
cat > /etc/profile.d/viztools.sh <<'EOF'
# Visualization tools installed for all users (see /opt/viztools)
export CUDA_HOME=/usr/local/cuda-13.2
export OptiX_INSTALL_DIR=/opt/viztools/optix-9.1.0
export IMGUI_DIR=/opt/viztools/imgui-1.93.0
export PATH=$CUDA_HOME/bin:/opt/viztools/usd-26.11/bin:$PATH
export CMAKE_PREFIX_PATH=/opt/viztools/sdl3:/opt/viztools/entt-4.0.0:/opt/viztools/usd-26.11:$CUDA_HOME${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}
EOF
chmod 644 /etc/profile.d/viztools.sh

echo "==> Done. Verifying:"
/usr/local/cuda-13.2/bin/nvcc --version | tail -1
ls "$DEST"
