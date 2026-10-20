# udsp

`libudsp` is a software voice DSP for analog FXS lines. It ships as a shared
library and runs these parts:

- line state machines and the SLIC control thread;
- TDM plane and media plane: codecs, RTP and SRTP, jitter buffer;
- tone, DTMF and caller ID generation and detection;
- line echo canceller.

libudsp loads two kinds of plug-in with `dlopen`: a SLIC plug-in and a T.38
fax gateway plug-in. A SLIC plug-in package provides the hardware driver.
Host builds and tests use the in-tree fake SLIC (`src/slic/fake.h`), so they
need no hardware. The package also builds two tools:

- `udsp-cli`: a line-level command tool (tones, ring, caller ID, DTMF,
  manufacturing loop test, events, a region profile print, a fake-SLIC
  selftest).
- `udsp-linecap`: a line capture and analysis tool.

## License

libudsp is licensed under LGPL-2.1-only (`LICENSE`). These files of the
plug-in interface are BSD-2-Clause (`LICENSES/BSD-2-Clause.txt`):

- `slic.h`, `fax.h` and `event.h` in `src/include/udsp/`;
- `host_linux.c`, `host_linux.h`, `plugin_load.c` and `plugin_load.h` in
  `src/slic/`.

Reason: a SLIC or fax plug-in and its tools compile against these files
without LGPL obligations. A probe tool of a plug-in can compile the host and
loader files and link no libudsp. Each source file names its license in an
SPDX line.

## Build on a host

A host build needs `cmake` 3.13 or later, a C11 compiler, `pkg-config`,
libsrtp2 and the Linux uapi header `sound/asound.h`. If cmake finds alsa-lib,
the library also gets the ALSA TDM backend (`tdm/alsa.c`).

| Option | Default | Effect |
|---|---|---|
| `UDSP_TESTS` | `OFF` | Build the unit and fuzz suites and register them with ctest. |
| `UDSP_TOOLS` | `OFF` | Build `udsp-cli` and `udsp-linecap`. |
| `UDSP_BENCH` | `OFF` | Build `bench_dtmf`, `lec_replay`, and `bench_lec` if pkg-config finds speexdsp. No bench is a ctest entry. |
| `UDSP_FAX_T30_SWEEP` | `OFF` | Register the T.30 loss sweep `test_fax_t30_sweep`. |
| `UDSP_OPUS` | `AUTO` | Opus codec: `ON` requires libopus, `OFF` leaves it out, `AUTO` probes `opus` through pkg-config. |
| `UDSP_REGION_DIR` | `/usr/share/udsp/regions` | Directory of the installed region files. |
| `UDSP_REPLAY_LEC_C` | `dsp/lec.c` | Canceller source that `lec_replay` links. |
| `UDSP_VERSION` | `PKG_VERSION` of `Makefile` | Library version and SONAME. |

A development build turns on the tests and the tools:

```
cmake -S udsp/src -B udsp/build -DUDSP_TESTS=ON -DUDSP_TOOLS=ON
cmake --build udsp/build -j8
(cd udsp/build && ctest --output-on-failure)
```

The set of ctest entries depends on the configuration. Most entries register
in every configuration. These entries are conditional:

- `test_opus`: the build links libopus.
- `test_fsk_demod`, `test_linecap_analyse`, `test_lec_capture`,
  `test_path_fit` and the fixture setup `test_lec_capture_fixture`: cmake
  finds `python3`. Without numpy, `test_path_fit` prints a skip line and
  passes.
- `test_fax_t30`: pkg-config finds spandsp 3.1 or later and `libtiff-4`.
- `test_fax_t30_sweep`: the conditions of `test_fax_t30`, and
  `UDSP_FAX_T30_SWEEP=ON`.

**spandsp reference.** spandsp is a host test dependency only. The library
and the package `Makefile` do not use it. If pkg-config finds spandsp,
`test_dsp` and `test_fax_det` use it to generate cross-check stimuli. The
independent reference cases of `test_g722` compare the codec against spandsp
3.1 or later. Reason: spandsp saturates its G.722 QMF output only from 3.1.
Configure output names the state of these cases:

- `spandsp <version>: G.722 reference on`;
- `spandsp <version>: G.722 reference off, 3.1 needed`;
- `no spandsp: G.722 reference off`.

The `spandsp` package of this feed has a host build. In an OpenWrt tree, set
`hostpkg` to `staging_dir/hostpkg`, which then holds spandsp 3.1.1:

```
PKG_CONFIG_PATH=$hostpkg/lib/pkgconfig cmake -S udsp/src -B <dir> -DUDSP_TESTS=ON
```

**T.30 suite.** `test_fax_t30` runs a real T.30 page between two spandsp fax
terminals, through two udsp streams with a G.711 passthrough description.
Time is virtual, and no engine runs. The suite asserts a floor:

- a clean A4 page passes with ECM on and with ECM off;
- a page from a sender 200 ppm fast passes;
- every concealed unit reaches the terminal as silence.

Record cases run three loss schedules from captured fax calls, and one loss
per 750 packets. They print `# t30 result:` and `# t30 frames:` lines, and
their recorded values are not assertions. `test_fax_t30_sweep` runs one loss
per 100, 250, 500, 750 and 1500 packets, with ECM on and off. Run it by name
with `ctest -R test_fax_t30_sweep`; the sanitizer legs leave it out.

**G.729 test vectors.** The ITU-T identity cases of `test_g729` read test
vector files from two directories. ctest sets neither variable, and a case
skips when its variable is unset:

- `G729_TV_DIR`: G.729 encoder and decoder vectors;
- `G729B_TV_DIR`: G.729 Annex B vectors for the VAD, encoder and decoder.

**Sanitizer legs.** Run both legs before a commit:

```
cmake -S udsp/src -B /tmp/udsp-asan -DUDSP_TESTS=ON -DUDSP_TOOLS=ON \
  -DCMAKE_C_FLAGS="-g -fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build /tmp/udsp-asan -j8
(cd /tmp/udsp-asan && ASAN_OPTIONS=detect_leaks=1 ctest --output-on-failure)

cmake -S udsp/src -B /tmp/udsp-tsan -DUDSP_TESTS=ON -DUDSP_TOOLS=ON \
  -DCMAKE_C_FLAGS="-g -fsanitize=thread"
cmake --build /tmp/udsp-tsan -j8
(cd /tmp/udsp-tsan && ctest --output-on-failure -R 'test_core|test_engine|test_media|test_cli|test_linecap')
```

The TSan leg passes clean on a quiet host, and the project accepts no failure
there. Single cases can fail under heavy concurrent load. A failure on a
quiet host is a defect.

## Build in OpenWrt

`Makefile` builds two packages:

- `libudsp`: the shared library, and the region files in
  `/usr/share/udsp/regions`. It depends on alsa-lib and libsrtp2.
- `udsp-cli`: `udsp-cli` and `udsp-linecap` in `/usr/bin`.

Config option `UDSP_OPUS` (default off) builds the Opus codec into libudsp
and adds libopus to its dependencies. The package passes `ON` or `OFF` to
cmake, never `AUTO`. Reason: a libopus in the staging directory must not
change the link of a configuration that did not select it.

The SONAME carries the full `PKG_VERSION`, because the ABI changes with each
release. An image therefore holds one matching libudsp and `uvoiced` pair.
`Build/InstallDev` stages these files for a plug-in build:

- headers of `src/include/udsp/`, with `host_linux.h`, `plugin_load.h` and
  `fake.h`, in `usr/include/udsp`;
- `host_linux.c` and `plugin_load.c` in `usr/share/udsp/src`.

## Plug-in interfaces

| Plug-in | Header | Symbol | Version macro | Path in `struct udsp_config` |
|---|---|---|---|---|
| SLIC control | `slic.h` | `udsp_slic_descriptor` | `UDSP_SLIC_ABI_VERSION` | `slic_plugin`; empty selects the fake SLIC |
| T.38 fax gateway | `fax.h` | `udsp_fax_descriptor` | `UDSP_FAX_ABI_VERSION` | `fax_plugin`; empty means G.711 passthrough only |

libudsp opens a plug-in with `dlopen` and resolves its one descriptor symbol.
It checks `abi_version` and `struct_size` before it calls the plug-in. It
accepts only the exact version of its own header. A plug-in therefore ships
in the same image as the libudsp that it builds against. A change to a
plug-in contract raises its version macro.

Every SLIC ops call except `caps_get` runs on the slic thread of the engine.
The plug-in reaches the bus only through `struct udsp_slic_host`, which
libudsp supplies at attach. No vendor SLIC header enters libudsp.

Without a fax plug-in, libudsp detects CED, CNG and V.21 and carries fax as
G.711 passthrough.

## Test timing

The engine measures every cadence against a real wall clock
(`udsp_mono_us()` in `src/core/clock.h`, and `ppoll` timeouts on the ack
path). A test that must let a deadline elapse cannot fast-forward. It
therefore drives the tick clock or shrinks the cadence.

Each timed cadence has a test-time override. The library reads each override
once from the environment. When a variable is unset, the production default
applies unchanged:

- `UDSP_MAX_TICKS`: bound of the engine tick loop.
- `UDSP_SYNC_TICK_MS`: synchronous tick ack deadline (default 100 ms).
- `UDSP_NET_ACK_MS`: net thread command ack deadline (default 100 ms).
- `UDSP_WATCHDOG_MS`: watchdog cadence that publishes the engine counters
  (default 1000 ms).
- `UDSP_SLIC_DEAD_RETRY_MS`: dead-line init retry cadence (default 10000 ms).
- `UDSP_TEV_PLAY_TIMEOUT_MS`: lost-end bound of a received telephone event
  (default 120 ms).

Two deadlines have no override: `UDSP_ATTACH_WAIT_MS` (`core/udsp.c`) and
`UDSP_SYNC_SLIC_MS` (`core/cmd.h`).

Rules when you add or change a timed cadence:

- Add its override in the same change. An override is env-gated, and the
  production default stays unchanged.
- Keep one assertion that checks the production default, so an override
  cannot hide a wrong constant.
- Do not add a real `sleep` for a deadline that an override could shrink.

Data directories:

- `UDSP_REGION_DIR` in the environment replaces the region directory of the
  library. ctest gives `test_region` the source `data/regions` as argument.
- `UDSP_LEC_CAPTURE_FIXTURE` names the file that the ctest fixture
  `test_lec_capture_fixture` writes. The fixture cases of `test_lec` skip
  when the variable is unset.

## Conventions

- `src/include/udsp/*.h` are stable ABI contracts. A change to one is a
  versioned decision, not an incidental edit. The version macros are
  `UDSP_SLIC_ABI_VERSION` in `slic.h`, `UDSP_CODEC_ABI_VERSION` in `codec.h`
  and `UDSP_FAX_ABI_VERSION` in `fax.h`. No loader checks the codec version,
  because every codec is built into libudsp.
- Design notes, the test catalog and bench procedures live in the feed that
  hosts the package.
