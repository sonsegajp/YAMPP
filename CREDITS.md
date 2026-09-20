# YAMPP credits and third-party notices

**Yet Another Melee PC Port** combines original integration work with the projects below. Thank you to their authors and contributors. Existing source headers and dependency licenses remain authoritative; this file does not relicense third-party work or Nintendo assets.

| Project / authors | Used for | License / source |
| --- | --- | --- |
| Nintendo and HAL Laboratory | Super Smash Bros. Melee; original game code, models, textures, UI, fonts, animations and audio supplied by the player's disc | Original rights holders; no game asset ownership is claimed |
| [Melee decompilation](https://github.com/doldecomp/melee), doldecomp contributors | Function identities, structures, original engine/menu behavior and strict reference | Pinned revision in `dependencies.json`; upstream file notices |
| [Aurora](https://github.com/encounter/aurora), Luke Street and contributors | GX graphics, platform integration and supporting libraries | MIT; pinned revision in `dependencies.json` |
| ExpansionPak, GXRuntime/DolRecomp-compatible runtime | PowerPC context, hardware/runtime support and native ABI | GPL-3.0-or-later; retained notices in `native/vendor` |
| [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) contributors | Floating-point estimate reference/table adaptation, DSP ADPCM accelerator boundary behavior, format references and comparison oracle | GPL-2.0-or-later notices retained at adapted code; not a runtime emulator dependency |
| [Dawn / Tint](https://dawn.googlesource.com/dawn/+/refs/heads/main/LICENSE), Dawn and Tint authors | WebGPU/Vulkan backend used by Aurora | BSD-style license; additional component notices in upstream source |
| [SDL](https://github.com/libsdl-org/SDL) contributors | Window, controllers, input and audio | zlib license |
| [nod](https://github.com/encounter/nod), encounter and contributors | Disc image support through Aurora | MIT or Apache-2.0; exact notices retained |
| [HSDLib / HSDRaw](https://github.com/Ploaj/HSDLib), Ploaj and contributors | Workshop HSD/DAT model, material, animation and texture conversion | MIT |
| [BrawlCrate / BrawlLib](https://github.com/soopercool101/BrawlCrate), contributors | Optional Brawl asset conversion tooling | LGPL-3.0; optional converter, separate from cosmetic Online runtime |
| [m-ex](https://github.com/akaneia/m-ex) and [MexManager](https://github.com/Ploaj/MexManager), their authors and contributors | Expanded fighter/stage/music format and selective local content conversion | Pinned sources used by the local selective-content importer; neither pinned repository supplies a top-level license, so no license grant is inferred from this credit |
| [The Akaneia Build](https://github.com/akaneia/akaneia-build), Akaneia team and credited contributors | Optional fighter, stage and music content acquired directly from their official GitHub release | Third-party mod; never included in YAMPP's repository, server content or base distribution |
| [Unicorn Engine](https://github.com/unicorn-engine/unicorn), Nguyen Anh Quynh and contributors | Execution of added/patched PowerPC instructions alongside the native game runtime | GPL-2.0 with component notices; the Windows content runtime bundles Unicorn 2.1.4, with exact notices in `native/vendor/unicorn` |
| [Lua](https://www.lua.org/), Lua.org / PUC-Rio | Restricted custom-fighter scripting | MIT; pinned Lua 5.5.1 build |
| [Electron](https://www.electronjs.org/), Electron contributors | Melee Workshop desktop shell | MIT; Chromium and other notices travel with packaged Electron |
| [Three.js](https://github.com/mrdoob/three.js), authors | Workshop model preview, GLTFLoader and OrbitControls | MIT; `tools/modkit/web/vendor/THREE-LICENSE.txt` |
| [Python](https://www.python.org/), Python Software Foundation | Build, asset and community workers | PSF license and bundled library notices |
| [py7zr](https://github.com/miurahr/py7zr), Hiroshi Miura and contributors | Extracting the official upstream content archive locally | LGPL-2.1-or-later; archive codec dependencies and exact package notices accompany the embedded Python runtime |
| [.NET runtime](https://github.com/dotnet/runtime), Microsoft and contributors | Self-contained Windows content importer | .NET 8.0.26; MIT and third-party notices in `licenses/third-party/content-importer` |
| [Six Labors](https://github.com/SixLabors), authors and contributors | Content-importer image/font processing | Exact ImageSharp 3.1.11, Drawing 2.1.4 and Fonts 2.0.4 licenses retained in `licenses/third-party/content-importer`; these packages do not all share the same license |
| [bodong.PropertyModels](https://github.com/bodong1987/Avalonia.PropertyGrid), bodong | Importer object-model dependency | MIT; exact pinned-source license and NuGet provenance retained |
| [NumPy](https://numpy.org/), developers | Workshop geometry/animation interchange | BSD-style; bundled numerical-library notices retained |
| [Pillow](https://python-pillow.org/), contributors | Texture/art conversion and tooling | HPND/Pillow notices and bundled image-library notices |
| [FreeType](https://freetype.org/), David Turner, Robert Wilhelm, Werner Lemberg and contributors | Aurora font support | FreeType License; original Melee menu fonts are separate game assets |
| [Dear ImGui](https://github.com/ocornut/imgui), Omar Cornut and contributors | PC settings/debug interface and host UI support | MIT |
| [Abseil](https://github.com/abseil/abseil-cpp), authors | Aurora C++ support | Apache-2.0 |
| [{fmt}](https://github.com/fmtlib/fmt), authors | Formatting | MIT |
| [libpng](http://www.libpng.org/pub/png/libpng.html), authors | PNG handling | libpng license |
| [zlib](https://zlib.net/), Jean-loup Gailly and Mark Adler | Compression | zlib license |
| [Zstandard](https://github.com/facebook/zstd), contributors | Compressed disc/cache support | BSD/GPL dual-license notices retained |
| [xxHash](https://github.com/Cyan4973/xxHash), Yann Collet and contributors | Hashing | BSD-2-Clause |
| [SQLite](https://sqlite.org/), authors | Aurora cache support | Public-domain dedication in source |
| [Tracy](https://github.com/wolfpld/tracy), Bartosz Taudul and contributors | Profiling infrastructure | BSD-3-Clause |
| [libjpeg-turbo](https://libjpeg-turbo.org/), authors and Independent JPEG Group | THP/JPEG decoding | libjpeg-turbo, IJG and SIMD notices retained |
| [FFmpeg](https://ffmpeg.org/legal.html), contributors | Native Linux custom-music decoding | LGPL-2.1-or-later or GPL when GPL components are enabled; use the exact system package notices |
| [OpenSSL](https://openssl-library.org/source/license/), contributors | Online TLS and SHA-256 verification | OpenSSL 3.x: Apache-2.0; bundled Windows DLLs and Linux system libraries |
| GCC / MinGW-w64 / winpthreads | Native compiler and required runtime DLLs | Their licenses and applicable runtime exceptions accompany the distribution |
| [Blender](https://www.blender.org/), Blender Foundation and contributors | Posing, lighting and rendering static costume artwork from the original model | GPL; authoring tool, not bundled |
| [Blender MCP](https://github.com/ahujasid/blender-mcp), Siddharth Ahuja and contributors | Local Blender scene control during artwork authoring | MIT; installed authoring tool, not bundled |
| Visual Studio, Clang, CMake, .NET, Windows Media Foundation, Node/npm and GoogleTest | Build, media decoding, conversion and testing tools | Respective upstream licenses; not all are shipped with the game |

Native Linux support originated in the original Linux pull request (#1) by [John Q. Herman (johnqherman)](https://github.com/johnqherman), integrated from revision `e8b2a57789956d2d7f33e2f4f2e7ec502298a3f0`. Local integration preserves the newer rollback, additive costumes and original-menu UI, and adds their POSIX worker/art-hash paths.

The catalog cache eviction fix from the original catalog-cache pull request (#2), also by John Q. Herman, is integrated from revision `9413e3f31ec3be264ab0289ae2f649ff79eda019`.

The HSDLib action compiler carries two narrow local fixes for parameter padding/field sizes and empty command lines. Their exact pinned-source patch is retained under `scripts/patches/hsdlib-action-compiler.*` and applied by bootstrap; HSDLib's original MIT notices remain intact.

The original indigo GameCube controller photograph and matching button cutouts are Nintendo's published hardware artwork. Exact source and extraction masks are documented in [the controller attribution](assets/ui/gamecube-original/README.md). Original Melee fonts and menu glyphs are generated from the player's extracted assets by the documented tools; they are excluded from source control.

FD Link uses original Melee Link geometry, UVs, skeleton and animations with locally authored/generated texture edits inspired by the user-supplied Fierce Deity reference. Texture-paint assistance used OpenAI image generation; the original Link and Fierce Deity designs are Nintendo's. That mod and its artwork are separate downloads and are not part of this source repository or the base distribution. The static portraits and matching stock icons were reconstructed in Blender using the original Melee Link artwork as the pose/lighting reference and the actual corrected skin textures; unsuccessful image-generation attempts were not packaged.

The hosted community service also uses Python, nginx and systemd; these server tools are not bundled game assets.

`licenses/third-party` preserves available exact notices from the pinned build dependencies. The distribution also retains Electron's full Chromium notices and the bundled Python/NumPy/Pillow notices. Source components with copyleft obligations retain their corresponding source and build instructions; a credits list alone does not replace those obligations. The public repository contains source, dependency references, patches, build instructions and notices; Windows preview releases include the matching native runtime and local content-importer tools. No blanket license grant for upstream dependencies is implied.

The flat Controller Options hover pictogram was generated with OpenAI image generation, explicitly referencing Melee Options technical illustrations and the GameCube controller layout. It is packed into a native texture; the original layered panel and animation are loaded from the player disc.

The guarded native m-ex joint-matrix variant is regenerated from Ploaj's `HSD/JObj/ScaleCompensate/MakeMTX.asm` in the pinned m-ex checkout. Its complete scale-compensation behavior and upstream attribution are retained; generated game-derived C remains in ignored local build output. devkitPro/devkitPPC GNU binutils assemble that optional optimization and are not bundled.

The YAMPP wordmark was generated with OpenAI image generation using the user-provided original Melee logo as the typography and color reference. It is fan-project branding and does not imply endorsement by Nintendo or HAL Laboratory.

[vgmstream](https://github.com/vgmstream/vgmstream) contributors: version r2117 provided an independent local HPS decoding oracle for the Akaneia audio check. It is a validation tool, not bundled in YAMPP. Its exact release notices remain with the private validation download.
