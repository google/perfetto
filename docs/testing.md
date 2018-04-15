# Testing Perfetto

The testing strategy for Perfetto is rather complex due to the wide variety
of build configurations and embedding targets.

Common test targets (all platforms / checkouts):

`perfetto_unittests`:  
Platform-agnostic unit-tests.

`perfetto_integrationtests`:  
End-to-end tests, involving the protobuf-based IPC transport and ftrace
integration (Linux/Android only).

`perfetto_benchmarks`:  
Benchmarks tracking the performance of: (i) trace writing, (ii) trace readback
and (iii) ftrace raw pipe -> protobuf translation.

Running tests on Linux / MacOS
------------------------------
```
$ tools/ninja -C out/default perfetto_{unittests,integrationtests,benchmarks}
$ out/default/perfetto_unittests --gtest_help
```

`perfetto_integrationtests` requires that the ftrace debugfs directory is
is readable/writable by the current user on Linux:
```
sudo chown  -R $USER /sys/kernel/debug/tracing
```

Running tests on Android
------------------------
1A) Connect a device through `adb`  
1B) Start the build-in emulator (supported on Linux and MacOS):  
```
$ tools/install-build-deps
$ tools/run_android_emulator &
```

2) Run the tests (either on the emulator or physical device):  
```
$ tools/run_android_test out/default perfetto_unittests
```


Continuous testing
------------------
Perfetto is tested in a variety of locations:

**Travis CI**: https://perfetto-ci.appspot.com/  
Builds and runs perfetto_{unittests,integrationtests,benchmarks} from then standalone checkout. Benchmarks are ran in a reduced form for smoke testing.

**Android CI** (see go/apct and go/apct-guide):  
runs only `perfetto_integrationtests`

**Android presubmits (TreeHugger)**:  
Runs before submission of every AOSP CL of `external/perfetto`.


**Android CTS** (Android test suite used run to ensure API compatibility):   Rolling runs internally.

Note that Travis uses the standalone build system and the others build as
part of the Android tree.

Unit tests
----------
Unit tests exist for most of the code in Perfetto on the class level. They
ensure that each class broadly works as expected.

Unit tests are currently ran only on  Travis.
Running unit tests on APCT and Treehugger is WIP.

Integration tests
-----------------
Integration tests ensure that subsystems (importantly ftrace and the IPC layer)
and Perfetto as a whole is working correctly end-to-end.

There are two configurations in which integration tests can be run:

**1. Production mode** (Android-only)  
This mode assumes that both the tracing service (`traced`) and the OS probes
service (`traced_probes`) are already running. In this mode the test enables
only the consumer endpoint and tests the interaction with the production
services. This is the way our Android CTS and APCT tests work.

**2. Standalone mode**:  
Starting up the daemons in the test itself and then testing against them.
This is how standalone builds are tested. This is the only supported way to
run integration tests on Linux and MacOS.

Android CTS tests
-----------------
CTS tests ensure that any vendors who modify Android remain compliant with the
platform API.

These tests include a subset of the integration tests above as well as adding
more complex tests which ensure interaction between platform (e.g. Android apps
etc.) and Perfetto is not broken.

The relevant targets are `CtsPerfettoProducerApp` and `CtsPerfettoTestCases`. Once these are built, the following commands should be run:
```
adb push $ANDROID_HOST_OUT/cts/android-cts/testcases/CtsPerfettoTestCases64 /data/local/tmp/
adb install -r $ANDROID_HOST_OUT/cts/android-cts/testcases/CtsPerfettoProducerApp.apk
```

Next, the app named `android.perfetto.producer` should be run on the device.

Finally, the following command should be run:
```
adb shell /data/local/tmp/CtsPerfettoTestCases64
```

Chromium waterfall
------------------
Coming soon!
