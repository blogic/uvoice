# uvtransport

`libuvtransport` carries one media stream over ICE with DTLS-SRTP, the
transport every WebRTC peer and every Jingle client expects. It joins three
libraries behind one API: libjuice runs the ICE agent with its STUN and TURN
client, OpenSSL runs the DTLS handshake of RFC 5764 over the connected pair,
and `libuvsrtp` protects the RTP and RTCP with the keys that handshake
exported.

## Scope

- Full ICE (RFC 8445) with trickled candidates, one component, rtcp-mux.
- DTLS 1.2 with the `use_srtp` extension; profiles
  `SRTP_AES128_CM_SHA1_80` and `SRTP_AES128_CM_SHA1_32`.
- One self-signed EC P-256 certificate per process, SHA-256 fingerprint. The
  far end's fingerprint is verified against the one the signalling carried.
- No signalling, no SDP, no XML. The caller moves the credentials, the
  fingerprints and the candidate lines.

## Threads

libjuice runs its own thread. Every callback of `struct uvt_cfg` runs on it.
`uvt_tick()` drives the DTLS retransmission timer and belongs in the caller's
own loop.

## API

The public header is `uvtransport/uvtransport.h`.

| Function | Purpose |
|---|---|
| `uvt_init` | Certificate, DTLS context and SRTP state of the process, once. |
| `uvt_fingerprint_get` | The local certificate fingerprint for the signalling. |
| `uvt_create` | One stream: the ICE agent, with the STUN and TURN servers. |
| `uvt_remote_set` | The remote ufrag, pwd, fingerprint and DTLS setup. |
| `uvt_candidate_add` / `uvt_remote_done` | Remote candidates, trickled. |
| `uvt_local_get` / `uvt_candidate_next` | Local credentials and candidates. |
| `uvt_send_rtp` / `uvt_send_rtcp` | Protect and send. |
| `uvt_tick` | DTLS retransmissions. |
| `uvt_destroy` | Release the stream and zeroise its keys. |

## Build on a host

```
cmake -S src -B build -DUVTRANSPORT_TESTS=ON
cmake --build build && (cd build && ctest --output-on-failure)
```

The suite connects two streams over the host's own address with host
candidates alone, so it needs one network interface and no server.
