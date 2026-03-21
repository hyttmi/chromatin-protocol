# syntax=docker/dockerfile:1

# ---- Build stage ----
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY db/ db/
COPY loadgen/ loadgen/
COPY tools/ tools/

RUN --mount=type=cache,target=/src/build/_deps \
    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DFETCHCONTENT_QUIET=ON \
      -DCMAKE_CXX_FLAGS="-Wno-restrict" \
    && cmake --build build --target chromatindb chromatindb_loadgen chromatindb_verify \
    && strip build/chromatindb build/chromatindb_loadgen build/chromatindb_verify

# ---- Runtime stage ----
FROM debian:bookworm-slim

RUN groupadd -r chromatindb && useradd -r -g chromatindb chromatindb

COPY --from=builder /src/build/chromatindb /usr/local/bin/
COPY --from=builder /src/build/chromatindb_loadgen /usr/local/bin/
COPY --from=builder /src/build/chromatindb_verify /usr/local/bin/

RUN mkdir -p /data && chown chromatindb:chromatindb /data
VOLUME /data

USER chromatindb
EXPOSE 4200

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-' || exit 1

ENTRYPOINT ["chromatindb"]
CMD ["run", "--data-dir", "/data", "--log-level", "debug"]
