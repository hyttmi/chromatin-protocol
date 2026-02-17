# Stage 1: Build
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ git ca-certificates make python3 libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel "$(nproc)" --target helix-node helix-integration-test

# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/src/helix-node /usr/local/bin/helix-node
COPY --from=builder /build/build/tests/integration/helix-integration-test /usr/local/bin/helix-integration-test

RUN mkdir -p /data

EXPOSE 4000/udp 4001

ENTRYPOINT ["helix-node"]
CMD ["--config", "/etc/helix/config.json"]
