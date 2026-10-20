# uvoice

`uvoiced` is a native voice stack for OpenWrt: one libubox and libubus
daemon. It runs these parts over a hardware abstraction layer (HAL):

- SIP user agents: SIP core, registration, and UDP, TCP and TLS transports;
- MGCP endpoints: the gateway side of RFC 3435 and PacketCable NCS 1.5,
  driven by a call agent;
- call state machines of the FXS lines;
- subscriber features: caller ID, call waiting, transfer, conference and
  message waiting;
- media plane.

The build selects one of two HAL backends:

- `stub`: an in-process fake with no hardware, for host tests and live
  network work.
- `udsp`: `libudsp`, the software voice DSP of the `udsp` package. A SLIC
  plug-in package provides the hardware driver.

The build also selects the signaling protocols the daemon carries: SIP, MGCP
or both. The `protocol` option of the base uci section picks one of them at
the daemon start.

## License

uvoice is licensed under GPL-2.0-only with a linking exception (`LICENSE`).
The exception permits a link with OpenSSL, and the load of plug-ins that
implement the libudsp SLIC or fax plug-in interfaces. OpenSSL and those
plug-ins are then not subject to the GPL. Each source file names its license
in an SPDX line.

## Build on a host

A host build needs `cmake` 3.13 or later, a C11 compiler, libubox with
`libblobmsg_json`, libubus and libuci, and OpenSSL for the SIP unit. If cmake finds no libubox, libubus
or libuci, configure stops and names the missing part. Without
`libubox/udebug.h`, the build compiles out the call trace rings.

A host rarely has libuci. Build it from the uci source of an OpenWrt download
cache into a private prefix:

```
cmake -S <uci source> -B $WORK/uci-build -DBUILD_LUA=OFF \
  -DCMAKE_INSTALL_PREFIX=$WORK/prefix
cmake --build $WORK/uci-build -j4
cmake --install $WORK/uci-build
```

| Option | Default | Effect |
|---|---|---|
| `UVOICE_HAL` | `stub` | HAL backend: `stub` or `udsp`. |
| `UVOICE_PROTO_SIP` | `ON` | Build the SIP unit, which links OpenSSL. |
| `UVOICE_PROTO_MGCP` | `OFF` | Build the MGCP unit. Configure refuses a build with both units off. |
| `UVOICE_TESTS` | `OFF` | Build the unit suites and the fuzz targets `fuzz_sip_msg`, `fuzz_sdp` and, with the MGCP unit, `fuzz_mgcp_msg`. Needs `test/` beside `src/`. |
| `UVOICE_UDSP_INCLUDEDIR` | empty | Directory that holds `udsp/udsp.h`, for the udsp backend outside a buildroot. |
| `UVOICE_UDSP_LIBDIR` | empty | Directory that holds libudsp, for the udsp backend outside a buildroot. |
| `UVOICE_UDSP_SRCDIR` | empty | libudsp source tree (`udsp/src`) for `test_hal_udsp`. Empty means `../../udsp/src` beside `src/`. |
| `UVOICE_VERSION` | `PKG_VERSION` of `Makefile` | Daemon version. |
| `UVOICE_SCHEMA_VERSION` | `version` key of `schema/uvoice-api-schema.json` | API schema version that each call trace manifest names. |

Host test build:

```
cmake -S uvoice/src -B uvoice/build -DUVOICE_HAL=stub -DUVOICE_TESTS=ON \
  -DCMAKE_PREFIX_PATH=<libuci prefix> \
  -DUVOICE_UDSP_SRCDIR=$PWD/udsp/src
cmake --build uvoice/build -j8
(cd uvoice/build && ctest --output-on-failure)
```

`test_hal_udsp` compiles the libudsp sources and finds libsrtp2 through
pkg-config. If cmake cannot find the udsp tree, it prints a skip message and
registers the other entries. Two entries are not suites:

- `sipp_scenario_lint` reads the SIPp scenario files.
- `lec_capture_e2e` replays a canceller capture that `test_hal_udsp`
  exports. It skips without the udsp tree, `python3` or udebug support.

The MGCP suites (`test_mgcp_msg`, `test_mgcp_txn`, `test_mgcp_ep`) register
only with `UVOICE_PROTO_MGCP` on. The end-to-end runner drives an MGCP flow
against the scripted call agent `test/e2e/mgcp_ca.py`.

With clang as compiler, the fuzz targets build in libFuzzer mode. None of
them is a ctest entry.

**IPv6 loopback.** Cases that bind `::1` skip on a host without an IPv6
loopback. For zero skips, run the suites in a network namespace with `lo` up.
Use `-V`, because `--output-on-failure` hides the skip lines of a passing
suite:

```
unshare -rn sh -c "ip link set lo up && cd uvoice/build && ctest -V"
```

**Sanitizer leg.** Run the whole set with the sanitizers before a commit:

```
cmake -S uvoice/src -B /tmp/uvoice-asan -DUVOICE_HAL=stub -DUVOICE_TESTS=ON \
  -DCMAKE_PREFIX_PATH=<libuci prefix> -DUVOICE_UDSP_SRCDIR=$PWD/udsp/src \
  -DCMAKE_C_FLAGS="-g -fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build /tmp/uvoice-asan -j8 && (cd /tmp/uvoice-asan && ctest --output-on-failure)
```

**CLI suite.** `test/cli/run_cli_tests.sh` drives the shipped
`files/voice-cli` and the `voip-cli-dispatch` of the `voice-scripts` package
against a stub daemon. It runs itself again under `unshare -rmn`, with an
overlay over `/usr` and a directory of its own over `/etc/init.d`. Shipped
absolute paths therefore run as shipped. If a host refuses this setup, the
run exits non-zero or prints a `Bail out!` line. The suite reads these
variables:

- `WORK`: work directory, default `/tmp/uvoice-host`.
- `UVOICED`: stub daemon, default `$WORK/build/uvoiced`.
- `UVOICE_PREFIX`: prefix with libuci and the `uci` CLI, default
  `$WORK/prefix`.
- `VOIP_CLI_DISPATCH`: dispatcher source, default the sibling
  `voice-scripts` package.
- `ARC_VOIP_UC`: `arc.voip` handler source for the vocabulary checks, which
  skip when it is unset.
- `VOIP_XML_DIR`: klish scheme directory for the xml checks, which skip when
  it is unset.
- `UVOICE_UCODE_MODDIR`: directory with a ucode `ubus` module for the
  formatter checks, default the modules of the host.

```
UVOICED=$PWD/uvoice/build/uvoiced UVOICE_PREFIX=<libuci prefix> \
  uvoice/test/cli/run_cli_tests.sh
```

Output is TAP with a plan line, and a skipped check still counts. `test/cli`
also holds
`run_seed_tests.sh` for the board defaults helper, and `run_init_tests.sh`
for the init script. Both run under `unshare -rm`.

## Build in OpenWrt

`Makefile` builds the package `uvoice`: `uvoiced`, an init script,
`voice-cli`, call trace tools and helper scripts. A package build copies
`src/` only, so it builds no tests. Four config options apply:

- HAL backend choice: `UVOICE_HAL_STUB` (default, no hardware) or
  `UVOICE_HAL_UDSP` (software voice DSP on ALSA and spidev). The package
  passes the choice to cmake as `UVOICE_HAL`. The udsp choice adds libudsp
  and alsa-utils.
- `UVOICE_PROTO_SIP` (default on) and `UVOICE_PROTO_MGCP` (default off): the
  signaling protocols the image carries. The SIP entry prompts only beside
  MGCP, so an image always carries a protocol. Only the SIP unit links
  OpenSSL, so an image with MGCP alone carries no libopenssl.
- `UVOICE_T38` (default off, needs `UVOICE_HAL_UDSP`): adds the T.38 fax
  gateway plug-in `udsp-fax-spandsp`. T.38 relay also needs `fax_t38_enable`
  on the channel and a plug-in that loads. Without it, fax uses G.711
  passthrough only.

In a buildroot, the libudsp package stages its headers and library, so the
build needs no path variable.

The init script needs the `voice-scripts` package of a SmartOS image. It
asks `voice-stack.sh` of that package which voice stack a platform runs. It
starts `uvoiced` only for the uvoice stack. Without `voice-scripts`, it
starts nothing. Reason: `uvoiced` owns the voice hardware, and two voice
stacks cannot share it.

## ubus API schema

`schema/uvoice-api-schema.json` is a JSON Schema (draft 2020-12). It holds
every ubus method of the `voice`, `voice.trace`, `voice.calltrace` and
`voice.stub` objects, with argument policies and reply payloads.
`schema/README.md` describes the capture envelope that the root validates.

The schema is versioned. Its `version` key is a dotted triple, and a schema
change moves it. `test/cli/schema_check.py` validates one capture against the
schema. It needs `python3` and the `jsonschema` module 4.18 or later. The
schema leg of the CLI suite validates status captures with it, and skips
without those two.

## Test timing

Unit suites drive the daemon's event loop (`uloop`) for bounded windows. A
daemon timer thus fires inside a pumped window, not after a wall-clock sleep.
A few helpers sleep in short steps: they wait for a child process, a file or
a backend thread, or pace a fake audio backend. Machine timer values above
86400 s clamp to `LINE_TIMER_MAX_S`, because `uloop_timeout_set()` takes an
int. If you add a timed behavior, drive the loop to the deadline. Do not add
a real `sleep`.

Suites read fixtures and hooks from the environment, never from the live
wall time. Main variables:

- `UVOICE_UCI_CONFDIR`: scratch `/etc/config` that the daemon reads.
- `UVOICE_UCODE_MODDIR`: ucode module directory that the CLI formatter
  needs.
- `UVOICED`, `UVOICE_PREFIX`, `WORK`: daemon and paths of the CLI suite.
- `UVOICE_STUB_DSP_START`, `UVOICE_STUB_RING_START`: behavior hooks of the
  stub backend.
- `UVOICE_STUB_RING_CAP_MS`: caps the stub ring timer, so that a suite can
  watch a long ring end inside a pumped window.
- `UVOICE_EVENT_POST`: event script that the daemon runs in place of
  `/usr/srg/scripts/event-post.sh`.
- `FAKE_REPLIES`, `VOIP_XML_DIR`: canned ubus replies and klish scheme
  directory of the CLI suite.
- `UVOICE_TLS_FIXTURES`: directory of the TLS fixtures.

If a tool or a library that a check needs is absent, the check skips and
prints its reason. In `test_sip_tls`, a missing fixture directory is a
failure, not a skip. `sipp_scenario_lint` without `awk` exits 2, also a
failure. Reason: a silent skip reports a pass for a suite that checks
nothing.

## Conventions

- `src/hal/hal.h` is the stable HAL contract. Feature bits are append only,
  and media blocks are appended at the end, so an older backend keeps its
  layout. Never renumber a bit or reorder a media block.
- `schema/uvoice-api-schema.json` is versioned. The schema leg of the CLI
  suite must pass.
- The binary carries no commit id; `md5sum` identifies a build.
- Design notes, the test guide and bench procedures live in the feed that
  hosts the package.
