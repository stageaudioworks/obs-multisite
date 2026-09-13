cmake_minimum_required(VERSION 3.16)
project(obs_multisite VERSION 0.1.17 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_OBS_PLUGIN "Build the OBS plugin module (needs libobs)" OFF)

# ── macOS build settings ─────────────────────────────────────────────────────
# Apple Silicon only, by decision rather than by omission: an Intel slice would
# double the build and the download for machines this project is unlikely to
# meet. Override CMAKE_OSX_ARCHITECTURES to build otherwise.
#
# The deployment target matches what OBS 30 and later require, so the plugin
# cannot claim to run on a system the host application does not.
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
    endif()
endif()

# ── Reliability core: no OBS, no FFmpeg. Builds everywhere. ───────────────────
# Windows: keep <windows.h> from defining min/max macros that break std::min /
# std::max, and trim the header surface.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(multisite_core STATIC
    src/core/spool_queue.cpp
    src/core/retry_uploader.cpp
    src/core/model.cpp
    src/core/session.cpp
    src/core/segment_cache.cpp
    src/core/decoder_session.cpp
    src/core/aws_sigv4.cpp
    src/core/s3_list_xml.cpp
    src/core/storage_health.cpp
    src/core/disk_health.cpp
    src/core/link_health.cpp
    src/core/event_catalog.cpp
    src/core/storage_manager.cpp
    src/core/storage_providers.cpp
    src/core/lan_object_server.cpp
    # The small HTTP server every consumer of the core shares: the appliance's
    # operator interface, the relay's browser, and the OBS plugin's
    # remote-control pages. Portable rather than POSIX-only, because a plugin
    # has to build on Windows.
    src/core/http_server.cpp)
target_include_directories(multisite_core PUBLIC src src/core)
# Must be position-independent: this static lib gets linked into the shared
# OBS plugin module.
set_target_properties(multisite_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Crypto backend: the platform's own, never a shipped dependency. The signer
# needs SHA-256 and HMAC-SHA256 and nothing else, which every one of these
# provides — so a plugin is never the reason a church has to install a library.
if(WIN32)
    target_sources(multisite_core PRIVATE src/core/crypto_bcrypt.cpp)
    target_link_libraries(multisite_core PUBLIC bcrypt)
elseif(APPLE)
    # CommonCrypto lives in libSystem, which is already linked. Nothing to find
    # and nothing to ship: an OpenSSL build would fail to load on any Mac
    # without Homebrew, which is most of them.
    target_sources(multisite_core PRIVATE src/core/crypto_apple.cpp)
    message(STATUS "crypto backend: CommonCrypto (macOS)")
else()
    find_package(OpenSSL REQUIRED)
    target_sources(multisite_core PRIVATE src/core/crypto_openssl.cpp)
    target_link_libraries(multisite_core PUBLIC OpenSSL::Crypto)
endif()

find_package(Threads REQUIRED)
target_link_libraries(multisite_core PUBLIC Threads::Threads)

# Winsock, for the shared HTTP server. Nothing else in the core opens a socket
# of its own: every request it makes goes out through libcurl.
if(WIN32)
    target_link_libraries(multisite_core PUBLIC ws2_32)
endif()

enable_testing()
add_executable(test_reliability tests/test_reliability.cpp)
target_link_libraries(test_reliability PRIVATE multisite_core)

# RetryUploader::stop() must not hang joining a thread stuck inside a
# transport call — the same shutdown hazard the decoder had (BUGS.md entry 1).
add_executable(test_retry_cancel tests/test_retry_cancel.cpp)
target_link_libraries(test_retry_cancel PRIVATE multisite_core)
add_test(NAME retry_cancel COMMAND test_retry_cancel)

add_executable(test_timebase tests/test_timebase.cpp)
target_link_libraries(test_timebase PRIVATE multisite_core)
add_test(NAME timebase COMMAND test_timebase)

# When a decoded frame is due at the output. Header-only arithmetic, but it
# decides lipsync, and it was wrong in four places at once.
add_executable(test_playout_clock tests/test_playout_clock.cpp)
target_link_libraries(test_playout_clock PRIVATE multisite_core)
add_test(NAME playout_clock COMMAND test_playout_clock)

# Where the dock's playhead reads between state samples. Header-only and free
# of Qt, so the arithmetic that once froze the dock is checked without a
# display — which is how it got shipped broken the first time.
add_executable(test_position_interp tests/test_position_interp.cpp)
target_link_libraries(test_position_interp PRIVATE multisite_core)
add_test(NAME position_interp COMMAND test_position_interp)

# What a decoded frame is allowed to do, and in what order. Every case in it is
# a bug that shipped: seeking broke five times in one afternoon and the suite
# went nowhere near this path.
add_executable(test_playout_timeline tests/test_playout_timeline.cpp)
target_link_libraries(test_playout_timeline PRIVATE multisite_core)
add_test(NAME playout_timeline COMMAND test_playout_timeline)

# Dividing one encoded picture into discrete ones. Every way of getting this
# geometry wrong is quiet — a rectangle a pixel out still shows a picture.
add_executable(test_tile_layout tests/test_tile_layout.cpp)
target_link_libraries(test_tile_layout PRIVATE multisite_core)
add_test(NAME tile_layout COMMAND test_tile_layout)

# The storage protocol's own version. Pins both directions at once: a document
# written before the field existed must still read, and one from a protocol this
# build does not know must be refused rather than half-read.
add_executable(test_protocol_version tests/test_protocol_version.cpp)
target_link_libraries(test_protocol_version PRIVATE multisite_core)
add_test(NAME protocol_version COMMAND test_protocol_version)

# One tile of a composited feed as a view: offsets into the same plane pointers,
# the trick the OBS source uses, spelled out for the appliance's output path.
add_executable(test_tile_crop tests/test_tile_crop.cpp)
target_link_libraries(test_tile_crop PRIVATE multisite_core)
add_test(NAME tile_crop COMMAND test_tile_crop)
add_test(NAME reliability COMMAND test_reliability)

# The core is shared with the planned headless appliance, which links neither
# OBS nor Qt. Enforce that boundary rather than trusting convention.
if(UNIX)
    # Invoked through sh explicitly: the executable bit is not preserved when
    # files are uploaded through GitHub's web interface, and ctest then reports
    # "permission denied" rather than running the check.
    add_test(NAME core_portable
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core_portable.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
endif()

# The signer's entire output derives from these two functions, and each
# platform uses a different implementation of them. A wrong digest fails as
# SignatureDoesNotMatch, which reads as a mistyped secret key — so every
# backend is checked against published vectors rather than against itself.
add_executable(test_crypto tests/test_crypto.cpp)
target_link_libraries(test_crypto PRIVATE multisite_core)
add_test(NAME crypto COMMAND test_crypto)

# The colo and the throughput figure an operator is shown when a campus
# stutters. Kept out of the transport so they can be tested with no network,
# no bucket and no libcurl — the transport itself is not built on a machine
# without curl, which is exactly where these would go untested.
add_executable(test_storage_health tests/test_storage_health.cpp)
target_link_libraries(test_storage_health PRIVATE multisite_core)
add_test(NAME storage_health COMMAND test_storage_health)

# The provider-dropdown derivation both docks use (PROJECT-SCOPE.md §8.6) —
# pure string arithmetic, kept out of the Qt layer so it's tested without it.
add_executable(test_storage_providers tests/test_storage_providers.cpp)
target_link_libraries(test_storage_providers PRIVATE multisite_core)
add_test(NAME storage_providers COMMAND test_storage_providers)

# The three-state link readout every operator surface shows. The arithmetic is
# kept in the core so it can be tested with no network and no libcurl.
add_executable(test_link_health tests/test_link_health.cpp)
target_link_libraries(test_link_health PRIVATE multisite_core)
add_test(NAME link_health COMMAND test_link_health)

# The low-disk readout the encoder dock and the appliance's web UI both show.
# Pure threshold arithmetic, kept out of the statvfs/std::filesystem::space()
# call sites so it can be tested with no filesystem at all.
add_executable(test_disk_health tests/test_disk_health.cpp)
target_link_libraries(test_disk_health PRIVATE multisite_core)
add_test(NAME disk_health COMMAND test_disk_health)

add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE multisite_core)
add_test(NAME session COMMAND test_session)

# Phase 3: the satellite receive path and timeslipping.
add_executable(test_decoder tests/test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE multisite_core)
add_test(NAME decoder COMMAND test_decoder)

# The local DVR store's orphan sweep — a crash mid-event used to leak its
# whole cache directory forever (BUGS.md entry 4).
add_executable(test_segment_cache tests/test_segment_cache.cpp)
target_link_libraries(test_segment_cache PRIVATE multisite_core)
add_test(NAME segment_cache COMMAND test_segment_cache)

# The dock reads a snapshot struct rather than the session directly, so the
# values it is built from must agree with their source. A stray line once reset
# one of them after it had been assigned, and the UI showed two contradictory
# states at once.
add_executable(test_snapshot tests/test_snapshot.cpp)
target_link_libraries(test_snapshot PRIVATE multisite_core)
add_test(NAME snapshot COMMAND test_snapshot)

# Guards the property that matters in live production: UI queries must stay
# fast while the decoder is downloading. Holding the state lock across network
# I/O once made a timeline click appear to lock OBS up entirely.
add_executable(test_responsive tests/test_responsive.cpp)
target_link_libraries(test_responsive PRIVATE multisite_core)
add_test(NAME responsive COMMAND test_responsive)

# Event listing reads a ListObjectsV2 response and signs a query string — the
# two things nothing else in the protocol does. Both are checked against
# captured response shapes rather than a live bucket.
add_executable(test_s3_list tests/test_s3_list.cpp)
target_link_libraries(test_s3_list PRIVATE multisite_core)
add_test(NAME s3_list COMMAND test_s3_list)

# What the event list shows an operator: which event is on air, which are
# finished recordings, and which were cut off by an encoder that died.
add_executable(test_event_catalog tests/test_event_catalog.cpp)
target_link_libraries(test_event_catalog PRIVATE multisite_core)
add_test(NAME event_catalog COMMAND test_event_catalog)

# Encoder-side storage management: listing the room's events with sizes and
# deleting them (one or older-than-N) without touching the live event.
add_executable(test_storage_manager tests/test_storage_manager.cpp)
target_link_libraries(test_storage_manager PRIVATE multisite_core)
add_test(NAME storage_manager COMMAND test_storage_manager)

# The shared HTTP server, spoken to over a real loopback socket: routing,
# verbs, the static web root, keep-alive, and the refusal of a path that climbs
# out of it. Runs on every platform rather than POSIX only, because the OBS
# plugin serves its remote-control pages from this on Windows too.
add_executable(test_http_server tests/test_http_server.cpp)
target_link_libraries(test_http_server PRIVATE multisite_core)
add_test(NAME http_server COMMAND test_http_server)

# The encoder's LAN delivery surface (PROJECT-SCOPE.md §8.7), exercised over
# a real loopback socket the same way test_http_server is.
add_executable(test_lan_object_server tests/test_lan_object_server.cpp)
target_link_libraries(test_lan_object_server PRIVATE multisite_core)
add_test(NAME lan_object_server COMMAND test_lan_object_server)

# The decoder-side "prefer LAN, fall back to cloud" composite (§8.7) and the
# encoder-side "cloud delivery disabled" transport — both plain Transport
# implementations with no network dependency, so they're tested with simple
# in-memory mocks rather than a real socket.
add_executable(test_fallback_transport tests/test_fallback_transport.cpp)
target_link_libraries(test_fallback_transport PRIVATE multisite_core)
add_test(NAME fallback_transport COMMAND test_fallback_transport)

add_executable(test_null_transport tests/test_null_transport.cpp)
target_link_libraries(test_null_transport PRIVATE multisite_core)
add_test(NAME null_transport COMMAND test_null_transport)

# The one list of command names (§8.3). Portable and header-only: the plugin's
# HTTP routes and its obs-websocket vendor requests are both built from it, so
# this is what a rename has to get past before it can orphan somebody's button.
add_executable(test_control_api tests/test_control_api.cpp)
add_test(NAME control_api COMMAND test_control_api)

# ── Optional: S3 transport (needs libcurl) ────────────────────────────────────
find_package(CURL QUIET)
if(CURL_FOUND)
    add_library(multisite_s3 STATIC
        src/core/s3_transport.cpp
        # The decoder's LAN client (§8.7) — grouped with S3Transport rather
        # than multisite_core because it needs libcurl too, and multisite_core
        # has to build with no network dependency at all.
        src/core/lan_transport.cpp)
    target_include_directories(multisite_s3 PUBLIC src src/core)
    target_link_libraries(multisite_s3 PUBLIC multisite_core CURL::libcurl)
    set_target_properties(multisite_s3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    message(STATUS "libcurl found — building S3 transport")

    add_executable(test_lan_transport tests/test_lan_transport.cpp)
    target_link_libraries(test_lan_transport PRIVATE multisite_s3)
    add_test(NAME lan_transport COMMAND test_lan_transport)

    # Endpoint/bucket values are pasted by operators, so the URL builder has to
    # tolerate schemes, trailing slashes and whitespace.
    add_executable(test_s3_url tests/test_s3_url.cpp)
    target_link_libraries(test_s3_url PRIVATE multisite_s3)
    add_test(NAME s3_url COMMAND test_s3_url)

    # Proves cancel_pending() actually aborts a stalled request quickly rather
    # than trusting that wiring curl's progress callback was enough. POSIX
    # sockets only — stands up a bare TCP listener to simulate a stalled link,
    # which needs WinSock2 init and different socket headers on Windows that
    # nothing else in this project uses; not worth adding for one test.
    if(NOT WIN32)
        add_executable(test_s3_cancel tests/test_s3_cancel.cpp)
        target_link_libraries(test_s3_cancel PRIVATE multisite_s3)
        add_test(NAME s3_cancel COMMAND test_s3_cancel)
    endif()
else()
    message(STATUS "libcurl not found — skipping S3 transport")
endif()

# ── Optional: CMAF muxer + tests (needs FFmpeg) ───────────────────────────────
# FFmpeg discovery has two paths:
#   1. pkg-config (Linux/macOS, and anywhere .pc files exist)
#   2. a direct search of CMAKE_PREFIX_PATH (Windows obs-deps ships headers and
#      import libs with no pkg-config metadata)
option(FORCE_FFMPEG_MANUAL_SEARCH "Skip pkg-config and use the manual search" OFF)

set(FFMPEG_FOUND FALSE)

if(NOT FORCE_FFMPEG_MANUAL_SEARCH)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET
            libavformat libavcodec libavutil libswresample libswscale)
    endif()
endif()

if(FFMPEG_FOUND AND TARGET PkgConfig::FFMPEG)
    add_library(ffmpeg_iface INTERFACE)
    target_link_libraries(ffmpeg_iface INTERFACE PkgConfig::FFMPEG)
    message(STATUS "FFmpeg found via pkg-config (${FFMPEG_libavformat_VERSION})")
else()
    # Manual search: works against obs-deps on Windows.
    find_path(FFMPEG_INCLUDE_DIR
        NAMES libavformat/avformat.h
        PATH_SUFFIXES include)
    set(_ffmpeg_libs "")
    set(_ffmpeg_missing "")
    foreach(_lib avformat avcodec avutil swresample swscale)
        find_library(FFMPEG_${_lib}_LIBRARY
            NAMES ${_lib} lib${_lib}
            PATH_SUFFIXES lib bin)
        if(FFMPEG_${_lib}_LIBRARY)
            list(APPEND _ffmpeg_libs "${FFMPEG_${_lib}_LIBRARY}")
        else()
            list(APPEND _ffmpeg_missing ${_lib})
        endif()
    endforeach()

    if(FFMPEG_INCLUDE_DIR AND NOT _ffmpeg_missing)
        set(FFMPEG_FOUND TRUE)
        add_library(ffmpeg_iface INTERFACE)
        target_include_directories(ffmpeg_iface INTERFACE "${FFMPEG_INCLUDE_DIR}")
        target_link_libraries(ffmpeg_iface INTERFACE ${_ffmpeg_libs})
        message(STATUS "FFmpeg found by manual search: ${FFMPEG_INCLUDE_DIR}")
    else()
        message(STATUS "FFmpeg not found "
                       "(include: ${FFMPEG_INCLUDE_DIR}, missing libs: ${_ffmpeg_missing})")
    endif()
endif()

if(FFMPEG_FOUND)
    add_library(multisite_cmaf STATIC
        src/core/cmaf_muxer.cpp
        src/core/cmaf_decoder.cpp)
    target_include_directories(multisite_cmaf PUBLIC src src/core)
    target_link_libraries(multisite_cmaf PUBLIC ffmpeg_iface)
    set_target_properties(multisite_cmaf PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(test_cmaf tests/test_cmaf.cpp)
    target_link_libraries(test_cmaf PRIVATE multisite_cmaf)
    add_test(NAME cmaf
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXE=$<TARGET_FILE:test_cmaf>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cmaf_test.cmake)

    # HEVC through the same muxer. The pipeline is codec-agnostic by design, so
    # the guarantee is worth asserting rather than assuming — especially with
    # the Raspberry Pi decoder tier depending on HEVC.
    add_test(NAME cmaf_hevc
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXE=$<TARGET_FILE:test_cmaf>
                     -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_hevc_test.cmake)

    # Round trip: decode the output the muxer test just produced. Generating
    # the fixture at test time keeps large media out of the repository while
    # still exercising encode -> decode on real bytes.
    add_executable(test_cmaf_decode tests/test_cmaf_decode.cpp)
    target_link_libraries(test_cmaf_decode PRIVATE multisite_cmaf)
    add_test(NAME cmaf_decode
             COMMAND $<TARGET_FILE:test_cmaf_decode>
                     "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out/init.mp4"
                     "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out/seg_000.m4s")
    # The muxer test writes that fixture, so it has to run first.
    set_tests_properties(cmaf_decode PROPERTIES DEPENDS cmaf)
else()
    message(STATUS "skipping CMAF muxer targets")
endif()

# The rule that decides who owns the appliance's screen on a given tick: a
# picture being held outranks the idle screen. Deliberately portable and outside
# the appliance's feature guard, because the combination that failed — Hold
# pressed on a box left on its default identity screen, so the held frame was
# replaced by the box's own address — needed a live event, a running decoder and
# an HDMI socket to reproduce by hand. Here it needs none of them.
add_executable(test_idle_screen tests/test_idle_screen.cpp src/appliance/screen.cpp)
target_include_directories(test_idle_screen PRIVATE src/appliance)
add_test(NAME idle_screen COMMAND test_idle_screen)

# The decoder's floats to whatever the sound card agreed to take. A card that
# advertises a single 32-bit integer format and nothing else made the player's
# float-only open fail with "will not take floating-point audio", so the install
# went silent instead of converting. The arithmetic is the part that can be
# quietly wrong — a clamp that wraps, a scale off by a step, a channel in the
# wrong slot — and it lives in a header with no ALSA in it, so this runs
# anywhere, including the laptop the fix was written on, where alsa_output.cpp
# is not compiled at all.
add_executable(test_pcm_convert tests/test_pcm_convert.cpp)
target_include_directories(test_pcm_convert PRIVATE src/appliance)
add_test(NAME pcm_convert COMMAND test_pcm_convert)

# The sound meter, tapped where the samples are handed to the card rather than
# where they arrive from the network — because on this box "is there audio?" is
# a property of what the card was given, and a muted box, a card that would not
# open and a genuinely silent event have to be told apart. Same reasoning as
# pcm_convert above for why the arithmetic lives in a header with no ALSA in it:
# a peak that misses the negative half of a waveform, a channel one slot over, a
# NaN read as full scale and a window that never closes are all faults that need
# no sound card to find.
add_executable(test_audio_levels tests/test_audio_levels.cpp)
target_include_directories(test_audio_levels PRIVATE src/appliance)
add_test(NAME audio_levels COMMAND test_audio_levels)

# How wide the sound card is opened. One function, and it is a test of its own
# because getting it wrong is invisible: the card opens, it reports success, the
# stream is published, and the sound is simply in the wrong places. A box opened
# its card with the two-channel fallback before any feed had arrived, and "follow
# the feed" then latched that two-channel stream against an eight-channel feed.
# No ALSA, no config file, no clock, so this runs on a laptop with no card in it.
add_executable(test_audio_plan tests/test_audio_plan.cpp)
target_include_directories(test_audio_plan PRIVATE src/appliance)
add_test(NAME audio_plan COMMAND test_audio_plan)

# How much silence the keep-alive thread keeps in the card, and how big the
# buffer it writes from has to be. Two numbers, computed in two places once, and
# on a card with a one millisecond period they came apart — so the top-up read
# past the end of its own buffer and fed the card uninitialised heap. Read as
# float that is not quiet: it is full-scale digital noise at 0 dBFS, on the
# AES67 stream, for as long as the box sits idle. The assertion that matters is
# not that a number is right but that the buffer is never shorter than the write
# made from it, for any geometry a driver might grant, which is why the card
# that found the fault is one of the rows in the table.
add_executable(test_idle_keepalive tests/test_idle_keepalive.cpp)
target_include_directories(test_idle_keepalive PRIVATE src/appliance)
add_test(NAME idle_keepalive COMMAND test_idle_keepalive)

# The AES67 daemon's REST interface, as shapes: the source body the player
# sends, the JSON it reads back, and the SDP the daemon publishes. All text, all
# easy to get quietly wrong — a channel map naming the wrong eight channels is
# heard as the wrong eight channels — and none of it needs the daemon, the
# kernel module or a card to check. aes67.cpp, which does talk to the daemon, is
# deliberately not linked here.
add_executable(test_aes67 tests/test_aes67.cpp)
target_include_directories(test_aes67 PRIVATE src/appliance)
add_test(NAME aes67 COMMAND test_aes67)

# ── Optional: the satellite appliance (headless player) ───────────────────────
# The Raspberry Pi / mini-PC receive box: the same core, with its own video and
# audio output and a browser-based operator interface instead of OBS.
#
# It builds anywhere the core does. On a machine with no KMS and no ALSA — a
# developer's laptop — the outputs fall back to null implementations, which is
# what lets the engine and the whole web interface be exercised without the
# hardware in front of you.
option(BUILD_PLAYER "Build the satellite appliance (headless player)" ON)

if(BUILD_PLAYER AND FFMPEG_FOUND AND CURL_FOUND AND NOT WIN32)
    # The one version there is. This was a separate string defaulting to
    # "0.1.0" that nothing ever set — not the installer, not CI — so a box
    # reported 0.1.0 for every release after the first, in the log and in the
    # System panel. A version an operator quotes in a bug report has to be the
    # real one, so it comes from the project.
    set(MULTISITE_PLAYER_VERSION "${PROJECT_VERSION}"
        CACHE STRING "Player version string" FORCE)
    # Where the operator interface is installed. Overridable at runtime with
    # --web-root so the page can be edited on a running box.
    set(MULTISITE_PLAYER_WEB_ROOT "${CMAKE_INSTALL_PREFIX}/share/multisite-player/web"
        CACHE STRING "Installed location of the operator interface")

    add_executable(multisite-player
        src/appliance/main.cpp
        src/appliance/config.cpp
        src/appliance/log.cpp
        src/appliance/player.cpp
        src/appliance/screen.cpp
        src/appliance/api.cpp
        src/appliance/sysinfo.cpp
        # The AES67 daemon on this box: configuring it, watching it and
        # switching it. Its shapes live in aes67.h and are tested separately —
        # this is only the part that talks to the daemon.
        src/appliance/aes67.cpp
        src/appliance/preview.cpp
        src/appliance/splash.cpp
        src/appliance/outputs.cpp)
    # The HTTP server arrives through multisite_core: it is shared with the
    # relay and the OBS plugin's remote-control pages, so there is one copy.

    target_link_libraries(multisite-player PRIVATE
        multisite_core multisite_s3 multisite_cmaf)
    target_compile_definitions(multisite-player PRIVATE
        MULTISITE_PLAYER_VERSION="${MULTISITE_PLAYER_VERSION}"
        MULTISITE_PLAYER_WEB_ROOT="${MULTISITE_PLAYER_WEB_ROOT}")

    # The preview re-encodes the same decoded frame to JPEG on every request,
    # so the encoder must be reusable across calls — otherwise the first frame
    # is the only one that ever arrives.
    add_executable(test_preview tests/test_preview.cpp src/appliance/preview.cpp)
    target_include_directories(test_preview PRIVATE src/appliance src/core)
    target_link_libraries(test_preview PRIVATE ffmpeg_iface)
    add_test(NAME preview COMMAND test_preview)

    # DRM/KMS: the appliance drives the HDMI output itself, with no desktop.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
        pkg_check_modules(ALSA IMPORTED_TARGET alsa)
    endif()

    if(TARGET PkgConfig::LIBDRM)
        target_sources(multisite-player PRIVATE src/appliance/drm_output.cpp)
        target_link_libraries(multisite-player PRIVATE PkgConfig::LIBDRM)
        target_compile_definitions(multisite-player PRIVATE MULTISITE_HAVE_DRM=1)
        message(STATUS "player: DRM/KMS display output enabled")
    else()
        message(STATUS "player: no libdrm — building without display output")
    endif()

    if(TARGET PkgConfig::ALSA)
        target_sources(multisite-player PRIVATE src/appliance/alsa_output.cpp)
        target_link_libraries(multisite-player PRIVATE PkgConfig::ALSA)
        target_compile_definitions(multisite-player PRIVATE MULTISITE_HAVE_ALSA=1)
        message(STATUS "player: ALSA audio output enabled")
    else()
        message(STATUS "player: no ALSA — building without audio output")
    endif()

    # QR code on the identity screen. Found, not required: a development build
    # without the library still compiles, and the splash simply has no code to
    # scan. The installer and CI install it, which is where a Pi's build gets
    # it — the same policy as libdrm and ALSA above.
    if(PkgConfig_FOUND)
        pkg_check_modules(QRENCODE IMPORTED_TARGET libqrencode)
    endif()
    if(TARGET PkgConfig::QRENCODE)
        target_link_libraries(multisite-player PRIVATE PkgConfig::QRENCODE)
        target_compile_definitions(multisite-player PRIVATE
            MULTISITE_HAVE_QRCODE=1)
        message(STATUS "player: QR code on the identity screen (libqrencode)")
    else()
        message(STATUS "player: no libqrencode — identity screen without a QR code")
    endif()

    # Smooth text on the identity screen, when freetype is present. Optional,
    # exactly like the QR code: a development build without it still compiles
    # (the splash falls back to its built-in bitmap font), and the installer
    # and CI install it so a Pi gets the nicer rendering.
    if(PkgConfig_FOUND)
        pkg_check_modules(FREETYPE IMPORTED_TARGET freetype2)
    endif()
    if(TARGET PkgConfig::FREETYPE)
        target_link_libraries(multisite-player PRIVATE PkgConfig::FREETYPE)
        target_compile_definitions(multisite-player PRIVATE
            MULTISITE_HAVE_FREETYPE=1)
        message(STATUS "player: anti-aliased splash text (freetype)")
    else()
        message(STATUS "player: no freetype — splash uses the built-in bitmap font")
    endif()

    install(TARGETS multisite-player RUNTIME DESTINATION bin)
    install(DIRECTORY src/appliance/web/
            DESTINATION share/multisite-player/web)
elseif(BUILD_PLAYER)
    message(STATUS "skipping the appliance "
                   "(needs FFmpeg and libcurl on a POSIX host)")
endif()

# ── Optional: the OBS plugin module ───────────────────────────────────────────
# Enable with -DBUILD_OBS_PLUGIN=ON. libobs is discovered either via its CMake
# config (obs-deps / an OBS build tree) or via pkg-config (distro libobs-dev).
if(BUILD_OBS_PLUGIN)
    if(NOT FFMPEG_FOUND OR NOT CURL_FOUND)
        message(FATAL_ERROR
            "OBS plugin needs both FFmpeg and libcurl.\n"
            "  FFmpeg found: ${FFMPEG_FOUND}\n"
            "  libcurl found: ${CURL_FOUND}\n"
            "Point CMAKE_PREFIX_PATH at obs-deps (which ships both), e.g.\n"
            "  cmake -S . -B build -DBUILD_OBS_PLUGIN=ON "
            "-DCMAKE_PREFIX_PATH=<obs-deps>")
    endif()

    # libobs discovery. Three paths, in order:
    #   1. explicit -DLIBOBS_INCLUDE_DIR / -DLIBOBS_LIBRARY (most deterministic;
    #      what CI uses on Windows against an OBS build tree)
    #   2. its CMake config package (obs-deps / installed OBS)
    #   3. pkg-config (distro libobs-dev)
    #
    # Path 2 is skipped on Windows: libobsConfig.cmake from a build tree calls
    # find_dependency(w32-pthreads), which isn't exported with it and hard-fails.
    option(LIBOBS_MANUAL_SEARCH "Bypass libobs's CMake config package" OFF)
    if(WIN32)
        set(LIBOBS_MANUAL_SEARCH ON)
    endif()

    if(LIBOBS_INCLUDE_DIR OR LIBOBS_LIBRARY)
        # On macOS the library is optional, because the plugin is linked with
        # -undefined dynamic_lookup: libobs is resolved out of the running
        # OBS.app at load time, so the build needs libobs's HEADERS and
        # nothing else. That is not a shortcut — carrying a link-time copy of
        # libobs would only invite a version other than the host's. It also
        # means a macOS build needs no OBS binary at all, just the source
        # tarball the headers come from.
        if(NOT LIBOBS_INCLUDE_DIR OR (NOT LIBOBS_LIBRARY AND NOT APPLE))
            message(FATAL_ERROR
                "Give both -DLIBOBS_INCLUDE_DIR and -DLIBOBS_LIBRARY, or neither.\n"
                "  LIBOBS_INCLUDE_DIR: ${LIBOBS_INCLUDE_DIR}\n"
                "  LIBOBS_LIBRARY: ${LIBOBS_LIBRARY}")
        endif()
        if(NOT EXISTS "${LIBOBS_INCLUDE_DIR}/obs-module.h")
            message(FATAL_ERROR
                "obs-module.h not found in LIBOBS_INCLUDE_DIR "
                "(${LIBOBS_INCLUDE_DIR}) — point it at obs-studio's libobs/ dir")
        endif()
        # obsconfig.h is GENERATED into the OBS build tree, so when building
        # against a build tree we need that directory on the include path too.
        set(_obs_includes "${LIBOBS_INCLUDE_DIR}")
        if(LIBOBS_CONFIG_INCLUDE_DIR)
            list(APPEND _obs_includes "${LIBOBS_CONFIG_INCLUDE_DIR}")
            message(STATUS "libobs generated headers: ${LIBOBS_CONFIG_INCLUDE_DIR}")
        endif()
        if(LIBOBS_LIBRARY)
            add_library(OBS::libobs UNKNOWN IMPORTED)
            set_target_properties(OBS::libobs PROPERTIES
                IMPORTED_LOCATION "${LIBOBS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${_obs_includes}")
            message(STATUS "libobs used explicitly: ${LIBOBS_LIBRARY}")
        else()
            # Headers only (macOS). An INTERFACE target carries the include
            # directories with nothing to link, so the rest of this file does
            # not have to care which of the two cases it is in.
            add_library(OBS::libobs INTERFACE IMPORTED)
            set_target_properties(OBS::libobs PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_obs_includes}")
            message(STATUS "libobs headers only: ${LIBOBS_INCLUDE_DIR} "
                           "(resolved from the host app at load time)")
        endif()
    else()
        if(NOT LIBOBS_MANUAL_SEARCH)
            find_package(libobs CONFIG QUIET)
            if(TARGET OBS::libobs)
                message(STATUS "libobs found via CMake config")
            endif()
        endif()
        if(NOT TARGET OBS::libobs)
            if(PkgConfig_FOUND)
                pkg_check_modules(LIBOBS IMPORTED_TARGET libobs)
            endif()
            if(TARGET PkgConfig::LIBOBS)
                add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
                message(STATUS "libobs found via pkg-config (${LIBOBS_VERSION})")
            else()
                message(FATAL_ERROR
                    "libobs not found. Either install libobs-dev, or pass\n"
                    "  -DLIBOBS_INCLUDE_DIR=<obs-studio>/libobs\n"
                    "  -DLIBOBS_LIBRARY=<path to obs.lib or libobs.so>")
            endif()
        endif()
    endif()

    # The Tools-menu items need obs-frontend-api, which OBS only builds as part
    # of its UI (and which pulls in Qt). Hotkeys live in libobs itself, so the
    # plugin is fully usable without this; enable it if you have the library.
    option(ENABLE_FRONTEND_API "Build Tools-menu items (needs obs-frontend-api)" OFF)
    # The operator docks need Qt6 and obs-frontend-api. OBS only builds
    # obs-frontend-api as part of its Qt UI, so a CI build must configure OBS
    # with ENABLE_UI=ON (building just the libobs and obs-frontend-api targets
    # is enough — the whole OBS app is not needed).
    option(ENABLE_QT "Build the operator docks (needs Qt6 + obs-frontend-api)" OFF)

    add_library(obs-multisite MODULE
        src/obs/plugin-main.cpp
        src/obs/multisite_output.cpp
        src/obs/multisite_source.cpp
        src/obs/multisite_ui.cpp
        src/obs/broadcast_controller.cpp
        src/obs/decoder_settings.cpp
        src/obs/plugin_role.cpp
        src/obs/plugin_log.cpp
        # The remote-control pages, served on the church network. No Qt (and so
        # no docks needed): a build without them still gets remote control,
        # because the machine that most needs it is the one nobody is at.
        src/obs/web/web_ui.cpp
        src/obs/web/commands.cpp
        src/obs/web/encoder_api.cpp
        src/obs/web/decoder_api.cpp
        src/obs/web/ui_thread.cpp
        src/obs/web/net_address.cpp
        # The obs-websocket vendor API: the same commands, reachable from a
        # Stream Deck or any automation. Header-only and adaptor-shaped, so it
        # adds no link dependency and no new control logic; absent
        # obs-websocket turns it off with one log line and nothing else.
        src/obs/websocket/vendor_api.cpp)
    target_include_directories(obs-multisite PRIVATE src src/core)
    target_compile_definitions(obs-multisite PRIVATE
        PLUGIN_VERSION="${PROJECT_VERSION}")

    if(ENABLE_QT)
        set(CMAKE_AUTOMOC ON)
        find_package(Qt6 REQUIRED COMPONENTS Widgets)
        if(LIBOBS_FRONTEND_LIBRARY)
            set(_qt_frontend "${LIBOBS_FRONTEND_LIBRARY}")
        elseif(APPLE)
            # As with libobs: dynamic_lookup resolves the frontend API from
            # OBS.app, so only the header is needed. Qt itself IS linked for
            # real — its symbols are not OBS's to provide.
            set(_qt_frontend "")
        else()
            find_library(_qt_frontend NAMES obs-frontend-api REQUIRED)
        endif()
        # obs-frontend-api.h is NOT beside the libobs headers: it lives in
        # OBS's own source tree (frontend/api in OBS 32, UI/obs-frontend-api in
        # older releases). Passing only the .lib is not enough.
        if(LIBOBS_FRONTEND_INCLUDE_DIR)
            set(_fe_inc "${LIBOBS_FRONTEND_INCLUDE_DIR}")
        else()
            find_path(_fe_inc obs-frontend-api.h
                      HINTS "${LIBOBS_INCLUDE_DIR}" "${LIBOBS_INCLUDE_DIR}/.."
                      PATH_SUFFIXES obs frontend/api UI/obs-frontend-api)
        endif()
        if(NOT _fe_inc)
            message(FATAL_ERROR
                "ENABLE_QT=ON but obs-frontend-api.h was not found. Pass "
                "-DLIBOBS_FRONTEND_INCLUDE_DIR=<obs-studio>/frontend/api")
        endif()
        message(STATUS "obs-frontend-api headers: ${_fe_inc}")
        target_include_directories(obs-multisite PRIVATE "${_fe_inc}")

        target_sources(obs-multisite PRIVATE
            src/obs/ui/encoder_dock.cpp
            src/obs/ui/decoder_dock.cpp
            src/obs/ui/storage_dialog.cpp
            src/obs/ui/role_selector.cpp
            src/obs/ui/settings_tabs.cpp
            src/obs/ui/web_box.cpp
            src/obs/ui/dock_register.cpp)
        set_target_properties(obs-multisite PROPERTIES AUTOMOC ON)
        target_link_libraries(obs-multisite PRIVATE Qt6::Widgets)
        if(_qt_frontend)
            target_link_libraries(obs-multisite PRIVATE "${_qt_frontend}")
        endif()
        target_compile_definitions(obs-multisite PRIVATE
            MULTISITE_HAVE_QT MULTISITE_HAVE_FRONTEND_API)
        if(_qt_frontend)
            message(STATUS "Qt docks enabled (Qt6 ${Qt6_VERSION}, frontend ${_qt_frontend})")
        else()
            message(STATUS "Qt docks enabled (Qt6 ${Qt6_VERSION}, "
                           "frontend resolved from the host app)")
        endif()
    endif()

    if(ENABLE_FRONTEND_API AND NOT ENABLE_QT)
        if(LIBOBS_FRONTEND_LIBRARY)
            set(_frontend_lib "${LIBOBS_FRONTEND_LIBRARY}")
        else()
            find_library(_frontend_lib NAMES obs-frontend-api)
        endif()
        if(_frontend_lib)
            target_link_libraries(obs-multisite PRIVATE "${_frontend_lib}")
            target_compile_definitions(obs-multisite PRIVATE
                MULTISITE_HAVE_FRONTEND_API)
            message(STATUS "obs-frontend-api: ${_frontend_lib} (Tools menu on)")
        else()
            message(WARNING
                "ENABLE_FRONTEND_API=ON but obs-frontend-api was not found; "
                "building without Tools-menu items (hotkeys still work)")
        endif()
    else()
        message(STATUS "Tools menu disabled (hotkeys only) — "
                       "set -DENABLE_FRONTEND_API=ON to include it")
    endif()
    target_link_libraries(obs-multisite PRIVATE
        OBS::libobs multisite_core multisite_s3 multisite_cmaf)
    # FFmpeg comes through multisite_cmaf's INTERFACE link (ffmpeg_iface).
    set_target_properties(obs-multisite PROPERTIES PREFIX "")

    if(APPLE)
        # macOS OBS loads plugins as .plugin BUNDLES, not bare shared objects:
        # a loose .so in the plugins directory is ignored silently. The bundle
        # carries the binary in Contents/MacOS and the locale files in
        # Contents/Resources, which is where obs_module_text looks.
        set_target_properties(obs-multisite PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "plugin"
            OUTPUT_NAME "obs-multisite"
            MACOSX_BUNDLE_INFO_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in")

        # libobs is linked from OBS.app at runtime, so the plugin must not
        # carry a copy and must resolve it through the host application.
        target_link_options(obs-multisite PRIVATE
            -Wl,-undefined,dynamic_lookup)

        # Every versioned library the plugin needs — libobs, obs-frontend-api,
        # Qt and FFmpeg — is one OBS already ships, and each is referenced as
        # @rpath/... So the only question is what @rpath resolves to, and the
        # answer must not be a path from the machine that built it.
        #
        # @executable_path is the HOST application, because a plugin has no
        # executable of its own: for a plugin loaded by OBS this is
        # OBS.app/Contents/MacOS, so ../Frameworks is OBS's own framework
        # directory wherever the user installed it. This is what OBS's
        # bundled plugins use.
        #
        # BUILD_WITH_INSTALL_RPATH stops CMake adding the build-time
        # dependency directories as well. Without it the obs-deps path from
        # the build machine is baked in FIRST, so the plugin would load
        # whatever it found there ahead of OBS's copies — working perfectly on
        # the machine that built it and nowhere else.
        set_target_properties(obs-multisite PROPERTIES
            MACOSX_RPATH TRUE
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/../Frameworks")

        # The data directory becomes Contents/Resources inside the bundle.
        file(GLOB_RECURSE _ms_data RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/data"
             "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
        foreach(_f ${_ms_data})
            get_filename_component(_d "${_f}" DIRECTORY)
            target_sources(obs-multisite PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/data/${_f}")
            set_source_files_properties(
                "${CMAKE_CURRENT_SOURCE_DIR}/data/${_f}" PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources/${_d}"
                HEADER_FILE_ONLY TRUE)
        endforeach()

        # LIBRARY, not BUNDLE: the BUNDLE keyword is for MACOSX_BUNDLE
        # executables. A MODULE library that is a CFBundle installs under
        # LIBRARY, and CMake fails at configure time if only BUNDLE is given.
        install(TARGETS obs-multisite
            LIBRARY DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
    elseif(WIN32)
        # A MODULE library installs under LIBRARY on every platform, including
        # Windows — RUNTIME applies to executables and DLLs from SHARED targets.
        install(TARGETS obs-multisite
            LIBRARY DESTINATION "obs-plugins/64bit"
            RUNTIME DESTINATION "obs-plugins/64bit")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
            DESTINATION "data/obs-plugins/obs-multisite")
    else()
        install(TARGETS obs-multisite
            LIBRARY DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/obs-multisite/bin/64bit")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
            DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/obs-multisite/data")
    endif()
    message(STATUS "Building OBS plugin module")
endif()

# ── Simulcast relay (separate sub-project) ───────────────────────────────────
# Off by default: it is a container-side service, not part of the plugins, and
# a plugin build should not be made to find sqlite3 for it. Its dependency
# direction is one-way — the relay uses the core, the core knows nothing of it.
option(MULTISITE_BUILD_RELAY "Build the multi-destination simulcast relay" OFF)
if(MULTISITE_BUILD_RELAY)
    add_subdirectory(relay)
endif()
