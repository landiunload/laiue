#include "network/certificate_validation.h"
#include "platform/system.h"

#include <limits.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ws2tcpip.h>
#else
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#if defined(_WIN32)
static uint8_t AsciiDnsFold(uint8_t value)
{
    return value >= (uint8_t)'A' && value <= (uint8_t)'Z'
               ? (uint8_t)(value + ((uint8_t)'a' - (uint8_t)'A'))
               : value;
}

static bool DnsSanMatches(
    const wchar_t *san, const char *host)
{
    if (san == NULL || host == NULL)
    {
        return false;
    }
    uint32_t index = 0;
    while (san[index] != L'\0' && host[index] != '\0')
    {
        if ((uint32_t)san[index] > 0x7fU ||
            (uint8_t)host[index] > 0x7fU ||
            AsciiDnsFold((uint8_t)san[index]) !=
                AsciiDnsFold((uint8_t)host[index]))
        {
            return false;
        }
        ++index;
    }
    return san[index] == L'\0' && host[index] == '\0';
}

static bool IpSanMatches(
    const CRYPT_DATA_BLOB *san,
    const NetworkEndpoint *endpoint)
{
    uint8_t expected[16];
    int32_t family =
        endpoint->kind == NETWORK_ENDPOINT_IPV4 ? AF_INET : AF_INET6;
    uint32_t expectedSize =
        endpoint->kind == NETWORK_ENDPOINT_IPV4 ? 4U : 16U;
    return san != NULL && san->pbData != NULL &&
           san->cbData == expectedSize &&
           InetPtonA(family, endpoint->host, expected) == 1 &&
           memcmp(san->pbData, expected, expectedSize) == 0;
}

bool NetworkCertificateValidateLeafIdentity(
    const NetworkEndpoint *endpoint,
    const uint8_t *der, uint32_t derSize)
{
    if (endpoint == NULL || der == NULL || derSize == 0)
    {
        return false;
    }
    PCCERT_CONTEXT context = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der, derSize);
    if (context == NULL)
    {
        return false;
    }
    if (context->cbCertEncoded != derSize ||
        memcmp(context->pbCertEncoded, der, derSize) != 0 ||
        CertVerifyTimeValidity(NULL, context->pCertInfo) != 0)
    {
        CertFreeCertificateContext(context);
        return false;
    }

    PCERT_EXTENSION extension = CertFindExtension(
        szOID_SUBJECT_ALT_NAME2,
        context->pCertInfo->cExtension,
        context->pCertInfo->rgExtension);
    DWORD decodedSize = 0;
    bool valid = extension != NULL &&
        CryptDecodeObjectEx(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            X509_ALTERNATE_NAME,
            extension->Value.pbData, extension->Value.cbData,
            0, NULL, NULL, &decodedSize) &&
        decodedSize >= sizeof(CERT_ALT_NAME_INFO);
    CERT_ALT_NAME_INFO *names =
        valid ? PlatformAllocate(decodedSize, false) : NULL;
    if (valid && names == NULL)
    {
        valid = false;
    }
    if (valid)
    {
        valid = CryptDecodeObjectEx(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            X509_ALTERNATE_NAME,
            extension->Value.pbData, extension->Value.cbData,
            0, NULL, names, &decodedSize);
    }

    bool matched = false;
    for (DWORD index = 0;
         valid && index < names->cAltEntry && !matched; ++index)
    {
        const CERT_ALT_NAME_ENTRY *entry =
            &names->rgAltEntry[index];
        if (endpoint->kind == NETWORK_ENDPOINT_DNS &&
            entry->dwAltNameChoice == CERT_ALT_NAME_DNS_NAME)
        {
            matched = DnsSanMatches(
                entry->pwszDNSName, endpoint->host);
        }
        else if (endpoint->kind != NETWORK_ENDPOINT_DNS &&
                 entry->dwAltNameChoice ==
                     CERT_ALT_NAME_IP_ADDRESS)
        {
            matched = IpSanMatches(
                &entry->IPAddress, endpoint);
        }
    }

    PlatformFree(names);
    CertFreeCertificateContext(context);
    return valid && matched;
}
#else
bool NetworkCertificateValidateLeafIdentity(
    const NetworkEndpoint *endpoint,
    const uint8_t *der, uint32_t derSize)
{
    if (endpoint == NULL || der == NULL || derSize == 0 ||
        derSize > (uint32_t)LONG_MAX)
    {
        return false;
    }
    const unsigned char *cursor = der;
    const unsigned char *end = cursor + derSize;
    X509 *certificate =
        d2i_X509(NULL, &cursor, (long)derSize);
    if (certificate == NULL || cursor != end)
    {
        X509_free(certificate);
        return false;
    }
    bool valid =
        X509_cmp_current_time(
            X509_get0_notBefore(certificate)) == -1 &&
        X509_cmp_current_time(
            X509_get0_notAfter(certificate)) == 1;
    if (valid &&
        (endpoint->kind == NETWORK_ENDPOINT_IPV4 ||
         endpoint->kind == NETWORK_ENDPOINT_IPV6))
    {
        valid = X509_check_ip_asc(
                    certificate, endpoint->host, 0U) == 1;
    }
    else if (valid && endpoint->kind == NETWORK_ENDPOINT_DNS)
    {
        valid = X509_check_host(
                    certificate, endpoint->host, 0U,
                    X509_CHECK_FLAG_NEVER_CHECK_SUBJECT |
                        X509_CHECK_FLAG_NO_WILDCARDS,
                    NULL) == 1;
    }
    else
    {
        valid = false;
    }
    X509_free(certificate);
    return valid;
}
#endif
