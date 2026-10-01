#!/usr/bin/env python3
"""Compatibility entry point for the current coordinated NABO greeting."""

from prepare_nabo_assets import ROOT, wave_rig
from preview_nabo_greeting import render_preview


if __name__ == "__main__":
    render_preview(ROOT, *wave_rig())
