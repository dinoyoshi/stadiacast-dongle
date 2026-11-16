# The built-in credential

`builtin_chain.pem` (leaf first) and `builtin_key.pem` are the same credential the Python
receiver ships as `src/stadiacast/data/identity.bin`, unpacked with
`openssl pkcs12 -in identity.bin -nodes -passin pass:`. It's a Google Trust Services chain
issued for a domain dedicated to the project and revoked before its key was published:
the controller checks only that the chain leads to a GTS root (see `docs/setup.md`,
"Certificate"), and every ordinary TLS client rejects it.
