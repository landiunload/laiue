#!/bin/sh
set -eu

echo '== identity =='
id
. /etc/os-release
printf 'os=%s\n' "${PRETTY_NAME}"
uname -m
dpkg --print-architecture 2>/dev/null || true

echo '== capacity =='
nproc
df -hT / /opt /var 2>/dev/null || true

echo '== network =='
ip -brief address
ip -4 route show default
ip -6 route show default 2>/dev/null || true
printf 'disable_ipv6='
cat /proc/sys/net/ipv6/conf/all/disable_ipv6
printf 'bindv6only='
cat /proc/sys/net/ipv6/bindv6only
ss -H -lunpt

echo '== firewall =='
ufw status verbose 2>/dev/null || true

echo '== build dependencies =='
cmake --version 2>/dev/null | head -n 1 || true
ninja --version 2>/dev/null || true
gcc --version 2>/dev/null | head -n 1 || true
clang --version 2>/dev/null | head -n 1 || true
pkg-config --version 2>/dev/null || true
openssl version
dpkg-query -W -f='${binary:Package}\t${Version}\n' 2>/dev/null |
    grep -E \
        '^(cmake|ninja-build|build-essential|gcc|g\+\+|clang|libssl|openssl|pkg-config|libmsquic)' ||
    true
ldconfig -p 2>/dev/null | grep -E 'msquic|libssl|libcrypto' || true

echo '== existing services =='
systemctl is-active nginx charlaiu-deploy.timer 2>/dev/null || true
systemctl list-unit-files 'laiue*' --no-legend 2>/dev/null || true

echo '== reserved paths =='
for path in \
    /opt/charlaiu /var/www/charlaiu \
    /opt/laiue-server /etc/laiue-server /var/lib/laiue-server \
    /etc/systemd/system/laiue-server.service
do
    if [ -e "${path}" ] || [ -L "${path}" ]; then
        stat -c '%F %A %U:%G %n -> %N' "${path}"
    else
        printf 'absent %s\n' "${path}"
    fi
done

echo '== laiue account =='
getent passwd laiue || true
getent group laiue || true

echo '== ssh host key =='
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
