#!/bin/sh
set -eu

install_dir=${1:?usage: smoke_linux_server.sh INSTALL_DIRECTORY}
server="${install_dir}/laiue_server"
if [ ! -x "${server}" ]; then
    echo "missing installed server: ${server}" >&2
    exit 1
fi

if ! readelf -d "${server}" | grep -Fq '$ORIGIN'; then
    echo "laiue_server has no \$ORIGIN RUNPATH" >&2
    exit 1
fi
if ldd "${server}" | grep -q 'not found'; then
    ldd "${server}" >&2
    exit 1
fi
if ! find "${install_dir}" -maxdepth 1 -name 'libmsquic.so*' |
    grep -q .
then
    echo "installed bundle has no libmsquic.so runtime" >&2
    exit 1
fi
for notice in LICENSE \
    licenses/msquic/LICENSE \
    licenses/msquic/THIRD-PARTY-NOTICES
do
    if [ ! -s "${install_dir}/${notice}" ]; then
        echo "installed bundle has no required notice: ${notice}" >&2
        exit 1
    fi
done

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
