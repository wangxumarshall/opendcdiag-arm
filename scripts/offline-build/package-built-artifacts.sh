#!/bin/bash
# package-built-artifacts.sh — 把容器构建产物(sdcshield + 运行时依赖库 + 运行脚本)
# 打包到对应 RPM submodule 的 built/ 目录,供目标机离线运行。
#
# 用法: ./package-built-artifacts.sh <series> <sp>
#   series: 24.03 | 22.03 | 20.03
#   sp    : LTS | SP1 | SP2 | SP3 | SP4
#
# 产物结构 (每个 SP):
#   third-party/rpms/openEuler-XX.03/openEuler-XX.03LTS_SPx/built/
#   ├── sdcshield              (stripped 二进制)
#   ├── libs/                   (非系统自带、需随包提供的 .so)
#   │   └── ...
#   └── run-sdcshield.sh       (设 LD_LIBRARY_PATH 后跑二进制)
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
BIN="$SRC_ROOT/build-out/${OS_TAG}/sdcshield"
RPMDIR="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/rpms"
OUTDIR="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/built"

[ -f "$BIN" ] || { echo "二进制不存在: $BIN" >&2; exit 1; }
[ -d "$RPMDIR" ] || { echo "RPM 目录不存在: $RPMDIR" >&2; exit 1; }

mkdir -p "$OUTDIR/libs"

# 1) stripped 二进制
echo "==> $OS_TAG: 拷贝 + strip 二进制"
cp "$BIN" "$OUTDIR/sdcshield"
strip --strip-debug --strip-unneeded "$OUTDIR/sdcshield" 2>/dev/null || true
chmod +x "$OUTDIR/sdcshield"

# 2) 运行时依赖库(非系统自带的)。用容器内 ldd 取真实路径。
#    方案: 在对应容器里跑 ldd, 提取 .so 路径, 只拷非 /lib64 /usr/lib64 系统路径
#    的库(主要是 toolset 的 libstdc++/libgcc_s/libatomic, 以及 24.03 的 ACL)。
echo "==> $OS_TAG: 收集运行时依赖库"
IMG="localhost/openeuler-offline:${SERIES}-LTS${SP#LTS}"
[ "$SP" = "LTS" ] && IMG="localhost/openeuler-offline:${SERIES}-LTS"

# 收集 ldd 输出的库路径
LIBS_FILE="$(mktemp)"
timeout 120 podman run --rm --user=0 \
    -v "$SRC_ROOT/build-out/${OS_TAG}:/b:ro,Z" \
    -v "$RPMDIR:/rpms:ro,Z" \
    "$IMG" bash -c '
mkdir -p /var/tmp /tmp
rpm -Uvh --nodeps --force /rpms/findutils-*.rpm >/dev/null 2>&1 || true
# 装最小运行时库(让 ldd 能解析 toolset 路径) — 仅 ldd 探测, 不跑二进制
ldd /b/sdcshield 2>/dev/null | awk "/=>/ {print \$3} /^[[:space:]]/ {print \$1}" | sort -u
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
        -v "$SRC_ROOT/third-party/rpms/openEuler-20.03/${OS_TAG}/rpms:/rpms:ro" \
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
        -v "$SRC_ROOT/third-party/rpms/openEuler-20.03/${OS_TAG}/rpms:/rpms:ro" \
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

# 对 22.03 和 24.03: 最小 KIWI 容器缺少 libatomic.so.1 (其余 libstdc++/libz/libzstd/libgmp
# 自带), 需从 RPM 树的 libatomic-*.rpm 提取 libatomic.so.1 + libatomic.so.1.2.0 到 libs/,
# 否则纯净容器内跑二进制会 "error while loading shared libraries: libatomic.so.1"。
#   注意: libatomic RPM 只在各系列的 LTS 目录里(GCC 版本全系列共享), SP1-SP4 目录没有,
#   所以一律从该系列 LTS 目录取, 而非当前 SP 目录。
if [ "$SERIES" = "22.03" ] || [ "$SERIES" = "24.03" ]; then
    LTS_RPMDIR="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/openEuler-${SERIES}LTS/rpms"
    LIBATOMIC_RPM=$(ls "$LTS_RPMDIR"/libatomic-*.rpm 2>/dev/null | head -1)
    if [ -n "$LIBATOMIC_RPM" ]; then
        rm -rf /tmp/libatomic-extract && mkdir /tmp/libatomic-extract
        if (cd /tmp/libatomic-extract && rpm2cpio "$LIBATOMIC_RPM" | cpio -idm --quiet 2>/dev/null); then
            cp /tmp/libatomic-extract/usr/lib64/libatomic.so* "$OUTDIR/libs/" 2>/dev/null \
                && echo "    ✓ libatomic 拷出 ($SERIES runtime, from LTS)" \
                || echo "    (libatomic 拷贝跳过)"
        fi
        rm -rf /tmp/libatomic-extract
    else
        echo "    (警告: $SERIES LTS RPM 树无 libatomic-*.rpm, 纯净容器内运行可能失败)" >&2
    fi
fi

# 3) 运行脚本
cat > "$OUTDIR/run-sdcshield.sh" <<'RUN_EOF'
#!/bin/bash
# run-sdcshield.sh — 在目标机上运行随包的 sdcshield 二进制。
# 自动设置 LD_LIBRARY_PATH 指向随包 libs/ 目录。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SCRIPT_DIR/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# 20.03 的二进制 RPATH 指向 /opt/openEuler/gcc-toolset-10/root/usr/lib64;
# 若目标机没装 toolset, 上面的 libs/ 提供了同名库, LD_LIBRARY_PATH 优先于 RPATH。
exec "$SCRIPT_DIR/sdcshield" "$@"
RUN_EOF
chmod +x "$OUTDIR/run-sdcshield.sh"

echo "==> $OS_TAG 打包完成:"
ls -la "$OUTDIR"
echo "    libs:"; ls "$OUTDIR/libs/" 2>/dev/null | head

# 4) 元数据:BUILD-HASH + MANIFEST.tsv + VERSION(溯源 + skip 判定 + 部署匹配)
#    BUILD-HASH: build-all.sh 的源码哈希(复用 skip 判定)。计算公式须与
#    build-all.sh 的 compute_build_hash 一致,否则 skip 失效。
write_metadata() {
    local macro cpp_std
    case "$SERIES" in
        24.03) macro=""; cpp_std="gnu++23" ;;
        22.03) macro="OPENEULER_22_03"; cpp_std="gnu++20" ;;
        20.03) macro="OPENEULER_20_03"; cpp_std="gnu++20" ;;
    esac
    # 源码树哈希 + container-build.sh(含 sed 适配)+ 配置 + 镜像 input-hash
    local src_hash cb_hash img_hash build_hash
    src_hash=$(git -C "$SRC_ROOT" ls-tree -r HEAD -- framework tests meson.build meson_options.txt 2>/dev/null | sha256sum | awk '{print $1}')
    cb_hash=$(sha256sum "$SCRIPT_DIR/container-build.sh" | awk '{print $1}')
    local sp_label
    case "$SP" in
        LTS) sp_label="LTS" ;; SP[1-4]) sp_label="LTS-$SP" ;;
    esac
    img_hash=$(grep -P "^${SERIES}-${sp_label}\t" "$SCRIPT_DIR/images/image-manifest.tsv" 2>/dev/null | awk -F'\t' '{print $2}' || echo "no-image")
    build_hash=$(printf '%s|%s|%s|%s|%s|%s-%s\n' "$src_hash" "$cb_hash" "$cpp_std" "${macro:-none}" "$img_hash" "$SERIES" "$sp_label" | sha256sum | awk '{print $1}')
    echo "$build_hash" > "$OUTDIR/BUILD-HASH"

    # MANIFEST.tsv:产物文件 sha256 + 体积 + 来源
    {
        echo "# MANIFEST.tsv — $OS_TAG 产物校验清单"
        echo -e "file\tsha256\tsize\tsource"
        for f in sdcshield run-sdcshield.sh; do
            [ -f "$OUTDIR/$f" ] && printf '%s\t%s\t%s\t%s\n' "$f" "$(sha256sum "$OUTDIR/$f" | awk '{print $1}')" "$(stat -c%s "$OUTDIR/$f")" "built"
        done
        for f in "$OUTDIR"/libs/*; do
            [ -f "$f" ] || continue
            printf '%s\t%s\t%s\t%s\n' "libs/$(basename "$f")" "$(sha256sum "$f" | awk '{print $1}')" "$(stat -c%s "$f")" "bundled"
        done
    } > "$OUTDIR/MANIFEST.tsv"

    # VERSION:人读元数据
    local git_sha bin_sha
    git_sha=$(git -C "$SRC_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    bin_sha=$(sha256sum "$OUTDIR/sdcshield" 2>/dev/null | awk '{print $1}' || echo "?")
    cat > "$OUTDIR/VERSION" <<VERSION_EOF
sdcshield $OS_TAG
git: $git_sha
built: $(git -C "$SRC_ROOT" log -1 --format=%ci HEAD 2>/dev/null | cut -d' ' -f1 || echo unknown)
series: $SERIES  sp: $SP
cpp_std: $cpp_std  macro: ${macro:-none}
image: ghcr.io/wangxumarshall/sdcshield-offline:${SERIES}-${sp_label}
binary-sha256: ${bin_sha:0:64}
build-hash: ${build_hash:0:64}
VERSION_EOF
    echo "    ✓ BUILD-HASH + MANIFEST.tsv + VERSION 写入"
}
write_metadata
