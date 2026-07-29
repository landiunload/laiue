#!/bin/sh
set -eu

install_dir=${1:?usage: smoke_linux_server.sh INSTALL_DIRECTORY}
install_dir=$(CDPATH= cd "${install_dir}" && pwd -P)
server="${install_dir}/laiue_server"
script_directory=$(CDPATH= cd "$(dirname "$0")" && pwd)
msquic_version=2.5.9
if [ ! -x "${server}" ]; then
    echo "missing installed server: ${server}" >&2
    exit 1
fi
for program in ldd openssl readelf readlink timeout; do
    if ! command -v "${program}" >/dev/null 2>&1; then
        echo "server smoke requires ${program}" >&2
        exit 1
    fi
done

if ! readelf -d "${server}" | grep -Fq '$ORIGIN'; then
    echo "laiue_server has no \$ORIGIN RUNPATH" >&2
    exit 1
fi
set +e
ldd_output=$(LD_LIBRARY_PATH= LD_PRELOAD= ldd "${server}")
ldd_status=$?
set -e
if [ "${ldd_status}" -ne 0 ]; then
    printf '%s\n' "${ldd_output}" >&2
    echo "ldd failed for installed server (exit ${ldd_status})" >&2
    exit 1
fi
if printf '%s\n' "${ldd_output}" | grep -q 'not found'; then
    printf '%s\n' "${ldd_output}" >&2
    exit 1
fi
if ! printf '%s\n' "${ldd_output}" |
    grep -F "${install_dir}/libmsquic.so.2" >/dev/null
then
    printf '%s\n' "${ldd_output}" >&2
    echo "server did not resolve bundled libmsquic.so.2" >&2
    exit 1
fi
msquic_runtime=$(find "${install_dir}" -maxdepth 1 -type f \
    -name "libmsquic.so.${msquic_version}" -print | LC_ALL=C sort)
if [ -z "${msquic_runtime}" ] ||
   [ "$(printf '%s\n' "${msquic_runtime}" | wc -l)" -ne 1 ]
then
    echo "installed bundle must have one libmsquic.so.${msquic_version}" >&2
    printf '%s\n' "${msquic_runtime}" >&2
    exit 1
fi
if [ ! -L "${install_dir}/libmsquic.so" ] ||
   [ "$(readlink "${install_dir}/libmsquic.so")" != libmsquic.so.2 ] ||
   [ ! -L "${install_dir}/libmsquic.so.2" ] ||
   [ "$(readlink "${install_dir}/libmsquic.so.2")" != \
        "libmsquic.so.${msquic_version}" ]
then
    echo "installed bundle has an invalid libmsquic symlink chain" >&2
    exit 1
fi
unexpected_symlinks=$(find "${install_dir}" -type l \
    ! -path "${install_dir}/libmsquic.so" \
    ! -path "${install_dir}/libmsquic.so.2" -print)
if [ -n "${unexpected_symlinks}" ]; then
    echo "installed bundle contains unexpected symlinks:" >&2
    printf '%s\n' "${unexpected_symlinks}" >&2
    exit 1
fi
for notice in LICENSE \
    licenses/msquic/LICENSE \
    licenses/msquic/THIRD-PARTY-NOTICES
do
    notice_path="${install_dir}/${notice}"
    if [ ! -s "${notice_path}" ] || [ ! -f "${notice_path}" ] ||
       [ -L "${notice_path}" ]
    then
        echo "installed bundle has no regular notice: ${notice}" >&2
        exit 1
    fi
done
metadata="${install_dir}/licenses/msquic/BUILD-METADATA"
version_file="${install_dir}/licenses/msquic/VERSION"
if [ -e "${metadata}" ]; then
    for lean_notice in "${version_file}" \
        "${install_dir}/licenses/msquic/QUIC-TLS-LICENSE"
    do
        if [ ! -s "${lean_notice}" ] || [ -L "${lean_notice}" ]; then
            echo "lean MsQuic install is missing: ${lean_notice}" >&2
            exit 1
        fi
    done
    sh "${script_directory}/check_lean_msquic.sh" \
        "${msquic_runtime}" "${metadata}"
fi
if [ -e "${version_file}" ]; then
    if [ ! -f "${version_file}" ] || [ -L "${version_file}" ] ||
       [ "$(tr -d '\r' <"${version_file}")" != "${msquic_version}" ]
    then
        echo "installed MsQuic VERSION is not a regular " \
            "${msquic_version} file" >&2
        exit 1
    fi
fi

smoke_root=$(mktemp -d)
trap 'rm -rf "${smoke_root}"' EXIT HUP INT TERM
runtime="${smoke_root}/runtime"
mkdir -p "${runtime}/certs"
cp -R "${install_dir}/." "${runtime}/"
openssl req -x509 -newkey rsa:2048 -sha256 -days 2 -nodes \
    -keyout "${runtime}/certs/server-key.pem" \
    -out "${runtime}/certs/server-cert.pem" \
    -subj '/CN=laiue package smoke' \
    -addext 'subjectAltName=IP:127.0.0.1,IP:::1' \
    >/dev/null 2>&1
chmod 600 "${runtime}/certs/server-key.pem"

run_listener()
{
    family=$1
    address=$2
    port=$3
    expected=$4
    log="${smoke_root}/${family}.log"
    set +e
    (
        cd "${runtime}"
        LAIUE_SERVER_ADDRESS_FAMILY="${family}" \
        LAIUE_SERVER_LISTEN_ADDRESS="${address}" \
        LAIUE_SERVER_PORT="${port}" \
        LAIUE_SERVER_CERTIFICATE_FILE="certs/server-cert.pem" \
        LAIUE_SERVER_PRIVATE_KEY_FILE="certs/server-key.pem" \
        LD_LIBRARY_PATH= \
        LD_PRELOAD= \
        timeout 3 ./laiue_server
    ) >"${log}" 2>&1
    status=$?
    set -e
    if [ "${status}" -ne 0 ] && [ "${status}" -ne 124 ]; then
        cat "${log}" >&2
        exit "${status}"
    fi
    if ! grep -Fq "${expected}" "${log}"; then
        cat "${log}" >&2
        echo "listener startup line missing: ${expected}" >&2
        exit 1
    fi
    if ! grep -Fq ":${port}" "${log}"; then
        cat "${log}" >&2
        echo "listener startup line has no UDP port ${port}" >&2
        exit 1
    fi
}

# Отказ старта обязан объяснить себя: под systemd молчаливый выход виден
# только как Restart=on-failure в цикле. Регрессия к пустому выводу должна
# ронять smoke, а не обнаруживаться оператором в проде.
assert_startup_failure_is_explained()
{
    log="${smoke_root}/no-credentials.log"
    set +e
    (
        cd "${runtime}"
        LAIUE_SERVER_CERTIFICATE_FILE="certs/missing-cert.pem" \
        LAIUE_SERVER_PRIVATE_KEY_FILE="certs/missing-key.pem" \
        LD_LIBRARY_PATH= \
        LD_PRELOAD= \
        timeout 10 ./laiue_server
    ) >"${log}" 2>&1
    status=$?
    set -e
    if [ "${status}" -ne 5 ]; then
        cat "${log}" >&2
        echo "expected exit 5 without credentials, got ${status}" >&2
        exit 1
    fi
    if ! grep -Fq 'startup failed:' "${log}"; then
        echo "server exited ${status} without explaining why" >&2
        exit 1
    fi
    if ! grep -Fq '(exit 5)' "${log}"; then
        cat "${log}" >&2
        echo "startup failure line does not name its exit code" >&2
        exit 1
    fi
}

assert_startup_failure_is_explained
run_listener ipv4 127.0.0.1 27180 '(UDP, IPv4)'
if [ -r /proc/sys/net/ipv6/conf/all/disable_ipv6 ] &&
   [ "$(cat /proc/sys/net/ipv6/conf/all/disable_ipv6)" = 0 ]
then
    run_listener dual '*' 27180 '(UDP, IPv4 + IPv6)'
    run_listener ipv6 ::1 37181 '(UDP, IPv6)'
    run_listener dual '*' 37182 '(UDP, IPv4 + IPv6)'
else
    echo "IPv6 listener smoke skipped: runner disabled IPv6" >&2
fi
