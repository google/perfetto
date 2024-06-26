# Perfetto build instructions

The source of truth for the Perfetto codebase lives in AOSP:
https://android.googlesource.com/platform/external/perfetto/

A read-only mirror is also available at https://github.com/google/perfetto .

Perfetto can be built both from the Android tree (AOSP) and standalone.
Standalone builds are meant only for local testing and are not shipped.
Due to the reduced dependencies, the standalone workflow is faster to iterate on
and the suggested way to work on Perfetto, unless you are working on code that
has non-NDK depedencies into Android internals. Profilers and internal HAL/AIDL
dependencies will not be built in the standalone build.

If you are chromium contributor, AOSP is still the place you should send CLs to.
The code inside chromium's
[third_party/perfetto](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/?q=f:third_party%2Fperfetto&ss=chromium)
is a direct mirror of the AOSP repo. The
[AOSP->Chromium autoroller](https://autoroll.skia.org/r/perfetto-chromium-autoroll)
takes care of keeping chromium's DEPS up to date.

## Standalone builds

#### Get the code

```bash
git clone https://android.googlesource.com/platform/external/perfetto/
```

#### Pull dependent libraries and toolchains

```bash
tools/install-build-deps [--android] [--ui] [--linux-arm]
```

`--android` will pull the Android NDK, emulator and other deps required
to build for `target_os = "android"`.

`--ui` will pull NodeJS and all the NPM modules required to build the
Web UI. See the [UI Development](#ui-development) section below for more.

`--linux-arm` will pull the sysroots for cross-compiling for Linux ARM/64.

WARNING: Note that if you're using an M1 or any later ARM Mac, your Python
version should be at least 3.9.1 to work around
[this Python Bug](https://bugs.python.org/issue42704).

#### Generate the build files via GN

Perfetto uses [GN](https://gn.googlesource.com/gn/+/HEAD/docs/quick_start.md)
as primary build system. See the [Build files](#build-files) section below for
more.

```bash
tools/gn args out/android
```

This will open an editor to customize the GN args. Enter:

```python
# Set only when building for Android, omit when building for linux, mac or win.
target_os = "android"
target_cpu = "arm" / "arm64" / "x64"

is_debug = true / false
cc_wrapper = "ccache"             # [Optional] speed up rebuilds with ccache.
```

See the [Build Configurations](#build-configurations) and
[Building on Windows](#building-on-windows) sections below for more.

TIP: If you are a chromium developer and have depot_tools installed you can
avoid the `tools/` prefix below and just use gn/ninja from depot_tools.

#### Build native C/C++ targets

```bash
# This will build all the targets.
tools/ninja -C out/android

# Alternatively, list targets explicitly.
tools/ninja -C out/android \
  traced \                 # Tracing service.
  traced_probes \          # Ftrace interop and /proc poller.
  perfetto \               # Cmdline client.
  trace_processor_shell \  # Trace parsing.
  traceconv                # Trace conversion.
...
```

## Android tree builds

Follow these instructions if you are an AOSP contributor.

The source code lives in [`external/perfetto` in the AOSP tree](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/).

Follow the instructions on https://source.android.com/setup/build/building .

Then:

```bash
mmma external/perfetto
# or
m traced traced_probes perfetto
```

This will generate artifacts `out/target/product/XXX/system/`.

Executables and shared libraries are stripped by default by the Android build
system. The unstripped artifacts are kept into `out/target/product/XXX/symbols`.

## UI development

This command pulls the UI-related dependencies (notably, the NodeJS binary)
and installs the `node_modules` in `ui/node_modules`:

```bash
tools/install-build-deps --ui
```

Build the UI:

```bash
# Will build into ./out/ui by default. Can be changed with --out path/
# The final bundle will be available at ./ui/out/dist/.
# The build script creates a symlink from ./ui/out to $OUT_PATH/ui/.
ui/build
```

Test your changes on a local server using:

```bash
# This will automatically build the UI. There is no need to manually run
# ui/build before running ui/run-dev-server.
ui/run-dev-server
```

Navigate to http://localhost:10000/ to see the changes.

The server supports live reloading of CSS and TS/JS contents. Whenever a ui
source file is changed it, the script will automatically re-build it and show a
prompt in the web page.

UI unit tests are located next to the functionality being tested, and have
`_unittest.ts` or `_jsdomtest.ts` suffixes. The following command runs all unit
tests:

```bash
ui/run-unittests
```

This command will perform the build first; which is not necessary if you
already have a development server running. In this case, to avoid interference
with the rebuild done by development server and to get the results faster, you
can use

```bash
ui/run-unittests --no-build
```

to skip the build steps.

Script `ui/run-unittests` also supports `--watch` parameter, which would
restart the testing when the underlying source files are changed. This can be
used in conjunction with `--no-build`, and on its own as well.

### Formatting & Linting

We use `eslint` to lint TypeScript and JavaScript, and `prettier` to format
TypeScript, JavaScript, and SCSS.

To auto-format all source files, run ui/format-sources, which takes care of
running both prettier and eslint on the changed files:

```bash
# By default it formats only files that changed from the upstream Git branch
# (typicaly origin/main).
# Pass --all for formatting all files under ui/src
ui/format-sources
```

For VSCode users, we recommend using the eslint & prettier extensions to handle
this entirely from within the IDE. See the
[Useful Extensions](#useful-extensions) section on how to set this up.

Presubmit checks require no formatting or linting issues, so fix all issues
using the commands above before submitting a patch.

## Build files

The source of truth of our build file is in the BUILD.gn files, which are based
on [GN][gn-quickstart].
The Android build file ([Android.bp](/Android.bp)) is autogenerated from the GN
files through `tools/gen_android_bp`, which needs to be invoked whenever a
change touches GN files or introduces new ones.
Likewise, the Bazel build file ([BUILD](/BUILD)) is autogenerated through the
`tools/gen_bazel` script.

A presubmit check checks that the Android.bp is consistent with GN files when
submitting a CL through `git cl upload`.

The generator has a list of root targets that will be translated into the
Android.bp file. If you are adding a new target, add a new entry to the
`default_targets` variable in [`tools/gen_android_bp`](/tools/gen_android_bp).

## Supported platforms

**Linux desktop** (Debian Testing/Rodete)

- Hermetic clang + libcxx toolchain (both following chromium's revisions)
- GCC-7 and libstdc++ 6
- Cross-compiling for arm and arm64 (more below).

**Android**

- Android's NDK r15c (using NDK's libcxx)
- AOSP's in-tree clang (using in-tree libcxx)

**Mac**

- XCode 9 / clang (maintained best-effort).

**Windows**

- Windows 10 with either MSVC 2019 or clang-cl (maintained best-effort).

### Building on Windows

Building on Windows is possible using both the MSVC 2019 compiler (you don't
need the full IDE, just the build tools) or the LLVM clang-cl compiler.

The Windows support in standalone builds has been introduced in v16 by
[r.android.com/1711913](https://r.android.com/1711913).

clang-cl support is more stable because that build configuration is actively
covered by the Chromium project (Perfetto rolls into chromium and underpins
chrome://tracing). The MSVC build is maintained best-effort.

The following targets are supported on Windows:

- `trace_processor_shell`: the trace importer and SQL query engine.
- `traceconv`: the trace conversion tool.
- `traced` and `perfetto`: the tracing service and cmdline client. They use an
  alternative implementation of the [inter-process tracing protocol](/docs/design-docs/api-and-abi.md#tracing-protocol-abi)
  based on a TCP socket and named shared memory. This configuration is only for
  testing / benchmarks and is not shipped in production.
  Googlers: see [go/perfetto-win](http://go/perfetto-win) for details.
- `perfetto_unittests` / `perfetto_integrationtests`: although they support only
  the subset of code that is supported on Windows (e.g. no ftrace).

It is NOT possible to build the Perfetto UI from Windows.

#### Prerequisites

You need all of these both for MSVC and clang-cl:

- [Build Tools for Visual Studio 2019](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2019)
- [Windows 10 SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk/)
- [Python 3](https://www.python.org/downloads/windows/)

The [`win_find_msvc.py`](/gn/standalone/toolchain/win_find_msvc.py) script will
locate the higest version numbers available from
`C:\Program Files (x86)\Windows Kits\10` and
`C:\Program Files (x86)\Microsoft Visual Studio\2019`.

#### Pull dependent libraries and toolchains

```bash
# This will download also the LLVM clang-cl prebuilt used by chromium.
python3 tools/install-build-deps
```

#### Generate build files

```bash
python3 tools/gn gen out/win
```

In the editor type:

```bash
is_debug = true | false

is_clang = true  # Will use the hermetic clang-cl toolchain.
# or
is_clang = false  # Will use MSVC 2019.
```

#### Build

```bash
python3 tools/ninja -C out/win perfetto traced trace_processor_shell
```

### Cross-compiling for Linux ARM/64

When cross-compiling for Linux you will need a sysroot. You have two options:

#### 1. Use the built-in sysroots based on Debian Sid

```bash
tools/install-build-deps --linux-arm
```

Then set the following GN args:

```python
target_os = "linux"
target_cpu = "arm"
# or
target_cpu = "arm64"
```

#### 2. Use your own sysroot

In this case you need to manually specify the sysroot location and the
toolchain prefix triplet to use.

```python
target_os = "linux"
target_sysroot = "/path/to/sysroot"
target_triplet = "aarch64-linux-gnu"  # Or any other supported triplet.
```

For more details see the [Using cutom toolchains](#custom-toolchain) section
below.

## Build configurations

TIP: `tools/setup_all_configs.py` can be used to generate out/XXX folders for
most of the supported configurations.

The following [GN args][gn-quickstart] are supported:

`target_os = "android" | "linux" | "mac"`:

Defaults to the current host, set "android" to build for Android.

`target_cpu = "arm" | "arm64" | "x64"`

Defaults to `"arm"` when `target_os` == `"android"`, `"x64"` when targeting the
host. 32-bit host builds are not supported.
Note: x64 here really means x86_64. This is to keep it consistent with
Chromium's choice, which in turn follows Windows naming convention.

`is_debug = true | false`

Toggles Debug (default) / Release mode. This affects, among other things:
(i) the `-g` compiler flag; (ii) setting/unsetting `-DNDEBUG`; (iii) turning
on/off `DCHECK` and `DLOG`.
Note that debug builds of Perfetto are sensibly slower than release versions. We
strongly encourage using debug builds only for local development.

`is_clang = true | false`

Use Clang (default: true) or GCC (false).
On Linux, by default it uses the self-hosted clang (see `is_hermetic_clang`).
On Android, by default it uses clang from the NDK (in `buildtools/ndk`).
On Mac, by default it uses the system version of clang (requires Xcode).
See also the [custom toolchain](#custom-toolchain) section below.

`is_hermetic_clang = true | false`

Use bundled toolchain from `buildtools/` rather than system-wide one.

`non_hermetic_clang_stdlib = libc++ | libstdc++`

If `is_hermetic_clang` is `false`, sets the `-stdlib` flag for clang
invocations. `libstdc++` is default on Linux hosts and `libc++` is
default everywhere else.

`cc = "gcc" / cxx = "g++"`

Uses a different compiler binary (default: autodetected depending on is_clang).
See also the [custom toolchain](#custom-toolchain) section below.

`cc_wrapper = "tool_name"`

Prepends all build commands with a wrapper command. Using `"ccache"` here
enables the [ccache](https://github.com/ccache/ccache) caching compiler,
which can considerably speed up repeat builds.

`is_asan = true`

Enables [Address Sanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer)

`is_lsan = true`

Enables [Leak Sanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizerLeakSanitizer)
(Linux/Mac only)

`is_msan = true`

Enables [Memory Sanitizer](https://github.com/google/sanitizers/wiki/MemorySanitizer)
(Linux only)

`is_tsan = true`

Enables [Thread Sanitizer](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
(Linux/Mac only)

`is_ubsan = true`

Enables [Undefined Behavior Sanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)

### {#custom-toolchain} Using custom toolchains and CC / CXX / CFLAGS env vars

When building Perfetto as part of some other build environment it might be
necessary to switch off all the built-in toolchain-related path-guessing scripts
and manually specify the path of the toolchains.

```python
# Disable the scripts that guess the path of the toolchain.
is_system_compiler = true

ar = "/path/to/ar"
cc = "/path/to/gcc-like-compiler"
cxx = "/path/to/g++-like-compiler"
linker = ""  # This is passed to -fuse-ld=...
```

If you are using a build system that keeps the toolchain settings in
environment variables, you can set:

```python
is_system_compiler = true
ar="${AR}"
cc="${CC}"
cxx="${CXX}"
```

`is_system_compiler = true` can be used also for cross-compilation.
In case of cross-compilation, the GN variables have the following semantic:
`ar`, `cc`, `cxx`, `linker` refer to the _host_ toolchain (sometimes also called
_build_ toolchain). This toolchain is used to build: (i) auxiliary tools
(e.g. the `traceconv` conversion util) and (ii) executable artifacts that are
used during the rest of the build process for the target (e.g., the `protoc`
compiler or the `protozero_plugin` protoc compiler plugin).

The cross-toolchain used to build the artifacts that run on the device is
prefixed by `target_`: `target_ar`, `target_cc`, `target_cxx`, `target_linker`.

```python
# Cross compilation kicks in when at least one of these three variables is set
# to a value != than the host defaults.

target_cpu = "x86" | "x64" | "arm" | "arm64"
target_os = "linux" | "android"
target_triplet =  "arm-linux-gnueabi" | "x86_64-linux-gnu" | ...
```

When integrating with GNU Makefile cross-toolchains build environments, a
typical mapping of the corresponding environment variables is:

```python
ar="${BUILD_AR}"
cc="${BUILD_CC}"
cxx="${BUILD_CXX}"
target_ar="${AR}"
target_cc="${CC}"
target_cxx="${CXX}"
```

It is possible to extend the set of `CFLAGS` and `CXXFLAGS` through the
`extra_xxxflags` GN variables as follows. The extra flags are always appended
(hence, take precedence) to the set of flags that the GN build files generate.

```python
# These apply both to host and target toolchain.
extra_cflags="${CFLAGS}"
extra_cxxflags="${CXXFLAGS}"
extra_ldflags="${LDFLAGS}"

# These apply only to the host toolchain.
extra_host_cflags="${BUILD_CFLAGS}"
extra_host_cxxflags="${BUILD_CXXFLAGS}"
extra_host_ldflags="${BUILD_LDFLAGS}"

# These apply only to the target toolchain.
extra_target_cflags="${CFLAGS}"
extra_target_cxxflags="${CXXFLAGS} ${debug_flags}"
extra_target_ldflags="${LDFLAGS}"
```

[gn-quickstart]: https://gn.googlesource.com/gn/+/master/docs/quick_start.md

## IDE setup

Use a following command in the checkout directory in order to generate the
compilation database file:

```bash
tools/gn gen out/default --export-compile-commands
```

After generating, it can be used in CLion (File -> Open -> Open As Project),
Visual Studio Code with C/C++ extension and any other tool and editor that
supports the compilation database format.

#### Useful extensions

If you are using VS Code we suggest the following extensions:

- [Clang-Format](https://marketplace.visualstudio.com/items?itemName=xaver.clang-format)
- [C/C++](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools)
- [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
- [Native Debug](https://marketplace.visualstudio.com/items?itemName=webfreak.debug)
- [GNFormat](https://marketplace.visualstudio.com/items?itemName=persidskiy.vscode-gnformat)
- [ESlint](https://marketplace.visualstudio.com/items?itemName=dbaeumer.vscode-eslint)
- [markdownlint](https://marketplace.visualstudio.com/items?itemName=DavidAnson.vscode-markdownlint)
- [Prettier](https://marketplace.visualstudio.com/items?itemName=esbenp.prettier-vscode)

#### Useful settings

In `.vscode/settings.json`:

```json
{
  "C_Cpp.clang_format_path": "${workspaceRoot}/buildtools/mac/clang-format",
  "C_Cpp.clang_format_sortIncludes": true,
  "files.exclude": {
    "out/*/obj": true,
    "out/*/gen": true,
  },
  "clangd.arguments": [
    "--compile-commands-dir=${workspaceFolder}/out/mac_debug",
    "--completion-style=detailed",
    "--header-insertion=never"
  ],
  "eslint.workingDirectories": [
    "./ui",
  ],
  "prettier.configPath": "ui/.prettierrc.yml",
  "typescript.preferences.importModuleSpecifier": "relative",
  "[typescript]": {
    "editor.defaultFormatter": "esbenp.prettier-vscode"
  },
  "[scss]": {
    "editor.defaultFormatter": "esbenp.prettier-vscode"
  },
}
```

Replace `/mac/` with `/linux64/` on Linux.

### Debugging with VSCode

Edit `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "request": "launch",
      "type": "cppdbg",
      "name": "Perfetto unittests",
      "program": "${workspaceRoot}/out/mac_debug/perfetto_unittests",
      "args": ["--gtest_filter=TracingServiceImplTest.StopTracingTriggerRingBuffer"],
      "cwd": "${workspaceFolder}/out/mac_debug",
      "MIMode": "lldb",
    },
  ]
}
```

Then open the command palette `Meta`+`Shift`+`P` -> `Debug: Start debugging`.
