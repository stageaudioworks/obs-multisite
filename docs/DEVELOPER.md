# Developer guide

Building the code yourself, running the tests, and knowing what is where. For
the project's state — what works and what is next — see the
[README](../README.md). For sending a change back, including the sign-off every
commit needs, see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Build and test

Core and tests, no OBS required:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and a C++17 compiler. On Linux and macOS you also need
OpenSSL headers; on Windows the crypto backend uses the built-in bcrypt, so
OpenSSL is not needed.

With the OBS plugin (adds libobs and FFmpeg):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON
```

With the operator docks (adds Qt6 and obs-frontend-api):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON -DENABLE_QT=ON
```

### macOS

Apple Silicon only, and the core needs no OpenSSL — it uses CommonCrypto from
libSystem, so a built plugin loads on a Mac that has never had Homebrew.
`ctest` should pass **24/24** with nothing installed but CMake and FFmpeg.
(`ctest -N` lists exactly what this tree built, which is the honest way to know
the number; what gates each suite is under
[What the tests cover](#what-the-tests-cover) below.)

For the **plugin**, the only real difficulty is ABI matching. OBS.app carries
its own FFmpeg, Qt and libobs, and a plugin has to use those exact copies. A
build against Homebrew's FFmpeg or Qt loads on the machine that built it and
fails elsewhere, because Homebrew tracks the latest version and OBS pins one —
at the time of writing that is libavcodec 63 against OBS's 62, and Qt 6.11.2
against 6.11.1. A second Qt is the worse of the two: the docks attach to the
host's `QApplication`, and a duplicate `QtCore` has none.

So take the dependencies from **obs-deps at the version OBS itself pins**,
which is in `CMakePresets.json` in the OBS source under the `dependencies`
preset. For OBS 32.2.2 that is `2026-07-15`:

```sh
OBS_TAG=32.2.2; DEPS_VER=2026-07-15
mkdir -p deps/root
for n in macos-deps-$DEPS_VER-arm64.tar.xz macos-deps-qt6-$DEPS_VER-arm64.tar.xz; do
  curl -L "https://github.com/obsproject/obs-deps/releases/download/$DEPS_VER/$n" | tar x -C deps/root
done
curl -L "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_TAG.tar.gz" | tar xz
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > obsconfig.h

DEPS=$PWD/deps/root; OBS_SRC=$PWD/obs-studio-$OBS_TAG
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_OBS_PLUGIN=ON -DENABLE_QT=ON \
  -DCMAKE_PREFIX_PATH="$DEPS" -DQt6_DIR="$DEPS/lib/cmake/Qt6" \
  -DFORCE_FFMPEG_MANUAL_SEARCH=ON \
  -DFFMPEG_INCLUDE_DIR="$DEPS/include" \
  -DFFMPEG_avformat_LIBRARY="$DEPS/lib/libavformat.dylib" \
  -DFFMPEG_avcodec_LIBRARY="$DEPS/lib/libavcodec.dylib" \
  -DFFMPEG_avutil_LIBRARY="$DEPS/lib/libavutil.dylib" \
  -DFFMPEG_swresample_LIBRARY="$DEPS/lib/libswresample.dylib" \
  -DFFMPEG_swscale_LIBRARY="$DEPS/lib/libswscale.dylib" \
  -DLIBOBS_INCLUDE_DIR="$OBS_SRC/libobs" \
  -DLIBOBS_CONFIG_INCLUDE_DIR="$PWD" \
  -DLIBOBS_FRONTEND_INCLUDE_DIR="$OBS_SRC/frontend/api"
cmake --build build --target obs-multisite
```
**`-DQt6_DIR` is not optional, and getting it wrong fails quietly.** If CMake
finds a different Qt first — Homebrew's, typically, since `/opt/homebrew` is on
the default search path — the plugin links against that instead of the one OBS
ships. It builds, it installs, and then `dlopen` fails at OBS startup with a
missing private symbol:

```
Symbol not found: __ZN14QWindowPrivateC2E16QtPrivate_6_11_2
Module '.../obs-multisite' not loaded
```

Nothing in the build says anything is wrong, because nothing is: the two Qts are
both valid, they are simply a patch release apart, and Qt tags its private ABI
with the exact version. The failure only appears once the code touches a symbol
the two versions do not share — so a build can be wrong for weeks and work,
until one new `#include` surfaces it.

Check it rather than assume, after configuring and after any change to the
dependencies:

```sh
cmake -S . -B build ... 2>&1 | grep "Qt docks enabled"   # says which Qt was found
otool -L build/obs-multisite.plugin/Contents/MacOS/obs-multisite | grep QtCore
nm -u build/obs-multisite.plugin/Contents/MacOS/obs-multisite | grep QtPrivate_
```

The version in the second command must match the Qt inside the OBS you are
installing into (`/Applications/OBS.app/Contents/Frameworks/QtCore.framework`),
and the third should print nothing at all — a plugin that needs a private ABI
tag is a plugin that will not load on any other OBS build.

**Two headers are vendored rather than linked.** `src/vendor/nlohmann/json.hpp`
is the JSON parser the plugin and the relay use throughout.
`src/vendor/obs-websocket/obs-websocket-api.h` is obs-websocket's vendor API: it
is header-only and talks to obs-websocket through OBS's proc handler, so the
plugin gains no link dependency and builds identically whether libobs comes from
an OBS source tree, obs-deps or a distro package. It is GPL-2.0-or-later, which
is compatible with this project's GPL-3.0-or-later, and it is kept byte-for-byte
as published so updating it is a straight copy.



Two things are worth knowing about that. Passing every FFmpeg path explicitly
and pinning `Qt6_DIR` is not belt-and-braces: if Homebrew's copies are
installed they are found first, and the result is the mismatched build this
recipe exists to avoid. And **no OBS binary is needed** — only headers. The
plugin is linked with `-undefined dynamic_lookup`, so libobs and
obs-frontend-api resolve out of the running OBS at load time. Qt *is* linked
for real, because those symbols are not OBS's to provide.

The result is `obs-multisite.plugin`, whose every versioned dependency is an
`@rpath` reference to something OBS already ships, with one rpath —
`@executable_path/../Frameworks`. A plugin has no executable of its own, so
`@executable_path` is the host: `OBS.app/Contents/MacOS`, making
`../Frameworks` OBS's own framework directory wherever OBS is installed.
Check a build with `otool -L` and `otool -l | grep -A2 LC_RPATH`; anything
that is not `@rpath`, `/System` or `/usr/lib` is a path from your machine and
will not exist on anybody else's. CI asserts exactly that.

With the public simulcast relay (adds SQLite; needs the `ffmpeg` command at
run time, not at build time):

```sh
cmake -S . -B build -DMULTISITE_BUILD_RELAY=ON -DBUILD_PLAYER=OFF
cmake --build build --target multisite-relay
```

Or build the container, which runs the relay's tests as part of the image so a
broken build cannot become something somebody deploys:

```sh
docker build -f relay/Dockerfile -t multisite-relay .
```

CI builds and tests the core on Linux x86, **Linux ARM64**, Windows and macOS,
and produces the installable Windows and macOS plugins. The ARM64 job exists
because the planned appliance runs there, so a regression is caught in CI
rather than on hardware. The macOS job asserts what makes a bundle loadable on
a machine other than the one that built it: package type `BNDL`, arm64, the
module entry points exported, exactly one rpath, no OpenSSL, and no absolute
path outside `/System` and `/usr/lib`.

### Checking the appliance's code on a machine that cannot build it

`alsa_output.cpp` and `aes67.cpp` are compiled only where ALSA and libcurl exist,
so a laptop builds neither — and those are exactly the files a change is most
likely to break without anybody noticing until it is on a Pi. Two things make
that checkable without the hardware:

- `aes67.h` is deliberately free of ALSA, curl and systemd. The JSON, the SDP and
  the address arithmetic are inline, and the `aes67` suite exercises them with
  nothing installed. What remains in `aes67.cpp` is the part that talks to the
  daemon and is one systemctl helper, and there is little in it to get wrong.
- For `alsa_output.cpp`, a **stub `<alsa/asoundlib.h>`** is kept in the
  repository at `tests/stubs/`, declaring only the surface that file uses.
  Pointing the compiler at it is enough to have the file checked:

  ```sh
  c++ -std=c++17 -fsyntax-only -Wall -Wextra -Itests/stubs \
      -Isrc -Isrc/appliance -Isrc/core src/appliance/alsa_output.cpp
  ```

  That catches the syntax, the types and the members that no longer exist —
  everything a change to that file can break that is not ALSA's own behaviour.
  It is not a substitute for the Linux build in CI, which compiles the same file
  against the real library; it is what stops a rename being discovered on a Pi.
  The stub must never go on the include path of a real build — it would shadow
  the real header — and it is worth keeping to exactly what the file calls, so
  that a declaration left behind after its last caller was removed is noticed.

---

## What the tests cover

Every suite runs without OBS. How many a given build produces depends on what
is installed, so `ctest -N` is the way to know rather than a number here that
goes stale — but the gates are these: the `cmaf*` suites need FFmpeg, `s3_url`
and `s3_cancel` need libcurl, `preview` and the whole appliance need FFmpeg and
libcurl together, the three relay suites need `-DMULTISITE_BUILD_RELAY=ON`, and
`core_portable` needs a POSIX host.

| suite | what it proves |
|---|---|
| `reliability` | durability across a crash, ordered drain through an outage, checksum rejection, permanent-failure handling |
| `session` | the write-ordering invariant holds continuously, including across a crash and resume; packed multi-channel audio round-trips, channel order intact |
| `decoder` | timeslipping: the cache fills while paused, resume continues exactly where it stopped, markers, seek-by-time, VOD playback, and that a gap stalls rather than silently skipping |
| `timebase` | `Manifest::stream_duration_hint()` maps sequence numbers onto clock times for every segment outside the rolling window, so it decides the timeline axis, "behind live" and a recording's total length — and a finished recording's last segment is the partial fragment the broadcast ended on, which must not be taken as typical |
| `storage_health` | the colo and the throughput figure an operator is shown when a campus stutters, kept out of the transport so they can be tested with no network, no bucket and no libcurl |
| `link_health` | the three-state link readout every operator surface shows, with the arithmetic in the core so it needs no network |
| `responsive` | UI queries stay fast while downloading — the property that keeps OBS usable during an event |
| `snapshot` | the figures the dock reads agree with the session they are built from |
| `http_server` | the shared HTTP server spoken to over a real loopback socket: routing, verbs, the static web root, keep-alive, a handler that throws, and the refusal of a path that climbs out of the web root |
| `control_api` | the one list of command names: the exact encoder and decoder surface, no name used twice under the single obs-websocket vendor, every name a well-formed path tail, and the path builder that the HTTP routes and the vendor requests both go through |
| `aes67` | the shapes the AES67 daemon's REST interface is made of: the source body the player sends (every key the daemon's parser insists on, and an eight-channel map that is `0..7` in order), the source list it reads back, and the SDP it publishes — read against a real SDP taken from the daemon's own documentation. Text only, so it needs no daemon, no kernel module and no sound card |
| `s3_list` | a ListObjectsV2 response is read correctly, including pagination and an access-denied body; a signed query string is canonicalised the way S3 does it |
| `event_catalog` | events are classified as live / recording / interrupted, rooms stay separate, a listing failure is not shown as "no recordings", an event that recorded nothing is not offered, and an event with no room-index entry still lists alongside those that have one |
| `storage_manager` | encoder-side storage management: the listing and the per-event size tally are separable, a tally that fails is reported as unknown rather than as `0 B`, cancellation returns early and is not counted as a store failure, and a prefix that will not finish paging gives up with a reason |
| `crypto` | SHA-256 and HMAC-SHA256 match the NIST and RFC 4231 vectors on whichever backend was compiled in — OpenSSL, Windows bcrypt or Apple CommonCrypto. Each CI platform runs its own, so all three are held to the same published answers and a signed request cannot differ by platform |
| `cmaf`, `cmaf_hevc`, `cmaf_av1` | the muxer produces decodable fragments for H.264, HEVC and AV1, with multi-track audio. Each skips cleanly on a machine whose ffmpeg has no encoder for it — except when a *software* encoder is present and the fixture still fails, which is a fault rather than an environment gap and must not be able to pass as a skip |
| `cmaf_decode` | the round trip: what the muxer wrote, the decoder plays back |
| `s3_url` | endpoint and bucket values survive being pasted with schemes, slashes and whitespace |
| `s3_cancel` | `cancel_pending()` actually aborts a stalled request quickly, rather than trusting that wiring curl's progress callback was enough |
| `core_portable` | the core has not acquired an OBS or Qt dependency |

Building with `-DMULTISITE_BUILD_RELAY=ON` adds three more, which the container
image runs as part of the build so a broken relay cannot become an image
somebody deploys on a Sunday morning:

| suite | what it proves |
|---|---|
| `stream_plan` | what may be sent onward and what must be refused — packed multi-channel audio, a sound feed that has vanished, a manifest whose track positions do not line up, an unknown codec; that HEVC and AV1 both go to an RTMP destination over Enhanced RTMP while AV1 over SRT is refused because ffmpeg cannot put it in MPEG-TS; that neither path re-encodes; that a pasted SRT address is pulled apart with the secrets taken out of it; and that no secret survives redaction for the log |
| `relay_state` | the awkward cases without a destination or a wait: a stall ridden out and then given up on, an unexpected exit and its backoff, ending cleanly versus being cut short, an edit that rebuilds a stream without counting as a fault, and an SRT listener with nobody attached waiting indefinitely rather than being treated as broken |
| `config_store` | destinations and storage settings survive a restart, an invalid one is refused before it reaches the database, and an SRT destination's stream id, passphrase and latency round-trip intact |

## Working in the same repository as another session

More than one agent works on this repository at a time, on the same `main`, and
both ways that goes wrong are silent: a push is rejected, or a release is cut
from a tree nobody expected. Neither is hypothetical — on 2026-09-11 a docs
commit was written, committed, and rejected on push because two other commits
had landed on `main` while it was being written.

**Fetch before you start, and fetch again before you push.** The only window in
which a rebase is needed at all is work that is committed but not pushed, so
committing in small pieces and pushing each one closes it. A rejected push is
information, not an obstacle to be worked around:

```sh
git fetch origin
git log --oneline HEAD..origin/main                      # what landed
git log --name-only HEAD..origin/main -- <your files>    # does it touch yours?
git rebase origin/main
```

An empty overlap listing means the other session changed something else and the
rebase will be clean. A non-empty one means read their commit before rebasing:
two sessions describing the same problem is a duplication to merge, not a
conflict to settle by picking a side.

**Never force-push `main`, or rewrite it.** Everything in `BUGS.md` about
tagging assumes a commit only ever gains descendants: a release is cut from a
commit, and its notes have to describe the tree that commit names. Rewriting
`main` detaches a tag from the work it points at, and a tag is public the moment
it lands.

**Four files are edited by nearly every piece of work** — `BUGS.md`,
`README.md`, `PROJECT-SCOPE.md` and `.github/RELEASE-NOTES.md`. Expect a rebase
to reach them, and when it does, prefer appending your section to rewriting
somebody else's paragraph to make room for it. `BUGS.md` entry 0 was shared
state: while `v0.1.12-alpha` was waiting to be tagged it recorded what was on
`main` and what was not yet written up in the release notes, and it went stale
twice in one day — once naming a commit hash that a later amend had changed, once
still saying the tag was pending after it had been cut. Re-read it after
fetching rather than trusting the copy read at the start of a session, and check
whether the commit it describes is still the newest one. The same discipline now
lives in "Recently landed" near the bottom of that file, which is where released
work goes once there is nothing left to act on.

**Nothing here is local to one session.** A tag publishes a Windows plugin
build and a container image to everyone who installs it, and a release deleted
and re-cut is public twice. Tag the current `main`, and check `gh release list`
and `git tag -l` before you do — worth running in every session, not only one
that intends to tag. `BUGS.md` kept that rule in a hold entry while
`v0.1.12-alpha` waited to be tagged; with the release cut, the rule now lives in
"Recently landed" at the bottom of that file.

