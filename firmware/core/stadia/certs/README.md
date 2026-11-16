# The built-in credential

`builtin_chain.pem` (leaf first) and `builtin_key.pem` are the same credential the Python
receiver ([jkfran/stadiacast](https://github.com/jkfran/stadiacast)) ships as `src/stadiacast/data/identity.bin`, unpacked with
`openssl pkcs12 -in identity.bin -nodes -passin pass:`. It's a Google Trust Services chain
issued for a domain dedicated to the project and revoked before its key was published:
the controller checks only that the chain leads to a GTS root (see its [`docs/setup.md`](https://github.com/jkfran/stadiacast/blob/3e047553644d7f1d60dc9f6d92b0f70bb6ec8c8c/docs/setup.md),
"Certificate"), and every ordinary TLS client rejects it.
