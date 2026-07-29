#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 <bundle> <output.tar.gz> [strip-program] [config]" >&2
    exit 2
fi

bundle_directory=$1
output_archive=$2
strip_program=${3:-}
build_configuration=${4:-Release}
script_directory=$(CDPATH= cd "$(dirname "$0")" && pwd)
msquic_version=2.5.9
tar_program=${LAIUE_TAR:-tar}
gzip_program=${LAIUE_GZIP:-gzip}

test -d "${bundle_directory}"
test -x "${bundle_directory}/laiue_server"
case "${build_configuration}" in
    Debug | Release)
        ;;
    *)
        echo "config must be Debug or Release: ${build_configuration}" >&2
        exit 2
        ;;
esac
for program in "${tar_program}" "${gzip_program}" readlink sha256sum; do
    if ! command -v "${program}" >/dev/null 2>&1; then
        echo "required packaging program not found: ${program}" >&2
        exit 1
    fi
done
if ! "${tar_program}" --version 2>&1 | grep -Fq 'GNU tar'; then
    echo "reproducible server archives require GNU tar" >&2
    echo "install the Alpine 'tar' package or set LAIUE_TAR" >&2
    exit 1
fi
if ! "${gzip_program}" --help 2>&1 |
    grep -Eq -- '(^|[[:space:],])-n([[:space:],]|$)'
then
    echo "reproducible server archives require gzip with -n support" >&2
    echo "install the Alpine 'gzip' package or set LAIUE_GZIP" >&2
    exit 1
fi

temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/laiue-server-package.XXXXXX")
trap 'rm -rf "${temporary_root}"' EXIT HUP INT TERM

package_root="${temporary_root}/root"
mkdir -m 0755 "${package_root}"
cp -a "${bundle_directory}/." "${package_root}/"

# Build trees placed on drvfs or other permission-less filesystems can expose
# every staged file as mode 0777. Normalize in a native temporary directory so
# the archive is safe and reproducible regardless of the build-tree mount.
find "${package_root}" -type d -exec chmod 0755 {} +
find "${package_root}" -type f -exec chmod 0644 {} +
find "${package_root}" -type f \
    \( -name 'laiue_server' -o -name '*.so' -o -name '*.so.*' \) \
    -exec chmod 0755 {} +

metadata="${package_root}/licenses/msquic/BUILD-METADATA"
if [ "${build_configuration}" = Release ]; then
    if [ -z "${strip_program}" ] ||
       ! command -v "${strip_program}" >/dev/null 2>&1
    then
        echo "Release packaging requires a valid strip program" >&2
        exit 1
    fi
    if [ -e "${metadata}" ]; then
        # A lean prefix records the SHA-256 of its already stripped runtime.
        # Never rely on a second strip invocation being byte-idempotent.
        find "${package_root}" -type f \
            \( -name 'laiue_server' -o -name '*.so' -o \
               -name '*.so.*' \) \
            ! -name "libmsquic.so.${msquic_version}" \
            -exec "${strip_program}" --strip-unneeded {} +
    else
        # Generic/system prefixes may provide an unstripped runtime.
        find "${package_root}" -type f \
            \( -name 'laiue_server' -o -name '*.so' -o \
               -name '*.so.*' \) \
            -exec "${strip_program}" --strip-unneeded {} +
    fi
fi

msquic_runtime=$(find "${package_root}" -maxdepth 1 -type f \
    -name "libmsquic.so.${msquic_version}" -print | LC_ALL=C sort)
if [ -z "${msquic_runtime}" ] ||
   [ "$(printf '%s\n' "${msquic_runtime}" | wc -l)" -ne 1 ]
then
    echo "bundle must contain one libmsquic.so.${msquic_version}" >&2
    printf '%s\n' "${msquic_runtime}" >&2
    exit 1
fi
if [ ! -L "${package_root}/libmsquic.so" ] ||
   [ "$(readlink "${package_root}/libmsquic.so")" != libmsquic.so.2 ] ||
   [ ! -L "${package_root}/libmsquic.so.2" ] ||
   [ "$(readlink "${package_root}/libmsquic.so.2")" != \
        "libmsquic.so.${msquic_version}" ]
then
    echo "bundle has an invalid libmsquic SONAME symlink chain" >&2
    exit 1
fi
unexpected_symlinks=$(find "${package_root}" -type l \
    ! -path "${package_root}/libmsquic.so" \
    ! -path "${package_root}/libmsquic.so.2" -print)
if [ -n "${unexpected_symlinks}" ]; then
    echo "bundle contains unexpected symlinks:" >&2
    printf '%s\n' "${unexpected_symlinks}" >&2
    exit 1
fi

version_file="${package_root}/licenses/msquic/VERSION"
for notice in "${package_root}/LICENSE" \
    "${package_root}/licenses/msquic/LICENSE" \
    "${package_root}/licenses/msquic/THIRD-PARTY-NOTICES"
do
    if [ ! -s "${notice}" ] || [ ! -f "${notice}" ] ||
       [ -L "${notice}" ]
    then
        echo "bundle is missing a regular notice file: ${notice}" >&2
        exit 1
    fi
done
if [ -e "${metadata}" ]; then
    if [ ! -f "${metadata}" ] || [ -L "${metadata}" ]; then
        echo "invalid MsQuic BUILD-METADATA file" >&2
        exit 1
    fi
    for lean_notice in "${version_file}" \
        "${package_root}/licenses/msquic/QUIC-TLS-LICENSE"
    do
        if [ ! -s "${lean_notice}" ] || [ -L "${lean_notice}" ]; then
            echo "lean MsQuic bundle is missing: ${lean_notice}" >&2
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
        echo "bundled MsQuic VERSION is not a regular " \
            "${msquic_version} file" >&2
        exit 1
    fi
fi

archive_epoch=${SOURCE_DATE_EPOCH:-}
if [ -z "${archive_epoch}" ] && [ -f "${metadata}" ]; then
    archive_epoch=$(sed -n 's/^source_date_epoch=//p' "${metadata}")
fi
archive_epoch=${archive_epoch:-0}
case "${archive_epoch}" in
    *[!0-9]* | '')
        echo "SOURCE_DATE_EPOCH must be an unsigned integer" >&2
        exit 2
        ;;
esac

mkdir -p "$(dirname "${output_archive}")"
temporary_archive="${temporary_root}/server.tar"
LC_ALL=C "${tar_program}" \
    --sort=name \
    --format=gnu \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --mtime="@${archive_epoch}" \
    -C "${package_root}" \
    -cf "${temporary_archive}" .
"${gzip_program}" -9 -n <"${temporary_archive}" \
    >"${temporary_archive}.gz"
mv "${temporary_archive}.gz" "${output_archive}"

(
    cd "$(dirname "${output_archive}")"
    archive_name=$(basename "${output_archive}")
    sha256sum "${archive_name}" >"${archive_name}.sha256"
)
