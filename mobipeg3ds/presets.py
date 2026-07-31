"""Versioned quality presets. EXPERIMENTAL PREVIEW.

Every preset below currently resolves to the exact same underlying values --
this investigation's accepted Candidate B settings (QP25/QYX3/Q-RDO enabled/
single-threaded video encoding). This is a deliberate, disclosed placeholder,
not a finished quality lineup: the actual quality-vs-size differentiation
between "Balanced", "Anime High Quality", and "Maximum Quality" has not been
decided. Do not present these as final or as equivalent to the official
Nintendo encoder's output. See docs/research/ for the investigation that
produced the one set of values every preset currently shares.
"""
from __future__ import annotations

from .job import QualitySettings

_CANDIDATE_B_PLACEHOLDER = dict(qp=25, mobi_qyx=3, q_rdo=True, chroma_dz=None, threads=1)

PRESET_REVISION = 0  # bump whenever any preset's resolved values change

PRESETS: dict[str, dict] = {
    "balanced": {
        "label": "Balanced",
        "unresolved": True,
        "description": (
            "EXPERIMENTAL PLACEHOLDER -- currently identical to every other "
            "preset (Candidate B's QP25/QYX3). Real balanced-tier values are "
            "not yet decided."
        ),
        "values": _CANDIDATE_B_PLACEHOLDER,
    },
    "anime_hq": {
        "label": "Anime High Quality",
        "unresolved": True,
        "description": (
            "EXPERIMENTAL PLACEHOLDER -- currently identical to every other "
            "preset. This investigation's own test content has been anime, "
            "but no anime-specific tuning has been validated yet."
        ),
        "values": _CANDIDATE_B_PLACEHOLDER,
    },
    "maximum": {
        "label": "Maximum Quality",
        "unresolved": True,
        "description": (
            "EXPERIMENTAL PLACEHOLDER -- currently identical to every other "
            "preset. A genuine maximum-quality tier (e.g. QYX lower than 3) "
            "has not been evaluated for budget impact."
        ),
        "values": _CANDIDATE_B_PLACEHOLDER,
    },
    "custom": {
        "label": "Custom",
        "unresolved": False,
        "description": "User-specified values via the Advanced section.",
        "values": None,
    },
}


def resolve_preset(name: str, overrides: dict | None = None) -> QualitySettings:
    """Resolve a preset name (+ optional Advanced-section overrides for
    'custom') into a concrete QualitySettings. Raises KeyError for an
    unknown preset name -- never silently falls back to a different one."""
    if name not in PRESETS:
        raise KeyError(f"unknown preset '{name}' (known: {', '.join(PRESETS)})")
    entry = PRESETS[name]
    if name == "custom":
        values = dict(_CANDIDATE_B_PLACEHOLDER)
        if overrides:
            values.update(overrides)
    else:
        values = dict(entry["values"])
    return QualitySettings(preset=name, revision=PRESET_REVISION, **values)
