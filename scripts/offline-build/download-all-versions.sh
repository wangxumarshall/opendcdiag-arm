#!/bin/bash
# download-all-versions.sh — 为 openEuler 各 LTS 版本下载与基准包列表相同的 RPM 集。
# 优化版: 先 repoquery 各版本可用包, 取与基准交集, 一次性下载(避免逐包慢)。
#
# RPM 存放在 third-party/rpms 下的三个并列子模块里(按大版本分仓,
# 各自独立 git 仓以避开 git 单 pack 2GB 上限):
#   third-party/rpms/openEuler-20.03/   openEuler 20.03 系列
#   third-party/rpms/openEuler-22.03/   openEuler 22.03 系列
#   third-party/rpms/openEuler-24.03/   openEuler 24.03 系列 (含 SP3 基准)
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

# 基准: 24.03 SP3 的包名列表(所有版本都与之取交集)
mapfile -t BASELINE_PKGS < <(ls third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/rpms/*.rpm 2>/dev/null | xargs -n1 basename | sed -E 's/-[0-9].*//' | sort -u)
echo "基准包名数: ${#BASELINE_PKGS[@]}"

MIRROR="https://repo.openeuler.org"
VERSIONS=(
  "openEuler-20.03-LTS|openEuler-20.03LTS"
  "openEuler-20.03-LTS-SP1|openEuler-20.03LTS_SP1"
  "openEuler-20.03-LTS-SP2|openEuler-20.03LTS_SP2"
  "openEuler-20.03-LTS-SP3|openEuler-20.03LTS_SP3"
  "openEuler-20.03-LTS-SP4|openEuler-20.03LTS_SP4"
  "openEuler-22.03-LTS|openEuler-22.03LTS"
  "openEuler-22.03-LTS-SP1|openEuler-22.03LTS_SP1"
  "openEuler-22.03-LTS-SP2|openEuler-22.03LTS_SP2"
  "openEuler-22.03-LTS-SP3|openEuler-22.03LTS_SP3"
  "openEuler-22.03-LTS-SP4|openEuler-22.03LTS_SP4"
  "openEuler-24.03-LTS|openEuler-24.03LTS"
  "openEuler-24.03-LTS-SP1|openEuler-24.03LTS_SP1"
  "openEuler-24.03-LTS-SP2|openEuler-24.03LTS_SP2"
  "openEuler-24.03-LTS-SP4|openEuler-24.03LTS_SP4"
)

# 根据版本目录名算出大版本子模块目录(20.03/22.03/24.03)
series_dir() {
    case "$1" in
        openEuler-20.03*) echo "third-party/rpms/openEuler-20.03" ;;
        openEuler-22.03*) echo "third-party/rpms/openEuler-22.03" ;;
        openEuler-24.03*) echo "third-party/rpms/openEuler-24.03" ;;
        *) echo "third-party/rpms/openEuler-24.03" ;;  # 兜底
    esac
}

# 探测可达子仓库
build_repo_args() {
    local repover="$1"; local -a args=(); local idx=0
    for sub in OS everything EPOL update; do
        local url="$MIRROR/$repover/$sub/aarch64/"
        if dnf repoinfo --repofrompath="probe,${url}" --disablerepo=* --enablerepo=probe 2>/dev/null | grep -qi "Repo-id"; then
            args+=(--repofrompath="r${idx},${url}" --enablerepo="r${idx}")
            idx=$((idx+1))
        fi
    done
    printf '%s\n' "${args[@]}"
}

# repoquery 该版本所有包名, 输出到 stdout (一行一个)
query_all_pkg_names() {
    local -n args_ref=$1
    dnf repoquery --disablerepo=* "${args_ref[@]/--enablerepo/}" --queryformat '%{NAME}' --available 2>/dev/null | sort -u
}

download_version() {
    local repover="$1" dirname="$2"
    local outdir
    outdir="$(series_dir "$dirname")/$dirname/rpms"
    mkdir -p "$outdir"
    echo "========== $dirname (仓库: $repover, 输出: $outdir) =========="

    mapfile -t REPOARGS < <(build_repo_args "$repover")
    if [ ${#REPOARGS[@]} -eq 0 ]; then echo "  警告: 无可达子仓库, 跳过"; return; fi
    echo "  可达子仓库参数数: ${#REPOARGS[@]}"

    # 1) repoquery 该版本所有可用包名(单次调用, 慢一次但只一次)
    echo "  查询该版本可用包名..."
    local avail_file; avail_file=$(mktemp)
    # REPOARGS 含 --repofrompath 和 --enablerepo 交替; repoquery 也支持这俩
    dnf repoquery --disablerepo=* "${REPOARGS[@]}" --queryformat '%{NAME}' --available 2>/dev/null | sort -u > "$avail_file"
    echo "  该版本可用包名数: $(wc -l < $avail_file)"

    # 2) 取基准包 ∩ 可用包
    local keep_file; keep_file=$(mktemp)
    printf '%s\n' "${BASELINE_PKGS[@]}" | grep -Fxf "$avail_file" - > "$keep_file"
    local keepn=$(wc -l < "$keep_file")
    echo "  基准∩可用: $keepn 包 (基准 ${#BASELINE_PKGS[@]}, 缺 $(( ${#BASELINE_PKGS[@]} - keepn )))"

    # 3) 一次性下载交集包 + 依赖树 (这些包都存在, 不会 No package 中断)
    mapfile -t KEEP < "$keep_file"
    dnf download --disablerepo=* "${REPOARGS[@]}" --resolve --alldeps \
        --destdir="$outdir" "${KEEP[@]}" 2>&1 | grep -iE "No package|error|downloaded" | tail -5 || true

    local count=$(ls "$outdir"/*.rpm 2>/dev/null | wc -l)
    echo "  $dirname: 共 $count RPM"
    rm -f "$avail_file" "$keep_file"
}

for entry in "${VERSIONS[@]}"; do
    repover="${entry%%|*}"; dirname="${entry##*|}"
    local_dir="$(series_dir "$dirname")/$dirname/rpms"
    if [ -d "$local_dir" ] && [ "$(ls "$local_dir"/*.rpm 2>/dev/null | wc -l)" -gt 100 ]; then
        echo "跳过 $dirname (已存在)"; continue
    fi
    download_version "$repover" "$dirname"
done

echo ""; echo "========== 全部完成 =========="
for series in third-party/rpms/openEuler-*/; do
    [ -d "$series" ] || continue
    for d in "$series"openEuler-*/; do
        [ -d "$d/rpms" ] && echo "  $(basename $d): $(ls $d/rpms/*.rpm 2>/dev/null | wc -l) RPM"
    done
done
