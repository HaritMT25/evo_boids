#!/bin/bash
# build.sh — rebuild web/vendor/three.bundle.min.js, the trimmed three.js the
# 3D scene (web/scene3d.js) loads. Only the classes listed in entry.js end up in
# the bundle (esbuild tree-shakes the rest), about 545 KB minified / 115 KB
# brotli instead of the whole library. Commit the output, like the wasm blobs:
# the site itself has no build step.
#
#   tools/three_bundle/build.sh            # three 0.186.1, esbuild 0.28.2
#
# Adding a class to web/scene3d.js means adding it to entry.js and rerunning.
set -euo pipefail
cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp entry.js "$TMP/"
(cd "$TMP" && npm init -y >/dev/null && npm i --no-audit --no-fund three@0.186.1 esbuild@0.28.2 >/dev/null)
(cd "$TMP" && npx esbuild entry.js --bundle --format=esm --minify --legal-comments=eof \
    --target=es2020 --outfile=three.bundle.min.js)
cp "$TMP/three.bundle.min.js" ../../web/vendor/three.bundle.min.js
echo "wrote web/vendor/three.bundle.min.js"
