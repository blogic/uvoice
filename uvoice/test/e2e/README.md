# uvoice daemon end-to-end tests

`run_e2e.sh` builds and tests the real `uvoiced` daemon with the stub HAL.
SIPp acts as peer, registrar and RTP echo endpoint. A scripted call agent,
`mgcp_ca.py`, is the peer of the MGCP flows (section "MGCP flows and call
agent").

The runner covers these flows:

| Name | Purpose |
|---|---|
| `flow-a` | Place an outbound UDP call to a SIPp UAS. Require dial tone, CONNECTED, and media on the negotiated port. Release after the scenario pause, which sends BYE, and require a stopped stream and IDLE. (note) |
| `flow-b` | Receive an inbound UDP call from a SIPp UAC. Require RINGING with caller ID, then answer into CONNECTED with media on the negotiated port. After the peer's BYE, require a stopped ring and a stopped stream. A hook release then returns line 0 to IDLE. (note) |
| `flow-c` | Drive verbs `voice seize`, `release` and `dial` end to end. Read back off-hook state, answered call, live media query, clear and call record. (note) |
| `uas_reinvite_hold` | Place an outbound call and run the committed remote hold and resume scenario. The peer sends hold and resume re-INVITEs, and the DUT must answer them 200 with `a=recvonly` and `a=sendrecv`. (note) |
| `uac_reinvite_hold` | Drive the DUT's hold and resume re-INVITEs with two stub flashes, and require both ACKs. Require hold silence on stream 0: `tx_silence` is true while the leg is held and false once it resumes. (note) |
| `uas_offerless_reinvite` | Place an outbound call and run the committed offerless re-INVITE scenario, `uas_offerless_reinvite.xml`, through the generic dial, run and release path of `committed_outbound_run`. Require IDLE and two successful SIPp instances: REGISTER and call. |
| `uas_t38_reject` | Place an outbound call and run the committed T.38 rejection scenario, `uas_t38_reject.xml`, through the generic path of `committed_outbound_run`. Unlike `uac_t38_reject`, this flow leaves T.38 refusal to its peer scenario and checks only two successful instances and IDLE. |
| `uas_session_timer_422` | Place an outbound call against a peer that answers its first INVITE 422 Session Interval Too Small. Require the corrected INVITE, with an 1800-second session timer, to connect. Then release and require IDLE. (note) |
| `uac_cancel` | Receive an inbound call that the peer cancels behind its 180. Require ring, cancel and ring stop in order, no media, and IDLE. (note) |
| `uas_491_glare` | Place an outbound call and drive the DUT's hold re-INVITE with a flash. The peer answers 491, and the DUT's retry must follow that 491 by 2.1 to 4.0 seconds (RFC 3261 section 14.1 backoff). (note) |
| `uas_503_retry_after` | The registrar answers REGISTER 503 with Retry-After and accepts a retry. Require that retry at least 5.0 seconds behind the 503, and a registered line with `last_code` 200. (note) |
| `reg-403-reorder` | The registrar challenges REGISTER and refuses its credential with 403 (`uas_register_403.xml`). Require status `failed` with `last_code` 403, then a seize that plays reorder (stub tone 6) in CLEARING. Digits and a flash over reorder place no call, and a new INVITE gets 480 with no ring. (note) |
| `reg-none-reorder` | Nothing answers REGISTER, while a literal proxy gives line 0 a target. Timer F is 8 s and the retry 60 s. Require reorder at a seize in `trying` and again in `backoff` with `last_code` 408. A dial verb then seizes into reorder and places no call. |
| `uas_mwi_notify` | Send an unsolicited message-summary NOTIFY and require its 200. Read message waiting state and both counts from `daemon.log`. A second NOTIFY clears that state in `voice status`, and a NOTIFY of an unserved package must be answered 489. (note) |
| `uas_cfu_redirect` | Set local unconditional call forwarding and offer a call. Require a 302 that carries the configured target, while line 0 never rings. (note) |
| `uas_fax_upspeed` | Drive the fax answer tone trigger on a call committed to G.729. Require a G.711-only upspeed re-INVITE, with the passthrough profile on stream 0. (note) |
| `uas_fax_upspeed_auth` | Same trigger, against a peer that answers the upspeed re-INVITE 401. Require a credential retry inside one second of the 401. The flow runs by name, outside the default set. (note) |
| `uas_far_fax_upspeed` | Calling side of that call. Report an answer tone from the wire, and require that it sets the modem kind and applies no profile. Then send a G.711-only re-INVITE. The DUT must follow it into fax passthrough with no offer of its own, and `fax_switched` stays false. |
| `uas_far_fax_upspeed_g729` | Same scenario, against a channel with G.729 alone, which shares no codec with that offer. Require the DUT to answer with fallback G.711 and to convert its DSP on that codec, not to refuse with 488. |
| `uas_far_fax_tone_g711` | Calling side with no upspeed behind the tone. Answer with PCMU and telephone-event, report an answer tone from the wire, and require the modem kind alone on the committed codec. The scenario holds a 16 s quiet window, sized for `uas_far_fax_tone_false` too. (note) |
| `uas_fax_upspeed_false` | Answering side, with a false answer tone. Convert on a local CED, then report that tone's end row with a 900 ms reading. Require the leg back on voice after its deferral. (note) |
| `uas_far_fax_tone_false` | Calling side of that rule, on the `uas_far_fax_tone_g711` scenario. Set the modem kind from a CED on the wire, then report its end row at 900 ms, and require the leg back on voice. (note) |
| `uac_t38_offer` | Answer an inbound G.729 call, convert it to passthrough, and report a V.21 preamble toward the phone. Require a T.38 upgrade on the wire, on stream 0, in `voice status` and in its call record. (note) |
| `uac_t38_reject` | Same call, with its image offer answered 488. Require G.711 failover: passthrough profile on stream 0 and a G.711 offer behind it. No second image session follows, and its call record names G.711. (note) |
| `srtp-outbound` | Place an outbound call with `srtp_enabled` 1, and require an SDES exchange and a keyed stream. The offer carries two crypto attributes, and the answer mirrors one. Require suite and both key digests on stream 0, and a zeroize at the stop. (note) |
| `srtp-inbound` | Answer an inbound RTP/SAVP call with two offered crypto attributes. Require exactly one mirrored attribute in the 200, its suite in `voice status`, both key digests on stream 0, and a zeroize at the stop. (note) |
| `srtp-accept-plain` | Answer an inbound RTP/AVP call with `srtp_enabled` 1, and require an RTP/AVP answer with no crypto attribute. Require no `srtp` key in `voice status`, and a running stream with no suite or key digest. Hold and resume re-INVITEs stay plain, and the stop zeroizes. (note) |
| `srtp-refused` | Offer RTP/SAVP to a DUT with `srtp_enabled` 0, and require 488 with no stream ever keyed. (note) |
| `stream-uac-tcp` | Place an outbound call over TCP. Require `SIP/2.0/TCP` in the INVITE and REGISTER Via, and `;transport=tcp` in both Contact URIs. Require media on the negotiated port, and the line's `transport`, `connection.state` `up`, `connection.peer` and `connection.queued` keys. (note) |
| `stream-uac-tls` | Same call over TLS through the second SIPp build, with `SIP/2.0/TLS` and `;transport=tls`. No certificate bundle is installed, so line 0 runs client only and `listener` is `false`. (note) |
| `stream-uas-tcp` | Answer an inbound call on the TCP listener. Require `;transport=tcp` in the 180 and 200 Contact, and `listener` `true` with a live `connection` table. Require a CRLF ping answered inside the connection, and the line's own CRLF keepalive counted. (note) |
| `stream-uas-tls` | Same inbound call on the TLS listener, with committed EC fixtures composed into `/tmp/voip_tls`. A capture of both signaling ports must carry no cleartext SIP line, no bare CRLF keepalive and no UDP packet. (note) |
| `stream-register-tcp` | Register and keep alive over TCP. Require `SIP/2.0/TCP` in the Via of REGISTER and of the OPTIONS ping, and `;transport=tcp` in the REGISTER Contact. Require one OPTIONS answered 200 and counted, and a CRLF ping answered inside the connection. (note) |
| `stream-register-tls` | Same registration and keepalive over TLS, through `stream_register_run tls`. The bundle is installed, because the listener answers the CRLF ping and a TLS context needs its server half. It runs the TCP flow's checks with the TLS token. |
| `stream-subscribe-tcp` | Subscribe to the message-summary event over TCP. Require `SIP/2.0/TCP` in the SUBSCRIBE Via and `;transport=tcp` in its Contact. (note) |
| `stream-subscribe-tls` | Same subscription over TLS (`stream_subscribe_run tls`), on a client-only line. The DUT is client of both requests, so no bundle is installed. Checks and the two-instance verdict follow the TCP flow. |
| `cert-fetch` | Fetch a TLS certificate bundle from a local http server through `uvoiced`. Require all three outcomes: install, unchanged ETag, and an unsupported scheme refused with the bundle still standing. (note) |
| `reload-loop` | Drive thirty live `voice reload`s over an installed TLS bundle. Each reload moves a `tls_*` option, so the certificate helper starts and the daemon rebuilds line 0 in one frame. Require its listener back after each reload, no failed bind and no line-less firewall reconcile. (note) |
| `xfer-blind` | Place a call, flash, dial `*072` and a target, and require a blind referral. The DUT sends no BYE: the peer plays a switch's order behind its 202 and ends that leg itself. Its call record carries the transfer key. (note) |
| `xfer-blind-grace` | Same gesture, against a peer that sends no BYE of its own. The DUT sends one BYE, one grace behind its answer to the verdict NOTIFY, read from SIPp's message log. |
| `xfer-attended` | Same call with `*073`, completed by a sipfrag 200. The machine ends in OFFHOOK_DIAL, a fresh dial tone. The DUT sends its BYE one grace behind its answer, and its call record carries the transfer key. (note) |
| `xfer-failed` | Same referral, refused with a sipfrag 486. The held call resumes, the machine returns to `CONNECTED`, and a hook release ends the call. Its call record carries no transfer key. (note) |
| `star-network` | Same gesture on the shipped config, which carries no star code. `star_codes_local` defaults to 0, so `*072` and its target form a plain star string for the dial plan. Require a second INVITE to the whole string, escaped per the shipped `digit_esc_enabled` 1 (`sip:%2A072...`). (note) |
| `xfer-transferee` | Answer a call and be referred. Require both notifications, and a child INVITE to the `Refer-To` stripped of its URI headers, with its `Referred-By` copied verbatim and no `Replaces`. (note) |
| `xfer-transferee-replaces` | Same referral, with an escaped Replaces in the `Refer-To`. Require exactly one promoted `Replaces` header on the child INVITE, and `Require: replaces` beside it (RFC 3891 section 4). (note) |
| `xfer-child-fail` | Same referral, with the child refused 486. The 486 reaches the referrer in its final notification, and the original call keeps its name, stream and record. (note) |
| `xfer-replaces` | Be target of a completed attended transfer (RFC 3891). The DUT answers a second call whose `Replaces` names the live dialog with no ring and no offer to the machine. It BYEs the replaced dialog, and the replacement takes its name. (note) |
| `xfer-onhook` | Flash, dial `*074` and a target, let the consulted peer answer, then go on hook. Require a consult `Refer-To`, escaped and receiver oriented, and exactly one BYE from the DUT, on the referee dialog. (note) |
| `xfer-onhook-blind` | Flash and dial a second number with `local_conf_enabled` 2 and the stub's drop flash service, then go on hook while it rings. Require a CANCEL of the ringing call, and a `Refer-To` with the dialed number and no `Replaces` on the held leg. (note) |
| `xfer-two-line` | Two subscribers on one daemon. The first transfers its call to the second's number with `*073`, and the peer completes that referral with a call to that number. Line 1 rings and answers. (note) |
| `conf-join` | Join an inbound first call and an outbound second call. Keep the conference through a remote hold and resume. Require both BYEs and both record marks. (note) |
| `conf-drop` | Join the mixed-direction pair, flash under the stub's drop flash service, and require only the outbound second call to end. The inbound first call survives. (note) |
| `conf-split` | Join two outbound calls and flash with `UVOICE_STUB_FLASH_SERVICE=1` (split). Require the resumed leg's `sendonly`, `sendrecv`, `sendonly` sequence before park and retrieve. (note) |
| `conf-onhook-xfer` | Join the mixed-direction pair with `local_conf_enabled` 2 and go on hook. Require exactly one REFER request on one dialog, with the escaped Replaces and the DUT's Contact on every copy. Require exactly one BYE request, on that dialog (the referee) and on no other. (note) |
| `conf-onhook-xfer-auth` | Same gesture, against a peer that answers REFER and BYE 401. Each request must come again with credentials: two REFER requests and two BYE requests on the referee dialog. (note) |
| `conf-remote-bye` | Join the mixed-direction pair, let the inbound peer send BYE, and require the outbound survivor with no conference and no tone path. (note) |
| `conf-ineligible` | Space two inbound offers and answer the waiting call. Require the next flash to swap directions without starting the conference mixer. (note) |
| `legacy-hold` | Drive the RFC 2543 hold sequence of a field switch on an outbound call. Two flashes hold and resume the call, while the peer answers with a stale hold SDP (`c=0.0.0.0`). Line 0 ends IDLE. (note) |
| `glare-mutual` | Same call under mutual glare. The peer answers the hold re-INVITE 491, retries after 1900 ms, and requires the DUT's 200 inside its own backoff. The DUT's retry, read from `traffic.pcap`, must come 2.1 to 4.0 seconds behind that 491. |
| `conf-legacy-hold` | Same stale hold answer, against a conference join. The join leaves the resumed leg `recvonly` inside a running conference. The offerless INVITE behind it puts both calls back on `sendrecv`. The conference and both records survive it. |
| `hold-dial-abandon` | Hold an inbound call with a flash and dial nothing. Require reorder when `start_digit_timer` expires, with the call still held and no resume re-INVITE, although `flash_service_timer_sec` is 1. A flash in reorder resumes the call. (note) |
| `hold-dial-abandon-onhook` | Same abandoned flash, ended with an on-hook in reorder. Require HOLDING with a recall ring and no hangup of the held call. A lift resumes the call. (note) |
| `daemon_dsp_fatal` | Fail DSP bring-up fatally through the stub setting `UVOICE_STUB_DSP_START`, and require the exit that procd respawns. Exit status is 1, `daemon.log` carries the teardown steps in order, and no `voice` object stays on the socket. (note) |
| `daemon_dsp_transient` | Fail DSP bring-up once through the same setting, and require one retry to carry it. Require ready state, a retry line in `daemon.log`, and a daemon that stays. (note) |
| `daemon_dsp_fatal_late` | Hold a fatal bring-up completion until after a SIGTERM, with the dsp stop held open behind it. Require the late completion to leave the stop alone: exit status 0, and a teardown that reaches its dsp stop. (note) |
| `daemon_dsp_transient_late` | Same shape, with a transient completion. Require the stopping daemon to arm no retry and to submit no bring-up behind the dsp stop, with exit status 0. (note) |
| `daemon_dsp_fatal_sync` | Refuse the bring-up submit at once, as a backend with no worker does, and require that exit. Exit status is 1, and `daemon.log` carries refusal, decision, `uvoiced stopping` and dsp stop in that order. (note) |
| `loop-test` | Enable the manufacturing loop test in self mode, and require both lines out of service with a `loop_test` table. Lift and drop one handset, let the capped ring end, lift again for audio, then disable the test. It drives no SIPp and places no call. (note) |
| `g726-call` | Offer G726-32 at dynamic payload type 111 ahead of PCMU against a DUT with a G.726 codec section. Require that payload type in the answer with no PCMU line, and stream 0 programmed as `g726_32`. (note) |
| `g726-refused` | Same offer, against the default codec list. Require a PCMU answer with no `G726-32` line, and stream 0 programmed as `pcmu`. (note) |
| `g722-call` | Answer an inbound G.722 offer with a G.722 codec section. Require static payload type 9 with `G722/8000` in the answer, and stream 0 programmed as `g722`. (note) |
| `g722-refused` | Same offer, against a DUT with no G.722 section. The default codec list skips payload type 9 and answers PCMU, and stream 0 runs narrowband as `pcmu`. (note) |
| `opus-call` | Answer an inbound Opus offer. Require `opus/48000/2` at payload type 96 with our decoder fmtp, `telephone-event/48000` and `a=maxptime:20`. Require stream 0 programmed at ptime 20, with peer bitrate cap, complexity, wideband ceiling and FEC flag intact. (note) |
| `opus-refused` | Offer Opus from a peer to a DUT whose codec sections exclude it. Require the refusal to come from the codec list while the backend itself advertises Opus (the premise check). (note) |
| `opus-narrow-dtmf` | An Opus call whose peer names no `telephone-event/48000`. Require a logged inband fallback, with `opus_dtmf_fallbacks` counting exactly one. (note) |
| `prack-require` | Offer `Require: 100rel` to a DUT with `prack` 1, and require the reliable 180 that it must send. Its `RSeq` on the wire equals the returned RAck's first number. Require one copy of the 180, one PRACK, and both reliable send and match in `daemon.log`. |
| `prack-supported` | Same exchange, under `Supported: 100rel` alone, against a DUT with `prack` 1. The `prack` option governs the reliable 180 under both `Supported` and `Require`. |
| `prack-supported-plain` | Same offer, against `prack` 0, and require a plain 180. The log shows no reliable send, the wire carries no `RSeq`, and no PRACK goes out. |
| `prack-require-plain` | `Require: 100rel` against `prack` 0, and require that plain 180, with the call still connected. The option gates the reliable 180 whole. This behavior deviates from a MUST of RFC 3262 section 3. |
| `prack-bad-rack` | Three PRACKs name no outstanding reliable provisional, and one does. Require three 481 on the wire, two refusals from the PRACK handler and one match. (note) |
| `prack-late` | Hold PRACK back 1700 ms and require retransmissions. The wire carries three copies of the 180, and `daemon.log` two retransmission traces, none behind the match. The copies come about 0.5 and 1.5 seconds behind the first copy. A missing capture or `tshark` gives PARTIAL. |
| `prack-giveup` | Send no PRACK with `sip_timer_t1` 100, and require the give-up. Require exactly seven copies of the 180, a 504 at 64\*T1 and a give-up trace. The ring starts and stops, and no stream is ever programmed. |
| `prack-cancel` | Cancel an unacknowledged reliable 180, and require the 487 to carry that 180's tag, with no copy of the 180 behind the 487. |
| `prack-uac-180` | The DUT places a call, and the peer answers reliably. Its INVITE advertises 100rel and PRACK and insists on neither. The RAck names `4242`, the INVITE CSeq number and INVITE, the PRACK CSeq is one higher, and exactly one PRACK goes out. |
| `prack-uac-dup` | Same reliable 180, sent twice. Require one PRACK, with the repeated `RSeq` reported as discarded. |
| `prack-uac-gap` | A reliable 183 with `RSeq: 10` and a reliable 180 with `RSeq: 12`. Require one PRACK for 10, `RSeq: 12` reported as out of order, and the call still connected. |
| `prack-uac-auth` | The peer challenges the INVITE 401. Both attempts advertise 100rel, the RAck names the retry's CSeq, and one PRACK goes out. |
| `prack-early-media` | An answer inside a reliable 183, with a config that names no `early_media_enabled`, so the default 1 applies. Require an early stream under local ringback, and the tone stop at its first packet. The bodiless 200 connects the call on the 183's port. (note) |
| `prack-early-media-off` | Same exchange, with the switch clear. Local ringback plays, no stream runs before the answer, the same bodiless 200 connects that call, and stream 0 runs sendrecv behind it. |
| `early-media-183` | Same answer inside a plain 183, against `prack` 0, the common field case. Require an early stream under local ringback, and a tone stop at the first packet. No PRACK leaves the DUT, and the daemon takes no reliable answer. (note) |
| `verstat-cid` | Offer the field INVITE. Its From carries the caller's number in the quoted display name and in its URI user part, each with a `verstat` parameter. Require the bare number on the line, with no name at all. It places three calls, one per verstat value. (note) |
| `inbound-churn` | Offer fifty calls to line 0, one at a time, on the field header block. Require every call's 180 inside one second. The binding refreshes every five seconds between calls. (note) |
| `service-route-absent` | Register against a registrar that grants no `Service-Route`, then place a call. Require no `Route` header at all: not on REGISTER, INVITE, ACK or BYE. That is a field case too. |
| `service-route-present` | Register against a registrar that grants two loose hops in one `Service-Route` header, then place a call. Require one `Route` line per hop, in the granted order, on each INVITE and on the ACK of the 407. (note) |
| `wan-flap-outage` | Run the field sequence of a WAN address change against a stub `network.interface` object. Require one REGISTER for the address change, then a refresh one interval later. A second daemon drives the recovery timer. (note) |
| `mgcp-restart` | Start the daemon with `protocol` `mgcp` against the call agent `mgcp_ca.py`. Require RSIP `RM: restart` for the wildcard endpoint as its first message. After the 200, a seize before any request gives NTFY `L/hd` with `X: 0`. Both endpoints then read `in_service` and `idle`, with the agent as notified entity. |
| `mgcp-outbound` | Request `L/hd`, seize, and require NTFY `L/hd`. An RQNT with a digit map and `S: L/dl` plays dial tone, and four digits give one NTFY. CRCX sendrecv, MDCX to a new port and DLCX follow. Require media on each port, `P:` in the 250 and NTFY `L/hu`. |
| `mgcp-inbound-cid-ring` | A CRCX recvonly with no remote description starts no stream. An RQNT with `S: L/rg, L/ci(...)` rings line 0 with the caller id and the one cadence of the region pattern. A seize sends NTFY `L/hd` and stops the ring. An MDCX sendrecv with the remote description starts media, and DLCX ends it. |
| `mgcp-call-waiting` | Connect a call, then CRCX a second connection with `S: L/wt, L/ci(...)`. Require the call waiting tone and the type 2 record of the caller. A flash sends NTFY `L/hf`. Two MDCX swap the modes, stream 1 runs the second call, and DLCX ends both. |
| `mgcp-hold` | Connect a call, then send MDCX `M: recvonly` and MDCX `M: sendrecv`. While held, stream 0 runs recvonly, and the 200 carries no description, because only the mode changed. The resume brings sendrecv back on the same port, and the release sends NTFY `L/hu`. (note) |
| `mgcp-fax-tone` | Connect a G.711 call that requests `G/mt` and `G/ft`, with no fax profile on stream 0, and report a CED from the phone. Require NTFY `G/mt` and the passthrough profile on stream 0. An MDCX with a T.38 offer then gets an `m=image` answer, and stream 0 runs `t38`. |
| `mgcp-disconnected` | Leave NTFY `L/hd` unanswered. Require the disconnected procedure in `voice status`, and RSIP `RM: disconnected` attempts whose delays double and whose `RD:` counts from the entry. The 200 to the third attempt returns endpoint 0 to `in_service`. (note) |
| `mgcp-piggyback` | Leave the restart RSIP unanswered, so both endpoints restart in the disconnected procedure. Require the 405 to an RQNT behind an RSIP `RM: restart` in one datagram. Then an RQNT and an AUEP in one datagram get two 200 responses in order. (note) |
| `mgcp-response-cache` | Send one CRCX twice with one transaction id. Require the same response with the same `I:`, and an AUEP that lists that connection alone. (note) |
| `mgcp-ncs` | Run with `dialect` `ncs`, and hold the answer to the restart RSIP. Require that RSIP in front of an AUEP 200 and an RQNT 520, under `MGCP 1.0 NCS 1.0`. Require `L/` events, 510 to EPCF, an `L/r2` ring with the two cadences of GR-506 pattern 2, and 522 to `L/wt`. |

Notes:

- **`flow-a`.** `flow_a_run`, artifact `flow-a-outbound`, scenario
  `uas_basic_call.xml`. It seizes, takes dial tone and dials.
  `media_port_wait` checks that `stream[0].remote_port` is the negotiated SDP
  port. In a private network namespace, stream 0 must carry no `bind_dev` key;
  in the host namespace, the flow skips that check. The release waits out the
  scenario's scripted pause, because a BYE inside that pause aborts the call.
  It requires two successful SIPp instances, REGISTER and call
  (`uas_sipp_verdict` 2).
- **`flow-b`.** `flow_b_run`, artifact `flow-b-inbound`, scenario
  `uac_basic_call.xml`. Its stub ring must carry `cid_number` `sipp` and
  `cid_name` `SIPp`. A seize answers the call, and `media_port_wait` checks
  its media port. The flow requires SIPp exit status 0 and one successful
  instance.
- **`flow-c`.** `verbs_run`, artifact `flow-c-verbs`, scenario
  `uas_churn.xml`. In order:
  1. Verb `seize` reaches OFFHOOK_DIAL, and verb `release` returns IDLE.
  2. Verb `dial` with `123#` makes its own seize and reaches CONNECTED, with
     one active outbound leg for number 123.
  3. Mid-call, `voice stats_live` returns stub values: `packets_sent` 1500,
     `packets_recv` 1498, `bytes_sent` 240000, `bytes_recv` 239680, `rfactor`
     88, `mos_lq` "4.40" and `mos_cq` "4.01".
  4. After one second of talk and a clear, the flow requires one closed
     answered record with `duration_ms` in [1000, 3000).

  The live media table must equal the record's media table, key for key.
  `daemon.log` must carry no "produced no answer" timeout.
- **`uas_reinvite_hold`.** `committed_outbound_run` with `uas_reinvite_hold`:
  `flow_prepare` and `outbound_dial_start`, then `uas_reinvite_hold.xml`,
  `sipp_finish_wait` 45, and a release. It requires IDLE and two successful
  instances, REGISTER and call.
- **`uac_reinvite_hold`.** `uac_reinvite_run`, scenario
  `uac_reinvite_hold.xml`. SIPp calls the DUT as UAC, and a seize answers into
  CONNECTED with its media port checked. A first flash reaches HOLD_DIAL with
  the hold re-INVITE, and a second returns CONNECTED with the resume. The
  peer answers and the DUT acknowledges both re-INVITEs. It requires SIPp exit
  status 0, IDLE and one successful instance.
- **`uas_session_timer_422`.** `committed_outbound_run` sets
  `sip_session_max_expires` 1800 and `needs_release` 1. CONNECTED after the
  dial proves that the corrected INVITE completed. It requires IDLE and two
  successful instances. On a failure, it reports `daemon_failure_reason`.
- **`uac_cancel`.** `uac_cancel_run`, scenario `uac_cancel.xml`. The scenario
  holds its ring for 500 ms before the CANCEL. The runner reads ring evidence
  from `daemon.log`, in this order:
  1. CALL_INCOMING in IDLE;
  2. stub ring start;
  3. CALL_REMOTE_CANCEL in RINGING;
  4. stub ring stop.

  It fails when "strm 0 start" appears, because an unanswered call starts no
  media. It requires SIPp exit status 0, IDLE, a stopped stub ring and a
  stopped stream. The wire carries 180, CANCEL, 200, 487 and ACK.
- **`uas_491_glare`.** `committed_outbound_run`, scenario
  `uas_491_glare.xml`. After CONNECTED, `flash_send` drives the re-INVITE.
  `timing_491_check` reads the retry delay from `traffic.pcap` with `tshark`.
  A missing `tshark` or capture gives PARTIAL, and a 491 with no retry behind
  it gives FAIL.
- **`uas_503_retry_after`.** `retry_503_run`, scenario
  `uas_503_retry_after.xml`. It requires IDLE, then `lines[0].state`
  `registered` with `last_code` 200. `timing_503_check` reads the delay from
  `traffic.pcap` with `tshark`; a missing capture or `tshark` gives PARTIAL.
- **`reg-403-reorder`.** In order:
  1. Digits dialed over reorder place no call: `out_attempted` of
     `voice stats` holds still, and `daemon.log` carries no INVITE
     transaction.
  2. A flash over reorder leaves line 0 in CLEARING with tone 6.
  3. After a release, a caller sends a new INVITE (`uac_refused_480.xml`)
     and requires 480 with no ring. Its out-of-call `register_ooc_403.xml`
     answers a REGISTER retry 403, so line 0 stays without a binding.
  4. `uas_register_403.xml` takes the credential retry. That retry must
     arrive 29.5 to 33 s after the first 403, read from `traffic.pcap`
     (`timing_403_check`). A missing capture gives PARTIAL.
- **`uas_mwi_notify`.** `mwi_notify_run`, scenario `uas_mwi_notify.xml`. The
  switch registers the DUT first. `daemon.log` must carry
  `[fsm] line 0 message waiting 1 new 2 old 1`, because the clear follows at
  once and no status poll can read the set state. After the clearing NOTIFY,
  `voice status` reads `mwi` false and `mwi_new` and `mwi_old` 0. Its verdict
  needs both NOTIFYs answered 200 and the unknown package answered 489. Line 0
  stays registered, and a line with unsolicited NOTIFYs alone carries no
  `mwi_subscription` key.
- **`uas_cfu_redirect`.** `cfu_redirect_run`, scenario
  `uas_cfu_redirect.xml`. The config sets `call_fwd_local` 1,
  `call_fwd_local_mode` 1 and `call_fwd_uncond_uri` 5552000, and the daemon
  starts before SIPp. It requires `call[0].cf_uncond` 5552000, one successful
  instance, IDLE, stub ring off, and a line that stays registered. The
  scenario aborts on any 180.
- **`uas_fax_upspeed`.** `fax_upspeed_run`, scenario `uas_fax_upspeed.xml`.
  Its config writes three codecs, G.729 first, and `rfc2833` DTMF, so the
  call commits to G.729 (`codec` 3). In order:
  1. A CED from the wire (`tone_send ced remote`) must not switch the call:
     `fax_applied` stays false.
  2. A local CED (`tone_send ced local`) applies the passthrough profile:
     `fax_applied` true, `fax_mode` 1 (passthrough), `fax_codec` 1 (PCMU),
     `fax_ec_disable`, `fax_vad_disable`, `fax_cng_disable` and
     `fax_silence_comp_disable` true, and `fax_jb_fixed` true.
  3. The upspeed moves stream 0 to `codec` 1.
  4. The kind on the applied description reads `passthrough` 1 (modem) after
     the upspeed answer, then `passthrough` 2 (fax) after a local V.21.

  It requires two successful instances, REGISTER and call, and IDLE.
- **`uas_fax_upspeed_auth`.** The retry must carry the challenge's nonce and
  opaque, and that G.711-only offer again. It counts three INVITE requests by
  CSeq number.
- **`uas_far_fax_tone_g711`.** `far_fax_tone_run`, scenario
  `uas_far_fax_tone_g711.xml`. Stream 0 keeps PCMU and takes no fax profile.
  `voice status` reads `passthrough` with `fax_kind` `modem` and neither
  latch, and the jitter buffer row reads `fixed`. An in-dialog INVITE from
  the DUT fails the scenario. The runner measures quiet from the stub sample
  that confirms the kind to the scenario's BYE, and fails a flow with less
  than 8 s.
- **`uas_fax_upspeed_false`.** Stream 0 carries no kind, `voice status` drops
  the `fax` word, and the stub line reads `latch_clears` 1. A second CED runs
  a second upspeed, which only a reset `fax_switched` permits.
- **`uas_far_fax_tone_false`.** `far_fax_tone_false_run`, scenario
  `uas_far_fax_tone_g711.xml`. A return to voice puts nothing on the wire, so
  this flow's wire equals that of `uas_far_fax_tone_g711`. Stream 0 keeps PCMU
  with no kind, the jitter buffer row reads `adaptive` again, and
  `latch_clears` is 1. An in-dialog INVITE from the DUT at any point fails
  the scenario. The 8 s quiet rule of `uas_far_fax_tone_g711` applies.
- **`uac_t38_offer`.** `t38_run` enables `fax_t38_enable` and connects the
  call on G.729 through `t38_call_connect`, which also builds the passthrough
  that the upgrade needs:
  - a V.21 preamble from the wire must not upgrade the call;
  - a local V.21 on a leg with no passthrough must not upgrade it either,
    because the far end owns that offer;
  - a local CED converts the leg, and the runner waits for the upspeed
    answer, because a leg with its own exchange open refuses the upgrade.

  A local V.21 (`tone_send v21 local`) then makes the DUT send the image
  offer. `uac_t38_offer.xml` carries the upspeed exchange, then accepts the
  image offer. The flow requires these results:
  - on stream 0: `codec_name` `t38`, `t38_max_bit_rate` 9600,
    `t38_max_datagram` 400 and `t38_ec` 1;
  - in `voice status`: `calls[0].fax` `t38`, `codec` `t38` and
    `t38_switched[0]` true;
  - `fax_applied` true: the upgraded leg carried the passthrough;
  - one successful instance, IDLE, and exactly one record with `codec`
    `t38`.
- **`uac_t38_reject`.** `t38_reject_run` reuses the connected call that the
  local CED converted to passthrough. A local V.21 makes the DUT offer image,
  and `uac_t38_reject.xml` answers 488 after the upspeed exchange; the
  scenario expects no INVITE after the 488. The refusal leaves the answer
  tone's carrier standing:
  - `fax_applied` true, `fax_mode` 1, `fax_codec` 1 and `fax_jb_fixed` true;
  - stream 0 on `codec` 1 with `codec_name` `pcmu` and `passthrough` 2, the
    fax kind that the preamble set;
  - `voice status` with `fax` `passthrough` and `t38_switched[0]` true.

  No second image session reaches stream 0. Its record names PCMU and no
  T.38 codec. The leg keeps its G.711 passthrough. It requires one
  successful instance and IDLE.
- **`srtp-outbound`.** `srtp_outbound_run`, scenario `uas_srtp_call.xml`.
  Stream 0 reads `srtp_suite` 1 and `srtp_suite_name`
  `aes_cm_128_hmac_sha1_80`, with transmit and receive key CRCs above 0. The
  INVITE carries exactly two bare crypto attributes. Transmit key CRC matches
  the INVITE's tag 1 key, and receive key CRC matches the answer's key.
  `srtp_digest_check` recomputes the zlib CRC32 from the base64 key on the
  wire. At the BYE, `srtp_zeroize_check` confirms suite 0 (`none`) and no key
  digest fields. It requires IDLE and two successful instances.
- **`srtp-inbound`.** `srtp_inbound_run`, scenario `uac_srtp_call.xml`, with
  `srtp_enabled` 1. `srtp_crypto_shape_check` requires exactly one bare
  crypto attribute in the 200, with no session parameter. The answered key
  differs from both offered keys. Transmit key CRC matches the 200's key, and
  receive key CRC matches the offered tag 1 key. Mid-call, `voice status`
  shows `calls[0].srtp` `AES_CM_128_HMAC_SHA1_80`. It requires a zeroize at
  the stop, IDLE and one successful instance.
- **`srtp-accept-plain`.** `srtp_accept_plain_run`, scenario
  `uac_srtp_plain_hold.xml`. Its re-offer follows the committed transport.
  A seize answers into CONNECTED with its
  media port checked, and stream 0 reads `srtp_suite` 0 with no key CRC. A
  first flash reaches HOLD_DIAL, and a second returns CONNECTED.
  `srtp_zeroize_check` confirms no suite and no key digest at the stop. It
  requires IDLE and one successful instance.
- **`srtp-refused`.** `srtp_refused_run`, scenario `uac_savp_refused.xml`.
  The answer build refuses after the ring, and `daemon.log` carries
  "the offer carries no usable codec or address". The warning
  "srtp_enabled over udp: the SDES master keys travel in clear" must be
  absent, which proves that the switch was off. No media starts, and no
  stream runs or is keyed. It requires IDLE and one successful instance.
- **`stream-uac-tcp`.** `stream_uac_run tcp` sets `flow_transport` tcp and
  runs `uas_stream_call.xml`. `connection.peer` must end with the SIPp port.
  After the scenario's pause, a release stops stream 0. It requires
  `registered` at the seize, IDLE and two successful instances: the scenario
  answers REGISTER over the stream transport too.
- **`stream-uac-tls`.** `stream_uac_run tls` takes the TLS lock
  (`tls_lock_take`). It checks `lines[0].listener` false only when
  `/tmp/voip_tls/cert.pem` is absent, which is the normal state before a
  bundle arrives.
- **`stream-uas-tcp`.** `stream_uas_run tcp` sets `keepalive_interval` 1,
  waits for `lines[0].listener` true, runs `uac_stream_call.xml`, and seizes
  into CONNECTED. `stream_ping_check` sends the CRLF ping. The connection
  must be up to the SIPp port. `keepalive.crlf_sent` must be at least 1: the
  DUT sent its own RFC 5626 ping. Line 0 registers first through
  `register_prime`. It requires `registered` at ring, connection check and
  keepalive check, and one successful instance.
- **`stream-uas-tls`.** `stream_uas_run tls` installs the certificate bundle,
  so the TLS server half comes up. It runs the TCP flow's checks, then
  `capture_finish` and `pcap_cleartext_check` read `traffic.pcap`. A missing
  or unreadable capture gives PARTIAL, and any cleartext gives FAIL. This
  check covers a cleartext keepalive on a TLS line.
- **`stream-register-tcp`.** `stream_register_run tcp` sets
  `options_interval` 2 and runs `uas_options_keepalive.xml`. It waits for the
  listener, runs `stream_ping_check`, and requires `keepalive.options_alive`
  of at least 1. `voice status` must report a tcp line with a listener. It
  requires one successful instance.
- **`stream-subscribe-tcp`.** `stream_subscribe_run tcp` sets
  `mwi_subscribe` 1 and runs `uas_stream_subscribe.xml`. `voice status` must
  name the tcp transport. It requires two successful instances, because
  registration is the subscription's precondition. No bundle is
  installed: the DUT is client of both requests.
- **`cert-fetch`.** `cert_fetch_run` runs on loopback with no SIPp. It builds
  an EC archive, starts the http server, points `tls_cert_path` at it, and
  exports `UVOICE_TLS_CERT` around one daemon start. Three outcomes:
  - Install: exit 0, the status file reads `Certificate updated and applied`,
    certificate, key and CA are unpacked, and the TLS listener comes up.
  - Unchanged: a reload with the same ETag reads
    `Certificate applied (etag unchanged)` and does not rewrite `cert.pem`.
  - Refusal: a `gopher://` URL reads `Unsupported file transfer protocol`,
    and the installed bundle and listener stay.
- **`reload-loop`.** `reload_loop_run` runs on loopback with no SIPp. It
  installs the certificate, brings the listener up and reaps the helper
  child. `E2E_RELOAD_ITERATIONS` sets the count (default 30). Each iteration:
  1. alternates `tls_cert_username`, so the config moves;
  2. removes the status file and runs `ubus voice reload`;
  3. waits for the "etag unchanged" verdict, then for the helper exit
     (`daemon_helper_wait`), so the next reload starts a fresh helper inside
     its reload frame;
  4. requires `lines[0].listener` true.

  At the end, `daemon.log` must carry no "Address already in use" and no
  "no active line" reconcile.
- **`xfer-blind`.** `xfer_transferor_run` with `uas_xfer_blind`, target
  5552000, dialed as `*0725552000#`. The machine ends in OFFHOOK_DIAL. The
  REFER carries exactly one `Refer-To` `<sip:5552000@127.0.0.1>`,
  `Referred-By` `<sip:5551000@127.0.0.1>`, the leg's `Contact`, and no
  `Replaces`. Behind the 202, the peer sends two NOTIFYs, a `c=0.0.0.0`
  re-INVITE that the DUT answers inactive, and its own BYE, all answered
  200. It counts BYE dialogs by Call-ID and requires 0. Its single record has
  `transferred` true, `answered` true and `transferred_from` null, and it
  requires two successful instances.
- **`xfer-attended`.** `xfer_transferor_run` with `uas_xfer_attended`. REFER
  checks follow `xfer-blind`. It counts one BYE dialog and requires two
  successful instances.
- **`xfer-failed`.** `xfer_transferor_run` with `uas_xfer_failed`. REFER
  checks follow `xfer-blind`. It counts one BYE dialog, the hangup. Its single
  record has `transferred` null, because nothing took the call away. It
  requires two successful instances.
- **`star-network`.** The second call connects beside the held one
  (TWO_CALLS), and no REFER goes out. A first on-hook ends the star call and
  rings the held leg back into HOLDING. A seize resumes it into CONNECTED,
  and a second on-hook ends it. It requires three successful instances:
  REGISTER and two calls.
- **`xfer-transferee`.** `xfer_transferee_run` with `uac_xfer_refer`. The
  child INVITE comes back through `xfer_peer_ooc.xml`. Checks:
  - two shadow legs while the child rings;
  - after the swap, one active leg on slot 1 that holds the parent's UI
    slot 0;
  - child request line `INVITE sip:xfer@127.0.0.1:<port>`, with
    `Referred-By` `<sip:sipp@127.0.0.1:<port>>` and no `Replaces` or
    `Require`;
  - Contact of the DUT on both NOTIFYs and on the 202;
  - two correlated records: seq 1, and seq 2 with `transferred_from` 1;
  - one successful instance.
- **`xfer-transferee-replaces`.** `xfer_transferee_run` with
  `uac_xfer_refer_replaces` and Replaces value
  `xfercall99@127.0.0.1;to-tag=peertag99;from-tag=duttag99`. Shadow, swap
  and correlation checks follow `xfer-transferee`. It requires one
  successful instance.
- **`xfer-child-fail`.** `xfer_child_fail_run` runs `uac_xfer_child_fail.xml`
  with `xfer_busy_ooc.xml` as the target that refuses. The shadow pair opens,
  then the original call remains as one answered leg on slot 0 with no
  shadow. Two records: parent (seq 1) answered with `transferred` null, and
  child (seq 2, `transferred_from` 1) not answered. It requires one
  successful instance.
- **`xfer-replaces`.** `xfer_replaces_run`: `xfer_call_place` seizes and
  dials `123#` (`digit_send`), and out-of-call peer `xfer_peer_ooc.xml`
  answers that call. Peer `uac_xfer_replaces.xml` then sends a second INVITE,
  with 5551000, the subscriber's `sip_username`, in its request URI. After
  the takeover, one inbound active leg sits on slot 1 with UI slot 0. The
  stub's ring stays off, and `daemon.log` carries no CALL_INCOMING, because
  the offer bypasses call waiting. Two records: replaced call (seq 1,
  outbound, answered) and replacement (seq 2, inbound, answered,
  `transferred_from` 1). Line 0 ends IDLE, and the flow requires one
  successful instance.
- **`xfer-onhook`.** `xfer_onhook_run`: the peer calls the DUT
  (`uac_xfer_onhook.xml`). A flash and `*0745552000#` reach TWO_CALLS, and
  an on-hook clears line 0 to IDLE. Details:
  - The `Refer-To` check is a regex that holds under every locale: a
    `Replaces` with `to-tag=sipp-peer-tag` and the DUT's from-tag.
  - The REFER carries `Referred-By` and `Contact`
    `<sip:5551000@127.0.0.1:<daemon port>>`.
  - The flow counts BYE dialogs by Call-ID and requires 1. That BYE comes one
    grace behind the DUT's answer to the verdict NOTIFY.
  - The consulted peer ends the replaced dialog with its own BYE, answered
    200, with no BYE of the DUT in front of it.
  - Both records close with the transfer key, and none stays in progress.
  - The flow requires one successful instance.
- **`xfer-onhook-blind`.** The stub's region read-back carries the drop flash
  service when `UVOICE_STUB_FLASH_SERVICE` is unset. The DUT sends one BYE,
  on the held leg (the referee). `xfer_grace_check` requires that BYE one
  grace behind the DUT's answer to the verdict NOTIFY. Line 0 reaches IDLE
  with no recall ring. It requires one transferred record and one unanswered
  record.
- **`xfer-two-line`.** `xfer_two_line_run` configures two channels, 5551000
  and 5551001, on one port, and runs two SIPp stages in sequence:
  1. `uas_xfer_attended.xml`: line 0 places a call, holds, dials
     `*0735551001#`, and reaches OFFHOOK_DIAL. The flow reads the target
     number from the REFER's `Refer-To` `<sip:5551001@127.0.0.1>`.
  2. `uac_xfer_two_line.xml` calls that number. `call[1]` rings, a seize
     answers line 1, then `call[0]` is IDLE and `call[1]` CONNECTED.

  The referred INVITE must follow the REFER in time. Line 0's record carries
  `transferred` true and `answered` true. Line 1's record is inbound and
  answered, with no transfer key. Stage one needs three successful
  instances: each line's REGISTER and the call. Both bindings hold through
  stage two, which needs one.
- **`conf-join`.** `conf_joined_start` brings the inbound leg up
  (`uac_conf_onhook_xfer.xml`), places a second call and takes the join flash
  into CONF. `conf_remote_cycle_wait` drives the far-end hold and resume
  through the mixer, and `line[0].conf` stays 1 across both. The peer must
  receive a BYE in both conference dialogs. `conf_closed_wait` requires IDLE
  with no legs and two records, none in progress, both with `conferenced`
  true. It requires one successful instance.
- **`conf-drop`.** `conf_joined_start` with `uac_conf_leg.xml`, then a drop
  flash. The dropped leg's record closes while the survivor stays in
  progress. The survivor then ends, and `conf_closed_wait` sees two closed
  records, both marked conferenced. It requires one successful instance.
- **`conf-split`.** `conf_split_run` calls `conf_config_write` with local
  conference on (1), and registers the line through `register_prime` with
  the split flash service. The DUT places the second call against
  `uas_conf_leg.xml`. After a join, a split flash holds one leg and keeps the
  other active. `conf_sdp_sequence_check` reads the direction attributes of
  the hold and resume re-INVITEs in order, which have the shapes of RFC 3264
  section 8.4. An on-hook then parks the held leg in HOLDING, a seize
  retrieves it into CONNECTED, and a second release ends it. Line 0 ends IDLE
  with two closed records, both marked, and two successful instances
  (`uas_sipp_verdict` 2).
- **`conf-onhook-xfer`.** `conf_onhook_xfer_run` with `uac_conf_onhook_xfer`,
  after `conf_joined_start`. The `Refer-To` carries the other dialog escaped
  behind the "?". The consulted peer ends the replaced dialog with its own
  BYE, answered 200. Line 0 ends IDLE with two closed records, both marked
  conferenced, and one successful instance.
- **`conf-onhook-xfer-auth`.** Each REFER copy carries the same escaped
  Replaces and Contact. The last copy of each request names the nonce of its
  own challenge.
- **`conf-remote-bye`.** `conf_joined_start` with `uac_conf_remote_bye.xml`,
  which ends its own dialog once the mixer runs. The survivor stays connected
  with no tone over it. The call then ends, and `conf_closed_wait` sees two
  closed records, both marked conferenced. It requires one successful
  instance.
- **`conf-ineligible`.** `conf_config_write` turns local conference on (1),
  under the default drop flash service. The acquisition-order guard makes a
  pair of two inbound calls ineligible. A first flash answers the waiting
  call, and the next flash swaps instead of a join. `stub_check` requires
  `line[0].conf` 0 after the swap, and `daemon.log` carries no
  `conf mode=1` line.

  An on-hook parks the held inbound leg, and a seize retrieves it. Line 0
  ends IDLE with two closed records and no conference mark. It requires two
  successful instances (`uas_sipp_verdict` 2), one for each inbound call of
  `uac_conf_leg.xml`.
- **`legacy-hold`.** `committed_outbound_run` with `legacy_hold_drive`. In
  order:
  1. One flash holds the call. The peer answers `c=0.0.0.0`, so the leg reads
     `inactive` on G.729.
  2. The peer then holds the leg itself, and the DUT must answer that
     re-INVITE with a 200 that carries `a=inactive` at its own address.
  3. A second flash resumes the call, and the peer answers it with the same
     stale hold SDP.
  4. The INVITE with no SDP behind it puts the leg back on PCMU `sendrecv`.
- **`hold-dial-abandon`.** `hold_dial_abandon_run`, scenario
  `uac_reinvite_hold.xml`, whose resume wait of 30 s outlasts the window. Its
  config writes `start_digit_timer` 3 and `flash_service_timer_sec` 1. SIPp
  calls the DUT as UAC, a seize answers, and a flash reaches HOLD_DIAL with
  the hold re-INVITE. In order:
  1. The flow waits for `CLEARING` with class `fastbusy`, and requires it 2
     to 5 s after it saw HOLD_DIAL. The bounds hold the status polls, not a
     second window.
  2. It requires the same count of received INVITEs in SIPp's message log as
     at the hold, and `tx_silence` still true. A resume timed by
     `flash_service_timer_sec` would leave at 1 s, and the flow would fail.
  3. A flash in reorder resumes the call into CONNECTED, and stream 0 is live
     again.
  4. A release returns IDLE with one successful instance.
- **`hold-dial-abandon-onhook`.** `hold_dial_abandon_onhook_run` shares the
  first steps of `hold-dial-abandon` (`hold_dial_abandon_start`). Then:
  1. A release in reorder reaches HOLDING, and the stub bell rings.
  2. A seize resumes the call into CONNECTED. The bell stops, and stream 0 is
     live again.
  3. A release returns IDLE. The scenario requires the resume re-INVITE, so
     a hangup of the held call fails the flow.
- **`daemon_dsp_fatal`.** `daemon_dsp_fatal_run` runs on loopback with no
  SIPp, with `UVOICE_STUB_DSP_START` `fatal`. The daemon must exit within
  15 s. Log lines, in this order:
  1. "dsp bring-up failed: fatal";
  2. "dsp bring-up is terminal, exiting for the procd respawn";
  3. "uvoiced stopping";
  4. "stub: dsp stop";
  5. "transport: socket 0 closed".

  After the exit, the ubus socket answers with no `voice` object.
- **`daemon_dsp_transient`.** `daemon_dsp_transient_run` runs on loopback
  with no SIPp, with value `transient_once`. Ready state is the success
  check, and "dsp bring-up retry" in `daemon.log` is the retry. A
  `process_wait` of 2 s that returns 124 proves that the daemon still runs.
- **`daemon_dsp_fatal_late`.** `daemon_dsp_fatal_late_run` runs on loopback
  with no SIPp, with value `fatal_late`, which delays its completion by 2 s
  (`STUB_JOB_LATE_DELAY_MS`). It waits for `hal.state` `starting`, sends
  SIGTERM ahead of the completion, and requires an exit within 15 s.
  `daemon_log_absent_after` checks that no "dsp bring-up is terminal" line
  follows "uvoiced stopping". `daemon.log` shows "uvoiced stopping", then
  "dsp bring-up failed inside the stop: fatal", and teardown reaches "stub:
  dsp stop". A completion outside the stop window gives PARTIAL.
- **`daemon_dsp_transient_late`.** `daemon_dsp_transient_late_run` runs on
  loopback with no SIPp, with value `transient_late`. It waits for
  `starting`, sends SIGTERM, and requires an exit within 15 s. After
  "uvoiced stopping", `daemon.log` must carry no "dsp bring-up retry" and no
  "stub: dsp start". Then it expects "dsp bring-up failed inside the stop:
  transient" and "stub: dsp stop". A completion outside the stop window gives
  PARTIAL.
- **`daemon_dsp_fatal_sync`.** `daemon_dsp_fatal_sync_run` runs on loopback
  with no SIPp. It uses value `fatal_sync`: the submit answers fatal and
  schedules no job, so the daemon decides the exit before its loop starts.
  The daemon must exit within 15 s. Log lines, in order: "dsp bring-up
  rejected: fatal", "dsp bring-up is terminal, exiting for the procd
  respawn", "uvoiced stopping" and "stub: dsp stop".
- **`loop-test`.** In order:
  1. A lift of one handset gives the self loop, with its delay in the stub
     dump.
  2. A drop gives the bell again.
  3. The capped ring ends on its own (`UVOICE_STUB_RING_CAP_MS`).
  4. A second lift gives audio.
  5. After the test is disabled, both lines return to service with the loop
     cleared.
- **`g726-call`.** `codec_inbound_run` with `uac_g726_call`,
  `g726_config_write` and codec 4. The flow requires RINGING, a seize into
  CONNECTED, and the SDP media port on stream 0. Line 0 ends IDLE with one
  successful instance.
- **`g726-refused`.** `codec_inbound_run` with `uac_g726_refused` and
  `g726_config_write` 0, which writes no codec section. The default codec
  list gives `codec` 1 and `codec_name` `pcmu`, and the flow requires IDLE
  and one successful instance.
- **`g722-call`.** `codec_inbound_run` with `uac_g722_call`,
  `g722_config_write` and codec 8. `g722_config_write` puts G.722 first in
  its codec section (priority 1), ahead of PCMU. The flow requires RINGING, a
  seize into CONNECTED, and its media port. After the BYE, a release reaches
  IDLE, and the flow requires one successful instance.
- **`g722-refused`.** `codec_inbound_run` with `uac_g722_refused` and
  `g722_config_write` 0, which writes no codec section. The default codec
  list gives `codec` 1 and `codec_name` `pcmu`. The flow requires CONNECTED
  with its media port, IDLE and one successful instance.
- **`opus-call`.** `opus_inbound_run` with `uac_opus_call`: codec 9, payload
  type 96, ptime 20, and no fallback. `opus_config_write` gives the DUT an
  Opus codec section (`tr104_id` 6, priority 1) ahead of PCMU. Our decoder
  fmtp is `maxplaybackrate=16000;maxaveragebitrate=64000;useinbandfec=1`.
  Stream 0 runs as opus at ptime 20 under the peer's cap of 20000 bit/s, at
  wideband, and no digit fallback is counted. The flow runs in the default
  set, because the stub advertises Opus (`src/hal/stub/stub.c`,
  `stub_open()`). The stub has no media plane, so no Opus packet crosses a
  wire, and the flow is not a live verification.
- **`opus-refused`.** `opus_inbound_run` with `uac_opus_refused`. The default
  codec list skips payload type 96 and answers PCMU with
  `telephone-event/8000`. Stream 0 runs as pcmu with an empty opus block, and
  no fallback is counted. The default set and the limit of `opus-call`
  apply.
- **`opus-narrow-dtmf`.** `opus_inbound_run` with `uac_opus_narrow_dtmf`,
  with a fallback expected. The peer offers only `telephone-event/8000`,
  which runs on the wrong clock for Opus. The answer takes Opus under the
  peer's cap of 20000 bit/s, with no telephone-event, so it refuses the event
  at 8000. The daemon falls back to inband digits, and it reports and counts
  the fallback once, read from its own counter. The default set and the
  limit of `opus-call` apply.
- **`prack-bad-rack`.** The copy with a foreign To tag names a dialog that
  the DUT never created. The branch for a request with no dialog answers it,
  so the PRACK handler never sees it.
- **`prack-early-media`.** Stream 0 runs sendrecv behind the 183, which is
  the direction that the answer negotiated, under local ringback.
  `voice.stub rtp_rx` then delivers the stream's first packet, and the tone
  stops. The bodiless 200 connects the call, and stream 0 still runs
  sendrecv on the 183's port. The one BYE comes from a hook release.
- **`early-media-183`.** Stream 0 runs sendrecv under local ringback, and the
  first packet stops the tone. The 200 carries the SDP again, because an
  unreliable 18x answer leaves the offer open.
- **`verstat-cid`.** Line 0 shows `cid_number` `2015550123`, an empty
  `cid_name`, the GR-30 absence letter `O` in `cid_blockinfo`, and neither
  privacy flag.
- **`inbound-churn`.** A hook driver answers odd call numbers, holds line 0
  for the talk time, and then ends each call from the subscriber side. The
  peer cancels even call numbers after their ring. That is a field sequence,
  whose device answered 100 and sent no 180 for 6.4 seconds.
- **`service-route-present`.** The registrar challenges the INVITE 407
  first. The two hops appear on the challenged INVITE, on its credential
  retry, and on the ACK of the 407. No `Route` appears on REGISTER, on the
  2xx ACK or on BYE. The hops name a host that the run never reaches, so the
  INVITE also proves that the next hop did not follow them.
- **`wan-flap-outage`.** Line 0 registers with no netifd, takes the address
  that the stub reports, and moves to a second address while the registrar is
  silent. The REGISTER for that change carries the old Call-ID, the new
  address in the Via and the Contact, and the old Contact with `expires=0`.
  The refresh behind it carries the new Contact alone. In the second daemon,
  two refused netifd queries each arm one attempt at 5000 ms. The attempt
  takes the address on the timer, and the ladder ends with the object. It
  needs the ucode `ubus`, `uloop` and `fs` modules, and fails by name
  without them.
- **`mgcp-hold`.** The MGCP path sets no `tx_silence` (`hal.h`). The stub's
  `direction` shows the hold: 2 is recvonly and 3 is sendrecv
  (`enum hal_media_dir`).
- **`mgcp-disconnected`.** The flow runs with `max1` 1, `max2` 2 and an RTO
  of 100 ms. The NTFY ends one RTO behind its last copy, and that end is the
  entry into the procedure. The agent reads each delay from `daemon.log`:
  "disconnected: entered, first attempt in N ms" and "attempt failed, next in
  N ms". It requires these readings (RFC 3435 4.4.7 and 2.3.12):
  - a first delay between 1 s and Tdinit, 15 s;
  - each next delay twice the one before;
  - each attempt within 0.1 s of its delay, counted from the end of its
    predecessor, or from the entry. One attempt lasts 0.3 s, so a delay
    counted from an attempt's start fails;
  - an `RD:` within 1 s of whole seconds since the daemon's entry: first
    attempt less first delay.

  A gap ratio, second gap over first, last copy to first copy, must also
  stay in 1.8 to 2.2. `mgcp_disc_wire_check` reads those gaps from
  `traffic.pcap` with `tshark`, and a missing capture gives PARTIAL. A third
  attempt comes 7 times the first delay after the entry: 105 s at most with
  Tdinit 15 s. The agent therefore waits 150 s for it, and the runner gives
  this flow 200 s.
- **`mgcp-piggyback`.** Under `rfc3435`, an RSIP goes in front of a response
  only for an endpoint in the disconnected procedure. The lost restart RSIP
  puts both endpoints there.
  The RQNT also starts the next attempt at once, and its 200 puts both
  endpoints in service. The first attempt can come 1 s after the entry, and
  no option bounds that delay. The RQNT must therefore leave within 0.5 s of
  the entry, and a slower host fails the flow with that reason. The order
  check takes the first final response to each command behind the one send.
- **`mgcp-response-cache`.** Limit: the flow does not cover a final response
  behind a provisional one. The daemon answers DLCX at once, because
  `conn_stats()` reads the stream statistics in the call. The stub has no
  hook that delays them either. That deferred final is the case that would
  drive the LONGTRAN path of the agent (section "MGCP flows and call agent").

## Run

Run all default flows from the repository root:

```sh
E2E_ARTIFACTS=/tmp/uvoice-e2e \
SIPP_BIN=/path/to/sipp \
uvoice/test/e2e/run_e2e.sh
```

With no flow named, `run_e2e.sh` runs its default set. That set is the
regression set for call handling. Run it again after each change to call
handling.

Run selected flows:

```sh
E2E_ARTIFACTS=/tmp/uvoice-e2e \
SIPP_BIN=/path/to/sipp \
uvoice/test/e2e/run_e2e.sh flow-a uac_cancel uas_491_glare
```

Use an existing daemon binary:

```sh
E2E_ARTIFACTS=/tmp/uvoice-e2e \
SIPP_BIN=/path/to/sipp \
UVOICED=/path/to/uvoiced \
uvoice/test/e2e/run_e2e.sh
```

**Daemon binary.** `UVOICED` must name `uvoiced` itself, not a wrapper around
it. The runner stops the pid that it started. If `UVOICED` names a wrapper
that starts the daemon (`strace`, `valgrind`), that stop reaches the wrapper,
and a traced daemon outlives its flow. A wrapper that execs the daemon in
place, such as `taskset`, is safe.

The host library prefix is `/tmp/uvoice-host/prefix`. Set
`UVOICE_HOST_PREFIX` to select another prefix.

Run tls flows with the second SIPp build:

```sh
E2E_ARTIFACTS=/tmp/uvoice-e2e \
SIPP_BIN=/path/to/sipp \
SIPP_TLS_BIN=/path/to/sipp-tls/sipp \
uvoice/test/e2e/run_e2e.sh stream-uac-tls stream-uas-tls
```

**Scenario lint.** Before any flow, `prerequisites_check()` runs
`test/tools/sipp_scenario_lint.sh` over `test/sipp` and `test/e2e`.
Out-of-call scenarios in `test/e2e` are scenarios too. A `<nop>` behind a
checking message erases that check's failure (`test/sipp/README.md`, "When a
failed check is judged"). A lint hit therefore aborts the run and prints what
the lint found. The lint runs once per run: `prerequisites_check()` skips it
in the frame that `main()` executes again under `unshare`. An absent lint
stops the run too, because the lint ships in this tree.

**Prerequisites.** The runner needs these tools:

- `jq`, `ubus`, `unshare` and `python3`;
- `cmake`, when `UVOICED` is not set;
- `/usr/sbin/ubusd`;
- a SIPp build, which `SIPP_BIN` names;
- a TLS SIPp build, which `SIPP_TLS_BIN` names, only when a run drives a tls
  flow. The default set drives tls flows.

`python3` computes SRTP key digests. It also runs the CRLF ping client of
stream flows, and it reads captures for the no-cleartext check. It runs the
call agent of the MGCP flows too.

**ucode stub of `wan-flap-outage`.** `wan-flap-outage` also needs a `ucode`
interpreter with modules `ubus`, `uloop` and `fs`. Reason: its netifd stub is
a ucode script. `UVOICE_UCODE_MODDIR` names a directory that holds those
modules. The CLI suite reads this variable too (`test/cli/run_cli_tests.sh`).

If `ucode` or a module is absent, `wan-flap-outage` fails and names what is
absent (`netifd_stub_check`). Without its stub, that flow measures nothing
about the daemon, so a run counts its failure and exits non-zero. Other flows
still execute.

```sh
UVOICE_UCODE_MODDIR=/tmp/uvoice-host/ucode-mod \
uvoice/test/e2e/run_e2e.sh wan-flap-outage
```

**Artifacts and result.** The runner creates one artifact directory for each
flow. Each directory holds:

- config;
- daemon log;
- SIPp logs;
- ubus snapshots;
- packet timing files, when `tcpdump` and `tshark` can capture loopback
  traffic.

The runner prints one `PASS`, `FAIL`, `PARTIAL` or `SKIP` line for each flow.
It returns a non-zero status when one or more flows fail. Only the MGCP flows
skip, on a daemon without the MGCP protocol, and a skip is no failure.

## Address family knob

`E2E_FAMILY` selects an address family for a whole run:

- An empty value or `4` runs flows over `127.0.0.1`. The default set runs in
  this family only.
- `6` runs the v6 set below over `::1`.
- Any other value stops a run with status 2.

```sh
E2E_ARTIFACTS=/tmp/uvoice-e2e-v6 \
E2E_FAMILY=6 \
SIPP_BIN=/path/to/sipp \
SIPP_TLS_BIN=/path/to/sipp-tls/sipp \
uvoice/test/e2e/run_e2e.sh flow-a flow-b uas_reinvite_hold uac_cancel \
  uas_mwi_notify srtp-inbound stream-register-tcp stream-uas-tcp \
  stream-uac-tls uac_t38_offer
```

The knob is one setting for a whole run, not a second set of flows. A flow
name selects what a run drives. The family selects the address that every part
of a flow uses, so name and family never disagree inside a flow. Under 4, the
runner writes no `ipv6_enabled` option, and every family reader returns at
once.

These sites follow the family:

| Site | Under 4 | Under 6 |
|---|---|---|
| `config_write` | `sip_proxy`, `sip_outbound_proxy`, `sip_domain` and `sip_registrar` are `127.0.0.1` | all four are `::1`, and `config base` carries `option ipv6_enabled '1'` |
| `sipp_start`, `sipp_uac_start` | destination `127.0.0.1:<port>`, `-i 127.0.0.1` | destination `[::1]:<port>`, `-i ::1` |
| `port_bound_check` | `/proc/net/udp` or `/proc/net/tcp`, port only | `/proc/net/udp6` or `/proc/net/tcp6`, address `::1` and port |
| `capture_start`, `stream_capture_filter` | `udp port N`, `port A or port B` | `ip6 and udp port N`, `ip6 and host ::1` |
| `stream_ping_check` | connects to `127.0.0.1` | connects to `::1` |
| `loopback_prepare` | link state | link state and one bind of `::1` |

**Stream capture filter.** Under 6, `stream_capture_filter` names no port, by
design. Reason: a pcap port primitive reads the fixed IPv6 header's
next-header field (RFC 8200 section 3) and stops there. A packet behind a
hop-by-hop, routing or fragment header therefore matches no port term and
never reaches a capture. The no-cleartext reader walks that chain and must give
a verdict on it. A filter that dropped such packets would make that walk, and
its unreadable verdicts, unreachable. The reader selects both signaling ports
itself, after its walk.

**v4 literals.** A second config writer in `xfer-two-line` keeps its
`127.0.0.1` literals. No flow of the v6 set reaches that writer. Transfer
flows also assert dialog constants that carry a v4 literal.

**No fallback.** `loopback_prepare` never falls back to v4. The build host
disables IPv6 outside a network namespace. If a run asks for 6 and `::1` does
not bind, that run stops with the reason that its bind gave. A run that used
`127.0.0.1` there would report a v4 result under a v6 name.

**v6 set.** The v6 set is `family_v6_set` in `run_e2e.sh`. It holds these
flows of the default set:

- `flow-a`
- `flow-b`
- `uas_reinvite_hold`
- `uac_cancel`
- `uas_mwi_notify`
- `srtp-inbound`
- `stream-register-tcp`
- `stream-uas-tcp`
- `stream-uac-tls`
- `uac_t38_offer`

Under 6, the runner drives that set and nothing else. With no argument, it
selects all of it, so the default set never runs under 6.
`family_selection_check` refuses a requested name outside the set before
prerequisites and before any build. It prints that name and the set on
stderr, and `run_e2e.sh` exits with status 2:

```
$ E2E_FAMILY=6 uvoice/test/e2e/run_e2e.sh xfer-two-line
ERROR: E2E_FAMILY 6 drives the v6 set alone, and xfer-two-line is not in it
The v6 set is: flow-a flow-b uas_reinvite_hold uac_cancel uas_mwi_notify ...
```

Every flow outside the set asserts a v4 literal of its own, as transfer flows
do, or has no family reader. A mixed run would therefore report a v4 result
under a v6 name.

**Registration.** Flows of the set check registration themselves; no separate
flow checks it. Every flow of the set carries a REGISTER from the daemon:

- A UAS flow answers it in its call scenario (`test/sipp/README.md`, "The
  REGISTER branch").
- `uas_mwi_notify` answers it in its own scenario.
- In a UAC flow, a separate registrar from `register_prime()` answers it.
  That registrar writes its message log under `register/` in the flow
  directory.
- `stream-register-tcp` registers over a connection and keeps its line alive
  with OPTIONS.

### Family readers

Under 4, every reader below returns at once, so these readers add no assertion
to the default set. Under 6, they are the whole claim that a family adds to a
run. A flow's class selects its readers, because a flow with no media cannot
pass a media reader. `family_check` takes that class as `call` or `signal`.

Every flow of the v6 set:

- The `Via` sent-by of one message from the daemon is exactly `[::1]:<port>`
  (`family_via_check`). Every `Via` line of that message must carry it. A
  folded line fails, for the reason that section "The stream transports" gives
  for its token reader.
- Every `<sip:...>` URI of every `Contact` header of that message names host
  `[::1]` (`family_contact_check`). RFC 3261 20.10 lets one header carry
  several name-addr values, so this reader reads each one. A second URI in
  another family fails; it does not pass behind the first one. A header with
  no URI fails too. RFC 3261 19.1.1 puts brackets on a URI host, so a bare
  `::1` fails.
- `voice status` reports `.lines[0].family` as `"ipv6"`.
- Its capture holds at least one ip6 frame and no ip4 frame
  (`family_pcap_check`).

A flow that carries a call adds:

- Session descriptions from the daemon carry `c=IN IP6 ::1` and no `c=IN IP4`
  line (`family_sdp_check`).
- Whole address half of the programmed stream, read from `voice.stub`
  `dump` (`family_stream_check`). This reader requires all eight keys that
  every dump writes:
  - `local_ipver` and `remote_ipver` are 6;
  - `local_addr` and `remote_addr` are `::1`;
  - `remote_port` is the media port that the peer offered;
  - `remote_rtcp_port` is that port plus one;
  - `local_rtcp_port` is `local_port` plus one.

  Both derived RTCP ports match what live backends compute when no `a=rtcp`
  names another port (`hal.h`, `struct hal_addr`). `test_hal_stub` links
  `src/hal/stub/stub.c` but reads no stream dump. This reader is therefore the
  only automated check of those keys. `uac_t38_offer` runs it twice: once for
  its audio leg, and once for the image session that replaces that leg, on the
  media port plus two.
- Keys `rtcp_remote_ipver` and `rtcp_remote_addr` are absent. Reason: a dump
  writes them only for a stream that took an RFC 3605 address, and no scenario
  of the set names one. For a scenario that names one, this reader requires
  `rtcp_remote_ipver` present and equal to 6. It does not check
  `rtcp_remote_addr`.

A tls stream flow adds an ip6 branch of the no-cleartext reader. In the v6
set, that flow is `stream-uac-tls`. Tcp stream flows do not run that reader,
because over tcp, SIP text is cleartext by design.

A UAC flow reads `Via` and `Contact` of its REGISTER off its registrar's
message log, `register/sipp-messages.log`. Its SDP, status and capture
readers stay on the flow directory. Every other flow of the set reads its one
log.

`stream-uas-tcp` is the one flow of the set whose daemon sends no request to
its call peer. Its line registers against a separate registrar, for a reason
that section "The stream transports" gives. Its `Via` reader therefore reads
the daemon's 200, and that 200 echoes the caller's `Via` back. That `Via`
shows a v6 exchange with a bracketed sent-by. The daemon wrote the `Contact`
of that 200 itself, so in this flow, the `Contact` reader checks the daemon's
own address.

`uac_cancel` carries a call that nobody answers, so it runs signaling readers
alone. Its daemon programs no stream and writes no session description.

### Reader assumptions

**Stream reader.** `family_stream_check` does not ask for a running stream. A
scenario can end its call a few hundred milliseconds after the answer, and
then leaves no window for a ubus poll. `uas_reinvite_hold` is one of those.
The stub keeps a stream's description after its stop, so the recorded family
is one answer before and after that stop. Flows check the running flag where a
flow owns a window for it: in `media_port_wait` and in `srtp_stream_wait`.

**Capture reader.** The ip4 count of `family_pcap_check` is zero by
construction. A v6 run captures with an `ip6` filter. A run that fell back to
v4 therefore leaves an empty window, not a window of v4 frames. Its ip6 count
is the reading that fails there.

The runner reports an unreadable capture as `PARTIAL`, and never counts it as
zero. These captures are unreadable:

- a file that does not open;
- a file shorter than a pcap header;
- an unknown magic;
- a link type other than ethernet;
- a record boundary past the file's end.

**ip6 walk.** The no-cleartext reader walks ip6 headers by RFC 8200
(`ip6_walk`). A fixed header is 40 bytes. Its payload length is at offset 4,
and its next header at offset 6. The walk follows the extension chain for
hop-by-hop (0), routing (43) and destination options (60). Each of these
headers states its own length in eight-octet units past its first eight.

A fragment header (44) and an unknown next header make a capture unreadable,
so neither passes silently. Reason: a fragment carries a piece that the reader
cannot put back together, and an unknown next header hides what follows it.

A v6 stream capture names no port, so every ip6 packet of the loopback address
reaches the walk. An ip6 packet of a protocol other than tcp or udp also makes
a capture unreadable. Loopback traffic in the namespace carries no such
packet. The IPv4 branch (0x0800) and the ip6 branch share one transport
reading, `transport_scan`, which selects both signaling ports.

### Scenario edits

Two scenario forms let one scenario run in both families. Under 4,
`[media_ip]` renders bytes identical to `[local_ip]`, and `[remote_host]`
bytes identical to `[remote_ip]`.

**Origin line.** Scenarios of the v6 set write their origin line as
`o=sipp 1 N IN IP[media_ip_type] [media_ip]`, not as
`o=sipp 1 N IN IP[local_ip_type] [local_ip]`. Reason: SIPp renders
`[local_ip]` bracketed inside a URI and `[media_ip]` bare (SIPp source
`src/call.cpp`, `local_ip_w_brackets` against `media_ip`). RFC 4566 section
5.2 gives an origin line an unbracketed address. Without `-mi`, SIPp sets
`media_ip` from `local_ip`, and `media_ip_type` from that socket's family
(`src/sipp.cpp`). Under v4, both forms therefore render one string. Each `c=`
line uses the media keywords too.

The v6 set drives these scenario files with an origin line:

- `uas_basic_call.xml`
- `uac_basic_call.xml`
- `uas_reinvite_hold.xml`
- `uac_cancel.xml`
- `uac_srtp_call.xml`
- `uac_stream_call.xml`
- `uas_stream_call.xml`
- `uac_t38_offer.xml`

**Received parameter.** Each scenario that rebuilds the `Via` of its REGISTER
answer writes `received=[remote_host]`, not `received=[remote_ip]`:

- registrar `uas_register_prime.xml`, which `register_prime()` drives for
  every UAC flow;
- registrar `uas_register.xml`, which only `test/sipp/run_sipp.sh` drives;
- REGISTER branches of UAS call scenarios.

Reason: RFC 3261 20.42 gives `received` a bare address, and `[remote_ip]`
renders bracketed under v6. `[remote_host]` is the bare form, because SIPp
strips brackets off its destination argument when it splits host from port
(`src/strings.cpp`, `get_host_and_port`). Under v4, both render one string.

**Family preference.** No e2e flow checks the resolver's family preference.
Every flow names a `::1` literal, and a literal reaches the account layer
without a family sort. Unit suite `test_account` checks that preference
instead (`sip_dns_family_sort`).

## The two SIPp builds

The harness drives SIPp 3.7.3 in two builds, and each flow names its build.
`SIPP_BIN` names the build without TLS. Every udp and tcp flow runs on it:

```sh
cmake . -DUSE_SSL=OFF -DUSE_GSL=OFF && make
```

`SIPP_TLS_BIN` names the TLS build. Tls flows run on it, and no other flow
does:

```sh
cmake . -DUSE_SSL=ON -DUSE_PCAP=ON -DUSE_GSL=OFF && make
```

**Why the split.** A TLS-enabled build (`sipp -v` prints `-TLS-`) fails every
flow whose scenario carries an `rtp_echo` action. Every scenario step of such
a run matches, with no timeout and no unexpected message. The call completes,
and SIPp then counts it failed and exits 253. Reason: under `USE_TLS`, all of
the `rtp_echo` path runs through SRTP contexts of the call (`src/call.cpp`,
actions `STARTAUDIO` and `STOPAUDIO`). A plain RTP/AVP session has no keys for
them.

Exit 253 is `EXIT_RTPCHECK_FAILED`, which SIPp defines as -3
(`include/defines.h`). `sipp_exit()` returns it only when its RTP or ECHO
error count is above zero (`src/sipp.cpp`).

**Stream-scenario rule.** Stream scenarios carry no `rtp_echo`. One scenario
file therefore serves both a tcp twin and its tls twin, and transport is the
only difference between their runs. Flows lose no coverage by it. The stub
backend has no media plane, so no RTP reaches SIPp on any of these flows.
These flows assert that media proceeds against the programmed stream
(`media_port_wait`). Twins do not run SRTP over TLS, because a tls flow would
then negotiate a session different from its tcp twin's.

These scenarios carry `rtp_echo` and run on the non-TLS build:

- `uac_basic_call`
- `uas_basic_call`
- `uac_reinvite_hold`
- `uac_g722_refused`
- `uac_g726_refused`
- `uac_verstat_call`

## The stream transports

Stream flows drive the TCP and TLS stack in twin pairs, one pair for each
message family:

- outbound call: `stream-uac-tcp` and `stream-uac-tls`;
- inbound call through a listener: `stream-uas-tcp` and `stream-uas-tls`;
- registration with its OPTIONS keepalive: `stream-register-tcp` and
  `stream-register-tls`;
- message-waiting subscription: `stream-subscribe-tcp` and
  `stream-subscribe-tls`.

All stream flows are in the default set.

**Harness.** Two parts of the harness depend on transport:

- `sipp_ready_wait` reads `/proc/net/tcp` for a tcp or tls flow, and
  `/proc/net/udp` for a udp flow (`port_bound_check`). Under family 6, it
  reads `/proc/net/tcp6` or `/proc/net/udp6` instead. Both tables hold the
  local address in one column. That column names a server's listening port and
  a client's bound source port, so one expression answers both.
- `config_write` takes a `transport` argument, which writes
  `voip.base.transport`. It also takes a block of further base options. Stream
  flows use that block for two options that no channel alias can reach
  (`mwi_subscribe` and `keepalive_interval`). A udp flow leaves both arguments
  empty, so its config file carries neither.

**Twins.** A flow takes its name from the DUT's role, and a scenario from the
SIPp role. `flow-a` follows this convention too. In `stream-uac-*`, the DUT
calls out, and SIPp runs `uas_stream_call.xml`. In `stream-uas-*`, the DUT
answers through its listener, and SIPp runs `uac_stream_call.xml`. Both twins
of a pair run one scenario file.

**Token checks.** The runner reads transport tokens off the message log;
scenarios do not assert them. Reason: a SIPp `regexp` attribute takes no SIPp
keyword, so a scenario cannot name a run's transport. One file must serve both
twins. `stream_via_check` and `stream_contact_check` read header lines of one
logged message, which they select by direction and by first line. They
assert:

- Every `Via` line of that message names the token, and each line carries
  exactly one Via value. These messages fail:
  - a message with a second Via over another transport, which does not pass
    on its topmost Via;
  - a message with no Via at all;
  - a message with a comma inside a Via line. A comma folds several values
    onto one line (RFC 3261 7.3.1), and a matcher reads only the first value.

  The DUT writes one value per line (`src/proto/shared/hdr.c`). This comma rule
  is therefore an assertion on that shape, not a limit of the reader.
- `Contact` carries `;transport=<token>` inside its angle brackets, where a
  URI parameter belongs (RFC 3261 19.1.1). A parameter after `>` is a header
  parameter and never reaches a registrar's binding, so it fails this check.

A message log is the wire's countable copy, as for SRTP attribute counts.
SIPp names the transport in its marker line ("`TCP message sent`", "`TLS
message received`"). Readers therefore match all three names.

**CRLF keepalive, both directions.** Both `stream-uas-*` flows and both
`stream-register-*` flows open one connection to the DUT's listener. They send
the double CRLF ping of RFC 5626 5.4 and read the answer off that socket
(`stream_ping_check`). That one exchange is the whole claim: a pong is a
connection payload, so over tls it is inside the record layer.

SIPp cannot make this exchange. Its `<send>` body is a SIP message, and no
scenario can put four bare bytes on a wire and read two back. Its client is
therefore a few lines of `python3`.

Both `stream-uas-*` flows also run with `keepalive_interval` at one second.
They require the line's `crlf_sent` counter to move. The DUT's own ping is
then inside the window that the capture check reads.

**No-cleartext check, and what it proves.** `stream-uas-tls` captures both
signaling ports and reads that capture with `python3`
(`pcap_cleartext_check`). It does not use `tshark`, so a host with `tcpdump`
but no dissector does not skip the check silently. Its exact matcher:

- It searches every TCP segment payload on either signaling port for byte
  strings `SIP/2.0`, `REGISTER sip:`, `INVITE sip:`, `OPTIONS sip:`,
  `SUBSCRIBE sip:`, `BYE sip:` and `Call-ID:`. One hit fails the flow.
- A payload of CR and LF bytes alone, of any length above zero, is a keepalive
  finding. This rule is a byte class, not an exact compare against `\r\n\r\n`
  and `\r\n`. Reason: TCP can coalesce two pings into one segment, and an
  exact compare would match neither ping and report that segment clean.
- Any UDP packet on either signaling port fails the flow, because a stream
  account sends no datagram at all.
- A capture with no payload on those ports is unreadable, not passed. An empty
  capture never reads as a clean one.
- A capture whose record boundaries do not end exactly at the file's end
  is truncated, and so unreadable, never clean. Without that rule, a capture cut
  short after one valid payload would pass on what it holds.

The reader rests on two assumptions that it does not check:

- It does no TCP reassembly. It searches each segment payload alone, so it
  would miss a token split across two segments. That assumption holds for
  these captures: they are loopback captures of a daemon on the stub backend,
  where every SIP message uses one segment.
- It handles IPv4 and IPv6 over ethernet, which `tcpdump -i lo` produces on
  the build host. A capture with another link type is unreadable, not clean.
  Section "Address family knob" states what the ip6 walk covers and where
  it stops.

A capture reading cannot prove that anything is encrypted. It proves that
cleartext forms are absent from the window that it saw. A tcp twin gives that
absence its meaning. Run by hand against a capture of `stream-uas-tcp`, this
reader reports that traffic as cleartext. That traffic includes the DUT's
`\r\n\r\n` pings, and the ping and pong of the client above.

**Two SIPp behaviors over a stream transport.** SIPp 3.7.3 shows these two
behaviors:

1. SIPp connects at startup. If the port that it dials is not listening, SIPp
   leaves at once ("Error on TCP connection, remote peer probably closed the
   socket: Connection refused"). Both `stream-uas-*` flows therefore start
   the daemon first, and wait for `listener` `true` before SIPp runs.
2. A SIPp UAC answers an out-of-call REGISTER on its dialog's socket. It
   closes that socket when its out-of-call scenario ends. A capture then shows
   a FIN from SIPp between the 180 and the 200, and the answer arrives on a
   dead connection.

Together, these behaviors are the reason that both `stream-uas-*` flows
register their line against a separate registrar:

- The daemon comes up before its caller, so that caller cannot take a first
  REGISTER.
- A caller would answer a REGISTER on its dialog's socket, and its call would
  then lose its connection.

**Registrar of the stream-uas flows.** `register_prime()` starts
`uas_register_prime` over the flow's transport, before the daemon. The line
connects to it and registers, and that registrar closes its connection when
it exits. The caller then connects at once from that address and port. The
daemon takes this connection as one that the peer opened, so the line keeps
its binding (`src/proto/sip/account.c`, `acct_reconnect_needed()`).

**Re-register check.** These flows require `registered` at ring time, at the
connection check and at the keepalive check. Those checks cannot see a
restart of registration. Reason: a caller answers any REGISTER that it gets,
so a line is registered again before its INVITE is read. Each flow therefore
also reads its daemon log. A re-register after the up report of an accepted
connection fails it (`accepted_reregister_check()`).

**The gap between the two runs.** A keepalive that a line sends between its
two SIPp runs opens a connection of the line's own. If the caller already
listens, that connection is a reconnect, and its REGISTER goes to the caller.
If nothing listens, that connect fails and forces nothing
(`src/proto/sip/account.c`, `st_conn_cb()`). The first keepalive leaves one
interval after the registration: 1 s in these flows, which set
`keepalive_interval` 1. The gap is about a tenth of that interval, so no
keepalive falls inside it.

REGISTER token checks stay in outbound twins. Those twins cover both
transports, and in them, the call peer answers each REGISTER.

**TLS bundle.** The daemon reads its server identity from fixed paths in
`src/proto/sip/tls.h` (`/tmp/voip_tls/{cert.pem,key.pem,ca.crt}`), which are
not configurable. `stream-uas-tls` therefore composes them from EC fixtures in
`test/unit/tls` before the daemon starts, and removes them after its run.
That directory is an absolute path outside the network namespace, and a
certificate fetch or a second harness run can already hold it. The harness
therefore refuses to install over a bundle that it does not own. That flow
then fails with a clear message and leaves a foreign bundle alone.

`tls_bundle_install()` reads all three files back before the daemon starts.
`tls_bundle_record()` writes each directory listing to `tls-bundle.txt`. A
pair that fails to load therefore names itself, instead of a bare "no
listener" verdict.

`stream-uac-tls` installs nothing and reads back a client-only state. A box
runs in that state until its bundle arrives. That flow asserts `listener`
`false` only when no foreign bundle is present.

**Limit: one unexplained listener failure.** `stream-uas-tls` failed once with
`inbound client only` and `tls listener stays down, no server certificate yet`
in its daemon log. Its certificate pair was not loadable at that instant,
although the harness wrote it just before. Its cause is not known. The
read-back is a diagnostic, not a fix.

## REGISTER traffic

An account sends REGISTER and call requests to one resolved outbound target.
`acct_cfg_derive()` stores a separate registrar URI, but `acct_targets_load()`
supplies one target set to both registration and calls.

Every flow that carries a call holds a live registration before its call peer
acts. Flows wait for `.lines[i].status` `registered`, not for a call state, so
a line in REFRESHING counts too (`registration_wait`).

**UAS flows.** SIPp takes an out-of-call scenario only in UAC mode, so one UAS
scenario takes every request of a run. Each UAS call scenario therefore
answers REGISTER in a branch of its own (`test/sipp/README.md`, "The REGISTER
branch"). That branch answers every refresh too. A flow waits for
`registered` on each served line before its first seize or dial.
`outbound_dial_start()` holds that wait for most flows.

**UAS budget.** A UAS run's `-m` budget is one instance per REGISTER that
reaches SIPp, plus one per call. `flow_prepare()` therefore passes `-m 2` for
one call. `uas_sipp_verdict()` requires at least that many successful
instances, so a REGISTER that the scenario fails to answer fails the count.
`sipp_abort_classify()` still counts REGISTER aborts apart, for a scenario
with no REGISTER branch. The runner checks successful instances and daemon
state, not SIPp's aggregate exit status.

**UAC flows.** A calling SIPp puts its INVITE on the wire as soon as it
starts. `register_prime()` therefore registers the line first:

1. It starts `uas_register_prime` as a separate registrar, with `-m` equal to
   its served line count and with `-key expires`. Its default expiry is 3600
   seconds, which the E2E config requests.
2. It starts the daemon, and passes the flow's startup mode and flash service
   to `daemon_start()`.
3. It waits for the registrar to exit with status 0, then for `registered` on
   each served line.

The caller then starts inside that binding. Its out-of-call scenario,
`register_ooc.xml`, answers a refresh with 200 and the expiry of keyword
`expires`. The registrar writes its logs under `register/` in the flow
directory.

**Expiry of the out-of-call scenario.** `sipp_uac_start()` passes
`-key expires 3600` on every run, because SIPp stops on a keyword that no
`-key` defines. A flow that needs another expiry passes its own
`-key expires` among its extra arguments, and that value wins. Reason: SIPp
keeps the last value of a repeated `-key` (SIPp 3.7.3 `src/sipp.cpp`, where
arm `SIPP_OPTION_KEY` assigns map `generic`).

## SRTP flows and their key digests

These flows cover SDES SRTP:

- `srtp-outbound`
- `srtp-inbound`
- `srtp-accept-plain`
- `srtp-refused`

`srtp-accept-plain` checks a plain answer. An SRTP-enabled leg answers an
initial RTP/AVP offer plain. Its own hold and resume re-INVITEs then stay
RTP/AVP, with no crypto line.

SRTP flows are in the default set, because they need no hardware and no
second SIPp build. `srtp_enabled` is a base option, so the runner writes it
inside `config base` through `config_write`. Fax flows append options behind
that function, but those options reach `config channel`, so that method
cannot carry `srtp_enabled`.

Scenarios assert SDP text. Flows assert what that SDP text produced:

- suite on the stream;
- both key digests;
- suite in `voice status`;
- zeroize behind the stop.

`srtp-accept-plain` asserts that its running plain leg has none of the first
three, and the zeroize behind the stop.

**Digest recipe.** The stub reports `srtp_key_tx_crc` and `srtp_key_rx_crc`
for a running stream, and never a key byte (`src/hal/stub/stub.c`,
`srtp_dump()`). Each digest is a standard reflected CRC-32 of the raw 30 byte
key, as `zlib.crc32()` and `binascii.crc32()` produce it. A digest travels as
a 64 bit field. Reason: a blobmsg INT32 renders signed in JSON, so about half
of all digests would reach a reader negative. A flow reads the `a=crypto` line
off the SIPp message log and decodes its base64. It then computes
`zlib.crc32()` of those 30 bytes and compares (`key_crc32_get`).

These two numbers come from different places. One comes from the programmed
stream, the other from SDP that its peer read off the wire. A stream keyed
with anything other than what it negotiated fails its flow. Direction decides
which key is which, because an SDES key is per direction (RFC 4568 section
5.1.2). The transmit key is the DUT's own key on the wire. The receive key is
the peer's offered or answered key.

**What the scenarios cannot do, and the runner does.** A SIPp regex cannot
count. A message that also carries a duplicate tag, or an attribute under a
tag that nobody offered, still satisfies every "this attribute is present"
check. An inverse check denies only one named tag. The runner therefore counts
`a=crypto` lines in the message itself (`sipp_crypto_lines_count`):

- exactly two in a DUT offer;
- exactly one in a DUT answer;
- every attribute bare: three fields separated by whitespace, and no session
  parameter.

**Line end.** Scenarios anchor a line end on its carriage return. They rest on
three facts about SIPp 3.7.3:

- SIPp honors `\r` and `\n` in a `regexp` attribute:
  `...{40}\r\na=crypto:2 AES` matches a real CRLF between two attributes.
- SIPp decodes no XML character reference, so `&#13;` reaches its regex
  engine as five literal characters and matches nothing.
- SIPp compiles without `REG_NEWLINE`, so `$` anchors at the whole body's
  end and fails on every line except the last.

A `[[:cntrl:]]` class accepts HTAB, so a tab separated session parameter would
read as a line end there.

**Echo guard.** An SDES key is per direction, and one side never echoes the
other side's key (RFC 4568 section 5.1.2). An implementation that answered
with its peer's key would satisfy both digest comparisons above. Reason: both
numbers would come from one set of 30 bytes. `srtp-inbound` therefore first
asserts that its answered key differs from both offered keys. Keys carry 240
bits of entropy, so an equal key means an echo. `srtp-outbound` runs no echo
guard, because there that comparison would assert nothing about the daemon
(`srtp_keys_differ_check()`).

**Switch check.** `srtp-refused` proves that the leg switch caused its 488.
`acct_srtp_transport_check()` in `src/proto/sip/account.c` warns at each
derive of a line when `srtp_enabled` meets a transport that does not encrypt
signaling. That warning is therefore present exactly when the switch is on,
and the flow requires it to be absent.

The flow reads the warning off `daemon.log`, not off the config that the
runner writes. A flow whose config drifts from what it claims to prove
therefore fails, and does not agree with itself.

**Post-stop dump contract.** A stopped stream reports `srtp_suite` 0 and
carries no digest field at all. Both keyed flows read that back after hangup.
That reading proves what a reader of a stopped stream sees, not the byte wipe
behind it. Reason: `srtp_dump()` omits both digests whenever the suite is
NONE, so a flow cannot tell a wiped buffer from a suite that was only cleared.
Tests one layer down cover the wipe itself:

- `stub_stream_stop()` clears the stub's srtp block with a memset.
- `test_srtp_wipe` in `test_call` checks the call layer's release wipe,
  through `call_slot_scan()` of `src/proto/sip/call.c`.

**What the artifacts hold.** SIPp message logs under `E2E_ARTIFACTS` carry
the full wire SDP, with each `a=crypto` line's base64 key. The digest check
reads those logs, so those keys are there by construction. Scenario keys are
fixed test constants. DUT keys are ephemeral: the DUT draws them for each leg,
and they die when their flow ends. Artifacts are scratch output; do not commit
them. The key material rule applies to the daemon: no key byte reaches a
uvoice trace, log or ubus dump.

**Limits.** These flows cannot assert wire crypto. The stub backend carries no
media plane, these scenarios run no `rtp_echo`, and nothing in these flows
encrypts or authenticates a packet. They assert negotiation, the keys' path
down to a backend, and the key wipe behind it. Proof of a protected wire
belongs to a hardware bench and to a run against a live provider.

The udsp backend drops a packet that fails authentication
(`udsp/src/core/net.c`, `net_slot_fill()`). On the host, udsp suite
`test_media` checks that drop with a damaged authentication tag
(`case_stream_diag_srtp()`).

## The transfer flows

**Scope.** Transfer flows drive all five REFER roles. Flow `cert-fetch` drives
the certificate fetch that a TLS line needs (section "Certificate fetch").
These flows are in the default set.

**Out-of-call peer.** A peer must answer calls that the DUT places by itself.
SIPp cannot do that from its `-sf` scenario. A UAS run originates nothing, and
a UAC run answers only what belongs to a call that it placed.

**Out-of-call scenarios.** A referred child INVITE, a consult call and the
ordinary call that a Replaces flow replaces all open a dialog that nobody
asked for. They therefore reach an out-of-call scenario (`-oocsf`). Four files
take that role:

- `test/sipp/xfer_peer_ooc.xml` answers a call;
- `test/sipp/xfer_busy_ooc.xml` refuses a call with 486;
- `test/sipp/xfer_replaced_ooc.xml` answers a consult call and later ends it
  with its own BYE;
- `test/sipp/xfer_ringing_ooc.xml` rings a call and never answers it.

None of the four answers a REGISTER. A line therefore registers first, against
the `register_prime()` registrar, and no REGISTER reaches these runs (section
"REGISTER traffic").

**Call generator.** Two SIPp counters do not mean what they seem to mean. In
UAC mode, `-m` drives the call generator. A value above one places that many
INVITEs; it does not allow that many calls. Transfer flows therefore keep
`-m 1` and raise `-l` for headroom.

**Call limit.** `-l` bounds the outgoing generator alone.
`CallGenerationTask` reads `open_calls_allowed`, and an instance that an
out-of-call scenario opens never passes through it. `-m` is what bounds these
runs.

**Success count.** SIPp counts only main-scenario calls in "Successful call".
A flow whose child call belongs to its out-of-call scenario therefore expects
one success, not two.

**End of each call.** If a call is still up when its last scenario ends, SIPp
aborts it with a BYE of its own. That BYE carries reversed tags. The DUT
answers it 481, the leg stays up, and its record never closes. Each transfer
flow therefore ends every call that it opened before SIPp leaves. Either the
subscriber's hook ends a call first, or its scenario holds a trailing pause
open for that end.

**Header readers.** The harness, not scenarios, checks header values, as
stream flows check their Via token in the harness. Reason: a SIPp regexp is an
XML attribute value. An attribute cannot hold the `<` that opens a name-addr
(RFC 3261 25.1; Refer-To grammar: RFC 3515 2.1). Five readers do these checks:

- `xfer_hdr_check` for an exact value;
- `xfer_hdr_match_check` for a value that carries a Call-ID and a tag that
  nobody can predict;
- `xfer_hdr_absent_check` for a header that must not be there;
- `xfer_hdr_all_check` for one exact value on every message that a first line
  selects, such as `Contact` on both NOTIFYs;
- `xfer_request_line_check` for a whole request line, such as a child
  INVITE's request URI.

**Duplicate headers.** `xfer_hdr_check` and `xfer_hdr_match_check` fail a
message that carries its header twice. That rule has three sources:

- For a promoted Replaces, it is a requirement of RFC 3891 section 3. Replaces
  admission in this stack enforces that rule too (`replaces_admit()` in
  `src/proto/sip/call.c`).
- For Refer-To, it is the single value that RFC 3515 2.1 allows.
- For other headers, it is a harness rule: a reader does not pass on the
  first of several lines.

**Bracket expressions.** Every bracket expression in these readers' patterns
names its characters. Reason: a range does not mean one thing across the
pattern engines of this repository:

- `[!-~]` matches through glibc `regcomp` under `LC_ALL=C` and
  `LC_ALL=POSIX`;
- `[!-~]` does not match under `en_US.UTF-8`, where the range collates;
- to a shell pattern, `[!` opens a negated class and not a range.

**Class in a scenario.** A SIPp scenario can still use a range class safely,
and `xfer_peer_ooc.xml` does. SIPp compiles an `ereg` pattern when it loads a
scenario, before its one `setlocale` call. It therefore compiles each pattern
in the C locale. A harness check that silently matched nothing would pass a
wrong value, so the harness names its characters.

**Two-name window.** The harness reads the two-name contract in a window
that the peer holds open.
That window covers two states:

1. Between a child INVITE and its answer, the parent is suppressed and its
   child holds no name yet. Both legs read as shadows.
2. After that answer, the child takes its parent's UI slot, and the parent
   becomes a shadow.

Neither state lasts long enough for a poll on a loopback host.
`xfer_peer_ooc.xml` therefore rings for two and a half seconds.
`uac_xfer_refer.xml` pauses for one and a half seconds before it ends the
parent with a BYE.

**Two-line flow.** `xfer-two-line` is the software half of the on-unit
two-line transfer. One peer address reaches
either subscriber, and the transfer target is an ordinary number. Two facts
make that possible:

- Two channels that ask for one `sip_local_port` share one socket
  (`src/proto/sip/transport.c`, `sip_sock_open`).
- The daemon routes an inbound request by its request URI's user part
  (`src/proto/sip/account.c`, `account_for_request`).

**Attended code.** `xfer-two-line` uses the attended code, not the blind one.
Both codes put one identical REFER on the wire. A blind transferor reports at
its 202, and never tells its machine whether the referral was carried out.
Completion would then not be observable on the DUT side. Blind transfer has
its own flows, `xfer-blind` and `xfer-blind-grace`.

**What the two-line flow does not prove.** The harness has no analog
hardware. `xfer-two-line` injects hook events over `voice.stub`, and no audio
crosses between its two subscribers. No flow of this harness puts a REFER in
front of a real switch.

## Certificate fetch

**Drive.** `cert-fetch` drives `files/uvoice_tls_cert.sh` through the daemon,
not by hand:

1. A TLS line starts the fetch (`src/proto/sip/account.c`, `acct_tls_take`).
2. The daemon spawns the helper with its four contract arguments, and reads
   its exit code (`src/event.h`, `event_tls_cert_start`).
3. A live `voice reload` reaches two more paths: an unchanged ETag and a
   refused scheme. Before each reload, the flow moves one of five options
   that a rebuild watches (`acct_tls_cfg_changed`).

**Lock.** One lock covers both fixed paths. `/tmp/voip_tls` and
`/tmp/voip_tls_stat.txt` are absolute paths outside the network namespace, so
two harness runs on one host meet on them. Every flow that reads or writes
either path first takes `/tmp/voip_tls.lock` with `mkdir`. Reason: `mkdir`
creates or fails in one step, with no read-then-write window of its own. A
flow that cannot take this lock fails and names its holder. A lock that a
killed run left behind names the pid of that run, so an operator can clear it.

**Scratch files.** Helper scratch files follow the status token
(`tls_status_owned`), not the three-valued bundle token (`tls_bundle_owned`).
A flow that only found the bundle directory did not write `voip_cert.tgz`,
`header.txt`, `prev_header.txt` or `last_error_reason.txt`. If that flow
removed them, it would destroy the work of another helper.

**Helper path.** `UVOICE_TLS_CERT` names the script that the daemon spawns
(`src/event.c`, `event_tls_helper_take`). It is an environment override, and
`cert-fetch` exports it around one spawn. Every other flow must find no helper
at all. Reason: a TLS line with no certificate URL would run that helper, and
it would write into `/tmp/voip_tls`, which `stream-uas-tls` owns.

**Server.** The server is a few lines of `python3`, not a general file server.
Reason: the helper reads three things off each response, and each one must be
exact:

- status line;
- a `Content-Type` of `application/x-gzip`;
- a `Content-Length` under the helper's ceiling of 5000 bytes
  (`TLS_MAX_FSIZE`).

The server answers a stable `ETag`, which an unchanged fetch compares.

**Listener rebind.** `src/spawn_run.c` starts each helper with
`posix_spawn(3)`, so no helper child holds a copy of a TLS listener when
its line binds the port again. A reload that moves a `tls_*` option starts the
certificate helper from `acct_tls_take()`. That same frame rebuilds the line:
it closes its listener and binds a new one on that port.

**Inherited listener.** A `fork()` child would hold a copy of every daemon
descriptor until its `execve`. While a child held that copy, a `close()` in
the daemon would not destroy its listening socket. Linux refuses a bind
against a `TCP_LISTEN` holder, whatever `SO_REUSEADDR` says on either side.
The line would then come up dead with `Address already in use`, and its VoIP
firewall rules would go with it until the next reload.

**Two C libraries.** `posix_spawn(3)` closes that window in a different way on
each C library:

- On musl, the caller blocks on a `CLOEXEC` pipe whose EOF arrives inside
  `do_close_on_exec()`. The `CLOEXEC` descriptors are gone from the child
  when that call returns.
- On glibc 2.34 and later, the caller wakes earlier, at the vfork wake. A
  file action of the spawn closes every inherited descriptor from 3 upward in
  its child before its exec (`posix_spawn_file_actions_addclosefrom_np(3)`).

**Rebind flows.** `cert-fetch` runs on the shared `daemon_port`, because port
state of another flow is not a cause of a failed bind. `reload-loop` drives
thirty such reloads by default (`E2E_RELOAD_ITERATIONS`), and reads the
listener back after each one.

**Status file.** The flow compares `/tmp/voip_tls_stat.txt` byte for byte, so
its comparison is an equality and not a search. The helper writes each status
string with no trailing newline. That file
is a fixed path like the bundle directory, so `/tmp/voip_tls.lock` serializes
it too. Teardown removes it with helper scratch files, under the status
token.

**Parts outside the flow.** The flow does not touch these parts of the helper:

- FTP branch;
- md5sum-unchanged path behind it;
- `clear` verb;
- retryable exit-1 classes;
- event post on a repeated failure.

**Unit tests of the helper.** `test_event` drives FTP branches, status strings
and exit contract of the shipped helper against a fake `curl` on PATH. No test
runs this helper against a real FTP server. Only such a run exercises its
credential handling and a server's 213 answer.

## Verstat caller identity flow

**Field message.** `verstat-cid` drives a field message at the real daemon
(`test/sipp/uac_verstat_call.xml`). Its scenario carries the header set of
that capture. Its From carries the caller's number twice: once
in its quoted display name and once in its user part. Each copy has a
`verstat` parameter behind it.

**Display rule.** `cid_display_take()` (`src/proto/shared/hdr.c`) drops a
display name that only repeats the caller's number up to its first semicolon.
Reason: in the field, a whole display name reached the line, and the FXS
caller identity read `2015550123;verstat=No-TN-Validat`, cut at the 33 bytes of
`struct line_cid` (`HAL_CID_STR_MAX`).

**Requirements.** The flow reads the line from its stub dump, and requires
these values:

- `cid_number` `2015550123`;
- an empty `cid_name`;
- `cid_blockinfo` `O`;
- both privacy flags clear.

**Absence letter.** The letter `O` is the GR-30 reason for a field that is
absent and not withheld. `cid_fill()` (`src/line/line.c`) sets it whenever
the number or the name is empty and that field is not withheld. In this flow,
an empty name sets it. A withheld field carries `P` instead, so the flow
reads both privacy flags beside that letter.

**Three values.** The flow places three calls under one name, one per verstat
value:

- `No-TN-Validation`;
- `TN-Validation-Passed`;
- `TN-Validation-Failed`.

Each call gets its own daemon, config and artifact directory. The rule under
test is the display name's shape, not one value's text. A pass on
one value alone says nothing about the other two. Its verdict line names
whichever value failed.

**Keyword.** The flow passes each value in the SIPp option `-key`, which it
hands to `sipp_uac_start` as an extra argument. Its position on the command line
does not matter. SIPp reads its argument list in four passes (SIPp 3.7.3
`src/sipp.cpp`, its option table and `CHECK_PASS` loop). It gives `key`
pass 1 and `sf` pass 2, so every keyword is defined before SIPp reads any
scenario.

**Request URI.** A request URI names the subscriber that the config writes,
as the field message names the SIP user name of its line.
`account_for_request()` (`src/proto/sip/account.c`) therefore finds a line by
name, not by socket owner.

## Inbound churn flow

**Field sequence.** `inbound-churn` drives a field sequence at the real
daemon (`test/sipp/uac_inbound_churn.xml`). Its field capture
holds these events:

- an INVITE that the device answered with 100 and no 180 for 6.4 seconds;
- a CANCEL behind it;
- a new offer 17 seconds later;
- a REGISTER refresh every 30 seconds on the same socket.

**Compressed shape.** The flow compresses that shape. It offers fifty calls,
one at a time. Its subscriber answers half of them, and its peer cancels the
other half. Its binding refreshes every five seconds during a run.

**Requirements.** The flow requires these readings:

| Reading | Requirement |
|---|---|
| every call of the capture | its first 180 stands inside 1.0 s of its first INVITE |
| capture against run | it holds all fifty calls, so a lost packet cannot shorten the reading |
| hook record and daemon log | twenty-five answered calls, each seized and released, and no call refused 486 |
| SIPp | fifty successful calls and no unattributed abort |
| daemon log | at least one registration refresh, with its count in the verdict |
| `voice status` and stub | line idle and silent behind the last call |

**180 reader.** `timing_180_check()` reads the capture the way
`timing_491_check()` reads the 491 retry. It keys each INVITE and 180 by
`sip.Call-ID`, and takes the first copy of each. It filters on source
port, so only an offer of the caller and a 180 of the DUT enter a call. It
writes three files:

- `timing-180.txt`, one line per call;
- `timing-180-fields.txt`, raw tshark fields;
- `timing-180-result.txt`, its verdict.

**Call count.** The reader takes the number of calls that a run placed.
Reason: a capture that lost packets holds fewer calls, and its readings would
still pass on calls that it kept. Each outcome has a verdict line of its
own:

- a short capture;
- a call with no 180 at all;
- a call past its bound;
- a capture with no call.

If a run has no capture, the flow reports `PARTIAL`, as other timing checks
do.

**Order of readings.** The flow reads its hook record and its 486 count before
the SIPp status. SIPp exits on the first call that broke, so its status alone
names no cause. A failure line therefore carries two counts: odd calls that
the hook answered, and offers that the daemon refused 486.

**Refresh.** `register_prime()` grants the first binding for ten seconds
(`-key expires 10`) before the caller starts. The out-of-call file is
`register_ooc.xml`, and the caller passes `-key expires 10` too, so each later
refresh also gets ten seconds.

**Refresh interval.** `sip_reg_refresh_secs()` (`src/proto/sip/reg.c`) takes the
lesser of half the expiry and thirty seconds before the expiry, with a
fifteen-second floor (`SIP_REG_REFRESH_MIN`). If that floor is not below the
expiry, it takes half instead, with a two-second floor
(`SIP_REG_REFRESH_FLOOR`). Half is therefore the answer for an expiry from
four to fifteen seconds, and two seconds below four. This line refreshes
every five seconds.

**Dead-call wait.** A refresh carries the Call-ID of the binding that it
renews (RFC 3261 section 10.2). SIPp remembers a finished call for 33
seconds, and discards every message that names one. This run therefore passes
`-deadcall_wait 0`, so each refresh opens an out-of-call instance of its own
and gets an answer. Those instances leave `-m 50` untouched. Reason: SIPp
counts an out-of-call call against its out-of-call scenario, and its limit
against its main one (SIPp 3.7.3 `socket.cpp` and
`call_generation_task.cpp`). SIPp 3.7.3 also routes a second and every later
INVITE of a main scenario while an out-of-call file is loaded.

**Hook driver.** `inbound_churn_hook_drive()` is the subscriber. It runs
beside SIPp and counts rings in the daemon log. For every odd ring, it seizes
the line, holds it for a talk time of 600 ms and puts it back. Its record is
`hook-driver.txt`.

**Release ends the call.** The daemon sends its BYE behind each release, and
the scenario answers that BYE. Only then does a call end at SIPp, so the line
is on hook before a next offer can exist.

**Other order.** In the other order, a peer sends its BYE before the release.
That order races the next offer, and a line that is still off hook answers an
offer 486.

**Rings, not calls.** The driver counts rings, not calls on the wire. The
daemon starts one ring per offered call, whether that loop was quick or not. A
late read therefore costs one seize, and never moves a count of later calls.
A count that jumps by more than one is a ring that the loop missed whole. The
driver stops there and names that ring.

**Pace.** Three daemon limits bind a run's pace. `test/sipp/README.md`,
section "The inbound churn scenario", states them under "The pace". None of
them touches the 180 under test, which the daemon sends when an offer
arrives. Two settings of this flow serve those limits.

**Timer T1.** The flow config writes `sip_timer_t1` 100. Reason: an answered
call keeps its 2xx record until 64\*T1, because a retransmitted ACK must not
reach the call layer twice (RFC 6026, `src/proto/sip/ua.c` `accept_ack`). A line
holds eight records (`src/proto/sip/ua.h` `UA_ACCEPT_MAX`). At a T1 of 500
ms, its shipped default, the ninth answered call of a run finds no record. The
daemon then logs "no owner for the 2xx retransmission", and ends that call with
a BYE of its own. T1 100 holds a record for 6.4 seconds; eight records then
suffice when answered calls stand over 0.8 seconds apart, an eighth of that
lifetime.

**Timer J.** This T1 does not shorten timer J, because `sip_timer_j` has its
own default of 32000 ms (`src/cfg/schema.h`). Every call therefore holds a
server transaction for 32 seconds (RFC 3261 section 17.2.2), and the daemon
holds 64 (`src/proto/sip/txn.h` `SIP_TXN_MAX`). Scenario pauses keep each
run inside that table. Calls closer together fill it, and the daemon then takes
an offer with no answer at all, which is the field symptom itself.

## RFC 2543 hold flows

**Switch.** Three flows drive the behavior of a field switch at the real
daemon:

- `legacy-hold`;
- `glare-mutual`;
- `conf-legacy-hold`.

In the field capture, the switch answers a hold with a stale hold SDP
(`c=0.0.0.0`). It then holds the leg itself with a re-INVITE of the same
shape. It answers the resume with the same stale hold SDP, and then puts the
leg back on voice with an INVITE that carries no SDP. `test/sipp/README.md`,
section "The RFC 2543 hold scenarios", carries its capture facts and all
three scenario files. These flows add leg state behind that signaling.

**No timing margin.** In `legacy-hold`, every subscriber gesture waits for a
state that the exchange in front of it produced. No step of that flow
therefore depends on a timing margin. `conf-legacy-hold` has one step that
does, and paragraph "One margin" below names it.

| Step of `legacy-hold` | What the runner waits for |
|---|---|
| hold flash | `CONNECTED` |
| hold exchange | `HOLD_DIAL`, with the call `inactive` on `G729` |
| re-INVITE of the peer | that peer's ACK in the SIPp message log, then the leg back at `active` |
| resume flash | nothing; both readings above are its cause |
| resume and offerless exchange | call `sendrecv` on `PCMU`, and stream programmed `sendrecv` |
| hook release | scenario answers the DUT's BYE |

**Two readings for the peer exchange.** That exchange needs two readings, not
one. The switch's hold answer and its hold re-INVITE leave one reading on the
leg: `inactive` on G.729. `voice status` alone cannot tell them apart.

**ACK and leg state.** An ACK in the message log names the second exchange,
and `sipp_msg_wait()` polls its count. After that ACK, `active` on the leg
says that it is free again. Reason: a leg inside an exchange takes no
operation of ours (`src/proto/sip/call.c`, `leg_free_is()`).

**Codec of the hold.** `voice status` exposes the hold codec:
`.call[0].calls[0].codec` reads `G729` from that hold answer onward. Reason:
the switch answers `18 0 8`, and `codec_select()` walks that order. The codec
reads `PCMU` again once an ACK of the offerless exchange arrives.

**Conference join.** `conf-legacy-hold` reaches `CONF` with its resumed leg
`recvonly`. The stale hold answer causes that direction. For that reason,
`conf_join_take()` takes both direction expressions as arguments. Its default
pair is the sendrecv pair that an ordinary resume leaves, and this flow names
a recvonly pair instead. It then reads a sendrecv pair after the offerless
exchange, which is its assertion.

**One margin.** The recvonly reading is the one margin of these three flows.
It stands between an ACK of the stale hold answer and the offerless INVITE
behind it. It costs a 100 ms poll and three ubus round trips. Only a scenario
pause holds that window open.

**Pause.** A causal order is not available: SIPp waits for no runner, and no
runner can make a scenario send its INVITE. That pause is therefore 1000 ms
(`test/sipp/uac_legacy_hold_conf.xml`). A comment beside it gives its
headroom.

**491 reader.** The 491 timing reader counts only INVITEs from the DUT:
`tshark` takes `udp.srcport == <daemon port>` on its INVITE term. Reason: in
`glare-mutual`, a peer INVITE goes on the wire at 1.9 s, in front of the retry
under test. Without that term, the reader measures 1.9 s against the
2.1-second floor (RFC 3261 section 14.1). That flow then fails for a reason
that is not the daemon's.

**Other terms.** A 491 term needs no source term of its own: no flow of this
harness sees a 491 from the DUT. `uas_491_glare` reads that same function, and
its reading does not change, because its peer sends no INVITE at all. The
bench runner applies this rule with a name that it can use, the DUT address
(`test/sipp/run_sipp.sh`).

**SIPp routing.** SIPp routes an in-dialog INVITE of a main scenario while an
out-of-call scenario is loaded. `conf-legacy-hold` needs that routing. It
runs `xfer_peer_ooc` for a second call that the DUT places, and still sends
its offerless INVITE inside its own dialog. A pass of that flow is the
evidence; no SIPp source is cited for it. Section "Inbound churn flow",
label "Dead-call wait", gives related SIPp facts.

## Challenged on-hook conference transfer

`conf-onhook-xfer-auth` drives the gesture of `conf-onhook-xfer` against a
peer that challenges DUT requests. In a field capture, the switch challenges
a BYE with 401. That capture does not show whether the
switch also challenges a REFER, so this flow's peer challenges both. One
runner, `conf_onhook_xfer_run()`, serves both flows. They differ in two
things:

- scenario (`test/sipp/uac_conf_onhook_xfer_auth.xml`);
- number of REFER and BYE requests that the referee dialog must carry.

| Reading | Requirement |
|---|---|
| dialogs that got a REFER | 1, the referee |
| REFER requests of that dialog | 2, counted by their CSeq numbers |
| every REFER copy on the wire | escaped Replaces of the other dialog, and the DUT's Contact |
| last REFER copy | an `Authorization` naming `uvoice-refer-nonce` |
| dialogs that got a BYE | 1, the referee |
| BYE requests of that dialog | 2, counted by their CSeq numbers |
| last BYE copy | an `Authorization` naming `uvoice-bye-nonce` |
| line and records | IDLE with two closed conference records |

**Counts.** The flow counts requests by their CSeq numbers. The DUT
retransmits a request over UDP until its peer answers it. A count of messages
would therefore read a retransmission as a second request. A credential retry
takes the next CSeq number (RFC 3261 section 22.2), so each number is one
request.

**Credentials.** The flow reads credentials on the last copy of each request,
for that same reason. Every copy of a first request stands in front of its
retry. The last copy is therefore the retry, however many copies the wire
carried. That reading takes copies in log order, and a loopback run delivers
them in that order.

**Headers.** The flow reads escaped Replaces and Contact on every REFER copy,
not on the first alone. A peer acts on a retry, not on a request that it
challenged. A retry that dropped either header would therefore pass a reading
of the first copy. On DC-SIP, a REFER with no Contact draws a 400.

**Nonces.** The two challenges carry different nonces.
`sip_auth_challenge_take()` (`src/proto/sip/auth.c`) refuses a challenge that
repeats nonce and realm of the challenge that it answered. Reason: the peer
refused credentials built from them, so a second build can only repeat that
refusal. A BYE challenge that repeated the REFER nonce would get no retry:

- `www challenge refused (repeat)` in the daemon log;
- no second BYE;
- a failure that reads "the referee dialog carried 1 BYE request(s), expected
  2".

**Password.** Its channel block carries no `sip_secret`, and the flow needs
none. `acct_cfg_derive()` (`src/proto/sip/account.c`) copies a channel secret
into `auth_pass`, and `cred_write()` (`src/proto/sip/call.c`) hashes the value
that it holds. An absent secret is therefore an empty password, not a refusal to
answer. No scenario check reads a digest response, so this run needs no
deterministic digest.

**Third request.** Two readings refuse a third REFER or a third BYE:

- Its scenario has no receive left for either request. SIPp aborts a call on
  a request that it does not expect (SIPp 3.7.3 `src/call.cpp`,
  `process_unexpected` and the `E_AM_UNEXP_BYE` branch of
  `automaticResponseMode`). `sipp_abort_classify()` reports that abort as an
  unattributed abort.
- Both table counts answer that question with the number that they found.

## Challenged fax upspeed

`uas_fax_upspeed_auth` drives the signaling of a field capture at the real
daemon. In that capture, the switch answers the first `0 8` re-INVITE with
401. The unit repeats its offer with credentials 62 ms later. One runner,
`fax_upspeed_run()`, serves this flow and `uas_fax_upspeed`. They differ in
three things:

- scenario (`test/sipp/uas_fax_upspeed_auth.xml`);
- channel credential;
- two readings that the table below adds.

| Reading | Requirement |
|---|---|
| stream, profile and kind | what `uas_fax_upspeed` requires, unchanged |
| INVITE requests, counted by their CSeq numbers | 3 |
| 401 to credential retry | 1.0 second at most, read off a capture |

**Count.** This count keys on CSeq number alone. It needs no dialog term: the
flow places one call, so every INVITE that its peer receives belongs to one
dialog. A count of messages would read a retransmission as one more request,
as section "Challenged on-hook conference transfer" states. A credential
retry takes the next number (RFC 3261 section 22.2). Its three requests are
offer, challenged upspeed and retry.

**What the count finds.** This count finds a retry that kept the CSeq number
of the offer that it repeats. Its scenario accepts such a message and passes.
That count then reads 2, and the flow fails with that number. SIPp reports a
fourth INVITE instead:

- its scenario has no receive left for it;
- SIPp aborts that call on a request that it does not expect;
- `sipp_abort_classify()` names that abort.

The flow then prints its SIPp verdict and returns before it reaches the count.

**Password.** Its channel block carries a real `option sip_secret`
(`uvoice-fax-secret`). Its digest on the wire is then a digest that a provider
checks. A channel with no secret also answers a challenge, over an empty
password (section "Challenged on-hook conference transfer", label
"Password"). That digest covers a request URI that holds the SIPp port of a
run, so its value changes from run to run. No scenario check reads that
response, so this run needs no fixed digest.

**Timing check.** `timing_fax_auth_check()` reads its delta as
`timing_180_check()` reads a 180. It keys each pair by CSeq number. A 401
answers the challenged re-INVITE, and its retry takes the next number. A
retransmission of the challenged offer therefore cannot pass for that retry.
Each outcome has a verdict line of its own in `timing-fax-auth.txt`:

- no 401 at all;
- no retry behind the 401;
- a retry past its bound, with delta and bound.

A run with no capture reports `PARTIAL`, as `uas_491_glare` does.

**Bound.** That bound is 1.0 second, and the field capture shows 62 ms. It
is generous on purpose. It is still the only reading of this flow that finds a
slow retry. Both other waits are much wider:

- its scenario gives a retry receive 10 seconds;
- a `stub_wait` on the upspeed gives its stream 15 seconds.

A retry that waited on a timer for a second satisfies both waits, and only
this reading reports it.

**Default set.** This flow runs by name and is not in the default set, unlike
other fax flows of the runner. Ask for it by name:

```sh
uvoice/test/e2e/run_e2e.sh uas_fax_upspeed_auth
```

## Service route flows

`service-route-absent` and `service-route-present` drive RFC 3608 at the real
daemon. Each flow does three things:

1. It registers the line against a SIPp registrar.
2. It places one outbound call from the line.
3. It reads the requests off the SIPp message log.

They differ in one thing: whether the 200 to the REGISTER carries a
`Service-Route` header (`test/sipp/uas_service_route.xml`,
`test/sipp/uas_service_route_none.xml`).

**Outbound call.** The DUT places the call, so each flow follows `flow_a_run`
and not `codec_inbound_run`. The registrar must answer the registration before
the call. An inbound flow registers against `uas_register_prime`, which grants
no route. Each flow waits for its registration and does not assume it. The
200 stores the route (`src/proto/sip/reg.c` `resp_ok`). An INVITE in front of
that answer would read an empty block, and the flow would check nothing.

**Messages.** The registrar of `service-route-present` challenges the INVITE
with 407, which is the field exchange. Six requests then carry a
verdict, and this flow reads every one of them:

| Message | Requirement | Why |
|---|---|---|
| REGISTER | no `Route` header | a REGISTER never carries the route it asks for (RFC 3608 section 5.1) |
| challenged INVITE | `<sip:sbc1.example.net:5065;lr>` then `<sip:sbc2.example.net:5065;lr>`, one header line each | granted order, unfolded (RFC 3261 7.3.1) |
| credential retry INVITE | same two hops | a leg writes its route into every build of a request (`src/proto/sip/call.c` `invite_build`) |
| ACK of the 407 | same two hops | that ACK mirrors the request it answers (`req_mirror_build`) |
| ACK of the 2xx | no `Route` header | an in-dialog request takes Record-Route from its answer alone, and this peer records none |
| BYE | no `Route` header | same rule as for the ACK of the 2xx |

`service-route-absent` reads the same requests, with one INVITE and one ACK.
It requires no `Route` header on any of them.

**Hop names.** The hops name a host that the run never reaches, as in the
field. There, the Route named the SBC of SRV priority 0, while the
INVITE went to the SBC of priority 1. uvoice resolves no hop name, and a hop
name does not change the next hop (`src/proto/sip/reg.h`). The INVITE
therefore still arrives at SIPp, and the flow can read it. A daemon that took a
hop as its destination would lose that INVITE, and the flow would fail on a
message that never came.

**Readers.** `route_hdr_check()` reads the hops with `sipp_msg_header_get()`,
the way `stream_via_check()` reads the Via token. `xfer_hdr_absent_check()`
checks absent headers. Both readers take an occurrence number for each
message, because the challenged call holds two INVITEs and two ACKs.
`sipp_msg_count()` counts copies of one start line, so the flow reads every
INVITE and not just the first. Otherwise, a retransmission that lost a header
would hide behind an earlier copy.

**ACKs.** The flow tells its two ACKs apart by CSeq number, not by log
position. An ACK of a non-2xx takes the number of the INVITE that it answers
(RFC 3261 section 17.1.1.3), so the two numbers differ.

**Refresh.** Neither flow sees a second REGISTER. The config asks for 3600
seconds (`config_write`), so the refresh comes half an hour after the
registration, long after the flow ends. A unit case covers that refresh
instead (`test_sip_reg.c`, "service route: the refresh REGISTER carries no
Route header").

## WAN flap flow and its netifd stub

`wan-flap-outage` drives a field sequence at the real daemon. In the field,
a WAN address change during a registrar outage left the registration lost
until a reboot.

The flow runs two daemons under one name. Each daemon has its own ubusd, stub,
config and artifact directory. The flow reads its two parts from two places:

- first part: the registrar's message log;
- second part: the daemon log.

### Stub

`netifd_stub.uc` publishes the two objects that netifd publishes:

- `network.interface.<name>` with a `status` method;
- `network.interface` with a `dump` method.

The stub answers both methods from a state file, and the flow rewrites that
file between its steps. Each answer carries the fields that the daemon's
interface tracker reads (`src/proto/sip/account.c`, `netif_pol` and
`netif_a_pol`), in the shapes that netifd sends:

| Field | Shape |
|---|---|
| `up` | boolean |
| `interface`, `l3_device` | string |
| `ipv4-address`, `ipv6-address` | array of tables with an `address` and a `mask` |

The dump answer is one `interface` array of those tables.

**Separate process.** The stub must be a process of its own. The status query
of the daemon is a synchronous `ubus_invoke()` (`src/proto/sip/account.c`,
`netifd_query`), and the runner cannot answer a call that it waits for.

**Refusal.** The state file also carries a refusal. If `answer` is `error`,
the method returns a ubus status and no reply. That is the failed invoke on
which the daemon arms its recovery timer. The unit suite has no ubus
connection at all, so this flow is the only test of that path.

**Rename.** The runner replaces the state file with a rename, never with a
truncating redirection. The stub reads that file while the daemon waits on its
call. An `ifdown` also drives a dump query of its own (`netifd_event_cb`). A
redirection leaves a window in which the stub reads half a file. A query in
that window can fail and arm the recovery timer under test.

### Addresses

In the first part, the stub reports `127.0.0.2` and then `127.0.0.3`. In the
second part, it reports `127.0.0.4` and then `127.0.0.5` (`wan_flap_addr_one`,
`wan_flap_addr_two`, `wan_flap_addr_start` and `wan_flap_addr_back` in
`run_e2e.sh`). The flow adds none of them to the loopback, for three reasons:

- all of `127.0.0.0/8` is local to `lo` by construction;
- sockets stay bound to the wildcard address with `SO_BINDTODEVICE`
  (`src/proto/sip/transport.h`);
- `Via` and `Contact` of an IPv4 line carry the address that netifd
  reports, whatever the socket's bind (`src/proto/sip/account.c`,
  `account_local_host`).

`loopback_prepare` therefore needs no second address. A later flow can still
add one: both `ip addr add 127.0.0.2/32 dev lo` and
`ip addr add 192.0.2.10/32 dev lo` work inside the namespace.

The first part also uses `127.0.0.1`: the route probe answers with it before
the first interface status arrives. Every other flow of the harness runs in
that state.

### What the first part asserts

The registrar is `uas_register_outage.xml`. It grants ten seconds per binding,
and it takes one refresh without an answer. Every REGISTER of that line
carries one Call-ID. SIPp routes that sequence into one scenario instance, so
all five exchanges are one call.

| Exchange | What the flow requires |
|---|---|
| 1 | `127.0.0.1` in the Via and one Contact on it. That line registered before any interface status arrived. The daemon log holds no `voice recovery armed` line, because an absent object arms nothing (`netifd_retry_needed`). |
| 2 | The stub is up with `127.0.0.2`, and an `ifup` carried that address in. Via and Contact name it, and `127.0.0.1` stands beside it with `expires=0`. |
| 3 | A refresh on `127.0.0.2`, with one Contact and no removal. The registrar answers nothing. The flow waits for two retransmissions of it, so that outage is a measured window and not a sleep. |
| 4 | REGISTER that the address change produces. `127.0.0.3` stands in Via and Contact, and `127.0.0.2` beside it with `expires=0`. Its CSeq is above that of exchange 3. |
| 5 | Refresh behind it: `127.0.0.3` alone, with no `expires=0` anywhere. |

**Call-ID.** The flow reads the Call-ID off the wire once per `ifup`, before
it reads any line state. A line that rebuilt its registration reaches the
same state under a new Call-ID. Only the registrar sees which Call-ID
arrived.

**One REGISTER.** The flow reads "one REGISTER" from the stamps of the
message log. Exchange 5 stands one refresh interval behind exchange 4, and
exchange 3 stands one interval behind exchange 2. A grant of ten seconds gives
a refresh after five seconds (`sip_reg_refresh_secs`), and the flow accepts
4 s to 9 s (`wan_flap_refresh_min_ms`, `wan_flap_refresh_max_ms`). A second
REGISTER from the daemon would arrive inside that interval and shorten it.

**Raw log.** Two readings also run over the raw log:

- No copy whose Via names `127.0.0.2` appears behind the first copy that names
  `127.0.0.3`. The daemon aborts the exchange that the address change drops,
  together with its transaction (`src/proto/sip/reg.c`, `attempt_prepare`
  through `txn_drop`).
- The daemon log holds no `voice recovery armed` line at all.

**Keys.** These readers key on the CSeq number and never on an occurrence
count. A retransmission repeats its whole text (RFC 3261 17.1.2.2), and SIPp
logs every copy that it reads. The number of messages in front of a message
under test comes from the outage, not from the daemon.

### What the second part asserts

The second daemon drives the recovery timer. It waits 5000 ms before its first
attempt, and doubles its delay up to 60 s. These delays form the ladder
(`NET_IFACE_RECOVER_MS` and `NET_IFACE_RECOVER_MAX_MS` in `src/net/iface.h`). The
daemon logs one `voice recovery armed` line per ladder step. This part places no
call and runs no SIPp. It reads two faults in four steps:

1. The state file refuses one status query, and an `update` event carries
   that fault into the daemon. The stub log must hold one refusal, and the
   daemon log one arm line that reads `5000 ms`.
2. The state file answers again, with a new address and no event behind it.
   An attempt repeats the failed query (`acct_recover_cb`), so that address
   reaches the line on the timer alone. That ladder then ends: no further arm
   line appears in the daemon log for a whole period.
3. The state file refuses a status query again, and an `update` event carries
   that fault into the daemon. A second ladder also starts at `5000 ms`,
   because the disarm reset its count (`acct_recover_disarm`).
4. The stub leaves the bus while an attempt is pending. An event then arrives
   with an address that the daemon already knows. That attempt and that event
   must add no step. Reason: a lookup that finds nothing is not a fault of the
   path (`acct_recover_lookup_failed`).

**Counts, not searches.** Every reading of this part is a count, never a plain
search. The flow reads the daemon log whole, so the first fault's step
would answer a search for the second fault. It also counts stub refusals
beside those steps. An event that the daemon did not handle yet then cannot
pass for a fault that it answered.

## MGCP flows and call agent

The ten `mgcp-*` flows drive the MGCP protocol of the daemon. SIPp speaks
no MGCP, so `mgcp_ca.py`, a call agent in python3, is their peer. It needs
the standard library alone. Each flow is a `mgcp_*` function of the agent,
and `mgcp_flow_run()` runs it in six steps:

1. `mgcp_config_write()` writes `protocol` `mgcp`, two lines and the `mgcp`
   section: `call_agent` 127.0.0.1, `restart_delay_max_s` 0, and `t_max_s`
   and `t_hist_s` 10.
2. `capture_start` captures the gateway port.
3. `mgcp_agent_start()` starts the agent and waits until its port is bound.
   The restart RSIP of the daemon then has an answer.
4. `daemon_spawn` starts the daemon with the trace `mgcp,hook,hal`.
   `mgcp_ready_wait()` first reads `signaling.available` until the daemon
   answers (`mgcp_image_probe()`), so a daemon without MGCP gives SKIP before
   any verdict of the agent counts. It then waits for `ready.json` or
   `verdict.txt` of the agent. The agent writes `ready.json` when
   `hal.state` is `ready` and both endpoints read `in_service`. Reason: a flow
   can take its endpoint out of service at once, and a poll of the runner
   could miss that window.
5. `mgcp_verdict_wait()` waits for the verdict line of the agent in
   `verdict.txt`: `PASS` or `FAIL`, and the detail.
6. `flow_stop` stops the daemon first and the agent behind it, so the RSIP of
   the daemon stop still gets its answer.

**Agent.** The agent binds UDP on 127.0.0.1 at its port. Its wire loop splits
a datagram into messages at a line with one "." (RFC 3435 3.5.5). It answers
a command of the gateway once, and a repeated command with the same source and
transaction id gets the same bytes (3.5.2). A command that the flow leaves
unanswered stays off that record, so the flow decides again on each copy.
`wait_for()` finds a received message by a predicate.

**Retransmission and stop.** Each transaction of a datagram keeps its own
state (RFC 3435 3.5.6):

- while one with no provisional response is missing, the agent resends the
  datagram on a doubling timer from the flow's RTO, at most Max1 times;
- once each missing one has a provisional response, it sends the datagram
  again every LONGTRAN, 5 s;
- a provisional response moves the deadline to T-HIST behind it, 10 s
  (`mgcp_t_hist_s`).

No flow drives a deferred final, so no flow runs the LONGTRAN path (note of
`mgcp-response-cache`). A SIGTERM ends each wait at once, and the agent
writes `ca.json` before it exits.

**Request ids.** The agent keeps the request id that each endpoint accepted
last, from a success response to a command with `X:`. Each NTFY must carry
the id in force when it came, or 0 before the first request (RFC 3435 2.3.4).
The agent checks each NTFY up to the one that a wait finds, and every NTFY
of the flow before its verdict.

**Stub and status.** The agent calls `ubus` on the private socket of each
flow. It drives `voice.stub` (`hook`, `digit`, `flash` and `tone`) and polls
`voice status` and the stub dump. A stub carries no RTP socket. Media on a
port therefore means a running stream whose `remote_port` is that port of the
agent's description, as in `flow-a`. Ring cadences come from the stub trace
"stub: line 0 ring start cadences=N" in `daemon.log`, because a stub dump
holds no ring template.

**Artifacts.** Each flow directory holds these files:

- `ca.log`: every datagram, with its `time.monotonic()` stamp, its direction
  and its peer;
- `ca.json`: the same datagrams as a list of `stamp`, `dir` and `text`;
- `flow.log`: the output of the agent, with its verdict line;
- `ready.json`, `verdict.txt`, `status-*.json` and `stub-*.json`;
- `disc-spacing.txt` and `disc-wire.txt` of `mgcp-disconnected`.

**Ports.** The pid seed gives each run a block of 20 ports:

| Offset | Port |
|---|---|
| 0 | SIPp signaling (`sipp_port`) |
| 1 | daemon SIP (`daemon_port`) |
| 2, 4 | SIPp audio and video for `rtp_echo` (`media_port`) |
| 3, 5 | RTCP of SIPp audio and video; 3 is also `http_port` |
| 6 | MGCP call agent (`E2E_CA_PORT`) |
| 7 | MGCP gateway (`E2E_GW_PORT`) |
| 8 | media offer of the agent (`E2E_CA_MEDIA_PORT`); SDP only, not bound |
| 10 | moved media offer of the MDCX in `mgcp-outbound`; SDP only, not bound |
| 12 | offer of the second call in `mgcp-call-waiting`; SDP only, not bound |
| 14 | T.38 offer in `mgcp-fax-tone`; SDP only, not bound |

SIPp binds the audio port and the video port 2 above it
(`bind_rtp_sockets()`), so the MGCP ports start behind that pair. An MGCP
socket is IPv4 alone, so no MGCP flow is in `family_v6_set`.

**Image without MGCP.** `daemon_build` builds with `-DUVOICE_PROTO_SIP=ON`
and `-DUVOICE_PROTO_MGCP=ON`. A daemon built without the MGCP option has no
`mgcp` in `signaling.available`. The first MGCP flow then prints `SKIP` with
that reason, and the later ones skip with no daemon start.

## Timing results

The 491 and 503 checks follow the packet filters of `test/sipp/run_sipp.sh`.
The runner marks either result `PARTIAL` when packet capture is unavailable.
The SIP message sequence must still pass before it reports `PARTIAL`.

Both 491 filters select an INVITE that the DUT sent, not the first INVITE of
either direction. They name the DUT differently:

- `run_e2e.sh` names the daemon port, because SIPp and daemon share one
  loopback address.
- `run_sipp.sh` names the DUT address, because the bench runner knows no DUT
  port.

Section "RFC 2543 hold flows" states why these filters need a source term.

The 180 check of `inbound-churn` follows the same rule. It answers for every
call of a run, not for one message pair. Section "Inbound churn flow"
names its files.

The fax challenge check of `uas_fax_upspeed_auth` also names the daemon port
on its INVITE term. Its 401 term names the peer port and the CSeq method.
Reason: that reader anchors on the peer's first 401, and a 401 to another
request would take its place. Section "Challenged fax upspeed"
states what that check reads.

## Call churn

`run_churn.sh` places sequential outbound calls through one daemon and one
SIPp instance. It answers whether anything leaks across calls. Its scenario is
`test/sipp/uas_churn.xml`. The first argument is the call count, and the
default is 1000. The soak run is the same script with 10000 calls.

```sh
CHURN_ARTIFACTS=/tmp/uvoice-churn \
SIPP_BIN=/path/to/sipp \
UVOICED=/path/to/uvoiced \
uvoice/test/e2e/run_churn.sh 1000
```

**One call.** One call has these steps:

1. `ubus call voice dial`;
2. a bounded wait for `CONNECTED`;
3. a talk pause;
4. `ubus call voice release`;
5. a bounded wait for `IDLE`.

**Final check.** The final check reads the `voice stats` counters and the
`voice cdr` record ring. At the run's end, these conditions must hold:

- `out_attempted` and `out_answered` both equal the call count;
- `voice stats` counts no failure;
- newest record belongs to the last call, and it is answered, closed and
  with its media present;
- `voice status` reads `lines[0].status` `registered`.

**Checkpoints.** Every hundredth call, the runner checks the invariants and
samples the resources:

| Check | Requirement |
|---|---|
| `voice status` events | `dropped` and `lost` stay at 0 |
| `voice status` hal | `op_errors` stays at its value from before the first call |
| `voice status` call slots | both slots drain within 8 seconds |
| `voice cdr` | newest record is the baseline sequence plus calls placed |
| `/proc/<pid>/status` | `VmRSS` stays within 5% of the run's 10% mark |
| `/proc/<pid>/fd` | descriptor count does not move after the run's 10% mark |

The slot check reads line 0 alone. Line 0 is the only line that the churn
config enables, and line 1 stays out of service for the whole run.

**Resource bound.** The 5% bound allows about 140 kB of growth against the
2.8 MB baseline of the stub build. That is 140 bytes per call over a thousand
calls, and 14 bytes per call over ten thousand calls. Below that size, the
sanitizer build is the leak check.

**Drain.** A call slot outlives its call. Its leg stays in `bye-sent` while
its BYE transaction waits for timer K, which is five seconds on UDP. The next
call starts at once only because the daemon hands a draining slot over. The
slot check therefore waits for the drain before it asserts. The drain costs
about five seconds per checkpoint.

**Artifacts.** The runner writes these files:

- `resources.csv`;
- final `voice stats` and `voice cdr` snapshots;
- SIPp logs;
- daemon log;
- a one-line verdict in `verdict.txt`.

The runner starts SIPp without `-trace_msg` and takes no packet capture.
Reason: a message log of ten thousand calls is gigabytes in size.

| Variable | Default | Purpose |
|---|---:|---|
| `CHURN_FAMILY` | `4` | Address family, 4 or 6. |
| `CHURN_ARTIFACTS` | `./churn-artifacts` | Artifact directory. |
| `CHURN_SCENARIO` | `test/sipp/uas_churn.xml` | UAS scenario file. |
| `CHURN_NUMBER` | `123#` | Number the dial verb injects. |
| `CHURN_TALK_MS` | `100` | Talk pause, which must clear the summed scenario pauses by 50 ms. |
| `CHURN_CHECK_EVERY` | `100` | Calls between invariant checkpoints, lowered to a tenth of the run when the run is shorter. |
| `CHURN_POLL_MS` | `20` | Poll interval of the state waits. |
| `CHURN_CONNECT_TIMEOUT_MS` | `15000` | Wait for `CONNECTED`. |
| `CHURN_IDLE_TIMEOUT_MS` | `5000` | Wait for `IDLE`. |
| `CHURN_DRAIN_TIMEOUT_MS` | `8000` | Wait for both call slots at a checkpoint. |
| `CHURN_RECORD_TIMEOUT_MS` | `10000` | Wait for the last record to close with its media. |
| `CHURN_RSS_GROWTH_PCT` | `5` | `VmRSS` growth allowed above the 10% mark. |
| `CHURN_SIPP_HEADROOM` | `128` | Scenario instances above the call count that `-m` allows. |
| `CHURN_TRACE` | none | Daemon trace domains, off by default because the log grows with every call. |
| `CHURN_BUILD_JOBS` | `4` | cmake job count when the runner builds the daemon itself. |
| `CHURN_SIPP_PORT` | pid-derived seed | SIPp signaling port. |
| `CHURN_DAEMON_PORT` | pid-derived seed plus one | Daemon SIP port. |
| `CHURN_MEDIA_PORT` | pid-derived seed plus two | SIPp media port. |

**Limits.** The call count must be at least 10, because the resource baseline
needs a sample between the 10% mark and the run's end. The runner
refuses a `CHURN_SCENARIO` with a pause that has no literal `milliseconds`
attribute. Reason: it cannot budget the talk pause against a distribution or
a variable.

**Timing figures.** The verdict reports two timing figures: one per call over
the whole run, and one per call between checkpoints. Reason: drains take most
of a short run's time.

**Registration.** The runner waits for `lines[0].status` `registered` before
the first call. `uas_churn.xml` answers each REGISTER and every refresh in its
REGISTER branch, so a soak keeps its binding. The final check requires
`registered` again.

**SIPp budget.** SIPp counts every call that it creates against `-m`, and each
daemon REGISTER opens one call. The budget is therefore the call count plus
the headroom. SIPp counts an answered REGISTER as a successful call. The
runner therefore requires one successful call more than the call count, for
the first REGISTER. It classifies REGISTER aborts as `run_e2e.sh` does. Any
other abort fails the run.

**Sanitizer build.** A sanitizer build is the run's leak check. It exits
non-zero when LeakSanitizer finds a leak, and the runner fails on that exit
status. Its allocator fills its own regions over the first hundred calls and
then settles. A short sanitizer run therefore states its own
`CHURN_RSS_GROWTH_PCT`. The 5% default describes the shipped build.

**Shared helpers.** `run_churn.sh` and `run_e2e.sh` both source `lib.sh`. It
holds the helpers that behave identically in both runners:

- `path_absolute()`, `process_exited()` and `daemon_build()`;
- `udp_bound_check()` and `udp6_bound_check()`;
- `sipp_success_count()`.

Each runner keeps its own version of a helper whose behavior differs, for
example `config_write()`, `sipp_start()`, `process_stop()` and
`sipp_abort_classify()`.

## Sources

- `test/sipp/README.md` and `test/sipp/run_sipp.sh` define the committed
  scenario contract.
- `src/proto/sip/call.c`, in `calls_status_dump()`, defines the call status
  output.
- `src/proto/sip/account.c`, in `acct_cfg_derive()` and `acct_targets_load()`,
  defines the actual target behavior.
- `src/hal/stub/stub.c`, in `stub_ubus_dump()`, defines the stub assertions.
- `src/proto/sip/stream.h` and `src/proto/sip/tls.h` define the
  connection and record layer contracts that the stream flows read back.
- `src/proto/sip/account.c`, in `acct_conn_status_dump()`, defines the
  `transport`, `listener` and `connection` keys.
- `src/net/iface.c`, in `netifd_status_cb()`, `netif_addr4_take()` and
  `net_iface_recover_settle()`, defines the interface tracker and the
  recovery ladder that `wan-flap-outage` drives through
  `test/e2e/netifd_stub.uc`.
- `test/unit/tls/README.md` describes the fixtures from which the harness
  composes the TLS bundle.
- `src/proto/mgcp/ep.h` and `src/proto/mgcp/proto.c` define the endpoint
  behavior that the MGCP flows assert, and `schema/uvoice-api-schema.json`
  (`MgcpTable`, `MgcpLineEntry`) defines the status keys that they read.
