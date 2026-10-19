# TraceParser streaming prototype audit

Open `catch-up.html` for the decisions, measurements and validation, or
`trace-parser-api.html` for the current API sketch. The chronological journal
is `journal.txt`. Historical measurements and logs are retained as evidence;
current hand-written sink measurements are identified explicitly in the report.

Generated `.pftrace` inputs are excluded from version control. Recreate the
current counter workload with:

```sh
python3 streaming-parser-audit/generate-counter-input.py 1000000 /tmp/counters.pftrace
CCACHE_DISABLE=1 tools/ninja -C out/mac_release streaming_parser_benchmark
out/mac_release/streaming_parser_benchmark full /tmp/counters.pftrace
out/mac_release/streaming_parser_benchmark sink /tmp/counters.pftrace
out/mac_release/streaming_parser_benchmark slice /tmp/counters.pftrace
```

`slice` is the diagnostic runner's historical name for the public TraceParser
mode; it automatically streams counters too. `sink` retains storage and emits
checksum output for comparison. These commands require a configured release
output directory. Input generators insert normal tracing-service sorting
checkpoints; the current public API has no Flush method.
