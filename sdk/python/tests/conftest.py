"""Shared pytest fixtures for chromatindb SDK tests."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any

import pytest

from chromatindb.identity import Identity


VECTORS_DIR = Path(__file__).parent / "vectors"


@pytest.fixture
def tmp_dir() -> Path:
    """Provide a temporary directory that is cleaned up after the test."""
    with tempfile.TemporaryDirectory() as d:
        yield Path(d)  # type: ignore[misc]


@pytest.fixture
def identity() -> Identity:
    """Generate a fresh ephemeral identity for testing."""
    return Identity.generate()


@pytest.fixture
def relay_host() -> str:
    """Relay host for integration tests (env override: CHROMATINDB_RELAY_HOST)."""
    return os.environ.get("CHROMATINDB_RELAY_HOST", "192.168.1.200")


@pytest.fixture
def relay_port() -> int:
    """Relay port for integration tests (env override: CHROMATINDB_RELAY_PORT)."""
    return int(os.environ.get("CHROMATINDB_RELAY_PORT", "4433"))


def load_vectors(filename: str) -> dict[str, Any]:
    """Load a JSON test vector file from the vectors/ directory."""
    path = VECTORS_DIR / filename
    if not path.exists():
        pytest.skip(f"Test vector file not found: {path}")
    with open(path) as f:
        return json.load(f)  # type: ignore[no-any-return]
