#!/bin/bash
# package-built-artifacts.sh — 把容器构建产物(opendcdiag + 运行时依赖库 + 运行脚本)
# 打包到对应 RPM submodule 的 built/ 目录,供目标机离线运行。
#
# 用法: ./package-built-artifacts.sh <series> <sp>
#   series: 24.03 | 22.03 | 20.03
#   sp    : LTS | SP1 | SP2 | SP3 | SP4
#
# 产物结构 (每个 SP):
#   third-party/rpms/openEuler-XX.03/openEuler-XX.03LTS_SPx/built/
#   ├── opendcdiag              (stripped 二进制)
#   ├── libs/                   (非系统自带、需随包提供的 .so)
#   │   └── ...
#   └── run-opendcdiag.sh       (设 LD_LIBRARY_PATH 后跑二进制)
#
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SERIES="${1:?usage: $0 <series> <sp>}"
SP="${2:?usage: $0 <series> <sp>}"

case "$SP" in
    LTS) SP_DIR="LTS" ;;
    SP[1-4]) SP_DIR="LTS_$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac

OS_TAG="openEuler-${SERIES}${SP_DIR}"
BIN="$SRC_ROOT/build-out/${OS_TAG}/opendcdiag"
RPMDIR="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}"
OUTDIR="$RPMDIR/built"

[ -f "$BIN" ] || { echo "二进制不存在: $BIN" >&2; exit 1; }
[ -d "$RPMDIR" ] || { echo "RPM 目录不存在: $RPMDIR" >&2; exit 1; }

mkdir -p "$OUTDIR/libs"

# 1) stripped 二进制
echo "==> $OS_TAG: 拷贝 + strip 二进制"
cp "$BIN" "$OUTDIR/opendcdiag"
strip --strip-debug --strip-unneeded "$OUTDIR/opendcdiag" 2>/dev/null || true
chmod +x "$OUTDIR/opendcdiag"

# 2) 运行时依赖库(非系统自带的)。用容器内 ldd 取真实路径。
#    方案: 在对应容器里跑 ldd, 提取 .so 路径, 只拷非 /lib64 /usr/lib64 系统路径
#    的库(主要是 toolset 的 libstdc++/libgcc_s/libatomic, 以及 24.03 的 ACL)。
echo "==> $OS_TAG: 收集运行时依赖库"
IMG="localhost/openeuler-offline:${SERIES}-LTS${SP#LTS}"
[ "$SP" = "LTS" ] && IMG="localhost/openeuler-offline:${SERIES}-LTS"

# 收集 ldd 输出的库路径
LIBS_FILE="$(mktemp)"
timeout 120 podman run --rm --user=0 \
    -v "$SRC_ROOT/build-out/${OS_TAG}:/b:ro" \
    -v "$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}:/rpms:ro" \
    "$IMG" bash -c '
mkdir -p /var/tmp /tmp
rpm -Uvh --nodeps --force /rpms/findutils-*.rpm >/dev/null 2>&1 || true
# 装最小运行时库(让 ldd 能解析 toolset 路径) — 仅 ldd 探测, 不跑二进制
ldd /b/opendcdiag 2>/dev/null | awk "/=>/ {print \$3} /^[[:space:]]/ {print \$1}" | sort -u
' > "$LIBS_FILE" 2>/dev/null || true

# 判定哪些库需随包(非标准系统路径): toolset 的 /opt/openEuler/..., 24.03 的 ACL
NEEDED_LIBS=()
while IFS= read -r lib; do
    [ -z "$lib" ] && continue
    case "$lib" in
        /opt/openEuler/*)
            NEEDED_LIBS+=("$lib") ;;
        /usr/lib64/libarm_compute*.so*|/usr/lib64/libarm_compute_graph.so*)
            NEEDED_LIBS+=("$lib") ;;
        # libstdc++/libgcc_s/libatomic/libgomp on 20.03 come from toolset — bundled
        # if the binary's RPATH points to /opt/openEuler (handled above by path match)
        *) ;;  # 系统 /lib64, /usr/lib64 标准库, 不随包
    esac
done < "$LIBS_FILE"

# 对 20.03: 若二进制 RPATH 是 toolset, 把 toolset 的 libstdc++/libgcc_s/libatomic/libgomp 拷出
if [ "$SERIES" = "20.03" ]; then
    TOOLSET_LIBS="/opt/openEuler/gcc-toolset-10/root/usr/lib64"
    timeout 120 podman run --rm --user=0 \
        -v "$SRC_ROOT/third-party/rpms/openEuler-20.03/${OS_TAG}:/rpms:ro" \
        "$IMG" bash -c "mkdir -p /var/tmp /tmp; rpm -Uvh --nodeps --force /rpms/findutils-*.rpm >/dev/null 2>&1 || true; for p in gcc-toolset-10-libstdc++ gcc-toolset-10-libgcc gcc-toolset-10-libatomic gcc-toolset-10-libgomp; do f=\$(ls /rpms/\${p}-*.rpm 2>/dev/null|head -1); [ -n \"\$f\" ] && rpm -Uvh --nodeps --force \"\$f\" >/dev/null 2>&1; done; ls $TOOLSET_LIBS/libstdc++.so* $TOOLSET_LIBS/libgcc_s.so* $TOOLSET_LIBS/libatomic.so* $TOOLSET_LIBS/libgomp.so* 2>/dev/null" \
        2>/dev/null | while IFS= read -r f; do [ -n "$f" ] && echo "$f"; done > /tmp/toolset-libs.txt
    # 把 toolset lib 路径加入待拷
    while IFS= read -r f; do [ -n "$f" ] && NEEDED_LIBS+=("$f"); done < /tmp/toolset-libs.txt
fi

# 24.03: 把 host 的 ACL .so 拷进包(binary 绑定了它)
if [ "$SERIES" = "24.03" ]; then
    for acl in /usr/lib64/libarm_compute.so /usr/lib64/libarm_compute_graph.so; do
        [ -f "$acl" ] && NEEDED_LIBS+=("$acl")
    done
fi

# 从容器里把需要的库拷出(库文件在容器内, 用 podman cp 或挂载)
echo "    待随包库: ${#NEEDED_LIBS[@]} 个"
rm -f "$OUTDIR/libs/"*.so*
for lib in "${NEEDED_LIBS[@]}"; do
    libname=$(basename "$lib")
    # 库可能在 host(ACL) 或容器内(toolset)。host 直接拷。
    if [ -f "$lib" ]; then
        cp -L "$lib" "$OUTDIR/libs/$libname" 2>/dev/null && echo "    ✓ $libname (host)"
    fi
done

# 对 20.03 toolset 库: 从容器拷出 (tar 流, 落到 libs/)
# 20.03-LTS 最小镜像无 GNU tar, 但有 bsdtar (libarchive); 用 `command -v` 兼容。
if [ "$SERIES" = "20.03" ]; then
    timeout 120 podman run --rm --user=0 \
        -v "$SRC_ROOT/third-party/rpms/openEuler-20.03/${OS_TAG}:/rpms:ro" \
        "$IMG" bash -c '
mkdir -p /var/tmp /tmp
rpm -Uvh --nodeps --force /rpms/findutils-*.rpm >/dev/null 2>&1 || true
for p in gcc-toolset-10-libstdc++ gcc-toolset-10-libgcc gcc-toolset-10-libatomic gcc-toolset-10-libgomp; do
    f=$(ls /rpms/${p}-*.rpm 2>/dev/null | head -1) || true
    [ -n "$f" ] && rpm -Uvh --nodeps --force "$f" >/dev/null 2>&1 || true
done
TAR=$(command -v tar bsdtar 2>/dev/null | head -1)
[ -z "$TAR" ] && TAR=tar
# -h 跟随 symlink, 两份(symlink 名 + 实文件名)都打包为硬拷贝
"$TAR" chf - -C /opt/openEuler/gcc-toolset-10/root/usr/lib64 \
    libstdc++.so.6 libstdc++.so.6.0.28 \
    libgcc_s.so.1 libgcc_s-10.so.1 \
    libatomic.so.1 libatomic.so.1.2.0 \
    libgomp.so.1 libgomp.so.1.0.0 2>/dev/null
' 2>/dev/null | tar xf - -C "$OUTDIR/libs/" 2>/dev/null \
        && echo "    ✓ toolset libs 拷出 (libstdc++/libgcc_s/libatomic/libgomp)" \
        || echo "    (toolset libs 拷贝跳过)"
fi

rm -f "$LIBS_FILE" /tmp/toolset-libs.txt

# 3) 运行脚本
cat > "$OUTDIR/run-opendcdiag.sh" <<'RUN_EOF'
#!/bin/bash
# run-opendcdiag.sh — 在目标机上运行随包的 opendcdiag 二进制。
# 自动设置 LD_LIBRARY_PATH 指向随包 libs/ 目录。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SCRIPT_DIR/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# 20.03 的二进制 RPATH 指向 /opt/openEuler/gcc-toolset-10/root/usr/lib64;
# 若目标机没装 toolset, 上面的 libs/ 提供了同名库, LD_LIBRARY_PATH 优先于 RPATH。
exec "$SCRIPT_DIR/opendcdiag" "$@"
RUN_EOF
chmod +x "$OUTDIR/run-opendcdiag.sh"

echo "==> $OS_TAG 打包完成:"
ls -la "$OUTDIR"
echo "    libs:"; ls "$OUTDIR/libs/" 2>/dev/null | head
