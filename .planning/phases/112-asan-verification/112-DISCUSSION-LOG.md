# Phase 112: ASAN Verification - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-14
**Phase:** 112-asan-verification
**Areas discussed:** Load driver tool, Signal test approach, ASAN leak policy, Fix strategy

---

## Load Driver Tool

| Option | Description | Selected |
|--------|-------------|----------|
| Python benchmark (Recommended) | Reuse relay_benchmark.py as-is. It already does auth + data ops at 1/10/100 clients. ASAN instruments the relay server process, not the client — so the driver language doesn't matter. | ✓ |
| New C++ stress tool | Build a dedicated C++ client linked with Asio. More control over connection lifecycle, but duplicates work and ASAN only needs to instrument the server. | |
| Shell script wrapper | Thin bash/Python script that launches ASAN relay, runs benchmark.py at each concurrency level, and collects ASAN output into a report file. | |

**User's choice:** Python benchmark (Recommended)
**Notes:** Straightforward — ASAN instruments the server, driver language is irrelevant.

---

## Signal Test Approach

| Option | Description | Selected |
|--------|-------------|----------|
| Automated script (Recommended) | Script launches ASAN relay, starts benchmark load, sends SIGHUP mid-run (verify config change observed + no crash), then SIGTERM (verify clean drain). Parse ASAN stderr for errors. Repeatable and CI-friendly. | ✓ |
| Manual testing | Run ASAN relay + benchmark in terminals, manually send kill -HUP / kill -TERM, visually inspect output. Quick but not repeatable. | |
| C++ integration test | Catch2 test that forks relay process, drives HTTP traffic, sends signals via kill(), and asserts on exit code + ASAN output. Most rigorous but heaviest to build. | |

**User's choice:** Automated script (Recommended)
**Notes:** Must be repeatable and CI-friendly. Verify rate limit change takes effect + TLS cert swap + clean drain.

---

## ASAN Leak Policy

| Option | Description | Selected |
|--------|-------------|----------|
| Suppression file (Recommended) | Create an ASAN suppression file (lsan_suppressions.txt) for known shutdown leaks (e.g., liboqs global state, OpenSSL cleanup). Fix any NEW leaks found. Clear separation between accepted and actionable. | ✓ |
| Fix everything | Zero tolerance — fix all leaks including shutdown leaks. May require atexit() cleanup for third-party libs (liboqs, OpenSSL) that's not worth the complexity. | |
| ASAN_OPTIONS=detect_leaks=0 | Disable leak detection entirely. Focus only on heap-use-after-free and other memory safety bugs. Leaks aren't safety bugs. | |

**User's choice:** Suppression file (Recommended)
**Notes:** Known third-party shutdown leaks get suppressed. New leaks are bugs.

---

## Fix Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Fix inline, relay only (Recommended) | Fix all ASAN-reported bugs in relay code within this phase. If a bug traces to node code (db/), document it but don't fix — node is frozen/shipped. Keep Phase 112 focused. | |
| Fix everything found | Fix bugs wherever they are — relay or node code. Could expand scope significantly if node bugs surface. | ✓ |
| Document only, fix in separate phase | Record all ASAN findings in a report, defer fixes to a new phase. Phase 112 becomes pure verification, not remediation. | |

**User's choice:** Fix everything found
**Notes:** No scope restriction on where fixes go. If architectural changes needed, discuss first. Simple bugs fixed inline.

---

## Claude's Discretion

- Script language for automated ASAN test harness
- Whether relay_benchmark.py needs minor modifications for ASAN testing
- Suppression file format and granularity
- Plan decomposition

## Deferred Ideas

None — discussion stayed within phase scope.
