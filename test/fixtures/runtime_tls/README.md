# Controlled TLS fixtures

These files provide the certificate authority, localhost server certificate,
and private key used by `test/python/runtime_curl_tls_tests.py`. The harness
decodes the PKCS#8 key into a temporary PEM file for its isolated loopback
server and deletes that file on exit.

The key is an unencrypted, deterministic test credential. Never reuse it
outside the test suite. This directory must remain excluded from production,
installed, and loadable source inventories.
