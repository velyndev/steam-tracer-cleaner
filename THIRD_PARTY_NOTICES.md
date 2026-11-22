# Third-party notices

`steam-tracer-cleaner.exe` is statically linked and contains code from the projects below.
Steam Tracer Cleaner itself is under the MIT License, see [LICENSE](LICENSE).

| Component | Version | License | Used for |
|---|---|---|---|
| [Facet](https://github.com/velyndev/imgui-facet) | pinned commit, see `third_party/CMakeLists.txt` | MIT | UI framework: widgets, theme, scaling |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | MIT | UI |
| [FreeType](https://freetype.org/) | 2.15.0 | FreeType License (FTL) | Font rasterization |
| [lunasvg](https://github.com/sammycage/lunasvg), with its bundled plutovg | 3.5.0 | MIT | SVG icons |
| [stb_image](https://github.com/nothings/stb) | 2.32 | MIT or public domain | Image decoding inside Facet |
| [spdlog](https://github.com/gabime/spdlog) | 1.17.0 | MIT | Logging |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Settings and backup manifests |
| [SQLite](https://www.sqlite.org/) | 3.38.2 | Public domain | Reading browser cookie databases |

Facet, Dear ImGui, FreeType and lunasvg are downloaded by CMake at configure time; stb_image
comes with Facet. spdlog, nlohmann/json and SQLite are vendored under `third_party/`.
[doctest](https://github.com/doctest/doctest) (MIT) is used by the unit tests only and is not
part of the executable.

The app ships no font. It loads Segoe UI from the Windows font folder at run time.

## FreeType

Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org).
All rights reserved.

FreeType is used under the FreeType License. Its full text is `docs/FTL.TXT` in the FreeType
sources.

## MIT License

The components below are under the MIT License. Each carries its own copyright notice, and the
permission notice after the list applies to each of them.

- Facet: Copyright (c) 2026 velyn
- Dear ImGui: Copyright (c) 2014-2026 Omar Cornut
- lunasvg and plutovg: Copyright (c) 2020-2025 Samuel Ugochukwu <sammycageagle@gmail.com>
- stb_image: Copyright (c) 2017 Sean Barrett
- spdlog: Copyright (c) 2016 - present, Gabi Melman and spdlog contributors
- nlohmann/json: Copyright (c) 2013-2026 Niels Lohmann

```
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

spdlog is built with `SPDLOG_USE_STD_FORMAT`, so the fmt library it can bundle is not compiled
into the executable.

## SQLite

SQLite is in the public domain. The amalgamation vendored in `third_party/sqlite` comes from
the sqlite-amalgamation packaging, which carries this notice:

```
Copyright (c) 2015, amir zamani
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of sqlite-amalgamation nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
