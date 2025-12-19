# R8 Retrace Compatibility Tests

Tests verifying Perfetto's deobfuscation is compatible with R8's retrace functionality.

## Reference Tests (r8.googlesource.com)

- [MethodWithInlinePositionsStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/MethodWithInlinePositionsStackSampleRetraceTest.java) - Commit [5ee95f8](https://r8.googlesource.com/r8/+/5ee95f8cd2f0db48dd985b6c8c6e84c27bfa6000)
- [HorizontalClassMergingStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/HorizontalClassMergingStackSampleRetraceTest.java) - Commit [8b4551b](https://r8.googlesource.com/r8/+/8b4551b8dbce8a594c1ac0eced0e960b5fa94025)
- [MethodWithOverloadStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/MethodWithOverloadStackSampleRetraceTest.java) - Commit [072bfdd](https://r8.googlesource.com/r8/+/072bfdd1cb8ebb90ed84382fe732a76fc7e7d639)
- [MethodWithRemovedArgumentStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/MethodWithRemovedArgumentStackSampleRetraceTest.java) - Commit [180ee7e](https://r8.googlesource.com/r8/+/180ee7ea87ef527d5940719691bd2d6632e57ba0)
- [StaticizedMethodStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/StaticizedMethodStackSampleRetraceTest.java) - Commit [180ee7e](https://r8.googlesource.com/r8/+/180ee7ea87ef527d5940719691bd2d6632e57ba0)
- [VerticalClassMergingStackSampleRetraceTest](https://r8.googlesource.com/r8/+/refs/heads/main/src/test/java/com/android/tools/r8/retrace/stacksamples/VerticalClassMergingStackSampleRetraceTest.java) - Commit [388bc2c](https://r8.googlesource.com/r8/+/388bc2cb759b81313086c4ae8642408077f29620)

Bug: b/460808033
