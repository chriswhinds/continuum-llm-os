#!/bin/sh
# post-build.sh -- copies this repo's own cross-built binaries into the
# Buildroot target rootfs (ARCH-002 §06). Buildroot calls this with
# $1 = the target rootfs directory once its own build finishes; it does
# not build Continuum's C sources itself (see continuum_defconfig's
# comment) -- run the aarch64-musl CMake build first:
#
#   cmake --preset aarch64-musl -S . -B build-aarch64
#   cmake --build build-aarch64 --target \
#       continuumd node-agentd swapd shard-execd schedulerd \
#       api-gatewayd page-directoryd membershipd consoled
#
# then point CONTINUUM_BUILD_DIR at build-aarch64 when invoking
# Buildroot, e.g.:
#   CONTINUUM_BUILD_DIR=$(pwd)/build-aarch64 make

set -e

TARGET_DIR="$1"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${CONTINUUM_BUILD_DIR:-${REPO_ROOT}/build-aarch64}"

if [ ! -f "${BUILD_DIR}/kernel/continuumd" ]; then
    echo "post-build.sh: ${BUILD_DIR}/kernel/continuumd not found." >&2
    echo "Cross-build Continuum for aarch64-musl first -- see this script's header comment." >&2
    exit 1
fi

mkdir -p "${TARGET_DIR}/opt/continuum/bin"
mkdir -p "${TARGET_DIR}/etc/continuum"
mkdir -p "${TARGET_DIR}/run/continuum"
mkdir -p "${TARGET_DIR}/var/lib/continuum"

# continuumd itself is PID 1, not a file under /opt -- see kernel/src/
# main.c and BR2_INIT_NONE in continuum_defconfig.
cp "${BUILD_DIR}/kernel/continuumd" "${TARGET_DIR}/init"

for svc in node-agentd swapd shard-execd schedulerd api-gatewayd \
           page-directoryd membershipd consoled; do
    for candidate in "${BUILD_DIR}/services/${svc}/${svc}" "${BUILD_DIR}/${svc}"; do
        if [ -f "${candidate}" ]; then
            cp "${candidate}" "${TARGET_DIR}/opt/continuum/bin/${svc}"
            break
        fi
    done
done

# Node-specific config (node_id, this board's role, peer lists, etc.) is
# NOT baked into the image -- it's provisioned per node at flash/first-
# boot time. This copies only the config *templates* checked into the
# repo, so an image built here is never accidentally node-specific.
cp -r "${REPO_ROOT}/image/buildroot/config-templates/." "${TARGET_DIR}/etc/continuum/" 2>/dev/null || true

echo "post-build.sh: staged continuumd + $(ls "${TARGET_DIR}/opt/continuum/bin" | wc -l) service binaries into ${TARGET_DIR}"
