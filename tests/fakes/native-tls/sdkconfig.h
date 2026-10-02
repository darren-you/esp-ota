#pragma once

/* The IDF bignum wrapper needs this switch in a native Mbed TLS build. */
#define CONFIG_MBEDTLS_HARDWARE_MPI 0

/* Native TLS retains real certificate dates; attach is the test CA adapter,
 * documented by the real HTTPS test, rather than a device bundle call. */
#define CONFIG_MBEDTLS_HAVE_TIME_DATE 1
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1
