# Dockerfile — the relay, as one image.
#
# Two stages so the runtime carries a binary, ffmpeg, and nothing else: no
# compiler, no headers, no build cache. The result is small enough to pull
# onto the cheapest VPS over a slow link, which is the deployment this is for.

# ── build ────────────────────────────────────────────────────────────────────
# trixie rather than bookworm, for the runtime stage's ffmpeg: see below.
FROM debian:trixie AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libcurl4-openssl-dev libssl-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY relay ./relay
# The test sources are needed to CONFIGURE, not just to run: the top-level
# build declares the core test targets unconditionally, so cmake fails before
# it reaches the relay if they are missing. They cost a few kilobytes and earn
# their place below by being run.
COPY tests ./tests

# The plugins and the CMAF muxer are not built here: the relay needs the core
# receive path and the S3 transport, and nothing else. That is what keeps the
# build dependencies down to curl, OpenSSL and SQLite.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DMULTISITE_BUILD_RELAY=ON \
        -DBUILD_OBS_PLUGIN=OFF \
        -DBUILD_PLAYER=OFF \
    && cmake --build build --target multisite-relay -j"$(nproc)"

# Run the relay's own tests as part of the image build, so a broken build
# cannot quietly become an image somebody deploys on a Sunday morning. They
# need no network, no bucket and no ffmpeg, and take under a second.
RUN cmake --build build --target test_stream_plan test_relay_state \
        test_config_store -j"$(nproc)" \
    && ctest --test-dir build -R "stream_plan|relay_state|config_store" \
        --output-on-failure

# ── runtime ──────────────────────────────────────────────────────────────────
FROM debian:trixie-slim

# ffmpeg is the relay's one external tool. ca-certificates is what lets it
# reach an HTTPS bucket at all — without it every request fails with a
# certificate error that reads like a credentials problem.
#
# The Debian release is pinned by what ffmpeg does with HEVC, not by taste:
# bookworm ships 5.1, whose FLV muxer has no HEVC in its codec-tag table and
# refuses outright; trixie ships 7.1, which writes the Enhanced RTMP
# extended video tag. Everything else in the relay is a copy remux and does not
# care which of the two it runs on.
#
# libcurl4t64 rather than libcurl4: the 64-bit time_t transition renamed it, and
# trixie has no package by the old name. The SONAME is unchanged, so the binary
# links the same way. Do not "tidy" this back.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ffmpeg ca-certificates libcurl4t64 libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/relay/multisite-relay /app/multisite-relay
COPY relay/web /app/web

# Everything that must survive a restart lives here: the database and the
# segment cache. Mount it, or a container replacement loses the destinations.
VOLUME ["/data"]
# Binds to every interface INSIDE the container, which is not the loosening it
# looks like. A container's 127.0.0.1 is its own loopback, and Docker publishes
# a port by forwarding to the container's eth0 — so binding loopback here makes
# the relay unreachable even when the port is published, which is exactly what
# happened when this was first set to 127.0.0.1.
#
# What actually decides who can reach it is the HOST side of the port mapping.
# docker-compose.yml publishes "127.0.0.1:8080:8080" for that reason: the
# container boundary plus that mapping is the control, not this line.
ENV RELAY_DATA_DIR=/data \
    RELAY_WEB_ROOT=/app/web \
    RELAY_BIND=0.0.0.0 \
    RELAY_PORT=8080
EXPOSE 8080

# No health check on the stream itself on purpose: a relay with nothing to send
# because no service is on air is perfectly healthy, and restarting it would be
# the wrong response.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
    CMD ffmpeg -version > /dev/null 2>&1 || exit 1

ENTRYPOINT ["/app/multisite-relay"]
