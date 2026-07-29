#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <server-bundle-directory> <output.tar.gz>" >&2
    exit 2
fi

bundle_directory=$1
output_archive=$2

test -d "${bundle_directory}"
test -x "${bundle_directory}/laiue_server"

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

mkdir -p "$(dirname "${output_archive}")"
temporary_archive="${temporary_root}/server.tar.gz"
tar -C "${package_root}" -czf "${temporary_archive}" .
mv "${temporary_archive}" "${output_archive}"

(
    cd "$(dirname "${output_archive}")"
    archive_name=$(basename "${output_archive}")
    sha256sum "${archive_name}" >"${archive_name}.sha256"
)
