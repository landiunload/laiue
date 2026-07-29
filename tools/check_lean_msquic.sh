#!/bin/sh
set -eu

msquic_version=2.5.9
msquic_commit=87b53085d76bd7920d490a6f226c9999b6614d14
quictls_commit=ff36838bb69801cad56823159a036977bcbe5c75

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <libmsquic.so.2.5.9> [BUILD-METADATA]" >&2
    exit 2
fi

library=$1
metadata=${2:-}
readelf_program=${LAIUE_READELF:-readelf}
maximum_bytes=${LAIUE_MSQUIC_MAX_BYTES:-4194304}

export LC_ALL=C

if [ ! -f "${library}" ] || [ -L "${library}" ]; then
    echo "MsQuic runtime must be a regular non-symlink file: ${library}" >&2
    exit 1
fi
if [ "$(basename "${library}")" != \
    "libmsquic.so.${msquic_version}" ]
then
    echo "unexpected MsQuic runtime filename: $(basename "${library}")" >&2
    exit 1
fi
if ! command -v "${readelf_program}" >/dev/null 2>&1; then
    echo "readelf is required to audit ${library}" >&2
    exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sha256sum is required to audit ${library}" >&2
    exit 1
fi
case "${maximum_bytes}" in
    *[!0-9]* | '')
        echo "LAIUE_MSQUIC_MAX_BYTES must be an unsigned integer" >&2
        exit 2
        ;;
esac

library_size=$(wc -c <"${library}" | sed 's/[[:space:]]//g')
if [ "${library_size}" -gt "${maximum_bytes}" ]; then
    echo "lean MsQuic is ${library_size} bytes; budget is " \
        "${maximum_bytes}" >&2
    exit 1
fi

elf_header=$("${readelf_program}" --file-header "${library}")
if ! printf '%s\n' "${elf_header}" |
    grep -Eq 'Class:[[:space:]]+ELF64'
then
    echo "MsQuic runtime is not ELF64" >&2
    "${readelf_program}" --file-header "${library}" >&2
    exit 1
fi
if ! printf '%s\n' "${elf_header}" |
    grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64'
then
    echo "MsQuic runtime is not x86_64" >&2
    "${readelf_program}" --file-header "${library}" >&2
    exit 1
fi

dynamic=$("${readelf_program}" --dynamic "${library}")
soname=$(printf '%s\n' "${dynamic}" |
    sed -n 's/.*(SONAME).*Library soname: \[\([^]]*\)\].*/\1/p')
if [ "${soname}" != libmsquic.so.2 ]; then
    echo "MsQuic runtime has an unexpected or missing SONAME" >&2
    "${readelf_program}" --dynamic "${library}" >&2
    exit 1
fi
if printf '%s\n' "${dynamic}" | grep -Eq '\((RPATH|RUNPATH)\)'; then
    echo "MsQuic runtime must not contain RPATH or RUNPATH" >&2
    "${readelf_program}" --dynamic "${library}" >&2
    exit 1
fi
if ! printf '%s\n' "${dynamic}" |
    grep -Eq '\(FLAGS(_1)?\).*(BIND_NOW|NOW)'
then
    echo "MsQuic runtime must use immediate symbol binding" >&2
    "${readelf_program}" --dynamic "${library}" >&2
    exit 1
fi

needed=$(printf '%s\n' "${dynamic}" |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
if ! printf '%s\n' "${needed}" | grep -Fxq libcrypto.so.3; then
    echo "lean MsQuic must use the platform OpenSSL 3 libcrypto" >&2
    printf 'NEEDED:\n%s\n' "${needed}" >&2
    exit 1
fi

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
    echo "lean MsQuic must target exactly one supported libc ABI" >&2
    printf 'NEEDED:\n%s\n' "${needed}" >&2
    exit 1
fi

for dependency in ${needed}; do
    case "${dependency}" in
        libcrypto.so.3 | \
        libc.so.6 | \
        libc.musl-x86_64.so.1 | \
        libatomic.so.1 | \
        libdl.so.2 | \
        libgcc_s.so.1 | \
        libm.so.6 | \
        libpthread.so.0 | \
        librt.so.1)
            ;;
        *)
            echo "unexpected MsQuic runtime dependency: ${dependency}" >&2
            printf 'NEEDED:\n%s\n' "${needed}" >&2
            exit 1
            ;;
    esac
done

program_headers=$("${readelf_program}" --program-headers --wide \
    "${library}")
if ! printf '%s\n' "${program_headers}" | grep -Fq GNU_RELRO; then
    echo "MsQuic runtime has no GNU_RELRO segment" >&2
    "${readelf_program}" --program-headers --wide "${library}" >&2
    exit 1
fi
gnu_stack=$(printf '%s\n' "${program_headers}" |
    grep 'GNU_STACK' || true)
if [ -z "${gnu_stack}" ] ||
   printf '%s\n' "${gnu_stack}" | grep -Eq '[[:space:]]RWE[[:space:]]'
then
    echo "MsQuic runtime has a missing or executable GNU_STACK" >&2
    "${readelf_program}" --program-headers --wide "${library}" >&2
    exit 1
fi

sections=$("${readelf_program}" --sections --wide "${library}")
if printf '%s\n' "${sections}" |
    grep -Eq '[[:space:]]\.(debug_|zdebug_|symtab([[:space:]]|$))'
then
    echo "MsQuic runtime still contains debug or full symbol tables" >&2
    "${readelf_program}" --sections --wide "${library}" >&2
    exit 1
fi

dynamic_symbols=$("${readelf_program}" --dyn-syms --wide "${library}")
for symbol in MsQuicOpenVersion MsQuicClose; do
    if ! printf '%s\n' "${dynamic_symbols}" |
        grep -Eq "[[:space:]]${symbol}(@.*)?$"
    then
        echo "MsQuic runtime does not export ${symbol}" >&2
        "${readelf_program}" --dyn-syms --wide "${library}" >&2
        exit 1
    fi
done

metadata_value()
{
    metadata_file=$1
    metadata_key=$2
    metadata_values=$(sed -n \
        "s/^${metadata_key}=//p" "${metadata_file}")
    metadata_count=$(printf '%s\n' "${metadata_values}" | wc -l)
    if [ -z "${metadata_values}" ] || [ "${metadata_count}" -ne 1 ]; then
        echo "BUILD-METADATA requires one ${metadata_key}= value" >&2
        exit 1
    fi
    printf '%s' "${metadata_values}"
}

assert_metadata()
{
    metadata_key=$1
    expected_value=$2
    actual_value=$(metadata_value "${metadata}" "${metadata_key}")
    if [ "${actual_value}" != "${expected_value}" ]; then
        echo "BUILD-METADATA ${metadata_key}=${actual_value}; " \
            "expected ${expected_value}" >&2
        exit 1
    fi
}

assert_metadata_file_sha256()
{
    metadata_key=$1
    metadata_path=$2
    if [ ! -f "${metadata_path}" ] || [ -L "${metadata_path}" ]; then
        echo "missing regular provenance file: ${metadata_path}" >&2
        exit 1
    fi
    expected_file_sha256=$(metadata_value \
        "${metadata}" "${metadata_key}")
    actual_file_sha256=$(sha256sum "${metadata_path}" |
        sed 's/[[:space:]].*$//')
    if [ "${actual_file_sha256}" != "${expected_file_sha256}" ]; then
        echo "${metadata_key} does not match BUILD-METADATA" >&2
        exit 1
    fi
}

if [ -n "${metadata}" ]; then
    if [ ! -f "${metadata}" ] || [ -L "${metadata}" ]; then
        echo "BUILD-METADATA must be a regular non-symlink file" >&2
        exit 1
    fi
    metadata_size=$(wc -c <"${metadata}" | sed 's/[[:space:]]//g')
    if [ "${metadata_size}" -gt 32768 ]; then
        echo "BUILD-METADATA exceeds the 32 KiB limit" >&2
        exit 1
    fi
    assert_metadata format laiue-msquic-build-metadata-v1
    assert_metadata profile laiue-lean
    assert_metadata version "${msquic_version}"
    assert_metadata source_commit "${msquic_commit}"
    assert_metadata quictls_commit "${quictls_commit}"
    assert_metadata architecture x86_64
    assert_metadata libc "${libc_abi}"
    assert_metadata tls quictls
    assert_metadata system_libcrypto ON
    assert_metadata xdp OFF
    assert_metadata logging OFF
    assert_metadata tools OFF
    assert_metadata tests OFF
    assert_metadata perf OFF
    assert_metadata embedded_git_hash OFF
    assert_metadata build_type Release
    assert_metadata strip strip-unneeded
    assert_metadata runtime_file "libmsquic.so.${msquic_version}"
    assert_metadata runtime_size "${library_size}"

    source_date_epoch=$(metadata_value "${metadata}" source_date_epoch)
    case "${source_date_epoch}" in
        *[!0-9]* | '')
            echo "BUILD-METADATA source_date_epoch must be numeric" >&2
            exit 1
            ;;
    esac
    expected_sha256=$(metadata_value "${metadata}" runtime_sha256)
    actual_sha256=$(sha256sum "${library}" |
        sed 's/[[:space:]].*$//')
    if [ "${actual_sha256}" != "${expected_sha256}" ]; then
        echo "MsQuic runtime SHA-256 does not match BUILD-METADATA" >&2
        exit 1
    fi

    metadata_directory=$(CDPATH= cd "$(dirname "${metadata}")" &&
        pwd -P)
    assert_metadata_file_sha256 license_sha256 \
        "${metadata_directory}/LICENSE"
    assert_metadata_file_sha256 third_party_notices_sha256 \
        "${metadata_directory}/THIRD-PARTY-NOTICES"
    assert_metadata_file_sha256 quictls_license_sha256 \
        "${metadata_directory}/QUIC-TLS-LICENSE"
fi

printf 'lean MsQuic: %s, %s bytes\n' "${libc_abi}" "${library_size}"
printf 'dependencies:\n%s\n' "${needed}"
