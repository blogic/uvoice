uvoice call trace
=================

Files
-----
README.txt describes the archive and the packet formats.
SUMMARY.txt is the plain text report of this trace. Read it first.
tools/calltrace-summary.uc is the generator of SUMMARY.txt.
manifest.json is the trace journal. It names the line, stage, reason, times,
options, counts, versions, daemon PID, boot ID, and child processes.
It also records the board identity, binary MD5, collector results, and notes.
rings.pcapng holds the log, SIP, status, and available plaintext media rings,
and the canceller ring when options.lec is true.
wire.pcap holds the packets tcpdump captured. A trace without wire capture
has no wire.pcap.
wire.stats holds the capture command, tcpdump version, diagnostics, and
captured and dropped packet counts. A trace without wire capture has none.
snapshots.jsonl holds one snapshot JSON object per line.
config.json holds the configuration at the arm. The daemon omits credentials.
The archive contains no raw copy of /etc/config/voip.

system/ holds the system context at the finish:
  uptime             System uptime.
  loadavg            /proc/loadavg.
  meminfo            /proc/meminfo.
  daemon-status      /proc/<daemon PID>/status.
  threads            Task names, scheduler data, and allowed CPUs.
  ps                 Process list from ps w.
  ip-addr.json       Interface addresses from ip -j addr.
  ip-route.json      IPv4 routes from ip -j route.
  ip6-route.json     IPv6 routes from ip -6 -j route.
  conntrack-rtp      Up to 5000 UDP connection tracking records whose ports
                     match the traced line's effective RTP port range.
  nft-voip           nftables chains whose names contain voip or uvoice,
                     and every rule line that carries one of those words
                     (the fw4 rendering of the daemon's Allow_VoIP_* rules)
                     with its table and chain headers. If nothing matches,
                     up to 2000 lines of the ruleset. The manifest notes
                     this fallback.
  logread            System log for the trace, with a 30 second margin.
                     The upper time limit is exclusive. If the time options
                     fail, this file holds the last 20000 records instead.
  dmesg              Last 500 kernel log lines.
  ssl.keylog         Copy of /tmp/uvoice/ssl.keylog when present.
  platform           Board identity, firmware release, and boot ID.
  packages           Installed packages whose names contain uvoice or udsp.
  md5                MD5 text for /usr/sbin/uvoiced.
  summary            Diagnostics of the summary generator. The file is
                     present only when the generator wrote a diagnostic.

Each collector has a 15 second limit when timeout is available.
The manifest's collectors table reports ok, omitted, timeout, or
failed <exit code>. Workers run at nice level 19. The finisher copies each
worker's output to a separate file before it creates the archive.
An omitted collector writes an OMITTED line. A failed collector can leave
partial output. Collector failures do not discard the archive.
If jq is absent, manifest-finisher.json carries the finisher's results and
notes instead. The original manifest.json then keeps the daemon's last stage.
The summary collector runs after the finisher publishes the results of the
collectors above, so the report names them. Its own result is in the
collectors table and not in the report. It has 30 seconds, not 15, because
it reads both capture files whole. It also gets one second for each MiB of
the ring file and the wire capture together (rings.pcapng and wire.pcap). A
timeout or a failure publishes no SUMMARY.txt.

The summary report
------------------
SUMMARY.txt holds ten sections: TRACE, TIMES, TIMELINE, CALLS, SIP,
MEDIA ON THE WIRE, RINGS, DIAG, SYSTEM and HOW TO READ.
Every number in it comes from a file of this archive.
TIMELINE merges the log ring, the SIP ring and the snapshots into one list in
time order. It folds a run of media and jb sweep lines into one line, which
names the domains of the run. A snapshot marker can print after a run whose
span covers it, because the fold runs while the file is read.
The timeline stops after 5000 lines and says how many the trace holds.
Above 100000 media records the report prints the manifest counts instead of
the media flows, and says so.
Pass --full for every sweep line, every media flow and every timeline line.
A ring at its size cap is a host job: --full on one costs seconds and
hundreds of megabytes.
SIP holds the ladder of the SIP ring and, when wire.pcap is present and
tcpdump is installed, the ladder of the capture beside it.
An input the archive does not carry reads as one line of the report.

Build the report again on a host, from the unpacked archive:
  ucode tools/calltrace-summary.uc <dir>
  ucode tools/calltrace-summary.uc -- <dir> --full   every sweep line
  ucode tools/calltrace-summary.uc -- <dir> -        to standard output
The generator needs ucode with the fs and struct modules. It reads
rings.pcapng itself and calls /usr/bin/tcpdump for wire.pcap.

Schema and versions
-------------------
The manifest's versions.schema names the API schema version for this build.
The call trace surface was introduced in schema 0.20.0.
The canceller ring and the lec and lec_max_mb options arrived in schema 1.4.0.
In the uvoice source tree, uvoice-api-schema.json defines CalltraceStatus and
the CalltraceStateWord, CalltraceWaitingWord, and CalltraceReasonWord
vocabularies.
The manifest's versions.uvoice identifies the daemon release.
The manifest's md5 identifies the installed daemon binary.
system/packages records the installed uvoice and udsp package versions.
wire.stats records the tcpdump version when a wire capture ran.

Clocks
------
The capture timestamps use wall time (CLOCK_REALTIME). The daemon measures
window duration and media timing with CLOCK_MONOTONIC.
Every snapshot carries clock.realtime_us, clock.monotonic_ns, and
clock.step_detected. Use this pair to compare the two clock domains.
A change in their offset above 100 ms sets step_detected.
The backend converts media timestamps to wall time at each drain.
An NTP time step can change the order of wall timestamps.
The archive does not promise an exact order across rings.

Packet formats and Wireshark
----------------------------
wire.pcap contains the actual packets, including encrypted TLS and SRTP data.
The SIP ring contains plaintext messages with synthetic IP and UDP headers.
A SIP message from TCP or TLS uses UDP framing with its real endpoint ports.
For a non-standard port, select Decode As SIP in Wireshark.
The synthetic header does not describe the original IP transport.

The synthetic IP header's DSCP byte encodes these values:
  Bits 7..6: direction, 0 transmit, 1 receive.
  Bits 5..2: kind, 0 SIP over UDP, 1 SIP over TCP, 2 SIP over TLS,
             3 RTP, 4 RTCP, 5 UDPTL.

The log and status rings use DLT 147 (DLT_USER0).
In Wireshark's DLT_USER preferences, map DLT 147 to the syslog protocol.
The status payload is JSON; the log payload is a trace line.

The SIP ring records send attempts. It can hold a message that wire.pcap
does not show because the socket send failed.
A UDP CRLF keepalive is a four-byte entry. Stream PING and PONG frames are
not recorded. If a socket reports an unspecified local address, the trace
uses the voice interface address instead. manifest.json notes[] records this
substitution, which includes wildcard-bound sockets.

The canceller ring
------------------
With options.lec true, the interface uvoiced_lec (DLT 148, DLT_USER1) holds
one entry per engine tick of the echo canceller of each line in a call.
The capture starts when the window starts, not at the arm.
The packet time stamp is the wall clock of the tick in microseconds.
An entry is a header of 64 bytes, then the sample planes. Each field and
each sample is in the byte order of the pcapng section:

  offset  width  field
  0       8      stamp_ns, CLOCK_MONOTONIC of the tick
  8       8      tick, the engine tick index
  16      4      lost_before, records of the line lost before this one
  20      4      frames of the canceller after the record
  24      4      hold_frames of the canceller after the record
  28      4      erle_mdb, signed
  32      4      echo_gain_micro
  36      4      coef_energy_micro
  40      4      ref_delay_samples
  44      2      gen
  46      2      flags
  48      2      samples, 80 or 160
  50      2      tail_ms
  52      2      noise_ddbov, tenths of dB; 1270 is unmeasured
  54      2      probes
  56      2      diverge_resets
  58      1      line
  59      1      kind: 0 RUN, 1 FEED, 2 END
  60      1      nlp_verdict: 0 off, 1 quiet, 2 dt, 3 unlike, 4 erle, 5 pass
  61      1      hold_left
  62      2      skipped, the periods the engine skipped before this record

Each plane holds samples int16 values:

  kind      planes          entry bytes
  0 RUN     ref, mic, out   64 + 6 x samples: 544 or 1024
  1 FEED    ref, mic        64 + 4 x samples: 384 or 704
  2 END     none            64

ref is the reference the canceller took, mic the microphone before the
cancel, and out the frame after the processor.
Read the ring with udsp/src/tools/lec-capture.py from the udsp source tree.
It writes a replay file, a CSV and WAV files for each line.
A line that is already in a call when the window starts has no NEW record:
its canceller took its first frame before the capture started. Its records
form a joined lineage, and lec_replay replays none of it. The next call of
that line starts at NEW.

Limits
------
options.wire_max_mb bounds the wire capture. The daemon checks size once per
second, so the file can exceed the cap between checks.
A capture that reaches the cap before the call restarts and leaves a gap.
A capture that reaches the cap during the call stops and sets wire.truncated.
options.rings_max_mb bounds media export. At this cap, the media ring stops,
but the log, SIP, and status rings continue. Their data can exceed the cap.
The bytes of the canceller ring do not count against this cap.
options.lec_max_mb bounds the canceller ring. At this cap, the canceller ring
stops alone, the capture of the canceller stops, and manifest.json notes[]
says so. The other rings continue. The daemon checks the cap before each
export step, so the ring can pass the cap by what one step writes.
The counts record truncated entries and entries overwritten before export.
Each SIP entry has a 16 KiB cap. Each status entry has a 256 KiB cap.
Each media entry has a 2048 byte cap. A cut entry is not a complete packet.
A canceller entry is at most 1024 bytes and is never cut.

Privacy
-------
Handle this archive like a packet capture. It can contain telephone numbers,
names, digest responses, SDP crypto keys, and plaintext call audio.
The data is not scrubbed. manifest.json contains[] names sensitive content:
sip_plaintext, srtp_keys_in_sdp, tls_keylog, rtp_plaintext, and lec_audio as
applicable.
The TLS key log is the whole file from the daemon run. It can decrypt other
connections from that same run, not only the traced call.
With options.lec true, the canceller ring holds the microphone before the
canceller: the near-end room in the clear. It holds every line in a call on
the unit, not only the traced line. contains[] then names lec_audio.
