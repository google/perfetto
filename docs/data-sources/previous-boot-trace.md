# Tracing across reboot

_This data source is supported only on the linux-based systems._

The "linux.frozen_ftrace" data source is used for reading the ftrace
trace data recorded in the previous boot on the persistent ring buffer.

This data source allows you to dump the last seconds of the ftrace
trace data in the previous boot time, for analyzing the system crash
reason from the ftrace trace log.

Therefore, this is expected that the user ran the another perfetto
trace session in background on the special persistent ring buffer.

### Creating a persistent ring buffer

You have to set up a ftrace persistent ring buffer via the kernel
cmdline. If you need a 20MiB persistent ring buffer, you need to
add following kernel options to the kernel cmdline when boot.

```
reserve_mem=20M:2M:trace trace_instance=boot_mapped^traceoff@trace
```

This creates a `boot_mapped` ftrace instance on a reserved memory area,
which will preseve the data and be attatched again in the next boot.
(Note: this is not 100% sure if the kernel configuration has been
 changed or kernel address mapping is changed by KASLR.)

### Use the persistent ring buffer

Normally, perfetto will record the ftrace data in the top level instance
instead of the sub-instances. Thus you need to specify `instance_name:`
option to your trace config. Also, you need to run the trace session as
long-time backend session. What you need are;

- Specify `RING_BUFFER` fill_policy to all buffers which receives ftrace
  data source.
- Specify `instance_name: "boot_mapped"` to the ftrace data source.
  (NOTE: Split the `atrace` data source from this data source, since
   atrace related events can not be used for this instance.)
- Do not specify `duration_ms:`.

And run the perfetto command with `--background` option.

Once you have done it, prepare for a crash.

### Read out the data after crash

After the system crash, you will see the `boot_mapped` instance, which
should keep the trace data recorded in the last seconds.

Run the perfetto with `"linux.frozen_ftrace"` data source like;

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