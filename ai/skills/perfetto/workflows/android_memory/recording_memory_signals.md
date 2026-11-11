---
requires: [record]
---
# Recording the Missing Memory Signals

Ask the user to record a new trace on the device while reproducing the same
scenario, with
[`android_memory.pftxt`]($SKILL_ROOT/infra-references/example-configs/android_memory.pftxt),
which enables every signal in the table in Step 1 of
[system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md):

```bash
curl -O https://raw.githubusercontent.com/google/perfetto/main/tools/record_android_trace
chmod +x record_android_trace
./record_android_trace \
  -c "$SKILL_ROOT/infra-references/example-configs/android_memory.pftxt" \
  -o memory.perfetto-trace
```

Raise `duration_ms` in a copy of the config so the trace covers the whole
scenario. To add only what is missing to the user's own config instead, merge
in the entries from the table (one `linux.ftrace` block per config; see
[trace_config_reference.md]($SKILL_ROOT/infra-references/trace_config_reference.md)).

Then run Step 1 of system_trace_memory.md again on the new trace. If the
user cannot re-record, continue with the signals that are present and state
which conclusions the missing ones leave open.
