# Linux `perf` symbolization test data

Test data for symbolizing Linux `perf` recordings against ELF files that have
been stripped down with `objcopy --only-keep-debug`. See
https://github.com/google/perfetto/issues/6258.

`--only-keep-debug` zeroes the file size of the executable `PT_LOAD` segment and
shifts its file offset, so the load bias computed from the program headers
(`p_vaddr - p_offset`) no longer matches the original binary. This data lets us
regression-test that traceconv / trace_processor still symbolize such files
correctly.

## Files

- `sdk_example.linux.perf.data` — `perf record` output captured against the
  stripped `sdk_example` executable.
- `bin/sdk_example.linux.debug` — the matching `--only-keep-debug` ELF used as
  the symbol source. Same GNU build ID (`444b6942f9c31518`) as the recorded
  binary, so it is matched by build ID during symbolization.

## How it was generated

Built `//examples/sdk:sdk_example` (a PIE) and ran, on Linux x86-64:

```sh
cp out/.../sdk_example sdk_example.linux

# Strip debug info; this is the binary that actually runs and whose build ID is
# recorded into perf.data.
objcopy --strip-debug sdk_example.linux sdk_example.linux.stripped

# The --only-keep-debug companion: keeps the symbol/debug sections but zeroes
# the allocated segment contents, which is what reproduces the load-bias bug.
objcopy --only-keep-debug sdk_example.linux sdk_example.linux.debug

# Record. (The issue uses `-f`; recent perf spells frequency `-F`.)
perf record --call-graph fp -F 10000 \
  -o sdk_example.linux.perf.data ./sdk_example.linux.stripped
```
