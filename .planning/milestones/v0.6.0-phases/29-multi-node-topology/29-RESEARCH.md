# Phase 29: Multi-Node Topology - Research

**Researched:** 2026-03-16
**Domain:** Docker Compose multi-node networking, chromatindb peer connectivity
**Confidence:** HIGH

## Summary

Phase 29 creates a Docker Compose file that runs 3-5 chromatindb nodes in a chain topology with correct connectivity, health checks, named volumes, and verified replication. This is pure infrastructure configuration -- no C++ code changes are needed. The existing Dockerfile (Phase 27) already includes a working healthcheck, the binary supports JSON config files with `bootstrap_peers`, and `trusted_peers` enables lightweight handshake between known Docker network IPs.

The main deliverable is a `docker-compose.yml` with per-node JSON config files that wire up a chain topology (node1 <- node2 <- node3, etc.) where each node bootstraps to its predecessor. Named volumes provide persistent libmdbx storage. The `depends_on: condition: service_healthy` pattern ensures nodes start in order so bootstrap targets are listening before dependents connect.

**Primary recommendation:** Create a `docker-compose.yml` with 3 nodes in a chain topology (node1 standalone, node2 bootstraps to node1, node3 bootstraps to node2), per-node config files in a `deploy/configs/` directory, named volumes for `/data`, and `depends_on` with `condition: service_healthy` for ordered startup. Include a late-joiner node (node4) as a separate profile or commented service.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DOCK-02 | docker-compose topology runs 3-5 nodes in chain connectivity with health checks, named volumes, and per-node configs | Docker Compose service definitions with depends_on/service_healthy, named volumes, per-node JSON config files with bootstrap_peers pointing to predecessor in chain |
</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| Docker Compose v2 | Latest (bundled with Docker) | Multi-container orchestration | Native `docker compose` CLI, no separate install |
| docker-compose.yml | Compose Specification | Service definitions | Industry standard for multi-container development |

### Supporting
| Tool | Version | Purpose | When to Use |
|------|---------|---------|-------------|
| chromatindb JSON config | Existing | Per-node configuration | Each node gets its own config with unique bootstrap_peers |
| Docker named volumes | Native | Persistent libmdbx storage | Every node needs persistent /data across container restarts |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Chain topology | Full mesh (every node bootstraps to every other) | Chain tests multi-hop propagation (PERF-03 in Phase 30); mesh would hide hop latency |
| Per-node config files | Environment variable overrides | chromatindb uses JSON config files; env vars not supported by the binary |
| depends_on service_healthy | Manual startup with sleep | Fragile, non-deterministic; service_healthy is the correct pattern |

## Architecture Patterns

### Recommended Project Structure
```
deploy/
  docker-compose.yml       # 3-node chain topology
  configs/
    node1.json             # No bootstrap_peers (seed node)
    node2.json             # bootstrap_peers: ["node1:4200"]
    node3.json             # bootstrap_peers: ["node2:4200"]
    node4-latejoin.json    # bootstrap_peers: ["node3:4200"] (late-joiner)
```

### Pattern 1: Chain Topology with Ordered Startup
**What:** Nodes form a linear chain where each node bootstraps only to its predecessor. Node1 is the seed with no bootstrap peers. Node2 points to node1, node3 points to node2.
**When to use:** Always for this phase -- chain topology is required for multi-hop propagation measurement in Phase 30 (PERF-03).
**Why chain, not mesh:** A blob written to node1 must traverse node1->node2->node3 to reach all nodes. This validates the sync protocol works across multiple hops. PEX (peer exchange) will eventually create direct connections, but the initial topology tests the bootstrap chain path.

```yaml
services:
  node1:
    image: chromatindb:latest
    command: ["run", "--config", "/config/node1.json", "--data-dir", "/data"]
    volumes:
      - node1-data:/data
      - ./configs/node1.json:/config/node1.json:ro
    networks:
      - chromatindb-net
    healthcheck:
      test: ["CMD-SHELL", "exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-"]
      interval: 5s
      timeout: 3s
      start_period: 10s
      retries: 3

  node2:
    image: chromatindb:latest
    command: ["run", "--config", "/config/node2.json", "--data-dir", "/data"]
    volumes:
      - node2-data:/data
      - ./configs/node2.json:/config/node2.json:ro
    networks:
      - chromatindb-net
    depends_on:
      node1:
        condition: service_healthy

  node3:
    image: chromatindb:latest
    command: ["run", "--config", "/config/node3.json", "--data-dir", "/data"]
    volumes:
      - node3-data:/data
      - ./configs/node3.json:/config/node3.json:ro
    networks:
      - chromatindb-net
    depends_on:
      node2:
        condition: service_healthy

volumes:
  node1-data:
  node2-data:
  node3-data:

networks:
  chromatindb-net:
    driver: bridge
```

### Pattern 2: Per-Node Config Files
**What:** Each node gets its own JSON config file mounted read-only into the container. The config specifies bootstrap_peers using Docker Compose DNS names (service names resolve to container IPs on the shared network).
**When to use:** Every node needs a config file.

Node1 config (seed, no bootstrap):
```json
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

Node2 config (bootstraps to node1):
```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node1:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

Node3 config (bootstraps to node2):
```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node2:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

### Pattern 3: Late-Joiner Node
**What:** A 4th node defined in the compose file but not started by default. Started manually after data has been loaded to test catch-up sync.
**When to use:** Success criterion 4 -- "A late-joiner node can be started after the initial topology and catches up on existing data."
**How:** Define node4 with `profiles: ["latejoin"]` so it does not start with `docker compose up` but can be started with `docker compose --profile latejoin up node4`.

```yaml
  node4:
    image: chromatindb:latest
    command: ["run", "--config", "/config/node4-latejoin.json", "--data-dir", "/data"]
    volumes:
      - node4-data:/data
      - ./configs/node4-latejoin.json:/config/node4-latejoin.json:ro
    networks:
      - chromatindb-net
    depends_on:
      node3:
        condition: service_healthy
    profiles:
      - latejoin
```

### Pattern 4: Healthcheck Override
**What:** The Dockerfile already defines a healthcheck (TCP connect on port 4200), but docker-compose.yml should override it with shorter intervals for faster topology startup.
**When to use:** Always -- the Dockerfile healthcheck has 30s interval and 10s start_period, which is too slow for development iteration. The compose file can use 5s interval and 10s start_period.

### Anti-Patterns to Avoid
- **Full mesh bootstrap:** Do not have every node bootstrap to every other node. This defeats the purpose of testing multi-hop propagation. Chain topology is required.
- **Bind-mount for data:** Do not use host bind-mounts for libmdbx data. Named volumes are Docker-managed and avoid permission issues. The Dockerfile creates `/data` owned by the `chromatindb` user.
- **Hardcoded container IPs:** Do not use IP addresses for bootstrap_peers in configs. Use Docker Compose service names (node1, node2, etc.) which resolve via Docker's internal DNS.
- **Shared data volume:** Each node MUST have its own named volume. libmdbx uses file-level locking and MMAP -- two processes cannot share the same database files.
- **Using trusted_peers with Docker DNS names:** The `trusted_peers` config takes IP addresses, not hostnames. Docker container IPs are dynamic. For benchmark simplicity, skip `trusted_peers` in this phase -- PQ handshake overhead is what we want to measure. If trusted mode is needed later (PERF-05), it can be added in Phase 30.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Service startup ordering | Shell scripts with sleep/retry | Docker Compose `depends_on: condition: service_healthy` | Battle-tested, deterministic, handles restarts |
| Container networking | Manual port mapping between containers | Docker Compose `networks` with service name DNS | Automatic DNS resolution, isolated bridge network |
| Persistent storage | Host directory mounts with chown hacks | Docker named volumes | Handles UID mapping, survives container removal |
| Late-joiner gating | Script that checks if other nodes are ready | Docker Compose `profiles` | Clean separation, node4 starts only when explicitly requested |

**Key insight:** Docker Compose provides all the orchestration primitives needed. The only custom work is writing the per-node JSON config files.

## Common Pitfalls

### Pitfall 1: sync_interval_seconds Too High for Testing
**What goes wrong:** Default sync interval is 60 seconds. With 3 nodes in a chain, a blob written to node1 takes up to 120 seconds to reach node3 (60s to node2, then 60s to node3).
**Why it happens:** Production default is optimized for low overhead, not fast validation.
**How to avoid:** Set `sync_interval_seconds` to 10 in all node configs. This gives a maximum 20-second end-to-end propagation for a 3-node chain.
**Warning signs:** "Replication seems to work but takes forever" -- check sync interval.

### Pitfall 2: Identity Key Collision
**What goes wrong:** All nodes generate identical keys because they share the same volume or start from the same image layer.
**Why it happens:** `NodeIdentity::load_or_generate()` generates a new key on first run and saves it. Each node needs its OWN /data volume so it gets a unique identity.
**How to avoid:** Each node has its own named volume (node1-data, node2-data, etc.). On first `docker compose up`, each node auto-generates a unique ML-DSA-87 keypair.
**Warning signs:** All nodes log the same namespace hash -- means they share identity files.

### Pitfall 3: Healthcheck Uses bash But Image Lacks It
**What goes wrong:** The Dockerfile healthcheck uses `bash -c 'exec 3<>/dev/tcp/...'` which requires bash. debian:bookworm-slim includes bash, so this works. BUT if the image were alpine-based, this would fail.
**Why it happens:** The TCP connect trick is bash-specific.
**How to avoid:** Keep using debian:bookworm-slim (already decided). The existing healthcheck works as-is.
**Warning signs:** Healthcheck always fails, container never becomes healthy.

### Pitfall 4: Node Starts Before Bootstrap Target Is Listening
**What goes wrong:** Node2 tries to connect to node1 before node1 has opened its TCP listener. Connection fails, enters exponential backoff reconnect loop.
**Why it happens:** `depends_on: condition: service_started` only waits for container creation, not TCP readiness.
**How to avoid:** Use `depends_on: condition: service_healthy`. The healthcheck verifies TCP port 4200 is open before marking the service as healthy.
**Warning signs:** Logs show "failed to connect to node1:4200" followed by reconnect backoff. Not fatal (reconnect loop recovers), but wastes startup time.

### Pitfall 5: libmdbx MMAP Needs Sufficient Memory
**What goes wrong:** libmdbx memory-maps its database files. With very large datasets, the container might need more memory than Docker's default limit.
**Why it happens:** MMAP virtual memory != RSS, but Docker may have memory limits.
**How to avoid:** For Phase 29 testing, default Docker memory limits are sufficient. For Phase 30 large-scale benchmarks, add `mem_limit` if needed.
**Warning signs:** SIGBUS or ENOMEM errors from libmdbx.

## Code Examples

### docker-compose.yml (Complete)
```yaml
services:
  node1:
    image: chromatindb:latest
    container_name: chromatindb-node1
    command: ["run", "--config", "/config/node1.json", "--data-dir", "/data", "--log-level", "debug"]
    volumes:
      - node1-data:/data
      - ./configs/node1.json:/config/node1.json:ro
    networks:
      - chromatindb-net
    ports:
      - "4201:4200"
    healthcheck:
      test: ["CMD-SHELL", "exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-"]
      interval: 5s
      timeout: 3s
      start_period: 10s
      retries: 3

  node2:
    image: chromatindb:latest
    container_name: chromatindb-node2
    command: ["run", "--config", "/config/node2.json", "--data-dir", "/data", "--log-level", "debug"]
    volumes:
      - node2-data:/data
      - ./configs/node2.json:/config/node2.json:ro
    networks:
      - chromatindb-net
    ports:
      - "4202:4200"
    depends_on:
      node1:
        condition: service_healthy
    healthcheck:
      test: ["CMD-SHELL", "exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-"]
      interval: 5s
      timeout: 3s
      start_period: 10s
      retries: 3

  node3:
    image: chromatindb:latest
    container_name: chromatindb-node3
    command: ["run", "--config", "/config/node3.json", "--data-dir", "/data", "--log-level", "debug"]
    volumes:
      - node3-data:/data
      - ./configs/node3.json:/config/node3.json:ro
    networks:
      - chromatindb-net
    ports:
      - "4203:4200"
    depends_on:
      node2:
        condition: service_healthy
    healthcheck:
      test: ["CMD-SHELL", "exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-"]
      interval: 5s
      timeout: 3s
      start_period: 10s
      retries: 3

  node4:
    image: chromatindb:latest
    container_name: chromatindb-node4
    command: ["run", "--config", "/config/node4-latejoin.json", "--data-dir", "/data", "--log-level", "debug"]
    volumes:
      - node4-data:/data
      - ./configs/node4-latejoin.json:/config/node4-latejoin.json:ro
    networks:
      - chromatindb-net
    ports:
      - "4204:4200"
    depends_on:
      node3:
        condition: service_healthy
    profiles:
      - latejoin
    healthcheck:
      test: ["CMD-SHELL", "exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-"]
      interval: 5s
      timeout: 3s
      start_period: 10s
      retries: 3

volumes:
  node1-data:
  node2-data:
  node3-data:
  node4-data:

networks:
  chromatindb-net:
    driver: bridge
```

### Node Config Files

node1.json (seed):
```json
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

node2.json:
```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node1:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

node3.json:
```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node2:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

node4-latejoin.json:
```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node3:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10
}
```

### Manual Verification Commands
```bash
# Build the image first
docker build -t chromatindb:latest .

# Start the 3-node topology
docker compose -f deploy/docker-compose.yml up -d

# Watch logs to confirm handshakes and sync
docker compose -f deploy/docker-compose.yml logs -f

# Write a blob to node1 via loadgen
docker compose -f deploy/docker-compose.yml run --rm \
  chromatindb_loadgen --target node1:4200 --count 5 --rate 1

# Check node3 has the blobs (multi-hop propagation)
# Look for "ingests" in SIGUSR1 metrics dump
docker kill -s USR1 chromatindb-node3
docker compose -f deploy/docker-compose.yml logs node3 | grep ingests

# Start late-joiner
docker compose -f deploy/docker-compose.yml --profile latejoin up -d node4

# Verify node4 catches up
docker kill -s USR1 chromatindb-node4
docker compose -f deploy/docker-compose.yml logs node4 | grep ingests
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `docker-compose` (v1 Python) | `docker compose` (v2 Go, built-in) | 2023 | v1 is deprecated; always use `docker compose` (no hyphen) |
| `version: "3.8"` in compose file | No `version` key needed | Compose Specification | The `version` field is informational only in v2; omit it |
| `condition: service_started` | `condition: service_healthy` | Docker Compose v2 | Required for ordered startup with readiness validation |
| `links:` for container DNS | `networks:` with service name DNS | Docker Compose v2 | `links` is legacy; service names auto-resolve on shared networks |

**Deprecated/outdated:**
- `docker-compose` (hyphenated): Use `docker compose` (space-separated, v2 CLI)
- `version:` key in docker-compose.yml: Informational only, not required
- `links:` directive: Replaced by Docker network DNS resolution

## Open Questions

1. **Loadgen as a Compose service?**
   - What we know: The loadgen binary is in the Docker image. Phase 30 will need it.
   - What's unclear: Whether to define the loadgen as a compose service now or defer to Phase 30.
   - Recommendation: Defer to Phase 30. Phase 29 is about topology, not benchmarking. The loadgen can be run via `docker compose run` or as a service added later.

2. **Port exposure for host access**
   - What we know: Mapping container port 4200 to host ports (4201, 4202, 4203) allows the loadgen to connect from the host.
   - What's unclear: Whether port exposure is needed for Phase 29 validation or only for Phase 30.
   - Recommendation: Include port mappings now -- useful for validation and later benchmark scripts. No harm in exposing them.

## Sources

### Primary (HIGH confidence)
- chromatindb source code (config.h, config.cpp, main.cpp, peer_manager.h, server.h) -- verified config format, CLI args, bootstrap_peers behavior, healthcheck approach
- Existing Dockerfile -- verified healthcheck, runtime image, entrypoint, volume mount
- Docker Compose official docs (https://docs.docker.com/compose/how-tos/startup-order/) -- depends_on conditions, service_healthy pattern

### Secondary (MEDIUM confidence)
- Docker Compose Specification (https://docs.docker.com/reference/compose-file/services/) -- service definition syntax
- libmdbx GitHub (https://github.com/erthink/libmdbx) -- file locking behavior on Linux (POSIX flock, works with Docker named volumes)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- Docker Compose is the only reasonable tool; no alternatives needed
- Architecture: HIGH -- chain topology directly maps to project's bootstrap_peers mechanism; verified in source code
- Pitfalls: HIGH -- all pitfalls verified against actual code (sync_interval default, identity generation, healthcheck dependency)
- Config format: HIGH -- verified JSON config fields match config.h/config.cpp exactly

**Research date:** 2026-03-16
**Valid until:** 2026-04-16 (stable -- Docker Compose spec and chromatindb config are not changing)
