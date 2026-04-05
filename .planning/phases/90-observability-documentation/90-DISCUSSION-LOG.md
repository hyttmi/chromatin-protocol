# Phase 90: Observability & Documentation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 90-observability-documentation
**Areas discussed:** Metrics HTTP endpoint architecture, Prometheus format specifics, Configuration and lifecycle, Documentation scope
**Mode:** Auto (all decisions auto-selected)

---

## Metrics HTTP Endpoint Architecture

| Option | Description | Selected |
|--------|-------------|----------|
| Minimal HTTP responder on shared io_context | No new threads, coroutine-based, parse GET /metrics only | ✓ |
| Separate HTTP library (cpp-httplib, Boost.Beast) | Full HTTP support, more features | |
| Dedicated thread for metrics HTTP | Isolation from main event loop | |

**User's choice:** [auto] Minimal HTTP responder on shared io_context
**Notes:** Consistent with existing zero-dependency philosophy. NodeMetrics is already thread-safe (single io_context). No need for a full HTTP stack.

---

## Prometheus Format Specifics

| Option | Description | Selected |
|--------|-------------|----------|
| Flat counters/gauges with chromatindb_ prefix | Standard naming, no labels, HELP/TYPE lines | ✓ |
| Labeled metrics with per-peer breakdown | More detail but complex implementation | |
| OpenMetrics format | Newer standard but less widely supported | |

**User's choice:** [auto] Flat counters/gauges with chromatindb_ prefix
**Notes:** Maps directly to existing NodeMetrics struct. No histograms needed — counters and gauges cover all current metrics.

---

## Configuration and Lifecycle

| Option | Description | Selected |
|--------|-------------|----------|
| metrics_bind config field (empty = disabled) | Opt-in, SIGHUP reloadable, follows existing pattern | ✓ |
| Always-on with fixed port | Simpler but less secure | |
| Command-line flag only | Not reloadable | |

**User's choice:** [auto] metrics_bind config field (empty = disabled)
**Notes:** Follows existing SIGHUP reload pattern. Localhost-only default when enabled.

---

## Documentation Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Update existing sections in-place | No restructure, add v2.1.0 features where they fit | ✓ |
| Major restructure with new sections | More comprehensive but higher risk of breaking links | |

**User's choice:** [auto] Update existing sections in-place
**Notes:** PROTOCOL.md needs SyncNamespaceAnnounce, envelope compression, /metrics. README needs observability section. SDK docs already partially updated from Phase 89.

## Claude's Discretion

- Exact Prometheus metric names (within chromatindb_ convention)
- HTTP response buffer strategy
- Documentation section ordering

## Deferred Ideas

None
