#!/bin/sh
set -eu

libc_name=${LAIUE_LINUX_LIBC:-gnu}
case "$libc_name" in
    gnu|musl) ;;
    *)
        echo "LAIUE_LINUX_LIBC must be gnu or musl" >&2
        exit 2
        ;;
esac

compiler=${CC:-cc}
binary="auto_bridge.linux-x86_64-${libc_name}.so"
entry_key="entry_linux_x86_64_${libc_name}"

"$compiler" -std=c17 -Wall -Wextra -Werror -O2 -fPIC -fvisibility=hidden \
    -shared -I../.. auto_bridge.c -o "$binary"
sed "s|@LAIUE_MOD_NATIVE_ENTRIES@|${entry_key} = ${binary}|" \
    mod.lm.in > mod.lm

echo "Built ${binary} and mod.lm"
