#!/usr/bin/env bash
# Checks the book against the sources, weaves it and typesets the PDF.
#   ./build.sh            -> UsdOptixViewer-literate.pdf
# Needs python3 with pygments, and tectonic (https://tectonic-typesetting.github.io).
set -euo pipefail
cd "$(dirname "$0")"

python3 lp.py check
mkdir -p build
python3 lp.py weave build/UsdOptixViewer-literate.tex
(cd build && tectonic --keep-logs UsdOptixViewer-literate.tex)
cp build/UsdOptixViewer-literate.pdf .
echo "wrote $(pwd)/UsdOptixViewer-literate.pdf"
