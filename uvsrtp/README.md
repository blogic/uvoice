# uvsrtp

`libuvsrtp` protects and unprotects one RTP or RTCP stream. It wraps Cisco
libsrtp2 behind a small API: one context per stream, with create, rekey,
protect, unprotect and counters. The SRTP layer of `libudsp` lives here, so a
voice stack links one small library instead of carrying the libsrtp2 glue.

## Scope

- Cipher suites: `AES_CM_128_HMAC_SHA1_80` and `AES_CM_128_HMAC_SHA1_32`
  (RFC 4568). SRTCP always uses the 80-bit tag (RFC 4568 6.2).
- Keys: the 30-byte SDES key and salt per direction (16 key, 14 salt).
- Replay window: 128. Transmit uses a wildcard outbound template; receive uses
  a wildcard inbound template, so a mid-session SSRC change is accepted.
- No key management, no SDP, no sockets. The caller supplies the keys and the
  packet buffers.

## API

The public header is `uvsrtp/uvsrtp.h`.

| Function | Purpose |
|---|---|
| `uvsrtp_init` | Initialise the process-global libsrtp2 state once. |
| `uvsrtp_create` | Allocate a context for one stream from a suite and two keys. |
| `uvsrtp_rekey` | Replace the keys of a live context; same keys are a no-op. |
| `uvsrtp_protect` / `uvsrtp_unprotect` | RTP in place. |
| `uvsrtp_protect_rtcp` / `uvsrtp_unprotect_rtcp` | RTCP in place. |
| `uvsrtp_stats_get` | Read the five counters. |
| `uvsrtp_destroy` | Release a context and zeroise its key material. |

Counters: protect ok, protect fail, unprotect ok, unprotect auth fail,
unprotect replay.

### Buffers and threads

- A packet buffer starts on a 32-bit boundary. RTP protect needs
  `len + UVSRTP_MAX_TRAILER` of capacity; SRTCP protect needs
  `len + UVSRTP_MAX_SRTCP_TRAILER`.
- Protect touches only the transmit session; unprotect only the receive
  session. The two run on different threads at the same time. Rekey and destroy
  must not overlap a packet call: the caller quiesces both threads first.
- `uvsrtp_stats_get` reads lock-free counters and runs at any time.

## Build on a host

A host build needs `cmake` 3.13 or later, a C11 compiler, `pkg-config` and
libsrtp2.

```
cmake -S uvsrtp/src -B uvsrtp/build -DUVSRTP_TESTS=ON
cmake --build uvsrtp/build -j8
(cd uvsrtp/build && ctest --output-on-failure)
```

The suite `test_uvsrtp` runs round trips, replay, rekey, the fault and restore
paths, and two known-answer tests. The known-answer vectors are the reference
octets of libsrtp's `srtp_validate()` (RFC 3711 test vectors), so the build
cross-checks the ciphertext and the authentication tag, not only the round
trip.

## Build in OpenWrt

`Makefile` builds the package `libuvsrtp`, which depends on libsrtp2.
`Build/InstallDev` stages `uvsrtp/uvsrtp.h`, the shared library and the
pkg-config file, so `libudsp` finds the library in the staging directory. The
SONAME carries the full `PKG_VERSION`.

## License

libuvsrtp is LGPL-2.1-only (`LICENSE`), the same as libudsp. The test header
`uvsrtp/uvsrtp_test.h` is not installed; it drives the rekey failure and
restore paths from the test suites.
