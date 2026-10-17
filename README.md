# Cubiomes Viewer that uses GPU

This community build keeps the standard Viewer interface and `.session` files. GPU BOOST filters supported search conditions, while Cubiomes checks candidates and handles conditions the GPU backend does not support. The bundled Cubiomes source currently lists Java 26.3 as its newest supported version.

## Download

This project does not have ready-to-run releases yet. The upstream Cubiomes Viewer releases do not include GPU BOOST. Until this project publishes its own Windows and Linux packages, build from source using the [build guide](buildguide.md).

## Have your clanker build it for you (I recommend this its a pain in the cheeks to build)

Give this prompt to your coding assistant:

```text
Check whether this repository is already downloaded. If it is missing, run `git clone --recurse-submodules https://github.com/Henryisdjfunkyho/Cubiomes-Viewer-that-uses-GPU.git`. If it is already downloaded, run `git submodule update --init --recursive` from the repository folder. Read buildguide.md, identify my operating system and GPU, then follow the matching build steps. Reuse tools that are already installed. Ask before installing software or changing drivers. If my GPU or build setup is unsupported, stop and explain what is missing.
```

## Project direction

The plan is to keep one source tree and the current HIP implementation. AMD builds use HIP; NVIDIA builds use HIP compatibility headers with Clang's CUDA frontend. Build steps for both GPU vendors are in the [build guide](buildguide.md). GPU BOOST filters eligible conditions during searches. Cubiomes checks candidates on the CPU and processes conditions the GPU backend does not support. If GPU initialization fails, the Viewer can continue using CPU processing.

Future releases should provide one tested download per operating system, with application libraries packaged alongside the Viewer. GitHub Actions can build those packages after platform checks pass. This source snapshot has no release packages or automated release workflows. Building GPU BOOST requires the vendor toolkit; users of a packaged release should not need development tools. GPU use still requires a compatible graphics driver.

## Build from source

The [build guide](buildguide.md) explains the GPU toolkit requirements and the standard Qt build. The same `qmake` and make workflow builds the Viewer and detected GPU backend.

## License

The Viewer is licensed under GPL-3.0. Cubiomes and other components retain their own license notices. See [LICENSE](LICENSE) and the component license files.

The project links Cubiomes as a Git submodule. Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` after cloning. Its source and license are available in the linked repository.

This is an independent community build. It is not an official Minecraft product and is not approved by or associated with Mojang or Microsoft.
