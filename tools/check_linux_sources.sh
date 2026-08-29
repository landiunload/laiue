#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${source_root}"

common_flags='-std=c17 -Wall -Wextra -Wpedantic -Werror -DLAIUE_VERSION_MAJOR=0 -DLAIUE_VERSION_MINOR=7 -DLAIUE_VERSION_PATCH=0 -DLAIUE_LINUX_LIBC_GNU=1'
for source in \
    src/platform/system_posix.c \
    src/world/*.c \
    src/physics/*.c \
    src/content/*.c \
    src/mod/*.c
do
    echo "checking ${source}"
    # shellcheck disable=SC2086
    gcc ${common_flags} -I./src -fsyntax-only "${source}"
done
