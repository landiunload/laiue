#!/bin/sh
set -eu

umask 022

msquic_version=2.5.9
msquic_commit=87b53085d76bd7920d490a6f226c9999b6614d14
quictls_commit=ff36838bb69801cad56823159a036977bcbe5c75

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <msquic-source> <empty-build-dir> <empty-prefix>" >&2
    echo "pinned MsQuic: ${msquic_version} (${msquic_commit})" >&2
    exit 2
fi

source_directory=$1
build_directory=$2
install_prefix=$3
script_directory=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
strip_program=${LAIUE_MSQUIC_STRIP:-strip}

for program in cmake git install ninja openssl readelf sed sha256sum
do
    if ! command -v "${program}" >/dev/null 2>&1; then
        echo "${program} is required to build lean MsQuic" >&2
        exit 1
    fi
done
if ! command -v "${strip_program}" >/dev/null 2>&1; then
    echo "strip program not found: ${strip_program}" >&2
    exit 1
fi

if [ ! -d "${source_directory}" ]; then
    echo "missing MsQuic source directory: ${source_directory}" >&2
    exit 1
fi
for output_directory in "${build_directory}" "${install_prefix}"; do
    output_parent=$(dirname "${output_directory}")
    if [ ! -d "${output_parent}" ]; then
        echo "output parent must already exist: ${output_parent}" >&2
        exit 1
    fi
    if [ -L "${output_directory}" ]; then
        echo "output directory must not be a symlink: ${output_directory}" >&2
        exit 1
    fi
    if [ -e "${output_directory}" ] && [ ! -d "${output_directory}" ]; then
        echo "output path is not a directory: ${output_directory}" >&2
        exit 1
    fi
    if [ -d "${output_directory}" ] &&
       [ -n "$(find "${output_directory}" -mindepth 1 -print -quit)" ]
    then
        echo "output directory must be empty: ${output_directory}" >&2
        exit 1
    fi
done

source_directory=$(CDPATH= cd "${source_directory}" && pwd -P)
build_parent=$(CDPATH= cd "$(dirname "${build_directory}")" && pwd -P)
build_name=$(basename "${build_directory}")
prefix_parent=$(CDPATH= cd "$(dirname "${install_prefix}")" && pwd -P)
prefix_name=$(basename "${install_prefix}")
case "${build_name}" in
    '' | . | ..)
        echo "invalid MsQuic build directory: ${build_directory}" >&2
        exit 1
        ;;
esac
case "${prefix_name}" in
    '' | . | ..)
        echo "invalid MsQuic install prefix: ${install_prefix}" >&2
        exit 1
        ;;
esac
build_directory="${build_parent}/${build_name}"
install_prefix="${prefix_parent}/${prefix_name}"

case "${source_directory}:${build_directory}" in
    *[[:space:]]* | *=*)
        echo "source/build paths must not contain whitespace or '='" >&2
        echo "deterministic compiler prefix maps require unambiguous paths" >&2
        exit 1
        ;;
esac

case "${build_directory}/" in
    "${source_directory}/"*)
        echo "MsQuic build directory must be outside the source tree" >&2
        exit 1
        ;;
esac
case "${install_prefix}/" in
    "${source_directory}/"*)
        echo "MsQuic install prefix must be outside the source tree" >&2
        exit 1
        ;;
esac
case "${install_prefix}/" in
    "${build_directory}/"*)
        echo "MsQuic install prefix must be outside the build tree" >&2
        exit 1
        ;;
esac
case "${build_directory}/" in
    "${install_prefix}/"*)
        echo "MsQuic build directory must be outside the install prefix" >&2
        exit 1
        ;;
esac

if [ "$(git -C "${source_directory}" rev-parse --is-inside-work-tree \
    2>/dev/null)" != true ]
then
    echo "MsQuic source must be a git checkout: ${source_directory}" >&2
    exit 1
fi
actual_commit=$(git -C "${source_directory}" rev-parse HEAD)
if [ "${actual_commit}" != "${msquic_commit}" ]; then
    echo "MsQuic commit mismatch: ${actual_commit}" >&2
    echo "expected pinned commit: ${msquic_commit}" >&2
    exit 1
fi
if [ -n "$(git -C "${source_directory}" status \
    --ignore-submodules=all --porcelain=v1 --untracked-files=all)" ]
then
    echo "MsQuic checkout must be clean" >&2
    git -C "${source_directory}" status \
        --ignore-submodules=all --short >&2
    exit 1
fi

actual_version=$(sed -n \
    's/^[[:space:]]*set([[:space:]]*QUIC_FULL_VERSION[[:space:]]*\([0-9][0-9.]*\)[[:space:]]*).*/\1/p' \
    "${source_directory}/CMakeLists.txt")
if [ "${actual_version}" != "${msquic_version}" ]; then
    echo "MsQuic version mismatch: ${actual_version}" >&2
    echo "expected pinned version: ${msquic_version}" >&2
    exit 1
fi

expected_quictls_commit=$(git -C "${source_directory}" ls-tree HEAD \
    -- submodules/quictls | sed -n \
    's/^[0-9][0-9]* commit \([0-9a-f][0-9a-f]*\).*$/\1/p')
if [ "${expected_quictls_commit}" != "${quictls_commit}" ]; then
    echo "pinned MsQuic tree has unexpected quictls commit: " \
        "${expected_quictls_commit}" >&2
    exit 1
fi
if [ ! -s "${source_directory}/submodules/quictls/Configure" ]; then
    echo "initialize the pinned quictls submodule before building" >&2
    exit 1
fi
actual_quictls_commit=$(git -C \
    "${source_directory}/submodules/quictls" rev-parse HEAD 2>/dev/null ||
    true)
if [ "${actual_quictls_commit}" != "${quictls_commit}" ]; then
    echo "quictls commit mismatch: ${actual_quictls_commit}" >&2
    echo "expected pinned commit: ${quictls_commit}" >&2
    exit 1
fi
if [ -n "$(git -C "${source_directory}/submodules/quictls" status \
    --porcelain=v1 --untracked-files=all)" ]
then
    echo "quictls checkout must be clean" >&2
    git -C "${source_directory}/submodules/quictls" status --short >&2
    exit 1
fi

if [ ! -s "${source_directory}/src/inc/msquic.h" ] ||
   [ ! -s "${source_directory}/LICENSE" ] ||
   [ ! -s "${source_directory}/THIRD-PARTY-NOTICES" ]
then
    echo "MsQuic checkout is incomplete" >&2
    exit 1
fi
if [ -s "${source_directory}/submodules/quictls/LICENSE.txt" ]; then
    quictls_license="${source_directory}/submodules/quictls/LICENSE.txt"
elif [ -s "${source_directory}/submodules/quictls/LICENSE" ]; then
    quictls_license="${source_directory}/submodules/quictls/LICENSE"
else
    echo "pinned quictls checkout has no license file" >&2
    exit 1
fi

source_date_epoch=$(git -C "${source_directory}" show -s \
    --format=%ct HEAD)
case "${source_date_epoch}" in
    *[!0-9]* | '')
        echo "invalid MsQuic source commit timestamp" >&2
        exit 1
        ;;
esac
export SOURCE_DATE_EPOCH="${source_date_epoch}"
export LC_ALL=C
export TZ=UTC

mkdir -p "${build_directory}"

reproducible_flags="-ffile-prefix-map=${source_directory}=/usr/src/msquic"
reproducible_flags="${reproducible_flags} -ffile-prefix-map=${build_directory}=/usr/src/msquic-build"
reproducible_flags="${reproducible_flags} -fdebug-prefix-map=${source_directory}=/usr/src/msquic"
reproducible_flags="${reproducible_flags} -fdebug-prefix-map=${build_directory}=/usr/src/msquic-build"
reproducible_flags="${reproducible_flags} -ffunction-sections -fdata-sections"
reproducible_flags="${reproducible_flags} -fstack-protector-strong"
linker_flags="-Wl,--as-needed,--gc-sections,-z,relro,-z,now"
linker_flags="${linker_flags},-z,noexecstack,--build-id=sha1"

# Source acquisition is deliberately outside CMake. FULLY_DISCONNECTED turns
# every missing pinned input into a configure failure instead of a download.
cmake -S "${source_directory}" -B "${build_directory}" -G Ninja \
    -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
    "-DCMAKE_C_FLAGS=${reproducible_flags}" \
    "-DCMAKE_CXX_FLAGS=${reproducible_flags}" \
    "-DCMAKE_SHARED_LINKER_FLAGS=${linker_flags}" \
    -DCMAKE_SKIP_RPATH=ON \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
    -DQUIC_BUILD_PERF=OFF \
    -DQUIC_BUILD_SHARED=ON \
    -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_TOOLS=OFF \
    -DQUIC_CI=OFF \
    -DQUIC_CODE_CHECK=OFF \
    -DQUIC_EMBED_GIT_HASH=OFF \
    -DQUIC_ENABLE_LOGGING=OFF \
    -DQUIC_ENABLE_POOL_ALLOC=ON \
    -DQUIC_ENABLE_SANITIZERS=OFF \
    -DQUIC_HIGH_RES_TIMERS=OFF \
    -DQUIC_LINUX_XDP_ENABLED=OFF \
    -DQUIC_OFFICIAL_RELEASE=OFF \
    -DQUIC_OPTIMIZE_LOCAL=OFF \
    -DQUIC_PGO=OFF \
    -DQUIC_SOURCE_LINK=OFF \
    -DQUIC_TELEMETRY_ASSERTS=OFF \
    -DQUIC_TLS_LIB=quictls \
    -DQUIC_USE_SYSTEM_LIBCRYPTO=ON
cmake --build "${build_directory}" --target msquic --parallel

if [ -n "$(git -C "${source_directory}" status \
    --ignore-submodules=all --porcelain=v1 --untracked-files=all)" ]
then
    echo "MsQuic build modified its source checkout" >&2
    git -C "${source_directory}" status \
        --ignore-submodules=all --short >&2
    exit 1
fi
if [ -n "$(git -C "${source_directory}/submodules/quictls" status \
    --porcelain=v1 --untracked-files=all)" ]
then
    echo "MsQuic build modified its quictls source checkout" >&2
    git -C "${source_directory}/submodules/quictls" status --short >&2
    exit 1
fi

runtime_candidates=$(find "${build_directory}" -type f \
    -name "libmsquic.so.${msquic_version}" -print | LC_ALL=C sort)
if [ -z "${runtime_candidates}" ] ||
   [ "$(printf '%s\n' "${runtime_candidates}" | wc -l)" -ne 1 ]
then
    echo "build must produce exactly one libmsquic.so.${msquic_version}" >&2
    printf '%s\n' "${runtime_candidates}" >&2
    exit 1
fi
runtime_source=${runtime_candidates}

staging_prefix=$(mktemp -d \
    "${prefix_parent}/.laiue-msquic-install.XXXXXX")
cleanup()
{
    if [ -n "${staging_prefix:-}" ] && [ -d "${staging_prefix}" ]; then
        rm -rf "${staging_prefix}"
    fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "${staging_prefix}/include" "${staging_prefix}/lib"
install -m 0755 "${runtime_source}" \
    "${staging_prefix}/lib/libmsquic.so.${msquic_version}"
"${strip_program}" --strip-unneeded \
    "${staging_prefix}/lib/libmsquic.so.${msquic_version}"
(
    cd "${staging_prefix}/lib"
    ln -s "libmsquic.so.${msquic_version}" libmsquic.so.2
    ln -s libmsquic.so.2 libmsquic.so
)

for header in \
    "${source_directory}"/src/inc/msquic*.h \
    "${source_directory}/src/inc/quic_sal_stub.h"
do
    install -m 0644 "${header}" "${staging_prefix}/include/"
done
install -m 0644 "${source_directory}/LICENSE" \
    "${staging_prefix}/LICENSE"
install -m 0644 "${source_directory}/THIRD-PARTY-NOTICES" \
    "${staging_prefix}/THIRD-PARTY-NOTICES"
install -m 0644 "${quictls_license}" \
    "${staging_prefix}/QUIC-TLS-LICENSE"
printf '%s\n' "${msquic_version}" >"${staging_prefix}/VERSION"

runtime="${staging_prefix}/lib/libmsquic.so.${msquic_version}"
dynamic=$(readelf --dynamic "${runtime}")
needed=$(printf '%s\n' "${dynamic}" |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
has_glibc=0
has_musl=0
if printf '%s\n' "${needed}" | grep -Fxq libc.so.6; then
    has_glibc=1
fi
if printf '%s\n' "${needed}" |
    grep -Fxq libc.musl-x86_64.so.1
then
    has_musl=1
fi
if [ "${has_glibc}" -eq 1 ] && [ "${has_musl}" -eq 0 ]; then
    libc_abi=gnu
elif [ "${has_glibc}" -eq 0 ] && [ "${has_musl}" -eq 1 ]; then
    libc_abi=musl
else
    echo "cannot identify one supported libc ABI from MsQuic NEEDED" >&2
    printf '%s\n' "${needed}" >&2
    exit 1
fi
if [ -n "${LAIUE_MSQUIC_LIBC:-}" ] &&
   [ "${LAIUE_MSQUIC_LIBC}" != "${libc_abi}" ]
then
    echo "MsQuic libc mismatch: ${libc_abi}, expected " \
        "${LAIUE_MSQUIC_LIBC}" >&2
    exit 1
fi

runtime_sha256=$(sha256sum "${runtime}" |
    sed 's/[[:space:]].*$//')
runtime_size=$(wc -c <"${runtime}" | sed 's/[[:space:]]//g')
license_sha256=$(sha256sum "${staging_prefix}/LICENSE" |
    sed 's/[[:space:]].*$//')
notice_sha256=$(sha256sum "${staging_prefix}/THIRD-PARTY-NOTICES" |
    sed 's/[[:space:]].*$//')
quictls_license_sha256=$(sha256sum \
    "${staging_prefix}/QUIC-TLS-LICENSE" |
    sed 's/[[:space:]].*$//')
cmake_version=$(cmake --version | sed -n \
    '1s/^cmake version /cmake-/p')
ninja_version=$(ninja --version | sed -n '1p')
openssl_version=$(openssl version | sed -n '1p')
c_compiler=$(sed -n \
    's/^CMAKE_C_COMPILER:[^=]*=//p' \
    "${build_directory}/CMakeCache.txt")
if [ -z "${c_compiler}" ] ||
   ! command -v "${c_compiler}" >/dev/null 2>&1
then
    echo "CMake cache has no executable C compiler: ${c_compiler}" >&2
    exit 1
fi
compiler_version=$("${c_compiler}" --version | sed -n '1p')
strip_version=$("${strip_program}" --version 2>&1 | sed -n '1p')

cat >"${staging_prefix}/BUILD-METADATA" <<EOF
format=laiue-msquic-build-metadata-v1
profile=laiue-lean
version=${msquic_version}
source_url=https://github.com/microsoft/msquic.git
source_commit=${msquic_commit}
quictls_commit=${quictls_commit}
source_date_epoch=${source_date_epoch}
architecture=x86_64
libc=${libc_abi}
tls=quictls
system_libcrypto=ON
xdp=OFF
logging=OFF
tools=OFF
tests=OFF
perf=OFF
embedded_git_hash=OFF
build_type=Release
strip=strip-unneeded
runtime_file=libmsquic.so.${msquic_version}
runtime_size=${runtime_size}
runtime_sha256=${runtime_sha256}
license_sha256=${license_sha256}
third_party_notices_sha256=${notice_sha256}
quictls_license_sha256=${quictls_license_sha256}
cmake=${cmake_version}
ninja=${ninja_version}
c_compiler=${compiler_version}
openssl=${openssl_version}
strip_tool=${strip_version}
EOF

sh "${script_directory}/check_lean_msquic.sh" \
    "${runtime}" "${staging_prefix}/BUILD-METADATA"

if [ -d "${install_prefix}" ]; then
    rmdir "${install_prefix}"
fi
mv "${staging_prefix}" "${install_prefix}"
staging_prefix=
trap - EXIT HUP INT TERM

printf 'lean MsQuic %s (%s, %s): %s\n' \
    "${msquic_version}" "${libc_abi}" "${runtime_size} bytes" \
    "${install_prefix}"
