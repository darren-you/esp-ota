#pragma once

/* The IDF bignum wrapper needs this switch in a native Mbed TLS build. */
#define CONFIG_MBEDTLS_HARDWARE_MPI 0
