# syntax=docker/dockerfile:1

# ---- Build stage --------------------------------------------------------
# Mirrors the CI toolchain (Ubuntu + GCC 14). git/ca-certificates are needed
# because nlohmann/json and cpp-httplib are pulled via CMake FetchContent.
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-14 cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Static libstdc++/libgcc so the runtime image needs only glibc (the binary
# is built with GCC 14, newer than the base image's default runtime).
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DPULSEDB_BUILD_TESTS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build build --parallel \
        --target pulsedb_collector_app pulsedb_sdk_app

# ---- Runtime stage ------------------------------------------------------
FROM ubuntu:24.04 AS runtime

WORKDIR /app
COPY --from=build /src/build/collector/pulsedb_collector /usr/local/bin/pulsedb_collector
COPY --from=build /src/build/sdk/pulsedb_sdk /usr/local/bin/pulsedb_sdk
COPY config/pulsedb.json /app/config/pulsedb.json

# Run unprivileged. /app/data is created and chowned here rather than left to the
# server, because the snapshot path is relative to WORKDIR and a non-root process
# cannot mkdir it under a root-owned /app -- the first save would fail and only
# surface as a periodic warning. Mount a volume over /app/data to persist
# snapshots across container recreation (see docker-compose.yml); the ownership
# set here is what makes that mount writable.
# uid/gid 10001 is deliberately outside the distro's system range (<1000), so it
# cannot collide with a host account when a volume is bind-mounted. That is also
# why --system is not used: it would warn that the id exceeds SYS_UID_MAX.
RUN groupadd --gid 10001 pulsedb \
    && useradd --uid 10001 --gid pulsedb --no-create-home --shell /usr/sbin/nologin pulsedb \
    && mkdir -p /app/data \
    && chown -R pulsedb:pulsedb /app
USER pulsedb

# 8080 = ingestion (POST /v1/events); 8081 = read-only metrics API.
EXPOSE 8080 8081

# Liveness via the collector's own /health route, probed with a bash /dev/tcp
# redirect rather than by installing curl: a runtime image with an HTTP client in
# it hands an attacker a fetch-and-execute primitive, and this needs no package.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080 \
        && printf "GET /health HTTP/1.0\r\n\r\n" >&3 \
        && head -n 1 <&3 | grep -q "200"'

CMD ["pulsedb_collector", "/app/config/pulsedb.json"]
