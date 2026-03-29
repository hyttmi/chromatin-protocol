# Phase 70: Crypto Foundation & Identity - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-03-29
**Phase:** 70-crypto-foundation-identity
**Areas discussed:** Key file format & storage, Python crypto library choices, SDK API surface & naming, Test vector strategy, FlatBuffers codegen workflow, Dev tooling & CI, Dependency pinning strategy, SDK directory layout

---

## Key File Format & Storage

| Option | Description | Selected |
|--------|-------------|----------|
| Same raw binary format | Identical .key/.pub files -- keys interoperable between SDK and node/relay | |
| PEM-wrapped format | Base64-encoded with headers, requires conversion for node interop | |
| Both formats supported | Raw binary default, optional PEM import/export | |

**User's choice:** Same raw binary format
**Notes:** Matches success criterion #1 (key interoperability)

| Option | Description | Selected |
|--------|-------------|----------|
| User-specified path only | No default location, user passes key_path | |
| ~/.chromatindb/ default | Default with override | |
| XDG_DATA_HOME convention | Linux standard dirs | |

**User's choice:** User-specified path only
**Notes:** Matches node/relay pattern (explicit config)

| Option | Description | Selected |
|--------|-------------|----------|
| generate() and from_keys() | Ephemeral in-memory + disk persistence options | |
| Always persist to disk | Every identity must have a file path | |

**User's choice:** Yes -- generate(), from_keys(), load(), generate_and_save()

---

## Python Crypto Library Choices

| Option | Description | Selected |
|--------|-------------|----------|
| liboqs-python | Official Python binding for liboqs | |
| pqcrypto | Alternative PQ crypto bindings | |

**User's choice:** liboqs-python

| Option | Description | Selected |
|--------|-------------|----------|
| PyNaCl | Python binding for libsodium | |
| cryptography (pyca) | OpenSSL wrapper | |
| PyCryptodome | Self-contained crypto | |

**User's choice:** PyNaCl

| Option | Description | Selected |
|--------|-------------|----------|
| hashlib (stdlib) | Built-in SHA3-256 | |
| liboqs-python SHA3 | liboqs SHA3 functions | |
| pysha3 | Dedicated SHA3 package | |

**User's choice:** hashlib (stdlib)

| Option | Description | Selected |
|--------|-------------|----------|
| Official flatbuffers package | pip install flatbuffers + flatc --python | |
| Raw struct packing | Hand-roll encode/decode | |

**User's choice:** Official flatbuffers package

---

## SDK API Surface & Naming

| Option | Description | Selected |
|--------|-------------|----------|
| chromatindb | Matches project name | |
| chromatindb-sdk | Distinguishes from server package | |
| chromatin | Shorter but potential PyPI clash | |

**User's choice:** chromatindb

| Option | Description | Selected |
|--------|-------------|----------|
| Flat under chromatindb/ | crypto.py, identity.py, wire.py, exceptions.py | |
| Nested subpackages | chromatindb.crypto.signing, etc. | |

**User's choice:** Flat under chromatindb/

| Option | Description | Selected |
|--------|-------------|----------|
| Async-first with sync wrapper | Core async, thin sync wrapper | |
| Sync-only for Phase 70 | Crypto is CPU-bound | |
| Async-only | Out of scope per requirements | |

**User's choice:** Async-first with sync wrapper

| Option | Description | Selected |
|--------|-------------|----------|
| snake_case (PEP 8) | Standard Python naming | |
| Match C++ naming | 1:1 mapping with source | |

**User's choice:** snake_case (PEP 8)

| Option | Description | Selected |
|--------|-------------|----------|
| Base + category exceptions | ChromatinError > CryptoError, IdentityError, etc. | |
| Flat exception set | All inherit directly from base | |

**User's choice:** Base + category exceptions

| Option | Description | Selected |
|--------|-------------|----------|
| Python 3.10+ | match/case, union types | |
| Python 3.12+ | Latest features | |
| Python 3.8+ | Maximum compatibility | |

**User's choice:** Python 3.10+

| Option | Description | Selected |
|--------|-------------|----------|
| Full type hints, no runtime checking | mypy/pyright catches errors | |
| Type hints + runtime validation | Beartype or isinstance | |

**User's choice:** Full type hints, no runtime checking

| Option | Description | Selected |
|--------|-------------|----------|
| Google style docstrings | Args/Returns/Raises sections | |
| NumPy style | Dashed section headers | |

**User's choice:** Google style

---

## Test Vector Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Extract from C++ test suite | Hardcode known input/output pairs | |
| Generate from live node | Connect to running node | |
| RFC/NIST vectors only | Published standards only | |

**User's choice:** Extract from C++ test suite

| Option | Description | Selected |
|--------|-------------|----------|
| Small standalone C++ binary | Prints JSON vectors, commit output | |
| Extract manually from tests | Grep existing tests | |

**User's choice:** Small standalone C++ binary

| Option | Description | Selected |
|--------|-------------|----------|
| pytest | Standard Python test framework | |
| unittest | stdlib, more verbose | |

**User's choice:** pytest

---

## FlatBuffers Codegen Workflow

| Option | Description | Selected |
|--------|-------------|----------|
| Commit generated code | Run flatc once, commit to generated/ | |
| Generate at install time | pyproject.toml build step | |

**User's choice:** Commit generated code

| Option | Description | Selected |
|--------|-------------|----------|
| Use existing db/schemas/ | Single source of truth | |
| Copy to sdk/python/schemas/ | SDK self-contained | |

**User's choice:** Use existing db/schemas/

---

## Dev Tooling & CI

| Option | Description | Selected |
|--------|-------------|----------|
| ruff (lint + format) | Single tool, fast | |
| flake8 + black | Traditional combo | |
| None for Phase 70 | Defer tooling | |

**User's choice:** ruff

| Option | Description | Selected |
|--------|-------------|----------|
| mypy | Standard type checker | |
| pyright | Faster, stricter | |
| Skip for Phase 70 | Defer | |

**User's choice:** mypy

---

## Dependency Pinning Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Compatible release pins (~=) | e.g., liboqs-python~=0.11.0 | |
| Exact pins in lockfile only | >= in pyproject.toml, == in lock | |
| Exact pins in pyproject.toml | e.g., liboqs-python==0.11.0 | |

**User's choice:** Compatible release pins (~=)
**Notes:** User wanted pinning for reproducibility; ~= balances this with allowing patch updates

---

## SDK Directory Layout

| Option | Description | Selected |
|--------|-------------|----------|
| No src/ -- flat layout | sdk/python/chromatindb/ directly | |
| Yes -- src/ layout | sdk/python/src/chromatindb/ | |

**User's choice:** Flat layout (no src/)
**Notes:** User confirmed sdk/python/, sdk/rust/, sdk/c++/ top-level pattern (already planned)

---

## Claude's Discretion

- Internal module organization within crypto.py
- FlatBuffers generated code directory structure
- pytest configuration details
- ruff rule selection
- Test vector generator binary location
- __init__.py re-exports

## Deferred Ideas

None -- discussion stayed within phase scope
