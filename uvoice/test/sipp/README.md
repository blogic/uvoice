# uvoice SIPp scenarios

`run_sipp.sh` requires SIPp 3.6.1 or later in its header comment. No runner
checks the version:

- `run_sipp.sh` checks only that `SIPP_BIN` (default `sipp` on PATH) names a
  command;
- `test/e2e/run_e2e.sh` and `test/e2e/run_churn.sh` default to a SIPp 3.7.3
  build (`default_sipp`) and check only that it is executable.

The end-to-end harness uses two SIPp 3.7.3 binaries. Each SIPp source
citation in this README refers to the 3.7.3 source. Each run writes logs
under `test/sipp/artifacts/`, unless `SIPP_ARTIFACT_DIR` selects another
directory. A run writes a packet capture
there too, if `tcpdump` starts (`capture_start()`).

## Scenarios

| Scenario | Check |
|---|---|
| `uas_register` | Accept a REGISTER without authentication and grant a 60-second expiry. |
| `uas_register_auth_2069` | Send an RFC 2069 challenge, check the opaque echo, and reject qop fields. |
| `uas_register_auth_qop` | Send a qop=auth challenge and require cnonce, nc, qop, and opaque. |
| `uas_register_423` | Send 423 with Min-Expires 300 and require a retry expiry of at least 300. |
| `uas_register_outage` | Grant ten-second bindings, take one refresh without an answer, and answer each later REGISTER. The outage sets up the field condition: a DUT that still holds a binding while its WAN address changes. |
| `uas_register_prime` | Accept a REGISTER without authentication and grant the expiry that the `expires` keyword names. The E2E harness passes `-key expires <seconds>` and runs it as a separate registrar before a caller starts (`test/e2e/README.md`, "REGISTER traffic"). |
| `uas_basic_call` | Answer a DUT call, wait five seconds, then receive and answer its BYE. Answer a REGISTER too. |
| `uac_basic_call` | Call the DUT, wait five seconds, then send BYE. |
| `uac_verstat_call` | The same call, with a field capture's header set. The runner passes `-key verstat No-TN-Validation`; `SIPP_KEYS` names another value. (note) |
| `uas_service_route` | Answer the DUT's REGISTER with a 200 that carries one `Service-Route` header with two loose hops behind a comma. Challenge the DUT's call with 407, then answer its credential retry. Require the first granted hop on the 407's ACK, on the retry and on the first INVITE (RFC 3608). (note) |
| `uas_service_route_none` | The same pair with no `Service-Route` in the 200 and no challenge. Require no `Route` header at all: not on REGISTER, INVITE, ACK or BYE. That is a field case too. |
| `uac_g726_call` | Offer G726-32 first and require a G726-32 answer. |
| `uac_g726_refused` | Offer G726-32; require PCMU from the default codecs. |
| `uac_g722_call` | Offer G.722 (static payload type 9), PCMU and telephone-event in that order, and require a G.722 answer. An inverse check refuses an answer that writes `G722/16000`: G.722 has an rtpmap rate of 8000 (RFC 3551 section 4.5.2). |
| `uac_g722_refused` | The same offer, byte for byte, against a DUT whose codec sections name no G.722. Require a PCMU answer and a call that runs to the caller's BYE. The two offers are equal; DUT configuration sets the expected answer. |
| `uac_opus_call` | Offer Opus (dynamic payload type 96), PCMU and both telephone-event clocks, 101 at 8000 and 102 at 48000. Require an Opus answer at `opus/48000/2`, the fmtp that this side states for its own decoder, and telephone-event 102 with no 101. Inverse checks refuse `opus/16000`, `opus/8000` and any `dtx` key. |
| `uac_opus_refused` | The same offer against a DUT whose codec sections name no Opus. Require a PCMU answer with telephone-event 8000, and a call that still completes. Inverse checks refuse every Opus artifact: encoding name, 48000 clock, `maxplaybackrate` fmtp key and `a=maxptime` attribute. |
| `uac_opus_narrow_dtmf` | Offer Opus first, PCMU second and narrowband telephone-event alone (payload type 101 at 8000). Require an Opus answer and no telephone-event at all: an event stream runs on its audio's clock (RFC 4733 section 2.1). The end-to-end flow reads the daemon's log line for its digit fallback. |
| `uas_reinvite_hold` | Send hold and resume re-INVITEs and check recvonly and sendrecv answers. Answer a REGISTER too. |
| `uac_reinvite_hold` | Answer the daemon's hold and resume re-INVITEs, and require an ACK for each 2xx. |
| `uas_offerless_reinvite` | Send an offerless re-INVITE and answer the 200 offer in ACK. Two inverse checks read the 200's offer: it must carry no `c=IN IP4 0.0.0.0` and no one-way direction attribute. That is the resume rule on an active leg. Answer a REGISTER too. |
| `uas_legacy_hold` | Hold sequence of a field switch, in capture order. The DUT's BYE ends the scenario. Answer a REGISTER too. (note) |
| `uac_legacy_hold_conf` | Same sequence as `uas_legacy_hold`, against a conference join, where this peer places the first leg. It answers the flash hold in the ordinary way, and the resume at the join with the stale hold SDP. It then sends an INVITE with no SDP. It needs out-of-call scenario `xfer_peer_ooc` for the DUT's second call. |
| `uas_t38_reject` | Offer T.38 and require image port zero with active audio preserved. Run it with `fax_t38_enable` 0, the disabled path under test. Answer a REGISTER too. |
| `uas_t38_accept` | Offer T.38 with the audio stream withdrawn and require the mirrored answer: audio at port zero, image at a live port, no direction attribute. Needs `fax_t38_enable` 1. |
| `uas_t38_page` | Receive one page from the DUT. Start `t38-peer --answer --receive FILE`, with `--port` set to `SIPP_MEDIA_PORT + 2`. `t38-peer` exits zero on T.30 success. Set `SIPP_TIMEOUT_SECONDS=600`. A page takes about one minute. A 180-second wait for the BYE covers a slow page before the DUT hangs up. |
| `uas_churn` | Answer a DUT call, pause briefly, then receive and answer its BYE. The call-churn harness runs it N times through one SIPp instance. Answer a REGISTER too. |
| `uas_options_keepalive` | Answer three DUT OPTIONS requests with 200. |
| `uas_cfu_redirect` | Register the DUT, then offer it a call while unconditional call forwarding is set with a target. Require a 302 that carries that target in its Contact. A 180 before a redirect is a failure, because the line must never ring. |
| `uas_mwi_notify` | Register the DUT. Then send an unsolicited message-summary NOTIFY outside any dialog and any subscription, and require its 200. Every shipped switch uses that path. Send a second NOTIFY that clears the state, and require its 200. Send a NOTIFY of an unserved package, and require a 489. |
| `uac_cancel` | Cancel after 180 and require 200, 487, and ACK processing. |
| `uac_inbound_churn` | Fifty inbound calls of a field sequence through one instance, one at a time, with the header block of `uac_verstat_call`. Require each call's 180 inside one second. Cancel even call numbers, as `uac_cancel` does. The DUT answers odd ones, and the subscriber ends each of those short calls with the handset. (note) |
| `uas_491_glare` | Send 491 and check that the DUT retries from 2.1 through 4.0 seconds later. Answer a REGISTER too. |
| `uas_glare_mutual` | Mutual glare on the retry interval of a field switch. Answer the DUT's hold re-INVITE 491. After 1900 ms, send a hold re-INVITE of this peer and require a 200 `a=recvonly`. Answer 200 to the DUT's retry behind it, which carries `a=inactive`. The runner reads that retry's delay off the capture, as it does for `uas_491_glare`. (note) |
| `uas_503_retry_after` | Send Retry-After 5 and check that the next REGISTER comes after five seconds. |
| `uas_register_403` | Challenge the REGISTER 401, require an `Authorization` that starts with `Digest` on the retry, then refuse it 403. The DUT holds no binding afterwards. End-to-end flow `reg-403-reorder` drives it. |
| `uac_refused_480` | Call a DUT line that holds no live registration, and require 480 before any ring; a 180 or a 2xx is a failure. Then ACK the 480 in its transaction. End-to-end flow `reg-403-reorder` drives it. |
| `uas_session_timer_422` | Send 422 with Min-SE 900 and require a corrected INVITE. Answer a REGISTER too. |
| `uac_prack_require` | Send `Require: 100rel` and require a reliable 180 with an `RSeq`, PRACK in Allow and 100rel in Supported. Acknowledge it with a PRACK and complete the call. Needs `prack` 1. |
| `uac_prack_supported` | Send `Supported: 100rel` alone and require the same reliable 180. Needs `prack` 1. |
| `uac_prack_supported_plain` | The same INVITE against a DUT with `prack` 0, and require a plain 180: no Require, no `RSeq`, and no PRACK of this peer. |
| `uac_prack_require_plain` | Send `Require: 100rel` against a DUT with `prack` 0 and require the same plain 180, with a call that still completes. That is the accepted deviation from RFC 3262 section 3, which makes the reliable 180 a MUST under Require. |
| `uac_prack_bad_rack` | Acknowledge the reliable 180 three times wrongly: by RAck CSeq number, by RAck method and from a foreign dialog. Require 481 for each. A fourth PRACK names the right triple, earns a 200, and completes the call. Each PRACK carries its own CSeq number, one higher than the last. Needs `prack` 1. |
| `uac_prack_late` | Hold the PRACK back 1700 ms, which spans the sends at T1 and 2\*T1, then acknowledge the 180 and complete the call. Needs `prack` 1. |
| `uac_prack_giveup` | Send no PRACK at all, and require the 504 that the DUT must send at 64\*T1. Then ACK it. Needs `prack` 1 and `sip_timer_t1` 100. |
| `uac_prack_cancel` | `uac_cancel` exchange under `Require: 100rel`: cancel an unacknowledged reliable 180 and require 200, 487 and ACK processing. Needs `prack` 1. |
| `uas_prack_180` | Answer a DUT call with a reliable 180 that carries `RSeq: 4242`. Require one PRACK addressed to this peer's Contact. Its RAck must name 4242, the INVITE CSeq number and INVITE. Its CSeq must be one higher than the INVITE's CSeq. Answer a REGISTER too. |
| `uas_prack_dup_180` | Send the same reliable 180 twice and require exactly one PRACK. Answer a REGISTER too. |
| `uas_prack_gap` | Send a reliable 183 with `RSeq: 10`, then a reliable 180 with `RSeq: 12`. Require the PRACK for 10 alone. Answer a REGISTER too. |
| `uas_prack_183_sdp` | Answer with a reliable 183 that carries the SDP answer, and require its PRACK. Then answer the INVITE with no body at all. Require the call to connect on the 183's media. Answer a REGISTER too. |
| `uas_prack_auth` | Challenge the INVITE 401 and require its retry to keep `Supported: 100rel`. The reliable 180 behind it draws a PRACK whose RAck names the retry's CSeq. Answer a REGISTER too. |
| `uas_183_sdp` | Answer a DUT call with a plain 183 that carries the SDP answer. Pause five seconds, then answer the INVITE with the same body. The DUT sends no PRACK. End-to-end flow `early-media-183` drives it. Answer a REGISTER too. (note) |
| `uas_fax_upspeed` | Answer a DUT call with G.729 alone and require the fax upspeed re-INVITE to offer G.711 alone. Answer a REGISTER too. |
| `uas_fax_upspeed_auth` | The same call with the upspeed re-INVITE answered 401. Require an ACK, then the credentialed retry: nonce and opaque of that challenge in `Authorization`, and the same G.711-only offer. A third re-INVITE fails the scenario. Answer a REGISTER too. |
| `uas_far_fax_upspeed` | Answer a DUT call with G.729 alone, then make the upspeed from this side. After a pause, re-INVITE with the audio list narrowed to G.711 and sendrecv. Require a G.711 answer with no G.729, and no re-INVITE from the DUT. Needs a CED tone from the wire during the pause. Answer a REGISTER too. |
| `uas_far_fax_tone_g711` | Answer a DUT call with PCMU and telephone-event, then send nothing for sixteen seconds. Needs a CED tone from the wire during that window. End-to-end flow `uas_far_fax_tone_false` also sends that tone's end row under 2 s inside the window. An in-dialog INVITE from the DUT is a failure. Answer a REGISTER too. |
| `uas_fax_upspeed_false` | Answer a DUT call with G.729 alone and require two fax upspeed re-INVITEs, each offering G.711 alone. Needs a CED tone toward the phone, then that tone's end row under 2 s, then a second CED. Answer a REGISTER too. |
| `uac_t38_offer` | Call the DUT with G.729 alone, and accept the T.38 upgrade that it offers after a V.21 preamble. Require an image stream in place of the audio stream. Needs `fax_t38_enable` 1. |
| `uac_t38_page` | Send one page to the DUT. Start `t38-peer --call --send FILE`, with `--port` set to `SIPP_MEDIA_PORT + 2`. `t38-peer` exits zero on T.30 success. Set `SIPP_TIMEOUT_SECONDS=600`. A page takes about one minute. A 180-second wait for the BYE covers a slow page before the DUT hangs up. |
| `uac_t38_reject` | The same call with the image offer answered 488. Require the G.711 passthrough offer that comes before it, and no second image attempt. Needs `fax_t38_enable` 1. |
| `uas_srtp_call` | Answer a DUT call that offers SDES SRTP. Require RTP/SAVP, a tag 1 `AES_CM_128_HMAC_SHA1_80` attribute, a tag 2 `AES_CM_128_HMAC_SHA1_32` attribute, and no tag 3. Answer RTP/SAVP with one attribute that mirrors tag 1 and carries a fixed key. Needs `srtp_enabled` 1. Answer a REGISTER too. (note) |
| `uac_srtp_call` | Call the DUT with RTP/SAVP and two crypto attributes. Require RTP/SAVP in the 200, a tag 1 attribute with the `_80` suite, and no tag 2. End-to-end flow `srtp-inbound` checks the exact attribute count. Needs `srtp_enabled` 1. |
| `uac_srtp_plain_hold` | Call an SRTP-enabled DUT with RTP/AVP. Require a plain 200, then require its hold and resume re-INVITEs to stay RTP/AVP with no crypto attribute. Needs `srtp_enabled` 1. |
| `uac_savp_refused` | Send the same RTP/SAVP offer to a DUT with `srtp_enabled` 0 and require 488. |
| `uas_stream_call` | Answer a DUT call over a reliable transport, pause two seconds, then receive and answer its BYE. Twin of `uas_basic_call`, without `rtp_echo`. Answer a REGISTER too. |
| `uac_stream_call` | Call the DUT over a reliable transport through its listener, pause two seconds, then send BYE. Twin of `uac_basic_call`, without `rtp_echo`. |
| `uas_stream_subscribe` | Answer the DUT's REGISTER and message-summary SUBSCRIBE with 200 and a 3600-second expiry. One scenario answers both. Reason: the DUT subscribes only from a registered line, and a UAS run has no out-of-call scenario. |
| `uas_xfer_blind` | Answer a DUT call, its flash hold re-INVITE, and its REFER with 202. Then play the switch order: a NOTIFY `100 Trying`, a `c=0.0.0.0` re-INVITE, a final NOTIFY `200 OK` and a BYE of this peer. The DUT answers all four 200 and sends no BYE; one anywhere is a failure. Answer a REGISTER too. (note) |
| `uas_xfer_blind_grace` | The same sequence with no BYE of this peer. It expects a DUT BYE behind the final NOTIFY. The harness checks that this BYE leaves one grace time after the DUT answers that NOTIFY. Answer a REGISTER too. |
| `uas_xfer_attended` | The same call and REFER, completed on the wire: 202, then one NOTIFY whose `message/sipfrag` body carries `SIP/2.0 200 OK` under a terminated subscription. The DUT must then send its BYE, one grace time after it answers that NOTIFY. The harness checks that delay. Answer a REGISTER too. |
| `uas_xfer_failed` | The same referral, refused: the sipfrag carries `SIP/2.0 486 Busy Here`. The DUT must resume the held call with a re-INVITE that carries no one-way direction attribute. Answer a REGISTER too. |
| `uac_xfer_refer` | Call the DUT and REFER it to a target of this peer. Require two notifications in order: `SIP/2.0 100 Trying` under an active subscription, then `SIP/2.0 200 OK` under a terminated one. Needs out-of-call scenario `xfer_peer_ooc`. |
| `uac_xfer_refer_replaces` | The same referral with an escaped Replaces inside the Refer-To URI, which is the shape of a consult transfer. The DUT must promote it into exactly one Replaces header of its child INVITE, beside `Require: replaces`. Needs `xfer_peer_ooc`. |
| `uac_xfer_child_fail` | The same referral with its child refused 486, and require the second notification to carry that code. The call that the referral could not move stays up. Needs out-of-call scenario `xfer_busy_ooc`. |
| `uac_xfer_onhook` | Call the DUT and answer the hold re-INVITE. Require the REFER of an on-hook consult. Its Refer-To names the other call's remote target, with that call's dialog escaped into a Replaces URI header. Answer 202 and send the verdict NOTIFY (sipfrag 200). Needs `xfer_replaced_ooc` for the consulted call. (note) |
| `uac_xfer_onhook_blind` | Call the DUT and answer the hold re-INVITE. Require the REFER of an on-hook with a ringing second call: a Refer-To that names the dialed number, and no Replaces. Needs `xfer_ringing_ooc` for the second call. (note) |
| `uac_xfer_replaces` | Let the DUT place the first call, then place a second call whose Replaces names the first dialog. Require a 200 with no 180 in front of it, and a BYE of the replaced dialog. Needs `xfer_peer_ooc`, which answers the DUT's first call and hands this scenario the dialog identity in SIPp global variables. |
| `uac_xfer_two_line` | Complete a referral by calling the number that Refer-To named, which on a two-line DUT is the other subscriber. `uas_xfer_attended` answers the transferor's call in a separate SIPp run that ends before this one starts. |
| `uas_star_dial` | Switch side of a star string that the DUT dials through. Answer the DUT call and its flash hold re-INVITE. Take the transfer code as a second INVITE, and require no REFER at all. One file serves both calls. Answer a REGISTER too. (note) |
| `xfer_peer_ooc` | Support scenario, never run on its own. This out-of-call peer answers a call that the DUT places by itself. It rings for two and a half seconds first, and ends when the DUT ends the call. It also records the dialog identity in SIPp global variables for `uac_xfer_replaces`. |
| `xfer_busy_ooc` | Support scenario, never run on its own. The same peer, refusing with 486. |
| `xfer_replaced_ooc` | Support scenario, never run on its own. The consulted peer of the on-hook consult flows answers the DUT's second call. It then ends that dialog itself with a BYE, and requires a 200 of the DUT. A DUT BYE before its own is a failure. (note) |
| `xfer_ringing_ooc` | Support scenario, never run on its own. The consulted peer that never answers. It rings the DUT's second call, and requires the CANCEL that an on-hook must send. It answers 200 to that CANCEL and 487 to the INVITE. |
| `t38_page_peer_uas` | Support scenario, never run against a DUT. Answering side of a loopback proof of `uac_t38_page`. It answers the G.729 call, offers the T.38 upgrade that `uac_t38_page` checks, takes the answer and ends the call. No media. |
| `t38_page_peer_uac` | Support scenario, never run against a DUT. Calling side of a loopback proof of `uas_t38_page`. It places the call, accepts the upgrade, mirrors the offered order in its answer and ends the call. No media. |
| `uac_conf_leg` | Place an inbound conference leg, answer the DUT hold and optional resume re-INVITEs, then answer its BYE. |
| `uac_conf_onhook_xfer` | Place the inbound conference leg and answer the join. Hold and resume from the far end, then answer an optional REFER and BYE. |
| `uac_conf_onhook_xfer_auth` | The same peer, challenging what the on-hook transfer sends. Answer the REFER 401, and require it again with `Authorization` and the first Refer-To. Then answer 202 and send the NOTIFY. Answer the BYE 401, require it again with `Authorization`, then answer 200. Its two challenges carry different nonces. Needs `xfer_replaced_ooc`. |
| `uac_conf_remote_bye` | Place the inbound conference leg, answer the DUT hold and join-resume re-INVITEs, then send BYE. |
| `uas_conf_leg` | Answer an outbound conference leg, answer each hold or resume re-INVITE, then answer the DUT BYE. Answer a REGISTER too. |

Notes:

- **`uac_verstat_call`.** Display name and URI user part of the From both
  carry `verstat=[verstat]`. The To has a header parameter and no angle
  brackets. The Contact has no user part. The INVITE also carries two
  `Supported` headers, a `Session-ID` and the field offer.
- **`uas_service_route`.** The check on the first INVITE reads a stored value.
  One scenario answers both requests.
- **`uas_legacy_hold`.** The scenario plays these steps in order:
  1. It answers the call `18 0 8`.
  2. It answers the DUT hold with `c=IN IP4 0.0.0.0` and no direction.
  3. It sends a re-INVITE of that shape, and requires a 200 `a=inactive` at
     the DUT's real address.
  4. It answers the DUT resume with the same stale hold SDP.
  5. It sends an INVITE with no SDP, and requires an active offer in the 200
     with the whole codec list. The ACK carries a PCMU sendrecv answer.
- **`uac_inbound_churn`.** The scenario is absent from `all`, because an odd
  call rings until somebody lifts the handset. Ask for it by name (section
  "The inbound churn scenario").
- **`uas_glare_mutual`.** The scenario answers a REGISTER too (section "The
  REGISTER branch").
- **`uas_183_sdp`.** An unreliable answer is provisional, so the DUT reopens
  the offer. The DUT would abandon the leg on a 200 with no body. Local
  ringback of the DUT plays until a packet reaches the early stream (RFC 3960
  section 3.2). Flow `early-media-183` injects that packet.
- **`uas_srtp_call`.** Each offered attribute must end its line with a 40
  character key. End-to-end flow `srtp-outbound` checks the exact attribute
  count, because a regex cannot count.
- **`uas_xfer_blind`.** The `c=0.0.0.0` re-INVITE requires a 200 `a=inactive`
  with no Session-Expires. The final NOTIFY carries
  `terminated;reason=timeout`.
- **`uac_xfer_onhook`.** The scenario requires a BYE of this leg, the
  referee, after the NOTIFY and not before. End-to-end flow `xfer-onhook`
  checks that the BYE comes one grace time after the DUT answers that NOTIFY
  (`xfer_grace_check()`). The DUT must not send BYE on the consulted call. It
  must answer a sendrecv re-INVITE on that call with a 200 sendonly.
- **`uac_xfer_onhook_blind`.** The scenario answers the REFER 202, then
  notifies progress and verdict. It requires a BYE of this leg after the DUT
  answers the verdict, and not before. End-to-end flow `xfer-onhook-blind`
  checks that the BYE comes one grace time after that answer
  (`xfer_grace_check()`).
- **`uas_star_dial`.** The scenario checks no request URI: end-to-end flow
  `star-network` reads the request line off the SIPp message log. Every branch
  ends on the BYE that an on-hook sends to both legs. The scenario carries no
  `rtp_echo`.
- **`xfer_replaced_ooc`.** This peer's BYE plays the network's BYE after the
  referred party replaced the dialog (RFC 5589). Its rule on an earlier DUT
  BYE checks that the DUT does not end the replaced leg itself.

UAS call scenarios wait for the DUT to start a call. Hold and glare scenarios
also require a matching DUT action during that call. Session timer scenario
`uas_session_timer_422` needs no action during the call: the DUT sends its
corrected INVITE at call setup.

### Verstat scenario takes a generic keyword

Two scenarios use a generic keyword that `-key` defines:

- `uac_verstat_call` uses `[verstat]`;
- `uas_register_prime` uses `[expires]` (section "The two service route
  scenarios", label "The call count of the other call scenarios").

`scenario_keys_get()` in `run_sipp.sh` gives each of them a default:
`verstat No-TN-Validation` and `expires 3600`. The runner passes the pairs as
`-key` arguments on every run of that scenario.

`[verstat]` stands twice in each From: in the quoted display name and in the
URI. The scenario writes that From in its INVITE, ACK and BYE. Its default
`No-TN-Validation` is the field capture's value. A default is required.
Reason: SIPp stops with "Unsupported keyword" when a scenario uses a generic
keyword that the command line does not define (SIPp 3.7.3 `src/message.cpp`).

The position of a `-key` on the command line does not matter. SIPp reads its
argument list in four passes, and gives `key` pass 1 and `sf` pass 2
(`src/sipp.cpp`, the option table and the `CHECK_PASS` loop). SIPp therefore
defines every keyword before it reads any scenario.

`SIPP_KEYS` replaces the list for one run. It holds whitespace separated
"keyword value" pairs, and an odd number of fields stops the run:

```sh
SIPP_KEYS='verstat TN-Validation-Passed' ./run_sipp.sh 192.0.2.10 uac_verstat_call
```

`uac_verstat_call` is in the `all` set with its default. It needs `prack` 0,
which is the shipped value. Reason: its `Supported: 100rel` draws a reliable
180 from a DUT with the option set, and this scenario sends no PRACK. The
end-to-end harness drives the three values unattended (`test/e2e/run_e2e.sh`,
flow `verstat-cid`). That flow checks caller identity behind the signaling.

### The inbound churn scenario

**Bound.** `uac_inbound_churn` plays a field sequence: one offered
call after another, each with a bound on its 180. The field device answered
100 and sent no 180 for 6.4 seconds. The 180's receive therefore carries
`timeout="1000"`, and a call fails when the DUT is slower than that.
End-to-end flow `inbound-churn` reads every call's distance off a capture.
The bound alone cannot give that measurement.

**Header block.** The scenario takes its header block from
`uac_verstat_call` and drops the `verstat` parameter. It therefore defines no
SIPp keyword and needs no `-key` of its own. The parameter belongs to the
verstat caller identity rule, which `uac_verstat_call` drives. The
From carries the caller number of that capture.

**Session-ID.** Every call builds a `Session-ID` of its own from pid and call
number (RFC 7989 section 8). SIPp writes neither `[pid]` nor `[call_number]`
with a width of its own. The leading `<nop>` therefore pads the two to 32
digits, and a regular expression takes the last 32 back out.

**Run.** The scenario is absent from the `all` set. Half its calls wait for
somebody to lift the handset, and an unattended bench run has nobody to do
it. Ask for it by name:

```sh
./run_sipp.sh 192.0.2.10 uac_inbound_churn
```

`scenario_run()` gives it fifty calls and a call limit of one. The limit makes
the calls sequential. Reason: a subscriber under test is one line, and a
second offer inside the first one's ring draws a busy answer.
`SIPP_CALL_LIMIT` replaces the limit for a DUT with more lines.

**Branch.** The DUT answers odd call numbers, and this peer cancels even ones.
The branch reads the call number. An `assignstr` writes `[call_number]` into a
variable, because SIPp expands a keyword in that value (3.7.3 `call.cpp`, the
`E_AT_ASSIGN_FROM_STRING` arm runs it through `createSendingMessage`). An
`ereg` with `search_in="var"` then reads its last digit.

The jump uses `next` and `test` on the 180's receive, not a `<nop>` behind it.
Section "When a failed check is judged" below gives the reason.

**Answered call.** An answered call ends at the subscriber. The scenario waits
for the DUT's BYE and answers it 200. The handset is therefore back on its
cradle before the call ends at this side, and before a new offer exists.
The other order, a BYE from this side after a talk pause, makes the handset
race that offer. A line that is still off hook answers an offer 486.

**The pace.** The scenario's ring pause is 700 ms. End-to-end flow
`inbound-churn` holds each answered call for a talk time of 600 ms
(`inbound_churn_hook_drive()`). Three limits of the DUT bind that pace, in
this order:

1. An answered call holds its 2xx record until 64\*T1, so that a
   retransmitted ACK does not reach the call layer twice (RFC 6026,
   `src/proto/sip/ua.c` `accept_ack()`). A line holds eight records
   (`src/proto/sip/ua.h` `UA_ACCEPT_MAX`). Two answered calls must therefore
   stand apart by more than an eighth of that window: 4 s at the shipped
   `sip_timer_t1` of 500 ms. Flow `inbound-churn` sets `sip_timer_t1` 100,
   which holds a record for 6.4 s. A ninth answered call inside the window
   finds no free record, and the daemon abandons that call
   (`src/proto/sip/call.c` `resp_200_send()`).
2. An answered call's hook sequence must fit inside the distance between two
   offers: its ring, its talk time and its release. Otherwise the next offer
   meets a line that is still off hook.
3. Every call leaves a server transaction for timer J (RFC 3261 section
   17.2.2). `sip_timer_j` has its own default of 32000 ms, so a lower T1 does
   not shorten it (`src/cfg/schema.h`). The table holds 64 entries
   (`src/proto/sip/txn.h` `SIP_TXN_MAX`). When the table is full, the daemon
   drops an offer with no answer at all (`src/proto/sip/account.c`
   `account_request_recv()`).

**Bench run.** `run_sipp.sh` sets no DUT option, so a bench DUT keeps the
shipped `sip_timer_t1` of 500 ms. Only flow `inbound-churn` sets
`sip_timer_t1` 100. Under limit 1, a bench run therefore needs answered calls
more than 4 s apart. A tester at the handset must pace the answers to meet it.

### The RFC 2543 hold scenarios

Three scenarios drive the behavior of a field switch. The capture holds four
switch facts, and `uas_legacy_hold` plays all four:

- hold: `c=IN IP4 0.0.0.0` with no direction attribute, which is the RFC 2543
  hold form of RFC 3264 section 8.4;
- answer to a hold offer: that address and the formats `18 0 8`;
- about 40 ms later: a re-INVITE of the same shape;
- answer to the resume offer: the same stale hold SDP, then an INVITE with no
  SDP.

**Offerless INVITE.** A device that answers the final INVITE with
`c=0.0.0.0` leaves that call dead after the resume. Two inverse checks
therefore read the 200: no zero address and no one-way direction attribute.

**Pauses.** The pauses are 50 ms, because the capture's 40 ms is below SIPp
pause resolution. `uac_legacy_hold_conf` holds one of them open at 1000 ms
instead. A comment beside that pause states the reading that needs the
window.

**Resume offer.** The DUT states a direction in an offer only for a hold
(`src/proto/shared/sdp.c` `offer_tail_write()`; RFC 4566 section 6 makes
sendrecv the default). A DUT resume offer therefore carries no direction
attribute at all, and every check on one is an inverse check.

**Mutual glare.** `uas_glare_mutual` drives the other half of that capture.
Both sides send a re-INVITE. The switch retries once after about 1.9 s and
then stops. An outbound DUT leg owns the Call-ID, so its own retry
comes 2.1 to 4.0 s later (RFC 3261 section 14.1). The switch's retry
therefore arrives while the DUT's leg still waits.

RFC 3261 section 14.2 asks for a 491 only while an INVITE of this side is in
progress. A leg in its own backoff has none, so the answer is a 200, and the
scenario requires it. The two peers hold each other by then. The answer
therefore reads `a=recvonly`, and the DUT's retry behind it reads
`a=inactive`.

**Conference variant.** `uac_legacy_hold_conf` is the conference variant.
`uas_legacy_hold` cannot serve that flow: there the DUT places the call, and a
conference join needs an inbound first leg. SIPp places the call in the
variant. The file therefore takes prefix `uac_`, which this suite's naming rule
gives that role.

What each one needs of the DUT:

| Scenario | out-of-call scenario | DUT action during the run |
|---|---|---|
| `uas_legacy_hold` | none | place a call, flash to hold, flash again to resume, then hang up |
| `uas_glare_mutual` | none | place a call, then flash to hold |
| `uac_legacy_hold_conf` | `xfer_peer_ooc` | answer the call, flash, dial a second number, flash to join, then hang up |

**`all` set.** `uas_legacy_hold` and `uas_glare_mutual` are in `all`, beside
`uas_491_glare`, which asks the operator for a flash too.
`uac_legacy_hold_conf` is not: it needs an out-of-call scenario, which `all`
arranges for no scenario.

**Timing check.** `uas_glare_mutual` gets the same timing check as
`uas_491_glare`, and `timing_491_check()` counts only a DUT INVITE for it.
Without that rule, the check takes the peer's re-INVITE at 1.9 s. It then
refuses the run for a reason that is not the DUT's fault. The filter term is
`ip.src` against the DUT address, or `ipv6.src` when that address holds a
colon. Reason: this runner knows no DUT port, and `capture_start()` filters on
the same address.

A run with SIPp and the DUT on one address cannot use that term. The
end-to-end harness runs on the loopback, so its own check names the daemon
port instead (`test/e2e/run_e2e.sh`, `timing_491_check()`).

The end-to-end harness runs all three scenarios unattended, in these flows:

- `legacy-hold`;
- `glare-mutual`;
- `conf-legacy-hold`.

### The two service route scenarios

`uas_service_route` and `uas_service_route_none` are in the `all` set. They
need no DUT option of their own: they differ in the registrar's answer, not in
DUT configuration.

An operator drives three actions inside one run, in this order:

1. Register the line against the running SIPp. Bring the line down and up.
2. Place a call from that line and let it connect.
3. Hang up. Each scenario waits for the DUT's BYE and answers it 200.

**Call count.** Each run takes two calls (`-m 2` in `scenario_run()`), one
for the REGISTER and one for the INVITE. A run that sees one request alone
waits for a second call until `SIPP_TIMEOUT_SECONDS` (180 by default). It then
fails on that timeout. A run that sees two REGISTERs and no INVITE ends inside
its call count, and SIPp reports success for it.

**The call count of the other call scenarios.** Other UAS call scenarios
answer a REGISTER too (section "The REGISTER branch"). `scenario_run()` gives
each of them one call. A REGISTER that reaches such a run therefore takes that
call, and SIPp exits before the INVITE arrives. On a bench, the line therefore
registers against `uas_register_prime` first. `scenario_keys_get()` gives
that scenario the key `expires 3600`.

**Message log check.** Because SIPp reports success for two REGISTERs,
`postcheck_run()` reads the SIPp message log after each run, through
`service_route_msg_check()`. It requires at least one received INVITE.
Under `uas_service_route`, it also requires both granted hops on every
received INVITE, one `Route` header line each and in granted order. Under
`uas_service_route_none`, it refuses any `Route` header on a received INVITE.
It prints what it found, for example "2 INVITE(s) received, each with Route
[<sip:sbc1.example.net:5065;lr> <sip:sbc2.example.net:5065;lr>]", or its
reason to refuse the run.

**One scenario for two requests.** Each scenario must answer both the REGISTER
and the INVITE. A DUT places its call from a registered line, and SIPp takes
an out-of-call scenario in UAC mode alone. A UAS run therefore has exactly one
scenario for every request that reaches it. The two requests carry different
Call-IDs and open two instances of one scenario.

**Shared receive.** The first receive therefore matches a regular expression
over both method names (`regexp_match="true"`, SIPp 3.7.3 `call.cpp`
`matches_scenario`). That receive carries the branch itself
(`next="call" test="invite_seen"`), and its action list reads the method off
the start line. The branch must not be a `<nop>`: see section "When a failed
check is judged".

**Challenge.** `uas_service_route` challenges the INVITE 407 before it
answers. That is the field exchange, whose capture connects after a
credential exchange. That challenge puts three more requests on the
wire:

- ACK of the 407: it carries the route, because that ACK mirrors the request
  that it answers (`src/proto/sip/call.c` `req_mirror_build`);
- credential retry: it carries that route again;
- ACK of the 2xx: it carries none.

**Hop names.** Hops of `uas_service_route` name a host of their own,
`sbc1.example.net:5065` and `sbc2.example.net:5065`. That is the field
message's shape. Its Route named the SBC of SRV priority 0, while
its INVITE went to the SBC of priority 1. uvoice resolves no hop name, and a
hop never changes the next hop (`src/proto/sip/reg.h`). Each INVITE
therefore reaches the scenario, whatever names its hops carry.

**First INVITE.** A check on the first INVITE reads a variable and not the
message. The shared receive's action list runs for a REGISTER too. A REGISTER
must carry no Route: a Route on a REGISTER is a defect too. A
`check_it` there would therefore fail the registration instance. The first
receive captures the header value without a check. The 2xx's ACK then checks
that value with `search_in="var"`.

A retry INVITE and a 407's ACK arrive in the call branch alone, so each one
checks its own header directly. `start_line="true"` keeps every such
reader off a `Record-Route`, whose name ends in the one that a reader looks
for (SIPp 3.7.3 `call.cpp`, `extractSubMessage`).

What a run of these two scenarios alone reads, and what it does not read:

| Reading | `uas_service_route` | `uas_service_route_none` |
|---|---|---|
| `Route` on the REGISTER | not read: the shared receive serves both methods | read, and any value refused |
| first hop of each INVITE | read by the scenario | read, and any value refused |
| first hop on the 407's ACK | read by the scenario | no 407 sent |
| `Route` on the 2xx's ACK | read, and any value refused | read, and any value refused |
| both hops, their count and their order | read by `postcheck_run()` on every INVITE, never on the 407's ACK | read by `postcheck_run()` on every INVITE |
| `Route` on the BYE | read, and any value refused | read, and any value refused |
| daemon state behind the signaling | not read | not read |

The end-to-end harness reads all of them. It adds the hops of the 407's ACK
and the line state (`test/e2e/run_e2e.sh`, flows `service-route-absent` and
`service-route-present`).

### The REGISTER branch

**Branch.** Each UAS call scenario that an end-to-end flow drives answers a
REGISTER too, and so does `uas_churn`. A DUT can register against its peer
before its call, and a UAS run has one scenario for every request that
reaches it. The first receive therefore matches `REGISTER|INVITE` with
`regexp_match="true"`, as in `uas_service_route`. Its `next="call"
test="invite_seen"` sends an INVITE to the call. A REGISTER takes the branch,
which answers 200 and jumps past the last message. SIPp then counts that
instance as a successful call.

**200 response.** The answer copies the `Via` handling of `uas_register`: it
returns sent-by and branch with `received` and `rport`. It echoes the
`Contact`, and `[last_Expires:]` stands alone on its line. Reason: the keyword
renders a whole header line, so `Expires: [last_Expires:]` would write the
name twice. SIPp drops the line when a REGISTER carries no `Expires`
(`src/call.cpp`, the `E_Message_Last_Header` arm of `createSendingMessage`).

**INVITE checks.** The first receive's action list runs for a REGISTER as
well, so a `check_it` there would fail that REGISTER instance. The first
receive therefore stores values without a check. The call's next receive
checks those values with `search_in="var"`. That receive is the first final
answer's ACK, or the first PRACK.

**`uas_prack_180` arithmetic.** Its `todouble` and `add` actions move to the
first PRACK too. Reason: SIPp warns when it converts a value that no match
set, and a REGISTER sets none.

**Refresh.** Each refresh opens an instance of its own and gets the same
answer, so a long run keeps its binding. SIPp remembers a finished call for 33
seconds (`-deadcall_wait`) and discards a message that names it. A refresh
inside that window therefore gets no answer.

**Scenarios without the branch.** These scenarios keep a REGISTER answer of
their own: `uas_503_retry_after`, `uas_mwi_notify`, `uas_cfu_redirect`,
`uas_service_route`, `uas_service_route_none` and `uas_register_outage`. These
scenarios carry no call and have no branch: `uas_options_keepalive` and
`uas_stream_subscribe`. `uas_t38_accept` and `uas_t38_page` run by name on a
bench and have no branch either.

### When a failed check is judged

**Stored result.** A failed `check_it` or `check_it_inverse` does not stop a
call. SIPp 3.7.3 stores the failure in `last_action_result` of that call and
runs the rest of the scenario (`src/call.cpp`, the
`actionResult != E_AR_NO_ERROR` arm of `process_incoming`).
`call::terminate()` reads the value once, when the call ends. It turns a
stored failure into a failed call (`src/call.cpp`, the
`if(call::last_action_result != call::E_AR_NO_ERROR)` block at the head of
that function).

**A `<nop>` erases it.** One message type overwrites the value: the
`MSG_TYPE_NOP` arm of `call::executeMessage()` assigns
`last_action_result = actionResult` with no condition. A `<nop>` that runs
after a failed check therefore erases the failure, and the call ends as a
successful one. Other message types never erase the value:

- A `<send>` and a `<pause>` call `executeAction()` and drop its result.
- A `<recv>` writes the value only for a failure of its own.

**Rule.** In this suite, a `<nop>` must not run after a message whose actions
carry `check_it` or `check_it_inverse`. Two ways keep a `<nop>` out:

- Put the `<nop>`'s actions on the message before it. A `<send>` and a
  `<recv>` both accept an `<action>` block. An `<exec rtp_echo="...">` can
  therefore go on an ACK or a 200 in front of the `<nop>`.
- Put `next` and `test` on the receive itself, not on a `<nop>` behind it.
  `call::next()` reads both from the message that it ran last, so the jump
  does not change.

**A label at the end.** A label can stand last in a file with no message
behind it. `call::next()` ends the call as a successful one when a jump goes
past the last message. A `<nop/>` under a trailing `<label>` therefore adds
nothing, and it erases the failure of every check above it.

**Lint.** `test/tools/sipp_scenario_lint.sh` enforces the rule. It reads
every `test/sipp/*.xml` and every `test/e2e/*.xml`, the end-to-end harness's
out-of-call scenarios. It refuses a file where a `<nop>` stands, in file
order, behind a judging message. It prints one line per hit and exits 1. Each
line names the `<nop>`'s position among the messages, and the judging message
in front of it. A clean set gives one summary line and exit 0. An
unreadable directory or file, a set with no scenario, and an absent awk give
exit 2.

**Judging message.** A judging message is one whose actions can store a
failure that the call's end reads:

- any message whose actions carry `check_it` or `check_it_inverse` with a
  true value;
- a `<recv>` or a `<recvCmd>` whose actions carry an `exec rtp_echo`. That
  action returns `E_AR_RTPECHO_ERROR` into the member that a check writes.
  The same action inside a `<nop>` ends the call at once, so it needs no
  protection.

**True value.** SIPp's own rule defines "true". `get_bool()` in
`scenario.cpp` reads "true" and "false" without case, and falls back to
`strtol(value, NULL, 0)`. `check_it="1"`, `"0x1"` and `"2"` are therefore
checks, and `check_it="0"` and `check_it="false"` are not.

**Reader.** The lint's reader follows SIPp on three more points:

- It separates a CDATA body and a comment from markup, so a `<nop>` written
  inside either is text to it.
- It walks tag by tag and not line by line, so a check and the `<nop>` behind
  it count on one physical line.
- It allows CR, LF, tab and space on both sides of an attribute's `=`, as
  `xp_get_value()` does.

**A conservative lint.** The lint is conservative on purpose. It reads file
order and follows no jump, so it also refuses a `<nop>` that a label could
route around. It also refuses a check on a `<send>` or a `<pause>`, which SIPp
never judges. Reason: the rule costs one file edit, and a jump analysis needs
a second SIPp written in awk.

**Runners.** `run_sipp.sh` and `test/e2e/run_e2e.sh` call the lint once per
run, before any scenario or flow starts, beside their other prerequisite
checks. A hit stops the run and prints the lint's output. The lint ships
beside both runners, so an absent file is a broken copy. Both runners then
stop with an error, and neither skips the check. They call the lint through
`sh`, so it needs no execute bit.

**No new prerequisite.** The lint is written in POSIX sh and awk, so it adds
no prerequisite to a bench host: this README names no python3. Its verdicts
are identical under dash, busybox sh, gawk, mawk and busybox awk. Run it by
hand over any directory of scenarios:

```sh
test/tools/sipp_scenario_lint.sh test/sipp test/e2e
test/tools/sipp_scenario_lint.sh -h
```

### Outage scenario runs by name, not through `all`

`uas_register_outage` is absent from the `all` set. It asks an operator for
an action that no other scenario asks for. The DUT's WAN address must change
while the registrar is silent, and `all` arranges nothing on the DUT. Ask for
it by name:

```sh
./run_sipp.sh 192.0.2.10 uas_register_outage
```

**What it does.** The scenario grants ten seconds per binding, so the DUT
refreshes after five (`sip_reg_refresh_secs()`). The third receive takes that
refresh and answers nothing. The DUT then holds a binding that the registrar
still has, and an exchange that goes nowhere.

**Address change.** While that refresh is unanswered, change the DUT's WAN
address. The fourth receive then takes the REGISTER that this change produces.
That REGISTER carries the same Call-ID, a new address in its Via and Contact,
and its old Contact with `expires=0`. The fifth exchange is the refresh behind
it, which must carry that new Contact alone.

**Window.** The window for the change is timer F, 32 seconds at the default
T1. A change that arrives later meets a line whose binding lapsed. Such a line
rebuilds its registration under a new Call-ID and does not move it in place.

**Contact of the answer.** Each answer names the binding that this registrar
holds. A REGISTER that moves its Contact carries two bindings, and
`[last_Contact:]` would echo both. The scenario captures the first Contact
line instead, which is the new binding (`src/proto/sip/reg.c wire_build` writes
it first).

**Silence.** The silence needs no pause and no restart of SIPp.
Retransmissions of the unanswered refresh repeat its whole text (RFC 3261
17.1.2.2). SIPp drops a copy of the message that it received last (SIPp 3.7.3
`src/call.cpp`, the `last_recv_hash` arm of `process_incoming`). The copies
never reach the scenario's next receive.

**Unattended run.** The end-to-end harness runs the whole sequence unattended
(`test/e2e/run_e2e.sh`, flow `wan-flap-outage`). In that flow, a stub
`network.interface` object supplies the address change.

### Fax scenarios run by name, not through `all`

The fax scenarios are absent from the `all` set, apart from `uas_t38_reject`.
No single DUT configuration satisfies them together. `uas_t38_reject` needs
`fax_t38_enable` 0, the shipped value, and other T.38 scenarios need it 1. The
runner resolves each scenario by name:

```sh
./run_sipp.sh 192.0.2.10 uas_t38_accept
```

What each one needs of the DUT:

| Scenario | `fax_t38_enable` | DUT action during the run |
|---|---|---|
| `uas_t38_reject` | `0` | place a call |
| `uas_t38_accept` | `1` | place a call |
| `uas_t38_page` | `1` | Start `t38-peer --answer --receive FILE`, with `--port` set to `SIPP_MEDIA_PORT + 2`, then start SIPp. The DUT terminal sends a page, then hangs up. |
| `uas_fax_upspeed` | either | place a call, then a CED tone toward the phone |
| `uas_fax_upspeed_auth` | either | place a call, then a CED tone toward the phone. The channel needs a `sip_secret`, because this scenario challenges the upspeed re-INVITE. |
| `uas_far_fax_upspeed` | either | place a call, then a CED tone from the wire within ten seconds of the re-INVITE that this scenario sends. It runs against a channel with G.711 and against one provisioned with G.729 alone. Both must answer the narrowing offer with a G.711. |
| `uas_far_fax_tone_g711` | either | place a call, then a CED tone from the wire inside the sixteen-second quiet window that this scenario holds. For the false tone case, also that tone's end row with a duration under 2 s, inside that window. The DUT must send no re-INVITE of its own. |
| `uas_fax_upspeed_false` | either | place a call, then a CED tone toward the phone and that tone's end row with a duration under 2 s. Once the leg is back on voice, a second CED tone. |
| `uac_t38_offer` | `1` | answer the call, then a CED tone toward the phone; after the upspeed exchange, a V.21 preamble toward the phone |
| `uac_t38_page` | `1` | Start `t38-peer --call --send FILE`, with `--port` set to `SIPP_MEDIA_PORT + 2`, then start SIPp. The DUT terminal answers the page, then hangs up. |
| `uac_t38_reject` | `1` | answer the call, then a CED tone toward the phone; after the upspeed exchange, a V.21 preamble toward the phone |

Run both page scenarios with `SIPP_TIMEOUT_SECONDS=600`. A page takes about
one minute. A 180-second BYE wait covers a slow page and keeps the local BYE
fallback reachable. Each scenario answers the BYE that the DUT sends.

### Proving the two T.38 page scenarios on loopback

A real page needs `t38-peer` and a fax terminal, so no end-to-end flow loads
`uac_t38_page` or `uas_t38_page`. Every check inside these two files reads
signaling alone. Two support scenarios drive those checks with no media. A
developer can therefore prove an edit to either file on any host.

Run each pair inside one network namespace, with its loopback up. One command
starts the peer, and the other runs the scenario. Their order differs because
the caller differs: the answering side starts first. Each pair has a
namespace of its own, and peer and scenario use different ports, so the two
pairs do not interfere.

`uac_t38_page`, where the scenario places the call:

```sh
unshare -rn sh -c '
ip link set lo up
sipp 127.0.0.1:5160 -sf test/sipp/t38_page_peer_uas.xml -m 1 -p 5170 \
     -mp 6200 -i 127.0.0.1 -nostdin -timeout 45 -timeout_error &
sleep 1
SIPP_PORT=5160 SIPP_MEDIA_PORT=6100 SIPP_TIMEOUT_SECONDS=45 \
     test/sipp/run_sipp.sh 127.0.0.1:5170 uac_t38_page
wait'
```

`uas_t38_page`, where the scenario answers the call:

```sh
unshare -rn sh -c '
ip link set lo up
SIPP_PORT=5160 SIPP_MEDIA_PORT=6100 SIPP_TIMEOUT_SECONDS=45 \
     test/sipp/run_sipp.sh 127.0.0.1:5170 uas_t38_page &
sleep 1
sipp 127.0.0.1:5160 -sf test/sipp/t38_page_peer_uac.xml -m 1 -p 5170 \
     -mp 6200 -i 127.0.0.1 -nostdin -timeout 45 -timeout_error
wait'
```

Both runs end with `PASS` and one successful call on each side. A peer that
breaks one rule fails the scenario by that rule:

- If `t38_page_peer_uas` has no `a=T38MaxBitRate:14400`, `uac_t38_page` fails
  on that check.
- If `m=audio` has a live port in `t38_page_peer_uac`, `uas_t38_page` fails on
  the withdrawn audio line.

On hardware, the two tones come from a real fax terminal on the port. The
end-to-end harness injects them over `voice.stub` instead. It runs these fax
flows unattended (`test/e2e/run_e2e.sh`):

- `uas_fax_upspeed`
- `uas_fax_upspeed_auth`
- `uas_far_fax_upspeed`
- `uas_far_fax_tone_g711`
- `uas_fax_upspeed_false`
- `uas_far_fax_tone_false`
- `uac_t38_offer`
- `uac_t38_reject`

Both false flows also inject a tone's end row. No hardware detector reports
that row until the tone really ends.

`uas_far_fax_upspeed` runs twice in the harness, as `uas_far_fax_upspeed` and
`uas_far_fax_upspeed_g729`. Both flows use one scenario file against two DUT
codec configurations. Reason: whether the DUT follows the upspeed also
depends on its own leg's codec list.

### Challenged fax upspeed

`uas_fax_upspeed_auth` is `uas_fax_upspeed` with a challenge on the upspeed
re-INVITE. In a field capture, the switch answers the first
`0 8` re-INVITE 401. The unit repeats the offer with credentials 62 ms later.
The scenario plays that switch. It sends a 401 and takes its ACK. It then
requires a retry with nonce `uvoice-fax-nonce` and opaque
`uvoice-opaque-fax` in `Authorization` (RFC 3261 section 22.2, RFC 2617
section 3.2.1).

The retry carries the three assertions that `uas_fax_upspeed` puts on its one
re-INVITE:

- format list `0 8 101` exactly;
- no `a=rtpmap:18`;
- no `a=rtpmap:13 CN`.

A clamp holds the leg's codec list at G.711 across that challenge
(`src/proto/sip/call.c`, `loc_g711_clamp`). The DUT therefore builds its retry
from that list again, and the checks read what the wire really carried.

The channel needs a `sip_secret`. A channel with none still answers the
challenge, over an empty password (`src/proto/sip/account.c`,
`acct_cfg_derive`). A run without a secret therefore measures a digest that no
provider would accept.

Three INVITE receives stand in the file: offer, challenged upspeed and
credential retry. A fourth INVITE finds no receive left, and SIPp aborts a
call on a request that it does not expect (SIPp 3.7.3 `src/call.cpp`,
`process_unexpected`).

The end-to-end harness runs it unattended, in the flow `uas_fax_upspeed_auth`.

### Four SRTP scenarios run by name too

`uas_srtp_call`, `uac_srtp_call`, `uac_srtp_plain_hold` and `uac_savp_refused`
are absent from the `all` set. As for the fax scenarios, no single DUT
configuration satisfies them together. Three need `srtp_enabled` 1.
`uac_savp_refused` needs `srtp_enabled` 0, because its refusal tests that
value. Ask for each one by name:

```sh
./run_sipp.sh 192.0.2.10 uac_srtp_call
```

What each one needs of the DUT:

| Scenario | `srtp_enabled` | DUT action during the run |
|---|---|---|
| `uas_srtp_call` | `1` | place a call, then hang up after the answer |
| `uac_srtp_call` | `1` | answer the call |
| `uac_srtp_plain_hold` | `1` | answer the call, flash to hold, then flash to resume |
| `uac_savp_refused` | `0` | answer the call; the DUT sends its 488 at that point |

The refusal needs the handset, because a refusal of this kind belongs to an
answer and not to a ring. Pre-ring checks read the offered address and its
family alone. The line therefore rings first, and the DUT's answer then
refuses the offer.

Scenario keys are fixed and readable on purpose. Each key is 30 ASCII bytes,
so a reader can decode it. A fixed key also lets the end-to-end harness
compute the digest that the backend must report.

The end-to-end harness runs all four unattended and asserts the stream behind
the signaling. It uses these flows of `test/e2e/run_e2e.sh`:

- `srtp-outbound`
- `srtp-inbound`
- `srtp-accept-plain`
- `srtp-refused`

### Three stream scenarios run by name too

`uas_stream_call`, `uac_stream_call` and `uas_stream_subscribe` are absent from
the `all` set. Each one needs the DUT on a reliable transport
(`voip.base.transport` `tcp` or `tls`), and the `all` set runs one DUT
configuration. Ask for each one by name. On hardware, give the runner the
matching `-t` mode through `SIPP_TRANSPORT`.

| Scenario | `transport` | DUT action during the run |
|---|---|---|
| `uas_stream_call` | `tcp` or `tls` | place a call, then hang up after the pause |
| `uac_stream_call` | `tcp` or `tls` | answer the call |
| `uas_stream_subscribe` | `tcp` or `tls` | register with `mwi_subscribe` 1 |

Stream scenarios carry no `rtp_echo` on purpose. A TLS-enabled SIPp routes
`rtp_echo` through its SRTP contexts. On a plain RTP/AVP session, it then ends
the run with `EXIT_RTPCHECK_FAILED`. A scenario that carries the action
therefore cannot serve the tls twin. One file serves both twins instead, and
that makes them twins: transport is the only difference between the two runs.
`test/e2e/README.md`, section "The two SIPp builds", states the rule.

Stream scenarios also name no transport in a `regexp`. A SIPp `regexp`
attribute takes no SIPp keyword, so a scenario cannot assert the transport
that the run was given. The end-to-end harness reads the `Via` token and
`Contact` parameter from the SIPp message log instead (`test/e2e/run_e2e.sh`,
the `stream-*` flows).

`uas_stream_subscribe` matches its first request with a regular expression
over two method names, `REGISTER|SUBSCRIBE` (`regexp_match="true"`, SIPp 3.7.3
`call.cpp` `matches_scenario`). It must answer both requests. Reason: the DUT
subscribes only from a registered line. "One scenario for two requests" in
section "The two service route scenarios" gives the rest of that reason.

### PRACK scenarios

PRACK scenarios of the table below are absent from the `all` set, and other
PRACK scenarios are in it. As for the fax and SRTP scenarios, each one needs a
DUT option that the one configuration behind `all` does not carry. Every
scenario that reads a reliable 180 from the DUT needs `prack` 1. Reason:
without that option, the DUT sends no reliable 180 at all
(`src/proto/sip/call.c` `rel_wanted()`).

| Scenario | DUT option | DUT action during the run |
|---|---|---|
| `uac_prack_require` | `prack` 1 | answer the call |
| `uac_prack_supported` | `prack` 1 | answer the call |
| `uac_prack_bad_rack` | `prack` 1 | answer the call |
| `uac_prack_late` | `prack` 1 | answer the call |
| `uac_prack_cancel` | `prack` 1 | none; the call is canceled |
| `uac_prack_giveup` | `prack` 1 and `sip_timer_t1` 100 | none; the call is never answered |

Ask for each one by name with its option in place:

```sh
./run_sipp.sh 192.0.2.10 uac_prack_require
./run_sipp.sh 192.0.2.10 uac_prack_supported
./run_sipp.sh 192.0.2.10 uac_prack_bad_rack
./run_sipp.sh 192.0.2.10 uac_prack_late
./run_sipp.sh 192.0.2.10 uac_prack_cancel
./run_sipp.sh 192.0.2.10 uac_prack_giveup
```

SIPp 3.7.3 supports `<test variable2="...">` comparisons and
`<nop next="..." test="...">` jumps. `uas_prack_*` scenarios use
`<strcmp variable2="...">`. It compares the CSeq number inside a captured
RAck with the DUT's INVITE CSeq. `uas_prack_180` also uses
`<test compare="equal" variable2="...">`, which compares the PRACK CSeq with
the INVITE CSeq plus one.

To check that a PRACK does not arrive, a scenario uses `ontimeout` and a
`<label>`, not a jump. The timeout is the passing path. An `<error>`
action on the unwanted `recv` stops the run.

`uas_prack_*` scenarios build the INVITE's 200 from variables, not from
`[last_Via:]` and `[last_CSeq:]`. By then, the last message received is the
PRACK, whose Via carries another transaction's branch.

`uac_prack_bad_rack` gives each PRACK its own CSeq number, one higher than
the last. The dialog layer answers a repeated or lower number 500 before the
PRACK handler runs. Four PRACKs under one number would therefore test the
wrong refusal. This scenario also writes the 180's To tag from a captured
variable. Reason: `[peer_tag_param]` follows the last received
response's To header, and the foreign dialog's 481 echoes that dialog's tag.

SIPp absorbs a retransmitted response that it received before. No scenario of
this suite therefore counts copies of an unacknowledged reliable 180. The
end-to-end harness counts them from the daemon log and the capture instead
(`test/e2e/run_e2e.sh`, flows `prack-late` and `prack-giveup`).

The end-to-end harness runs all PRACK scenarios unattended, in the `prack-*`
flows of `test/e2e/run_e2e.sh`. `uas_prack_183_sdp` runs twice there: once on
the default `early_media_enabled` 1, and once with that option clear.

### Transfer scenarios run by name too

Transfer scenarios are absent from the `all` set, for the reason that keeps
the fax, SRTP and stream scenarios out. Each one needs the subscriber to act
during the run. Several also need a second scenario to answer a call that the
DUT places by itself. Ask for each one by name.

| Scenario | out-of-call scenario | DUT action during the run |
|---|---|---|
| `uas_xfer_blind` | none | place a call, flash, dial `*072` and a target |
| `uas_xfer_blind_grace` | none | place a call, flash, dial `*072` and a target |
| `uas_xfer_attended` | none | place a call, flash, dial `*073` and a target |
| `uas_xfer_failed` | none | place a call, flash, dial `*073` and a target |
| `uac_xfer_refer` | `xfer_peer_ooc` | answer the call |
| `uac_xfer_refer_replaces` | `xfer_peer_ooc` | answer the call |
| `uac_xfer_child_fail` | `xfer_busy_ooc` | answer the call |
| `uac_xfer_onhook` | `xfer_replaced_ooc` | answer the call, flash, dial `*074` and a target, wait until the consulted peer answers, then go on hook with both up |
| `uac_xfer_onhook_blind` | `xfer_ringing_ooc` | with `local_conf_enabled` 2, answer the call, flash, dial a second number, then go on hook while that call rings |
| `uac_xfer_replaces` | `xfer_peer_ooc` | place a call and stay on it |
| `uac_xfer_two_line` | `test/e2e/register_ooc.xml` (note) | on line 1 place a call, flash, dial `*073` and line 2's number; then answer line 2 |

Notes:

- **`uac_xfer_two_line`.** `uas_xfer_attended` answers the call of line 1 in a
  separate SIPp run, which ends before this run starts. In the end-to-end flow
  `xfer-two-line`, `xfer_two_line_run()` passes `test/e2e/register_ooc.xml` as
  the second run's out-of-call scenario. That scenario answers a
  REGISTER.

A run of a scenario with an out-of-call half uses three settings:
`SIPP_OOC_SCENARIO`, `SIPP_CALL_LIMIT` and `SIPP_SERVICE`. For example:

```sh
SIPP_OOC_SCENARIO=./xfer_peer_ooc.xml SIPP_CALL_LIMIT=3 \
./run_sipp.sh 192.0.2.10 uac_xfer_refer
```

`SIPP_CALL_LIMIT` is the SIPp `-l` value. It is headroom and not a
requirement. Reason: `-l` bounds the outgoing call generator alone, and an
instance that the out-of-call scenario opens never passes through it. `-m`
therefore bounds these runs. A higher limit still matches a run that holds
three instances at once:

- a call that this scenario places;
- a call that the DUT places back at it;
- an instance that the DUT's REGISTER aborts.

`SIPP_SERVICE` sets the request URI's user part. The two-line scenario needs
it to name the second subscriber.

Transfer scenarios assert no exact `Refer-To`, `Referred-By` or promoted
`Replaces` value against a fixed pattern. A SIPp regexp is an XML attribute
value. It therefore writes the `<` that opens a name-addr as `&lt;` (RFC 3261
25.1; Refer-To grammar in RFC 3515 2.1). `uas_service_route.xml` uses that
form. `uac_conf_onhook_xfer_auth` compares two whole `Refer-To` values with a
`strcmp` (section "Challenged on-hook conference transfer"). Transfer scenario
files check these items:

- message order;
- sipfrag codes;
- subscription states;
- presence or absence of a Replaces.

The end-to-end harness checks the values byte for byte, from the SIPp message
log (`test/e2e/run_e2e.sh`, `xfer_hdr_check`).

The end-to-end harness runs all transfer scenarios unattended, in
`xfer-*` flows of `test/e2e/run_e2e.sh`. Those flows assert the daemon state
behind the signaling.

### Challenged on-hook conference transfer

`uac_conf_onhook_xfer_auth` is `uac_conf_onhook_xfer` with a challenge on each
request that the on-hook transfer sends. It answers the REFER 401
and the BYE 401, and requires each one again with credentials (RFC 3261
section 22.2). It is absent from the `all` set for the reason that keeps
`uac_legacy_hold_conf` out: it needs an out-of-call scenario, which `all`
arranges for no scenario.

| Scenario | out-of-call scenario | DUT action during the run |
|---|---|---|
| `uac_conf_onhook_xfer_auth` | `xfer_replaced_ooc` | answer the call, flash, dial a second number, flash to join, then go on hook with both up |

```sh
SIPP_OOC_SCENARIO=./xfer_replaced_ooc.xml SIPP_CALL_LIMIT=3 \
./run_sipp.sh 192.0.2.10 uac_conf_onhook_xfer_auth
```

The two challenges carry different nonces, `uvoice-refer-nonce` and
`uvoice-bye-nonce`. A DUT that reads a nonce and realm again takes them as a
refusal of its credentials, and it answers nothing (`src/proto/sip/auth.c`,
`sip_auth_challenge_take`). A BYE challenge with the REFER nonce would
therefore get no retry and check nothing.

The scenario checks the retry's `Refer-To` by comparison. Reason: no scenario
can write that value down. It carries the replaced dialog's Call-ID and
from-tag, and the DUT generates both at run time. That comparison has three
steps:

1. An `ereg` captures the first REFER's whole header.
2. A second `ereg` captures the retry's header.
3. A `strcmp` with `check_it` requires both to be equal.

The retry builds its `Refer-To` again and does not replay the request
(`src/proto/sip/call.c`, `refer_resend`). A retry that lost its Replaces would
give one call away, where the subscriber asked to join two. The scenario also
reads the escaped Replaces for its shape. Its to-tag is the fixed tag of
`xfer_replaced_ooc.xml`.

A third REFER or a third BYE finds no receive left in the scenario. SIPp
aborts a call on a request that it does not expect (SIPp 3.7.3
`src/call.cpp`, `process_unexpected` and the `E_AM_UNEXP_BYE` arm of
`automaticResponseMode`).

The end-to-end harness runs it unattended, in the flow `conf-onhook-xfer-auth`.

## Runner

Run one scenario:

```sh
./run_sipp.sh 192.0.2.10 uas_register_auth_2069
```

Run selected scenarios or the full suite:

```sh
./run_sipp.sh 192.0.2.10 uas_register uas_register_423
./run_sipp.sh 192.0.2.10 all
```

If `tcpdump` is available, the runner starts it. The runner uses `tshark` for
the 491 and 503 timestamp checks. If packet analysis is unavailable, those
scenarios fail.

If the defaults do not match the test host, set these variables:

| Variable | Default | Purpose |
|---|---:|---|
| `SIPP_LOCAL_IP` | route-selected | SIP signaling source address. |
| `SIPP_PORT` | `5060` | Local SIP listen port. |
| `SIPP_MEDIA_PORT` | `6000` | Base RTP port. |
| `SIPP_RECV_TIMEOUT_MS` | `120000` | SIPp receive timeout in milliseconds. |
| `SIPP_TIMEOUT_SECONDS` | `180` | SIPp global timeout in seconds. |
| `SIPP_CAPTURE_INTERFACE` | `any` | Packet capture interface. |
| `SIPP_ARTIFACT_DIR` | `artifacts` in the directory of `run_sipp.sh` | Run artifact directory. |
| `SIPP_RFC2833_CHECK` | `0` | Enable the telephone-event packet check. |
| `SIPP_RFC2833_PAYLOAD` | `101` | Expected telephone-event RTP payload type. |
| `SIPP_TRANSPORT` | none | SIPp `-t` mode. Empty keeps the udp command line. Use `t1` for a tcp DUT and `l1` for a tls one. |
| `SIPP_TLS_CERT` | `../unit/tls/server.crt` from the directory of `run_sipp.sh` | Certificate SIPp presents under `l1` or `ln`. |
| `SIPP_TLS_KEY` | `../unit/tls/server.key` from the directory of `run_sipp.sh` | Private key of that certificate. |
| `SIPP_BIN` | `sipp` | SIPp binary to run, the way `test/e2e/run_e2e.sh` names it. |
| `SIPP_OOC_SCENARIO` | none | SIPp `-oocsf`, the scenario that answers a request belonging to no call the run placed. The transfer scenarios name one. |
| `SIPP_SERVICE` | none | SIPp `-s`, the request URI's user part that a UAC scenario writes. |
| `SIPP_CALL_LIMIT` | call count; `1` for `uac_inbound_churn` | SIPp `-l`, a bound on the outgoing call generator. Out-of-call instances bypass it, so it is headroom for a run that holds several at once and not a requirement. |
| `SIPP_KEYS` | scenario's own list | SIPp `-key` pairs of the running scenario, as whitespace separated "keyword value" pairs. `uac_verstat_call` defaults to `verstat No-TN-Validation` and `uas_register_prime` to `expires 3600`; every other scenario names no keyword. |

A `l1` or `ln` run needs a SIPp built with `USE_SSL=ON`, which is a second
binary. Reason: the TLS build fails every scenario that carries `rtp_echo`
(`test/e2e/README.md`, section "The two SIPp builds"). Name that build with
`SIPP_BIN`, and do not put it on PATH. The udp scenarios then keep the
non-TLS build:

```sh
SIPP_TRANSPORT=l1 SIPP_BIN=/path/to/sipp-tls/sipp \
./run_sipp.sh 192.0.2.10 uac_stream_call
```

### Running a scenario over IPv6

A scenario runs over IPv6 as it runs over IPv4. Name the scenario, and give
the runner a v6 source address and a v6 DUT address:

```sh
SIPP_LOCAL_IP=fd00:5150::1 ./run_sipp.sh fd00:5150::2 uas_register
```

`SIPP_LOCAL_IP` is SIPp option `-i`. It sets the family of every socket that a
run opens. It must name an address of the DUT's family. Reason: SIPp resolves
the DUT address in the family of its local address.

If the DUT address carries a port, put the address in brackets. SIPp splits
an unbracketed argument into host and port at a single colon. It reads a
second colon as a v6 address with no port. A bare `fd00:5150::2` is therefore
that address on 5060, and `'[fd00:5150::2]:5061'` is the same address on
port 5061. Quote the bracketed form, because otherwise the shell reads `[` as
a pattern.

The bracketed form loses the capture. The runner builds its tcpdump filter as
`host <DUT address>`, and `host [fd00:5150::2]:5061` is not a filter
expression. tcpdump refuses it, and the run reports the capture as skipped.
`uas_491_glare`, `uas_glare_mutual` and `uas_503_retry_after` read the capture
and fail without it. Give those three the bare form on port 5060.

Four keywords carry an address into a message, and they differ:

| Keyword | Renders | Where it belongs |
|---|---|---|
| `[local_ip]`, `[remote_ip]` | bracketed under v6, bare under v4 | a URI host, where RFC 3261 19.1.1 asks for the brackets |
| `[remote_host]` | bare under both | a `received` parameter, which RFC 3261 20.42 gives a bare address |
| `[media_ip]` | bare under both | `c=` and `o=` addresses, which RFC 4566 sections 5.2 and 5.7 give bare |

If a run passes no `-mi`, `[media_ip]` follows `[local_ip]`, and
`[media_ip_type]` follows `[local_ip_type]`. A scenario that writes
`o=sipp 1 1 IN IP[media_ip_type] [media_ip]` therefore renders the same bytes
under v4 as one that writes the local keywords. Under v6, it renders a legal
origin line. Scenarios of the end-to-end v6 set use that form:

- `uas_basic_call`
- `uac_basic_call`
- `uas_reinvite_hold`
- `uac_cancel`
- `uac_srtp_call`
- `uac_stream_call`
- `uas_stream_call`
- `uac_t38_offer`

PRACK scenarios, both service route scenarios, `uac_verstat_call`,
`uac_inbound_churn`, `uac_refused_480` and `uas_183_sdp` use that form too.
Every other scenario with an origin line writes the local keywords in it.
Under v6, such a scenario puts a bracketed address in its origin line, which
RFC 4566 section 5.2 does not allow. Every `c=` line that names an address of
this side uses the media keywords.

Every scenario that writes a `received` parameter writes
`received=[remote_host]`, for example `uas_register.xml`,
`uas_register_auth_2069.xml` and `uas_register_auth_qop.xml`. Reason: the
bracketed form of `[remote_ip]` is not a `received` value.

The host needs a working v6 address of its own. If the host disables IPv6,
`unshare -rn` with `ip link set lo up` gives a namespace where `::1` binds.
`test/e2e/run_e2e.sh` uses such a namespace for its own v6 run.

Scenario header comments cite the applicable RFC sections, except in these
five files, which cite no RFC:

- `t38_page_peer_uas.xml`;
- `uac_xfer_two_line.xml`;
- `uas_star_dial.xml`;
- `xfer_busy_ooc.xml`;
- `xfer_peer_ooc.xml`.

The RFC 2069 challenge has the form of a live provider's challenge: opaque
present, no qop and no algorithm.
