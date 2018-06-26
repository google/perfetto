# Perfetto UI

Quick Start
-----------
Run:

```
$ git clone https://android.googlesource.com/platform/external/perfetto/
$ cd perfetto
$ tools/install-build-deps --no-android --ui
$ tools/gn gen out/debug --args='is_debug=true is_clang=true'
$ tools/ninja -C out/debug ui
```

For more details on `gn` configs see
[Build Instructions](../docs/build-instructions.md).

To run the tests:
```
$ out/debug/ui_unittests
$ out/debug/ui_tests
```

To run the tests in watch mode:
```
$ out/debug/ui_unittests --watch
```

Finally run:

```
$ ./ui/run-dev-server out/debug
```

and navigate to `localhost:3000`.
