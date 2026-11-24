# Voxilog

Voxilog is a C workbench for Minecraft Java 1.16.2 redstone. Build circuits in
3D and drive them from C.

## Commands

Run from the repository root.

| Command | What it does |
| --- | --- |
| `voxi list` | List saved worlds. |
| `voxi create NAME` | Create `saves/NAME/NAME.mcw`. |
| `voxi load NAME` | Open a world in 3D. Try `voxi load examples`. |
| `voxi delete NAME` | Permanently delete `saves/NAME/` and its contents. |
| `voxi run-c DRIVER.c` | Compile and run a C source file inside the repository. |
| `voxi export-c DRIVER.c` | Export using the paths and model name in the driver's `// mcmux-model:` line. |
| `voxi export-c INPUT.mcs OUTPUT.c [SYMBOL]` | Export the structure at `INPUT.mcs` to `OUTPUT.c`. The model name `SYMBOL` defaults to `mcmux_circuit`. |
| `voxi --help` | Show command usage. |

Quote world names with spaces, as in `voxi create "Timing Lab"`.

## Example driver

In voxilog, you write your own drivers. You express the behavior you want through the redstone circuit’s own logic, then use a simulation backend to run the circuit faster.

For example:
```sh
voxi export-c saves/examples/carry_cancel_array.c
voxi run-c saves/examples/carry_cancel_array.c
```

This generates `build/carry_cancel_array_model.c` and runs the 16-bit adder.

## Build

On Linux or macOS, build from the repository root. Linux requires X11 and
OpenGL development libraries.

```sh
cc -std=c11 -O2 nob.c -o nob
./nob build
```

On Windows, use Clang with `llvm-windres` or `windres`.

```powershell
clang -std=c11 -O2 nob.c -o nob.exe
.\nob.exe build
```

Add the directory containing `voxi` to your `PATH`.

## Local Minecraft textures

Set an absolute path to your Minecraft Java 1.16.2 client JAR or resource-pack
ZIP in `voxilog/config.toml` under your user configuration directory
(`~/.config` by default on Linux and macOS, `%APPDATA%` on Windows).
`VOXILOG_CONFIG` can point to a different TOML file.

```toml
[assets]
resource_pack = '/home/you/.minecraft/versions/1.16.2/1.16.2.jar'
```

Run `voxi load examples` to see the textures. Voxilog reads supported PNGs
from the archive and uses its bundled textures wherever a replacement is
unavailable. Remove `resource_pack` to return to the bundled look.

## C API reference

[English](https://daylighthu.github.io/voxilog/api-en.html) ·
[简体中文](https://daylighthu.github.io/voxilog/api-zh.html)

## References

- [raylib](https://github.com/raysan5/raylib)
- [MCHPRS](https://github.com/MCHPR/MCHPRS)
- [Carpet](https://github.com/gnembon/fabric-carpet)
- [Tweakeroo](https://github.com/maruohon/tweakeroo)
- [WorldEdit](https://github.com/EngineHub/WorldEdit)
- [Verilog](https://www.verilog.com)
- [nob.h](https://github.com/tsoding/nob.h)
