# Third-party notices

evo_boids is licensed under CC BY-NC 4.0 (see `LICENSE`). The parts listed here
come from other projects and stay under their own licenses. Those licenses are
permissive and require that the notices below travel with copies of the code.

## Source code

### three.js — MIT

`web/vendor/three.bundle.min.js` is a trimmed build of three.js r186
(`three` 0.186.1 on npm, `tools/three_bundle/`), used by the 3D scene in
`web/scene3d.js`. The bundle keeps three.js's license header.

```
The MIT License

Copyright © 2010-2026 three.js authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### musl libc — MIT

Portions of `src/core/portable_math.hpp` (`tanhf`, `sinf`, `cosf`, `atanf`,
`atan2f`, `expm1f`, `expf`, `logf`, double `tanh` and `expm1`, their kernels,
tables and error-handling helpers) are taken from musl, as bundled with
Emscripten, so native and WebAssembly builds compute bit-identical results.

```
Copyright © 2005-2020 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Arm optimized-routines — MIT

musl's `expf`, `logf` and their tables (`exp2f_data.c`, `logf_data.c`), used
above, come from Arm's optimized-routines and carry the notice below, under
the MIT License (the same terms as the musl text above).

```
Copyright (c) 2017-2018, Arm Limited.
SPDX-License-Identifier: MIT
```

### FreeBSD / Sun Microsystems fdlibm

musl's `sinf`, `cosf`, `atanf`, `atan2f`, `expm1f` and `expm1` above derive
from FreeBSD's `s_sinf.c`, `s_cosf.c`, `s_atanf.c`, `e_atan2f.c`,
`s_expm1f.c`, `s_expm1.c`, `e_rem_pio2f.c`, `k_rem_pio2.c`, `k_sinf.c` and
`k_cosf.c`, which carry:

```
Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.

Developed at SunPro, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

### biosim4 — MIT

The genome encoding (one gene = one weighted source→sink connection) and the
construction of the neural net from it (`src/core/genome.hpp`), the
thread-local random generator wrapper (`src/core/random.hpp`), and several
sensors and actions (`src/core/sensors_actions.hpp`) are adapted from biosim4,
https://github.com/davidrmiller/biosim4.

```
MIT License

Copyright (c) 2021-2022 David R. Miller and other contributors. For the
exact contribution history, see the revision logs at the project page
https://github.com/davidrmiller/biosim4.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Compiled browser modules

`web/evo_boids.{js,wasm}` and `web/evo_boids_mt.{js,wasm}` (and the copies
`wasm/build_site.sh` puts in `dist/`) are produced by Emscripten and contain,
besides evo_boids itself:

- **Emscripten's JavaScript runtime and system libraries**: MIT or
  University of Illinois/NCSA, at the recipient's choice.
  `Copyright (c) 2010-2014 Emscripten authors, see AUTHORS file.`
  Full text: https://github.com/emscripten-core/emscripten/blob/main/LICENSE
- **musl libc**: MIT, notice above.
- **LLVM libc++, libc++abi and compiler-rt**: Apache License 2.0 with LLVM
  Exceptions. The exception waives the attribution requirement for these
  runtime libraries when they are embedded in compiled output.
  https://llvm.org/LICENSE.txt

## Not third-party

- **`img/rocks.jpg`**: the background art, by the author.
- **`img/circle.png`**: generated by `tools/make_circle_png.py`. It replaced an
  earlier file from a third-party project in v0.25.0.
