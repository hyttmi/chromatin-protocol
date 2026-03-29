"""Shared pytest fixtures for chromatindb SDK tests."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import Any

import pytest


VECTORS_DIR = Path(__file__).parent / "vectors"


@pytest.fixture
def tmp_dir() -> Path:
    """Provide a temporary directory that is cleaned up after the test."""
    with tempfile.TemporaryDirectory() as d:
        yield Path(d)  # type: ignore[misc]


def load_vectors(filename: str) -> dict[str, Any]:
    """Load a JSON test vector file from the vectors/ directory."""
    path = VECTORS_DIR / filename
    if not path.exists():
        pytest.skip(f"Test vector file not found: {path}")
    with open(path) as f:
        return json.load(f)  # type: ignore[no-any-return]
