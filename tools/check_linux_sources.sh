#!/bin/sh
set -eu

msquic_include=${1:?usage: check_linux_sources.sh MSQUIC_INCLUDE_DIRECTORY}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${source_root}"

common_flags='-std=c17 -Wall -Wextra -Wpedantic -Werror -DLAIUE_VERSION_MAJOR=0 -DLAIUE_VERSION_MINOR=5'
for source in \
    src/platform/system_posix.c \
    src/world/*.c \
    src/physics/*.c \
    src/gameplay/*.c \
    src/interaction/*.c \
    src/content/*.c \
    src/mod/*.c \
    src/network/certificate_validation.c \
    src/network/endpoint.c \
    src/network/network_content.c \
    src/network/protocol.c \
    src/server/main.c \
    src/server/server_config.c
do
    echo "checking ${source}"
    # shellcheck disable=SC2086
    gcc ${common_flags} \
        -DLAIUE_LINUX_LIBC_GNU=1 \
        -I./src -I./sdk -isystem "${msquic_include}" \
        -fsyntax-only "${source}"
done

echo 'checking src/network/network_msquic.c'
# shellcheck disable=SC2086
gcc ${common_flags} \
    -DLAIUE_LINUX_LIBC_GNU=1 \
    -DLAIUE_HAS_MSQUIC=1 \
    -DLAIUE_BUILD_NETWORK=1 \
    -I./src -I./sdk -isystem "${msquic_include}" \
    -fsyntax-only src/network/network_msquic.c
