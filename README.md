# dusty
dusty is an experimental redstone simulator written in C, based on Minecraft 1.16.2. It started as a project to learn raylib and later evolved into an effort to combine redstone construction with programming.

The goal is to give users control over circuit inputs, execution backends, and simulation time, enabling faster execution of circuits built entirely from redstone. The computation remains in the circuits themselves, with their logic and in-game timing preserved.


> [!TIP]
> **📖 API Reference**
>
> Explore public functions, grouped by purpose, with concise descriptions.
>
> [**English →**](https://daylighthu.github.io/mcmux/mcmux-api-en.html) · [**简体中文 →**](https://daylighthu.github.io/mcmux/mcmux-api-zh.html)
>

## Build

Use a C compiler to compile `nob.c` into the `nob` build runner. Then run the
`./nob` commands from the project root. On Windows, use the equivalent
`nob.exe` form.

| Command | Purpose |
| --- | --- |
| `./nob` or `./nob build` | Build the project. |

## References

- https://github.com/raysan5/raylib
- https://github.com/MCHPR/MCHPRS
- https://github.com/gnembon/fabric-carpet
- https://github.com/maruohon/tweakeroo
- https://github.com/EngineHub/WorldEdit
- https://github.com/tmux/tmux
- https://github.com/neovim/neovim
- https://github.com/cktan/tomlc17
