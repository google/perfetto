# Perfetto - Performance instrumentation and logging for POSIX platforms

This project is meant to be built both as part of the Android tree and
from a standalone checkout

For internal docs see [this page][internal-docs]


Supported platforms
-------------------
Android is the platform targeted in the first milestones.
Right now Linux desktop and OSX are maintained best-effort.

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
See [docs/build_instructions.md](docs/build_instructions.md)

Running Perfetto
----------------
See [docs/running_perfetto.md](docs/running_perfetto.md)

Continuous integration
----------------------
Continuous build and test coverage is available at [perfetto-ci.appspot.com](https://perfetto-ci.appspot.com).
Trybots: CLs uploaded to gerrit are automatically submitted to TravisCI
within one minute and made available on the CI page above.
The relevant code lives in the [infra/](infra/) directory.

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


[internal-docs]: https://goo.gl/pNTTpC
[perfetto-gerrit]: https://android-review.googlesource.com/q/project:platform%252Fexternal%252Fperfetto+status:open
[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
[depot-tools]: https://dev.chromium.org/developers/how-tos/depottools
[repo]: https://source.android.com/source/using-repo
[adb-docs]: https://developer.android.com/studio/command-line/adb.html
