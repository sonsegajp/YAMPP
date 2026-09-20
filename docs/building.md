# Setup and building YAMPP

## Local distribution

Extract the entire distribution to a writable folder and run **Play YAMPP.cmd**. On first use, Setup asks for your own Melee **NTSC-U 1.02 (GALE01 revision 2)** image. The native extractor accepts ISO/GCM and RVZ/WIA. It validates `main.dol` against SHA-1 `08e0bf20134dfcb260699671004527b2d6bb1a45` and writes extracted files under `data/GALE01`; a different revision is rejected.

Keep `bin`, `config`, `tools` and the launchers together. The launcher creates/uses `user/settings.xml`, a memory card under `user/saves`, and a local renderer cache. **Setup.cmd** can select another image. Windows x64, a Vulkan-capable graphics driver and the Visual C++ x64 runtime are required by the renderer. Workshop additionally requires the .NET 8 runtime for its HSD bridge; the prepared distribution bundles its Python worker and NumPy/Pillow.

Online uses the configured public hostname. Deployment addresses, publisher credentials, installed mods and personal settings are not shipped in source or the base distribution. A private development endpoint configuration is not evidence that the public hostname resolves for every machine; verify the intended service from the target machine before distributing a build.

## Build from source

Use Windows x64 with Python 3.10+, Git, Visual Studio 2022 C++ and Clang tools, CMake 3.25+, MSYS2 MinGW-w64 GCC/libjpeg-turbo/Zstandard, Node/npm, and .NET 8 SDK. Current native scripts expect MinGW under `C:/msys64/mingw64`. Python asset tooling needs NumPy and Pillow. Use your own USA 1.02 uncompressed ISO for the Python extraction step.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
python scripts/extract_disc.py "C:/path/to/your-melee-1.02.iso"
python -m pip install -r tools/modkit/requirements.txt
dotnet build tools/modkit/bridge/ModBridge.csproj -c Release -o build/modkit/bridge
python scripts/extract_embedded.py
python tools/menu_labels.py
python tools/extract_netplay_fonts.py
python scripts/regenerate_game.py
python scripts/build_game.py --hot-reload --output-name YAMPP.exe
```

Bootstrap checks the pinned Melee, Aurora and HSDLib revisions recorded in `config/project.xml` / `dependencies.json`. It refuses to replace a different local checkout. It then runs `scripts/patch_hsdlib.py` to apply the two reviewed HSDLib action-compiler fixes before the bridge build. The patch validates complete original/result hashes, is safe to repeat, and refuses unrelated source changes. For an existing dependency checkout, run `python scripts/patch_hsdlib.py` before rebuilding ModBridge; the exact patch and pinned hashes are under `scripts/patches/hsdlib-action-compiler.*`. Lua is pinned and hash-verified by `scripts/build_lua.py`. Font and label generators read the player's extracted game files; their derived outputs are ignored by Git. Generated game C and its declaration/dispatch files are recreated under `build/native-game/generated`.

Configure and build the matching Aurora DLL from a Visual Studio developer shell. Use the Visual Studio CMake executable if another CMake on PATH does not support its generator:

```powershell
python scripts/patch_aurora_protocol.py
python scripts/patch_aurora_runtime.py
cmake -S . -B build/pc -G "Visual Studio 17 2022" -A x64 -DAURORA_DAWN_PROVIDER=package
cmake --build build/pc --config Release --target melee_aurora_yampp --parallel 4
npm.cmd --prefix tools/modkit/desktop ci
npm.cmd --prefix tools/modkit/desktop run package
```

The game executable and renderer must use the same UI ABI. Keep test builds under a sibling filename while an older executable/DLL is open. Launch a matching runtime profile with `python scripts/project_config.py --launch game --runtime-profile config/<profile>.xml`. Never edit generated C to fix a game behavior; change the emitter/runtime/hooks and regenerate.

The older `scripts/build.ps1` is a renderer/source-audit entry point, not the complete native game build sequence above. A successful renderer check does not demonstrate gameplay correctness.

The optional comparison tools accept `MELEE_DOLPHIN_SOURCE` for an existing Dolphin source/build tree and `RENDERDOC_PATH` for the RenderDoc capture-layer directory. They are not needed to play. For selective-content importer prerequisites and build commands, see [the importer instructions](../tools/modkit/mex_bridge/README.md).

## Native Linux build

The Linux runtime uses POSIX threads/sockets, FFmpeg for custom music, OpenSSL for package-art hashes, and the same Aurora renderer. Use a prepared checkout with the pinned dependencies, your extracted NTSC-U 1.02 data and the generated C/font/label outputs from the source steps above. The current HSD texture-export bridge still requires the Windows/.NET preparation step; its outputs contain your disc-derived pixels and must remain local and ignored by Git.

On Ubuntu, install the build dependencies, then build in separate Linux output folders. This preserves a Windows build in the same checkout:

```sh
sudo apt-get install build-essential cmake ninja-build python3 pkg-config libsdl3-dev libvulkan-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libturbojpeg0-dev libzstd-dev libssl-dev
python3 scripts/build_game.py --jobs 4
cmake -S . -B build/pc-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_DAWN_PROVIDER=package -DAURORA_DAWN_LINKAGE=shared -DAURORA_SDL3_PROVIDER=system -DAURORA_SDL3_LINKAGE=shared -DAURORA_NOD_PROVIDER=package -DAURORA_NOD_LINKAGE=shared
cmake --build build/pc-linux --target melee_aurora_mod --parallel 4
sh run-melee.sh /path/to/your-melee-1.02.iso
```

Online compatibility includes the exact executable hash. Windows and Linux executables therefore cannot share an Online room under the current protocol, even when built from the same source. This integration keeps that check intact.

The runtime and Lua objects go to `build/native-game-linux`; renderer libraries go to `build/pc-linux`. The Linux launcher rebuilds costume metadata with standard-library Python into `build/native-game-linux/costumes.tsv`, and Online helpers continue using that path. `MELEE_COSTUME_REGISTRY` overrides it; the Windows registry cache is preserved. The launcher validates the disc ID/revision and extracted DOL hash. Settings and saves default to `$XDG_DATA_HOME/melee-pc` or `$HOME/.local/share/melee-pc`. Linux's shared input-file locks are advisory: the Online worker also checks file identities and timestamps before room entry/Ready/start, but cannot provide Windows deny-write handle semantics against an unrelated writer racing those checks.

Validation on Ubuntu 26.04 LTS / GCC 15.2.0 under WSL2 compiled and linked both native artifacts and passed the core, costume and process-worker checks. The local boot probe loaded the DOL but found only the llvmpipe software Vulkan adapter; the existing hardware-Vulkan guard rejected it. Linux gameplay, visible output and audio still need validation on a hardware-Vulkan Linux system.

## Checks and local packaging

```powershell
python scripts/check_costumes.py
python tools/modkit/test_community.py
python -m unittest discover -s server -p "test_*.py"
```

Native pair tests and visual captures have separate scripts under `scripts/check_netplay_local.py` and `scripts/check_online_native_ui.py`; inspect `--help` and the recorded [validation](netplay-validation.md). Keep scripted input separate from a manual play window. Validate a real match, original stock colors, additional colors, room joining and actual visible UI.

Stage an installed 64-bit Python 3.10 with NumPy/Pillow using `python scripts/stage_workshop_python.py --source "C:/path/to/Python310"` (see `--help` for options). This produces the isolated `build/distribution-python` worker without copying unrelated site packages. After building the tested pair, assemble a new local folder:

```powershell
python scripts/make_release.py --output dist/YAMPP-local-test --executable build/native-game/YAMPP.exe --renderer build/pc/bin/melee_aurora_yampp.dll --zip
```

The builder refuses an existing output directory, copies an explicit runtime/tool set, verifies native DLL imports and rejects disc, DAT/HPS, save, mod and private-config files. It writes file hashes and a ZIP checksum. It never publishes anything. Public Windows previews are available on the [Releases page](https://github.com/sonsegajp/YAMPP/releases). Their matching source, dependency references, patches and build instructions remain available in this repository.
