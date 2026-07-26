cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS
        OPENSSL_EXECUTABLE TEST_EXECUTABLE RUNTIME_DIRECTORY)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing required variable: ${required_variable}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${RUNTIME_DIRECTORY}")

set(ca_configuration
    "${RUNTIME_DIRECTORY}/openssl-ca.cnf")
set(server_configuration
    "${RUNTIME_DIRECTORY}/openssl-server.cnf")
set(ca_certificate_file
    "${RUNTIME_DIRECTORY}/test-ca.pem")
set(ca_private_key_file
    "${RUNTIME_DIRECTORY}/test-ca-key.pem")
set(certificate_file
    "${RUNTIME_DIRECTORY}/server-certificate.pem")
set(private_key_file
    "${RUNTIME_DIRECTORY}/server-private-key.pem")
set(certificate_request
    "${RUNTIME_DIRECTORY}/server-certificate.csr")
set(certificate_der
    "${RUNTIME_DIRECTORY}/server-certificate.der")
set(pinned_certificate_file
    "${RUNTIME_DIRECTORY}/pinned-server-certificate.pem")
set(pinned_private_key_file
    "${RUNTIME_DIRECTORY}/pinned-server-private-key.pem")
set(pinned_certificate_der
    "${RUNTIME_DIRECTORY}/pinned-server-certificate.der")
set(ca_serial_file
    "${RUNTIME_DIRECTORY}/test-ca.srl")

file(REMOVE
    "${ca_certificate_file}"
    "${ca_private_key_file}"
    "${certificate_file}"
    "${private_key_file}"
    "${certificate_request}"
    "${certificate_der}"
    "${pinned_certificate_file}"
    "${pinned_private_key_file}"
    "${pinned_certificate_der}"
    "${ca_serial_file}")

file(WRITE "${ca_configuration}" [=[
[req]
distinguished_name = distinguished_name
x509_extensions = ca_certificate
prompt = no

[distinguished_name]
CN = Laiue QUIC integration test CA

[ca_certificate]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
]=])

file(WRITE "${server_configuration}" [=[
[req]
distinguished_name = distinguished_name
x509_extensions = server_certificate
prompt = no

[distinguished_name]
CN = laiue-quic-integration

[server_certificate]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = @subject_alternative_names

[subject_alternative_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
IP.2 = ::1
]=])

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" req
        -x509
        -newkey rsa:2048
        -sha256
        -nodes
        -days 1
        -config "${ca_configuration}"
        -keyout "${ca_private_key_file}"
        -out "${ca_certificate_file}"
    RESULT_VARIABLE ca_result
    OUTPUT_VARIABLE ca_output
    ERROR_VARIABLE ca_error)
if(NOT ca_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to generate the test CA:\n"
        "${ca_output}\n${ca_error}")
endif()

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" req
        -new
        -newkey rsa:2048
        -sha256
        -nodes
        -config "${server_configuration}"
        -keyout "${private_key_file}"
        -out "${certificate_request}"
    RESULT_VARIABLE request_result
    OUTPUT_VARIABLE request_output
    ERROR_VARIABLE request_error)
if(NOT request_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to generate the server request:\n"
        "${request_output}\n${request_error}")
endif()

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" x509
        -req
        -in "${certificate_request}"
        -CA "${ca_certificate_file}"
        -CAkey "${ca_private_key_file}"
        -CAcreateserial
        -days 1
        -sha256
        -extfile "${server_configuration}"
        -extensions server_certificate
        -out "${certificate_file}"
    RESULT_VARIABLE sign_result
    OUTPUT_VARIABLE sign_output
    ERROR_VARIABLE sign_error)
if(NOT sign_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to sign the server certificate:\n"
        "${sign_output}\n${sign_error}")
endif()

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" req
        -x509
        -newkey rsa:2048
        -sha256
        -nodes
        -days 1
        -config "${server_configuration}"
        -keyout "${pinned_private_key_file}"
        -out "${pinned_certificate_file}"
    RESULT_VARIABLE pinned_result
    OUTPUT_VARIABLE pinned_output
    ERROR_VARIABLE pinned_error)
if(NOT pinned_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to generate the pinned self-signed certificate:\n"
        "${pinned_output}\n${pinned_error}")
endif()

# NetworkServerCreate refuses symlinks and group/world-accessible keys.
file(CHMOD
    "${private_key_file}"
    "${pinned_private_key_file}"
    "${ca_private_key_file}"
    PERMISSIONS OWNER_READ OWNER_WRITE)

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" x509
        -in "${certificate_file}"
        -outform DER
        -out "${certificate_der}"
    RESULT_VARIABLE der_result
    OUTPUT_VARIABLE der_output
    ERROR_VARIABLE der_error)
if(NOT der_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to encode the test certificate as DER:\n"
        "${der_output}\n${der_error}")
endif()

file(SHA256 "${certificate_der}" certificate_sha256)

execute_process(
    COMMAND "${OPENSSL_EXECUTABLE}" x509
        -in "${pinned_certificate_file}"
        -outform DER
        -out "${pinned_certificate_der}"
    RESULT_VARIABLE pinned_der_result
    OUTPUT_VARIABLE pinned_der_output
    ERROR_VARIABLE pinned_der_error)
if(NOT pinned_der_result EQUAL 0)
    message(FATAL_ERROR
        "OpenSSL failed to encode the pinned certificate as DER:\n"
        "${pinned_der_output}\n${pinned_der_error}")
endif()

file(SHA256 "${pinned_certificate_der}" pinned_certificate_sha256)

function(run_quic_case
         mode trust_test_ca server_certificate server_key
         certificate_fingerprint out_failure)
    if(trust_test_ca)
        set(environment_arguments
            "SSL_CERT_FILE=${ca_certificate_file}"
            --unset=SSL_CERT_DIR)
    else()
        # Exact pinning must work as its own trust anchor, while the
        # untrusted-system case must fail. Do not inherit a developer or CI
        # trust-store override containing the test CA.
        set(environment_arguments
            --unset=SSL_CERT_FILE
            --unset=SSL_CERT_DIR)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            ${environment_arguments}
            --
            "${TEST_EXECUTABLE}"
            "${mode}"
            "${server_certificate}"
            "${server_key}"
            "${certificate_fingerprint}"
        RESULT_VARIABLE case_result
        OUTPUT_VARIABLE case_output
        ERROR_VARIABLE case_error)
    if(case_output)
        message("${case_output}")
    endif()
    if(NOT case_result EQUAL 0)
        set(${out_failure}
            "QUIC ${mode} integration case failed (${case_result}):\n${case_error}"
            PARENT_SCOPE)
    else()
        set(${out_failure} "" PARENT_SCOPE)
    endif()
endfunction()

run_quic_case(
    pin FALSE
    "${pinned_certificate_file}"
    "${pinned_private_key_file}"
    "${pinned_certificate_sha256}"
    pin_failure)
run_quic_case(
    system TRUE
    "${certificate_file}"
    "${private_key_file}"
    "${certificate_sha256}"
    system_failure)
run_quic_case(
    wrong-pin FALSE
    "${pinned_certificate_file}"
    "${pinned_private_key_file}"
    "${pinned_certificate_sha256}"
    wrong_pin_failure)
run_quic_case(
    untrusted-system FALSE
    "${certificate_file}"
    "${private_key_file}"
    "${certificate_sha256}"
    untrusted_system_failure)
run_quic_case(
    dns-failure FALSE
    "${certificate_file}"
    "${private_key_file}"
    "${certificate_sha256}"
    dns_failure)

# Test credentials are generated afresh for every invocation. Retain only the
# public certificate/DER evidence in the binary tree.
file(REMOVE
    "${ca_private_key_file}"
    "${private_key_file}"
    "${pinned_private_key_file}"
    "${certificate_request}"
    "${ca_serial_file}")

if(pin_failure OR system_failure OR wrong_pin_failure OR
   untrusted_system_failure OR dns_failure)
    message(FATAL_ERROR
        "${pin_failure}\n${system_failure}\n${wrong_pin_failure}\n"
        "${untrusted_system_failure}\n${dns_failure}")
endif()
