# Perfetto - Performance instrumentation and logging for POSIX platforms

This project is meant to be built both as part of the Android tree and
from a standalone checkout

For internal docs see [this page][internal-docs]


Supported platforms
-------------------
Android is the platform targeted in the first milestones.
Right now Linux desktop and OSX are maintained best-effort.


Get the code
------------
### Prerequisites
All dependent libraries are self-hosted and pulled by the
`tools/install-build-deps` script.  
The only requirements on the host are
python, git and a compiler (preferably clang, gcc is maintained best-effort):  
`$ sudo apt-get update && sudo apt-get install git clang python`

Then:  
`$ git clone https://android.googlesource.com/platform/external/perfetto.git`


Contributing
------------
This project uses [Android AOSP Gerrit][perfetto-gerrit] for code reviews and
uses the [Google C++ style][google-cpp-style].
Currently targets `-std=c++11`.

You can use both `git cl upload` from [Chromium depot tools][depot-tools] or
[Android repo][repo] to upload patches.

`git cl` is quite convenient as it supports code auto-formatting via
`git cl format`.

See https://source.android.com/source/contributing for more details about external contributions and CLA signing.


Build instructions
------------------
### Build from a standalone checkout
If you are a chromium developer and have depot_tools installed you can avoid
the `tools/` prefix below and just use gn/ninja from depot_tools.

`$ tools/install-build-deps` to install third-party build deps (NDK etc)

`$ tools/gn args out/android` to generate build files and enter in the editor:
```
target_os = "android"          # Leave empty for local testing
target_cpu = "arm" or "arm64"  # Only when building for Android
```
(See the [Build Configurations](#build-configurations) section below for more)

`$ tools/ninja -C out/android all`


### Build from the Android tree
TODO. The plan is to autogenerate the Android.bp build files from the master GN build files (or temporarily maintain both until we can autogenerate them).


Run tests
---------
### On the host (Linux / OSX)
```
$ tools/ninja -C out/default (tracing_unittests | tracing_benchmarks)
$ out/default/tracing_unittests --gtest_help
```

### On Android
Either connect a device in [ADB mode][adb-docs] or use the bundled emulator.

To start the emulator:  
`$ tools/run_android_emulator (arm | arm64) &`

To run the tests (either on the emulator or physical device):  
`$ tools/run_android_test out/default tracing_unittests`


Build configurations
--------------------
The following [GN args][gn-quickstart] are supported:

`target_os = "android" | "linux" | "mac"`:  
Defaults to the current host, set "android" to build for Android.

`target_cpu = "arm" | "arm64" | "x86" | "x64"`:  
Defaults to `"arm"` when `target_os` == `"android"`, `"x64"` when targeting the
host. 32-bit host builds are not supported.

`is_debug = true | false`:  
Toggles Debug (default) / Release mode.

`is_clang = true | false`:  
Use Clang (default) / GCC. It requires clang 3.5+ to be installed on the host.
Clang is the default compiler on Mac (% having installed Xcode).
On Linux: `sudo apt-get update && sudo apt-get install clang`

`cc = "gcc" / cxx = "g++"`:  
Uses a different compiler binary (default: autodetected depending on is_clang).

`is_asan = true`:  
Enables [Address Sanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer)

`is_lsan = true`:  
Enables [Leak Sanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizerLeakSanitizer)
(Linux/Mac only)

`is_msan = true`:  
Enables [Memory Sanitizer](https://github.com/google/sanitizers/wiki/MemorySanitizer)
(Linux only)

`is_tsan = true`:  
Enables [Thread Sanitizer](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
(Linux/Mac only)

`is_ubsan = true`:  
Enables [Undefined Behavior Sanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)


[internal-docs]: https://goo.gl/pNTTpC
[perfetto-gerrit]: https://android-review.googlesource.com/q/project:platform%252Fexternal%252Fperfetto+status:open
[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
[depot-tools]: https://dev.chromium.org/developers/how-tos/depottools
[repo]: https://source.android.com/source/using-repo
[gn-quickstart]: https://chromium.googlesource.com/chromium/src/+/lkgr/tools/gn/docs/quick_start.md
[adb-docs]: https://developer.android.com/studio/command-line/adb.html
