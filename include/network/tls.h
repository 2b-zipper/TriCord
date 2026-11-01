#ifndef TLS_H
#define TLS_H

#include <mbedtls/x509_crt.h>

namespace Network {

mbedtls_x509_crt *sharedCaChain();

} // namespace Network

#endif // TLS_H
