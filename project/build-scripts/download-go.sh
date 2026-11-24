#!/usr/bin/env bash
set -euo pipefail

TOOLCHAIN_VERSION_DEFAULT="1.26.11"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

detect_host() {
    local uname_out
    uname_out="$(uname -a)"
    if [[ "${uname_out}" == *"x86_64"* && "${uname_out}" == *"Linux"* ]]; then
        printf '%s\n' "linux-amd64"
    elif [[ "${uname_out}" == *"arm64"* && "${uname_out}" == *"Darwin"* ]]; then
        printf '%s\n' "darwin-amd64"
    elif [[ "${uname_out}" == *"aarch64"* && "${uname_out}" == *"Linux"* ]]; then
        printf '%s\n' "linux-arm64"
    else
        printf '%s\n' "This can only be run on x86_64 Linux, arm64 Linux, or arm64 macOS systems at the moment." >&2
        return 1
    fi
}

clean_dir() {
    local target="$1"
    if [[ -d "${target}" ]]; then
        chmod -R u+w "${target}" || true
        find "${target}" -delete
    fi
}

install_patched_go() {
    local go_ver="$1"
    local host="$2"
    local patch_file="$3"
    local dest_dir="$4"
    local marker="${dest_dir}/go/.vicos-arm-time64-patched"
    if [[ -f "${marker}" ]]; then
        return 0
    fi
    command -v patch >/dev/null 2>&1 || { printf '%s\n' "Missing required tool: patch" >&2; return 1; }
    local boot_dir
    boot_dir="$(mktemp -d "${dest_dir}/boot.XXXXXX")"
    trap 'clean_dir "${boot_dir:-}"' RETURN
    local boot_tgz="go${go_ver}.${host}.tar.gz"
    local src_tgz="go${go_ver}.src.tar.gz"
    wget -q --show-progress -O "${boot_dir}/${boot_tgz}" "https://go.dev/dl/${boot_tgz}"
    wget -q --show-progress -O "${boot_dir}/${src_tgz}" "https://go.dev/dl/${src_tgz}"
    tar -C "${boot_dir}" --strip-components=1 -zxf "${boot_dir}/${boot_tgz}"
    clean_dir "${dest_dir}/go"
    tar -C "${dest_dir}" -zxf "${boot_dir}/${src_tgz}"
    patch -d "${dest_dir}/go" -p1 < "${patch_file}"
    (cd "${dest_dir}/go/src" && GOROOT_BOOTSTRAP="${boot_dir}" ./make.bash)
    touch "${marker}"
}

install_stock_go() {
    local go_ver="$1"
    local host="$2"
    local dest_dir="$3"
    if [[ -d "${dest_dir}/go" ]]; then
        return 0
    fi
    local go_tgz="go${go_ver}.${host}.tar.gz"
    echo "Downloading Go ${go_ver}..."
    wget -q --show-progress -O "${dest_dir}/${go_tgz}" "https://go.dev/dl/${go_tgz}"
    echo "Extracting Go ${go_ver}..."
    tar -C "${dest_dir}" -zxf "${dest_dir}/${go_tgz}"
    rm -f "${dest_dir}/${go_tgz}"
}

main() {
    local go_ver="${1:-${TOOLCHAIN_VERSION_DEFAULT}}"
    local host
    host="$(detect_host)"
    echo "Go platform: ${host}"
    local dest_dir="${HOME}/.anki/go/dist/${go_ver}"
    mkdir -p "${dest_dir}"
    local patch_file="${SCRIPT_DIR}/patches/0001-runtime-arm-time64-go${go_ver}.patch"
    if [[ -f "${patch_file}" ]]; then
        install_patched_go "${go_ver}" "${host}" "${patch_file}" "${dest_dir}"
    else
        install_stock_go "${go_ver}" "${host}" "${dest_dir}"
    fi
}

{
    main "$@"
    exit $?
}
