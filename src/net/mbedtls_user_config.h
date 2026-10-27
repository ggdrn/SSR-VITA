// mbedTLS adjustments for the Vita, applied on top of the default
// include/mbedtls/mbedtls_config.h (MBEDTLS_USER_CONFIG_FILE).
// Sockets, files and timers come from the loader, not from mbedTLS.

#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

// Entropy from sceKernelGetRandomNumber (src/net/tls.c).
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// Millisecond clock from sceKernelGetProcessTimeWide (src/net/tls.c).
#define MBEDTLS_PLATFORM_MS_TIME_ALT
