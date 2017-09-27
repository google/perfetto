# Perfetto - Open-source building blocks for the full performance lifecycle of Google client platforms

Docs
----
See [go/perfetto-one-pager](go/perfetto-one-pager)

Building from a standalone checkout
-----------------------------------

If you are a chromium developer and have depot_tools installed you can avoid
the `build/` prefix below and just use gn/ninja from depot_tools.

- Run `build/install-build-deps` to install third-party build deps (NDK etc)
- Run `build/gn args out/android` to generate build files and enter in the editor:
  ```
  target_os = "android"  # or "linux" for local testing
  target_cpu = "arm"  # or "arm64"
  is_debug = true  # or false for release
  ```
- Run `build/ninja -C out/android all`


Building from the Android tree
------------------------------
TODO. The plan is to autogenerate the Android.bp build files from the master
GN build files (or temporarily maintain both until we can autogenerate them).
Will come in next CLs.
