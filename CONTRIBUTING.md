# Contributing

Thanks for helping improve **qeeg-neurofeedback-opensoftware**.

This repository tries to stay:

* **Dependency-light** (standard C++17 + the STL)
* **Portable** (Linux/macOS/Windows)
* **Scriptable** (CLI-first tools, machine-readable JSON/CSV outputs)

## Building

### Using CMake presets

The repo ships with `CMakePresets.json`.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Other useful presets:

* `debug`
* `asan` (Linux/macOS; best-effort)

### Without presets

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQEEG_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Project layout

* `include/qeeg/` — public headers
* `src/` — library + CLI tool implementations
* `tests/` — unit tests
* `cmake/` — CTest helper scripts used for CLI integration tests

## Adding or changing CLI tools

* Put the tool implementation in `src/<name>_cli.cpp` (or `<name>_cli_main.cpp` if the heavy code is shared).
* Wire it into `CMakeLists.txt` under `if (QEEG_BUILD_CLI)`.
* If it should be available in the **offline multicall toolbox** (`qeeg_offline_app_cli`), also add it to the multicall section.

## Tests

* New library functionality should come with a unit test under `tests/`.
* Prefer tests that are deterministic and fast.
* For CLI orchestration/integration, consider adding a small `cmake/test_*.cmake` CTest script.

## Code conventions

### Formatting

The project uses **clang-format** for C/C++ formatting. The repo includes a `.clang-format` file, and CI checks formatting on changed C/C++ files.

* Check (no changes):

```sh
clang-format --style=file --dry-run --Werror path/to/file.cpp
```

* Apply formatting in-place:

```sh
clang-format --style=file -i path/to/file.cpp
```

Tip: clang also ships `git clang-format` to format only lines changed in your branch.


### General

* Keep headers small and self-contained (include what you use).
* Prefer clear error messages (throw `std::runtime_error` with context).
* Avoid undefined behavior and be careful with numeric edge cases (`NaN`, `inf`, empty inputs).

## Reporting security issues

Please see `SECURITY.md`.
