# TLS test certificates

These files are the fixtures of `test/unit/test_sip_tls.c`. They are
committed, so a test run never calls `openssl`. `gen.sh` makes them. When
they expire, only `gen.sh` must run again.

The suite reads `UVOICE_TLS_FIXTURES` from its environment first, and the
compile define `UVOICE_TLS_FIXTURE_DIR`, which cmake sets, second. So the
variable overrides the define, and a binary built by hand finds its files
through it. If the suite finds neither, it fails.
Reason: a silent skip would report a pass.

## Files

| file | what it is |
| --- | --- |
| `ca.crt` | Test CA. A verifying client loads it as its trust store. |
| `ca.key` | Private key of that CA. It signed `server.crt`. |
| `server.crt` | Server identity: CN `uvoice-test`, SAN DNS `sip.example.test`. |
| `server.key` | Private key of the server identity. |
| `other-ca.crt` | A second, unrelated CA. It signed nothing here, so a client that trusts only this one refuses `server.crt`. That is the verify-failure case. |
| `gen.sh` | Makes the five files above with the `openssl` CLI and no interaction. |

Every key is EC P-256. Every certificate is valid for 7300 days from the day
`gen.sh` ran. To read the exact window, run
`openssl x509 -in server.crt -noout -dates`.

## Keys are test material

`ca.key` and `server.key` are private keys of a CA and a server that exist
only in this directory. The source tree publishes them on purpose. They
authenticate nothing. Nothing outside this test trusts them, and no shipped
image carries them. Never use them for anything else. Never copy this CA into
a real trust store.
