# Build tools

The project and its releases hold no third-party programs. The first build downloads the tools it needs from their official
pages, about 330 MB, once. After that they are unpacked in `engine\ps2-native-recomp\tools` and `third_party`, and no build
downloads anything again.

## Building without internet (or without downloading again)

Save these nine files under the names below into one of these folders. The launcher and the engine use them instead of
downloading:

- `engine\ps2-native-recomp\downloads` (where the engine looks)
- a `downloads` or `build tools` folder next to `RawDangerPC.exe`, or one folder above it (the launcher copies them into the
  engine's folder before a build)
- the `downloads` folder of another copy of the engine near this one

Each file is checked against its SHA-256 before use: a wrong or damaged file is skipped and downloaded again. In the launcher,
**Build tools...** (on the MODERN tab and on the LEGACY options page) shows which files are already on the PC, with their
links, and opens the folder.

| Tool | File | Official link |
| --- | --- | --- |
| llvm-mingw 20260908 (the C++ compiler, 182 MB) | `llvm-mingw-20260908-ucrt-x86_64.zip` | https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-x86_64.zip |
| CMake 4.4.4 (52 MB) | `cmake-4.4.4-windows-x86_64.zip` | https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip |
| Ninja 1.13.2 | `ninja-win.zip` | https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip |
| Python 3.13.16, embeddable (11 MB) | `python-3.13.16-embed-amd64.zip` | https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip |
| MinGit 2.56.0.2 (bash and the Unix tools, 38 MB) | `MinGit-2.56.0.2-64-bit.zip` | https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-64-bit.zip |
| BusyBox for Windows FRP-6075 | `busybox-w64u-FRP-6075-g169694ebd.exe` | https://frippery.org/files/busybox/busybox-w64u-FRP-6075-g169694ebd.exe |
| wgpu-native v29.0.1.1 (the GPU library) | `wgpu-windows-x86_64-gnu-release.zip` | https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-windows-x86_64-gnu-release.zip |
| SDL2 2.32.10 (window, sound, controllers) | `SDL2-devel-2.32.10-mingw.zip` | https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip |
| DirectX Shader Compiler v1.9.2609 | `dxc_2026_09_29.zip` | https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip |

The versions and SHA-256 sums are pinned in `engine\ps2-native-recomp\scripts\portable.ps1` and the `scripts\fetch_*.sh` files.

## Why the files are not in the releases

Each tool has its own licence. MinGit and BusyBox are GPL, which requires their source to be offered next to them. Linking to
the official pages keeps every file with its licence and source, and lets the SHA-256 checks prove it is the same file. A
"tools pack" that you put together yourself from these links, for your own PCs, works the same way.
