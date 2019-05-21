# Changes from Android Q

## New features
* Allow to specify whether profiling should only be done for existing processes
  or only for newly spawned ones using `no_startup` or `no_running` in
  `HeapprofdConfig`.
* Allow to get the number of bytes that were allocated at a callstack but then
  not used.

## Bugfixes
