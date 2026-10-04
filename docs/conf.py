# Sphinx configuration for the fs-9p documentation (Markdown via MyST).
# Build with the `docs` CMake target, or: sphinx-build -W -b html docs <out>
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent

# The version is set in one place, CMakeLists.txt (docs/dev/releasing.md).
_m = re.search(r"project\(fs-9p VERSION (\d+\.\d+\.\d+)", (root / "CMakeLists.txt").read_text())
if not _m:
    raise RuntimeError("version not found in CMakeLists.txt")
release = _m.group(1)
version = release

project = "fs-9p"
author = "Giovanni Vergine"
copyright = "2026, Giovanni Vergine. Apache License 2.0"

extensions = ["myst_parser"]
myst_enable_extensions = ["colon_fence", "deflist"]
myst_heading_anchors = 3
source_suffix = {".md": "markdown"}
exclude_patterns = ["requirements.txt"]

html_theme = "furo"
html_title = f"fs-9p {release}"
html_show_sourcelink = False
