# Running Perfetto tests

Perfetto has two main test targets: `perfetto_unittests` and
`perfetto_integrationtests`. These can either be built using standalone or in
the Android tree; we provide instructions for running both below. CTS tests can
also be run when building in the Android tree; they cannot be built using
standalone.

First, ennsure that you can build Perfetto (see [build
instructions](build_instructions.md)). On Android, also setup the service and
the kernel event producer (see [running Perfetto](running_perfetto.md))

## Standalone build
On Linux and Mac, out/$target/perfetto_unittests and
out/$target/perfetto_integrationtests can be run directly.

On Android, the following commands should be run (with the appropriate target in
curly braces depending on which tests suites are to be run):
```
adb push out/$target/perfetto_{unittests, integrationtests} /data/local/tmp/
adb shell out/$target/perfetto_{unittests, integrationtests}.
```

# Inside the Android tree
Unit tests, once built, can be run using the following commands:
```
adb push $OUT/data/nativetest/perfetto_unittests/perfetto_unittests /data/local/tmp/
adb shell /data/local/tmp/perfetto_unittests
```

Integration tests, once built, can be run using the following commands:
```
adb push $OUT/data/nativetest/perfetto_integrationtests/perfetto_integrationtests /data/local/tmp/
adb shell /data/local/tmp/perfetto_integrationtests
```

Building in the Android tree also allows building of CTS tests. The relevant
targets are `CtsPerfettoProducerApp` and `CtsPerfettoTestCases`. Once these are
built, the following commands should be run (to manually run these tests):
```
adb push $ANDROID_HOST_OUT/cts/android-cts/testcases/CtsPerfettoTestCases64 /data/local/tmp/
adb install -r $ANDROID_HOST_OUT/cts/android-cts/testcases/CtsPerfettoProducerApp.apk
```
Next, the app with name 'android.perfetto.producer' should be run on device.
Finally, the following command should be run:
```
adb shell /data/local/tmp/CtsPerfettoTestCases64
```
