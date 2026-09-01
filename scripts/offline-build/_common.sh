#!/bin/bash
# _common.sh — offline-build 脚本共享的 openEuler 版本检测与校验逻辑。
# 被 download-deps.sh / install-deps.sh / build.sh 三者 source。
#
# 提供函数:
#   detect_os_sp            -> 返回 SP 级别短名 (如 sp3 / sp4 / rawhide)
#   detect_os_version_full  -> 返回完整版本串 (如 openEuler-24.03LTS_SP3)
#   require_openeuler       -> 非 openEuler 系统则报错退出
#   require_min_os         -> 校验满足最低支持的大版本
#
# SPDX-License-Identifier: Apache-2.0

# 支持的最低 openEuler 大版本基线 (24.03 LTS)。SP 级别不限。
SDCSHIELD_OFFLINE_MIN_OS="24.03LTS"

# 返回 openEuler 的 SP 级别短名 (小写): sp3 / sp4 ...
# 从 /etc/os-release 的 VERSION="24.03 (LTS-SPx)" 提取; 回退到
# openEuler-release RPM 的 Provides 串。
detect_os_sp() {
    local ver
    ver=$(grep -E '^VERSION=' /etc/os-release 2>/dev/null | head -1)
    # VERSION="24.03 (LTS-SP3)" -> 提取 LTS-SP3 -> 转小写 -> sp3
    local sp
    sp=$(echo "$ver" | sed -nE 's/.*LTS[-_]?(SP[0-9]+).*/\1/p' | tr '[:upper:]' '[:lower:]')
    if [ -n "$sp" ]; then
        echo "$sp"
        return 0
    fi
    # 回退: openEuler-release 的 Provides 含 24.03LTS_SPx
    local rel
    rel=$(rpm -q --qf '%{VERSION}-%{RELEASE}' openEuler-release 2>/dev/null)
    sp=$(echo "$rel" | sed -nE 's/.*_?SP([0-9]+).*/sp\1/p' | head -1)
    echo "${sp:-unknown}"
}

# 返回完整版本串, 形如 openEuler-24.03LTS_SP3
# 作为版本标记文件 .os-version 的内容, 供下载/安装两侧比对。
detect_os_version_full() {
    local sp
    sp=$(detect_os_sp)
    # 大版本 24.03 LTS 固定 (当前仅支持该基线); SPx 来自 detect_os_sp
    echo "openEuler-${SDCSHIELD_OFFLINE_MIN_OS}_${sp^^}"
}

# 确认当前运行在 openEuler 上 (脚本仅支持 openEuler)。否则报错退出。
require_openeuler() {
    local id
    id=$(grep -E '^ID=' /etc/os-release 2>/dev/null | head -1)
    # ID="openEuler" -> openEuler (去引号)
    id=${id#ID=}
    id=${id#\"}
    id=${id%\"}
    if [ "$id" != "openEuler" ]; then
        echo "错误: 这些脚本仅支持 openEuler (当前系统 ID='$id'), 不支持跨发行版离线安装。" >&2
        echo "       原因: RPM 依赖树与 openEuler 版本强绑定, 非 openEuler 上下载/安装会失败。" >&2
        return 1
    fi
    return 0
}

# 校验当前 OS 大版本满足最低基线 (24.03 LTS)。SP 级别不在此校验
# (SP 由下载/安装两侧的严格匹配负责, 见 install-deps.sh)。
require_min_os() {
    local ver_id
    ver_id=$(grep -E '^VERSION_ID=' /etc/os-release 2>/dev/null | head -1)
    ver_id=${ver_id#VERSION_ID=}
    ver_id=${ver_id#\"}
    ver_id=${ver_id%\"}
    if [ "$ver_id" != "24.03" ]; then
        echo "警告: 当前 openEuler 大版本 '$ver_id', 脚本基线为 24.03 LTS, 可能有兼容风险。" >&2
    fi
}
