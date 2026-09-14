# Capture ftrace data across a reboot

_This data source is supported only on Linux-based systems._

The "linux.frozen_ftrace" data source reads ftrace data recorded during the
previous boot in the persistent ring buffer.

You can dump the last seconds of ftrace data from the previous boot to
investigate a system crash.

This requires another Perfetto trace session to have run in the background using
the persistent ring buffer.

### Creating a persistent ring buffer

Set up an ftrace persistent ring buffer through the kernel command line. For a
20MiB persistent ring buffer, add the following kernel options at boot.

```
reserve_mem=20M:2M:trace trace_instance=boot_mapped^traceoff@trace
```

This creates a `boot_mapped` ftrace instance on a reserved memory area, which
will preserve the data and be attached again on the next boot. (Note: this is
not 100% guaranteed if the kernel configuration has changed or KASLR changes the
kernel address mapping.)

### Use the persistent ring buffer

Perfetto normally records ftrace data in the top-level instance rather than
sub-instances, so you need to specify the `instance_name:` option in your trace
config. You also need to run a long-running background session:

- Specify the `RING_BUFFER` fill_policy for all buffers that receive ftrace
  data.
- Specify `instance_name: "boot_mapped"` to the ftrace data source. (NOTE: Split
  the `atrace` data source from this data source, since atrace related events
  cannot be used for this instance.)
- Do not specify `duration_ms:`.

Run the perfetto command with the `--background` option.

Once the session is running, prepare for a crash.

### Read out the data after crash

After a system crash, you will see the `boot_mapped` instance, which should keep
the trace data recorded in the last seconds.

Run perfetto with the `"linux.frozen_ftrace"` data source:

```
buffers {
  size_kb: 65536
  fill_policy: DISCARD
}

data_sources {
 config {
   name: "linux.frozen_ftrace"
   frozen_ftrace_config {
     instance_name: "boot_mapped"
    }
  }
}

duration_ms: 5000
```