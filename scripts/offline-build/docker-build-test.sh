#!/bin/bash
# docker-build-test.sh — 在一个 openEuler 容器里离线装依赖 + 构建 + 运行验证。
#
# 用法: docker-build-test.sh <版本tag> <rpm子模块系列目录名>
#   例: docker-build-test.sh 24.03-lts-sp3 openEuler-24.03
#
# 容器内挂载: 源码仓根 + 第三方 RPM 树 + 离线脚本。
# 产物 builddir 放在宿主机(持久), 挂载进容器。
set -euo pipefail

TAG="$1"            # 镜像 tag, 如 24.03-lts-sp3
SERIES="$2"         # third-party/rpms 下的系列目录, 如 openEuler-24.03
REPO="$(git rev-parse --show-toplevel 2>/dev/null || echo "$PWD")"
IMG="openeuler-${TAG}:latest"

echo "============================================"
echo "  版本: $TAG  (系列: $SERIES)"
echo "============================================"

# 容器里:装依赖 + 构建 + 测试。挂载源码到 /src, RPM 树到 /rpms
# 用 privileged 以让 dnf 离线安装能写 /var 等
sudo podman run --rm \
    -v "$REPO:/src:ro" \
    -v "$REPO/third-party/rpms/$SERIES:/rpms:ro" \
    -v "buildcache-${TAG}:/src/builddir" \
    --privileged \
    "$IMG" /bin/bash -c '
set -euo pipefail
echo "--- 容器内版本 ---"
cat /etc/os-release | head -3

echo "--- 离线装依赖(install-deps.sh) ---"
# install-deps.sh 需可写工作目录; 复制脚本到 /tmp 并运行
cp /src/scripts/offline-build/*.sh /tmp/
chmod +x /tmp/*.sh
# 对该系列下每个版本目录, 装依赖(版本应与容器匹配的才装; 不匹配的跳过)
# 取与容器版本匹配的那个版本目录
MY_VER=$(source /tmp/_common.sh; detect_os_version_full)
echo "容器版本串: $MY_VER"
MATCH_DIR=""
for d in /rpms/openEuler-*/; do
    [ -f "$d/.os-version" ] || continue
    tag=$(cat "$d/.os-version" | tr -d "[:space:]")
    if [ "$tag" = "$MY_VER" ]; then MATCH_DIR="$d"; break; fi
done
if [ -z "$MATCH_DIR" ]; then
    echo "错误: 在 $SERIES 下未找到与容器 $MY_VER 匹配的版本目录" >&2
    exit 1
fi
echo "匹配的 RPM 目录: $MATCH_DIR"
/tmp/install-deps.sh "$MATCH_DIR" 2>&1 | tail -20

echo "--- 构建(build.sh) ---"
# build.sh 内部用 git rev-parse, 需源码可写? builddir 用独立卷可写
# 但 /src 是 ro, build.sh 的 git rev-parse 在 ro 下能跑(只读)
cd /src
/tmp/build.sh /src /src/builddir 2>&1 | tail -25

echo "--- 运行验证 ---"
/src/builddir/opendcdiag --list-tests 2>/dev/null | wc -l | xargs echo "list-tests 行数:"
/src/builddir/opendcdiag -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3
echo "--- $TAG 完成 ---"
'
