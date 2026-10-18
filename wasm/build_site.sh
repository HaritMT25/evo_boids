#!/bin/bash
# build_site.sh — assemble the static demo site in dist/ for Vercel.
#
#   dist/index.html            no-cache; references everything below by hash
#   dist/assets/NAME.HASH.EXT  every script, wasm and image, content-hashed,
#                              served with a one-year immutable cache
#   dist/vercel.json           headers: COOP/COEP (cross-origin isolation for
#                              the threaded module), wasm MIME, caching
#
# Static output only: no build step on Vercel, no serverless functions.
# Does NOT compile anything — build both modules first:
#   source ~/emsdk/emsdk_env.sh
#   cmake --preset wasm-st && cmake --build --preset wasm-st
#   cmake --preset wasm-mt && cmake --build --preset wasm-mt
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=dist
SRC=(web/evo_boids.js web/evo_boids.wasm web/evo_boids_mt.js web/evo_boids_mt.wasm
     web/loader.js web/gl_renderer.js web/randomizer.js web/renderer.js web/guide.js
     web/worker.js img/rocks.jpg web/index.html web/vercel.json)
for f in "${SRC[@]}"; do
    [ -f "$f" ] || { echo "missing $f — build the wasm-st and wasm-mt presets first" >&2; exit 1; }
done

# Clear the previous build but keep dist/.vercel: that is the Vercel project
# link (`vercel link --cwd dist`), and losing it would make the next deploy
# set up a new project.
mkdir -p "$OUT"
find "$OUT" -mindepth 1 -maxdepth 1 ! -name .vercel -exec rm -rf {} +
mkdir -p "$OUT/assets"

# hashed copy: prints the published path, relative to dist/
publish() {
    local src=$1 base ext h
    base=$(basename "$src"); ext=${base##*.}; base=${base%.*}
    h=$(sha256sum "$src" | cut -c1-12)
    cp "$src" "$OUT/assets/$base.$h.$ext"
    echo "assets/$base.$h.$ext"
}

ST=$(publish web/evo_boids.js);       ST_WASM=$(publish web/evo_boids.wasm)
MT=$(publish web/evo_boids_mt.js);    MT_WASM=$(publish web/evo_boids_mt.wasm)
LOADER=$(publish web/loader.js);      RENDERER=$(publish web/renderer.js)
GLR=$(publish web/gl_renderer.js);    RAND=$(publish web/randomizer.js)
GUIDE=$(publish web/guide.js)
WORKER=$(publish web/worker.js);      BG=$(publish img/rocks.jpg)

# Rewrite the EVO_ASSETS block and the <script src> tags. Fails loudly if
# index.html no longer has the shape this expects.
python3 - "$OUT/index.html" <<PY
import re, sys
src = open("web/index.html").read()
assets = '''window.EVO_ASSETS = {
      st: "$ST", stWasm: "$ST_WASM",
      mt: "$MT", mtWasm: "$MT_WASM",
      worker: "$WORKER",
      background: "$BG"
    };'''
out, n1 = re.subn(r'window\.EVO_ASSETS = \{.*?\};', lambda m: assets, src, count=1, flags=re.S)
out, n2 = re.subn(r'<script src="loader\.js"></script>', '<script src="$LOADER"></script>', out, count=1)
out, n3 = re.subn(r'<script src="renderer\.js"></script>', '<script src="$RENDERER"></script>', out, count=1)
out, n4 = re.subn(r'<script src="gl_renderer\.js"></script>', '<script src="$GLR"></script>', out, count=1)
out, n5 = re.subn(r'<script src="randomizer\.js"></script>', '<script src="$RAND"></script>', out, count=1)
out, n6 = re.subn(r'<script src="guide\.js"></script>', '<script src="$GUIDE"></script>', out, count=1)
assert (n1, n2, n3, n4, n5, n6) == (1, 1, 1, 1, 1, 1), ("index.html changed shape", n1, n2, n3, n4, n5, n6)
open(sys.argv[1], "w").write(out)
PY

cp web/vercel.json "$OUT/vercel.json"

echo "built $OUT/:"
(cd "$OUT" && find . -type f | sort | xargs ls -la | awk '{printf "  %9d  %s\n", $5, $9}')
