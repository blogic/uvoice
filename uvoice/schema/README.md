# uvoice machine-readable API schema

`uvoice-api-schema.json` is a JSON Schema (draft 2020-12). It is the contract
for every answer that `uvoiced` gives over ubus. Its descriptions cite their
owning functions, and one document covers the whole API through envelopes.
The schema is written code first, from the uvoice source.

The UCI configuration encoding is a contract of its own. This schema holds
the shape of the `dump_config` reply, because it is a reply.

## Files

| File | Contents |
|---|---|
| `uvoice-api-schema.json` | Every ubus method of the `voice`, `voice.trace`, `voice.calltrace` and `voice.stub` objects: argument policies and reply payloads. The root validates one capture envelope. |

The `voice.stub` object exists on the stub build only (`UVOICE_HAL_STUB`).

The `voice` object sends three notifications (`ubus_notify`):

- `registration` under every `voip.base.ui`, on each registration state
  post (`$defs/RegistrationEvent`, `host_reg_notify()` in
  `src/proto/proto.c`);
- `call` and `device` under `voip.base.ui desk` (`$defs/DeskCallEvent`,
  `$defs/DeskDevice`, `desk_op_call_changed()` and
  `desk_op_device_changed()` in `src/proto/sip/line.c`).

The root validates method captures only; the notification payloads have
their own `$defs`.

With `voip.base.ui desk` the SIP protocol adds the desk phone verbs
(`sip_desk_rows[]` in `src/proto/sip/proto.c`). Its `dial` replaces the
injection verb of the core and answers `$defs/DeskDialReply`.

Sources: `ubus_connect_cb()` in `src/ubus.c`; every `ubus_send_reply` call
site in `src/ubus.c` and `src/hal/stub/stub.c`.

## Envelope

A capture is one object:

```json
{
  "object": "voice",
  "method": "status",
  "args": {},
  "reply": { "version": "<PKG_VERSION>", "...": "..." }
}
```

A capture can omit `args` for a NOARG method (declared with
`UBUS_METHOD_NOARG`). It can omit `reply` for a call that answered a bare
status code. Every refusal answers a bare status code, and so do the injection
verbs of `voice.stub`.

Optional argument `end` of `StubToneArgs` selects which edge of a tone row
the stub injects. Its default is false, the start row, so a caller that omits
it injects the start row. Sources: `stub_ubus_tone()` in
`src/hal/stub/stub.c`.

## Integer widths

blobmsg has no unsigned JSON rendering. The schema encodes the wire
behavior, not the C type:

- `U32`: a `blobmsg_add_u32` field. Both `ubus` and the ucode binding
  render a value at or past 2^31 sign-extended, so the schema accepts
  `-2147483648 .. 4294967295`. The semantic value of a negative
  reading is `value + 2^32`.
- `I32`: a signed value in the `INT32` slot, rendered signed. The producer
  casts it to `uint32_t` for `blobmsg_add_u32`. The slot is the same one, so a
  negative reading renders as itself and not as `value + 2^32`. The four
  signed engine figures of the status `diag` table follow this rule:
  `delay_ref`, `loop_ref`, `delay_drift_min` and `delay_drift_max`
  (`diag_engine_dump()` in `src/status.c`).
- `U64`: a `blobmsg_add_u64` field, rendered as a signed 64-bit
  integer.
- Booleans travel as `blobmsg_add_u8` and render `true`/`false`.

## Presence rules

**Strict objects.** Every reply object refuses an unknown key. Most use
`additionalProperties: false`. The jb rows that compose `$defs/JbCounters` use
`unevaluatedProperties: false`. Conditional keys carry their gate in the
schema, not only in prose.

**Dependent keys.** `dependentRequired` covers the keys that arrive with
another key. Among them:

- `registrar` arrives with `registrar_port`.
- A leg's `session_expires` arrives with `we_refresh`.
- A line entry's `expires` requires `last_code`, `contact`, `targets` and
  `dns_answer`. A registration that never initialized reports none of the five
  keys.
- The `version` key of the status `hal` table requires the capability keys and
  the backend counters. The counters `holds`, `capture_drops`, `trim_drops`,
  `playback_expired`, `hook_gaps`, `hook_moved_gaps` and `hook_gap_max_ms`
  arrive with `version`, like the counters beside them. `status_hal_dump()`
  writes that part of the table only while a HAL is attached.

**State gates.** `if`/`then` covers the keys that follow a state:

- a cdr record's `media` table, with `media_state == "present"`;
- a status jb or lec row's `stamp_ms`, with `sample == "last_call"`;
- a lec row's `state` table, with any `sample` other than `"never"`;
- a status monitor row's `stamp_ms`, its measurements, its raw words and its
  decoded booleans, with any `sample` other than `"never"`;
- a call row's `fault`, with `oos: true`;
- a call row's `loop_test` table (`$defs/CallLoopTest`), with `oos: true`;
- a signaling table's `error` (`$defs/SignalingTable`), with
  `state == "unavailable"`, and forbidden with `running`;
- every key of the `voice.calltrace` reply other than `state`, with the
  `state` word ("Call trace reply" below).

**Else branches.** Several gates carry an `else` as well. A leg's `fax_kind`
is required beside `fax == "passthrough"` and forbidden anywhere else.
Reason: `calls_status_dump()` writes the word in that one branch.

**Fax words.** The `fax` and `fax_kind` words of a leg name its media policy,
not its codec. The calling side takes the passthrough kind from an answer tone
on the wire, and it keeps the codec it committed to. A compressed codec can
therefore stand beside `fax: passthrough`.

**Absent keys.** A key that uvoice cannot answer is absent. It is never null,
and never a zero that uvoice did not measure.

**Constant arrays.** One deliberate exception is the constant `jb`,
`lec` and `monitor` arrays of `voice status`. The `jb` array carries one row
per (line, stream), and the `lec` and `monitor` arrays one row per line, for
the life of the daemon. A pair or a line that never answered reads
`sample: "never"`. A jb row there carries every counter 0 and
`mode: "unknown"`, and no zero is a measurement. A lec row there carries no
`state` table at all, so it holds no counter to misread. The word itself tells
the states apart, so a cached `"last_call"` reading needs no sentinel stamp.

**Backend counters.** The other exception is the backend counters of the
status `hal` table. A backend that cannot lose a completion, or that has no
TDM engine or no SLIC thread, reports 0 there. Sources: `status_hal_dump()` in
`src/status.c`.

**Monitor row.** A monitor row keeps its own three words,
`MonitorSampleWord`: `sampled`, `stale` and `never`. `$defs/StatusMonitorRow`
carries them. A `sampled` row and a `stale` row both carry `stamp_ms`, the
instant of the reading. The jb and lec rows carry a stamp for `last_call`
alone. A monitor reading is up to ten seconds old by design, because the
backend samples the SLIC on its own cadence. A `never` row carries the three
fault fields and no measurement.

**Backend tables.** Two tables of the status reply depend on the backend:

- Table `diag` (`$defs/StatusDiagTable`) is absent on a backend that
  carries neither diagnostic getter: the stub backend. Its `streams` array
  holds one `StatusDiagStreamRow` per stream that the getter answered for,
  so an idle stream has no row. Its `engine` table
  (`StatusDiagEngineRow`) is present once the backend published a snapshot.
  `voice status` renders the table for every line. A call trace snapshot
  renders it for the traced line, through the same function.
- Table `rt` under the `hal` table is present only when the backend
  reported one, which is the udsp backend. The stub backend leaves it out.
  `RtPolicyWord` holds the words of its `policy` and `net_policy` keys.

Sources: `status_diag_dump()` and `hal_rt_dump()` in `src/status.c`.

**Protocol shapes.** The protocol in force writes `lines[]` and its keys of
each call row, so one `oneOf` of `StatusReply` keys both by `signaling`:

- `sip` and `running`: `lines[]` holds `AccountEntry` rows, and each call row
  requires the SIP machine keys `state`, `call_waiting`, `dnd` and `calls`;
- `mgcp` and `running`: `lines[]` holds `MgcpLineEntry` rows, each call row
  carries endpoint `state` and core keys alone, and the reply requires the
  `mgcp` table;
- `unavailable`: `lines[]` is empty, and a call row carries core keys of
  `lines_status_dump()` alone.

The `sip` and `unavailable` branches refuse the `mgcp` table. A schema that
adds a protocol adds its branch. Sources: `proto_status_dump()` and the null
table in `src/proto/proto.c`; `mgcp_status_dump_lines()`,
`mgcp_status_dump_call()` and `mgcp_status_dump_top()` in
`src/proto/mgcp/proto.c`.

**Refusal counts.** A call row carries its `refused` table
(`$defs/CallRefused`) only after its line refused a call. The table counts
calls that the line turned away. Until the first refusal the whole table is
absent, so it adds no required key to a capture. Sources:
`refused_dump()` in `src/proto/sip/call.c`.

**Stub dump.** A stub line entry carries the three loop keys `loop_mode`,
`loop_peer` and `loop_delay_ms` whatever the mode is, as it carries `conf`. A
capture of a board that runs no loop test therefore validates with the three
keys at 0. Sources: `stub_ubus_dump()` in `src/hal/stub/stub.c`.

**Call trace reply.** The three methods of `voice.calltrace` answer the
same reply, so one definition serves them: `$defs/CalltraceStatus`. Their
envelopes are `CalltraceArmCall`, `CalltraceCancelCall` and
`CalltraceStatusCall`, and `arm` takes `$defs/CalltraceArmArgs`. The `state`
key (`CalltraceStateWord`) is the only unconditional key. A `last_error`
key can stand in any state, until the next arm clears it. A `failure` key
stands in a `failed` reply alone. Every other key carries an `if`/`then`
gate on `state`:

- An `idle` reply names no trace at all.
- An `armed` reply names what it waits for (`waiting_for`,
  `CalltraceWaitingWord`), and no window is open yet.
- A `running` reply carries the instants of the window and its two counts,
  `calls` and `snapshots`.
- A `finishing` reply adds the end of the window and still names no archive.
  Once the finisher started, it names the budget of that run
  (`finish_budget_ms`), and no other state carries that key.
- A `done` reply names the reason (`reason`, `CalltraceReasonWord`) and the
  archive. It names a work dir only while the finisher did not remove the dir.
  The finisher is the child process that packs the work dir into the archive.
- A `failed` reply keeps its work dir until the next arm that passes its
  preflight. It carries `failure`, which names what failed the trace, and
  `last_error`. A refused arm writes its word into `last_error` and leaves
  `failure` as it is.

Every reply other than `idle` carries the id, the line, the options
(`CalltraceOptions`) and the two live tables. The live tables are `wire`
(`CalltraceWire`) and `rings` (`CalltraceRings`, one `CalltraceRingCounts` per
ring). `calltrace.c` names the signaling ring `sip` in every image
(`ct_ring_names[]`). The reply refuses an unknown key. A key
that `calltrace_status_dump()` gains without its schema row therefore fails
schema validation, and no other check fails on it.

## Vocabularies

One concept has a different spelling in different replies, on purpose. The
schema keeps one enum per spelling:

1. Codecs: `hal.codecs[]` and the stub dump use lower snake case (`pcmu`,
   `g726_32`). A leg's `codec` and a cdr record use the SDP names (`PCMU`,
   `G726-32`, `opus`, `t38`).
2. SRTP suites: a leg's `srtp` is the RFC 4568 upper spelling; the
   stub dump's `srtp_suite_name` is lower.
3. States: `call[].state` is the upper-snake FSM vocabulary; a leg's
   `state` is lower-hyphenated.

The enums hold fallback words that name functions can return (`?`,
`unknown`). A capture that holds one still validates, so such a drift is
visible in review and does not crash the validator.

## Maintenance rules

- The schema is hand-authored. If the code changes shape, update the schema
  and its `version` in the same change. A change of shape is a new reply
  field, a new enum word or a new method. Reason: every reply object refuses
  an unknown key, and every enum refuses an unknown word. A capture with a new
  key or word fails validation until the schema holds it.
- The fields of `version` move as follows:
  - The major field moves for a break.
  - The minor field moves for an additive change that a reader has to
    handle, such as a new required key in a reply it parses.
  - The patch field moves for a description fix. It also moves for an
    additive change that every existing reader can ignore, such as a new
    method or object.
- The `version` key of `uvoice-api-schema.json` holds the current version. A
  commit that moves the version states the reason in its message.
- Most definitions, and some properties, carry a `description` that names
  the owning function in the source, so claims stay verifiable.
- Validate a capture with python jsonschema (>= 4.18) from this directory, or
  with any 2020-12 validator. `test/cli/schema_check.py` validates one capture
  envelope with jsonschema. The voice-cli suite `test/cli/run_cli_tests.sh`
  runs it.
