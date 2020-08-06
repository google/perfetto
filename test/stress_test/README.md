# Perfetto Stress Test

This is a test harness that to stress test the client library (DataSource-level
only for now).

The test is based on a number of configs in /test/stress_test/configs/*.cfg
(NOTE: they must be listed in configs/BUILD.gn).
The config is a /protos/perfetto/config/stress_test_config.proto message, which
embeds the configuration of the test and a whole trace config.

Each configs defines a testing scenario, determining the general trace config
and all the settings of the test (e.g., how many producer processes to spawn,
the write timings).

The test is based on exec()-ing `traced` (the tracing service), `perfetto` (the
consumer cmdline client) and a variable number of `stress_producer` instances.

`stress_producer` emits events at a configurable rate, writing predictable
sequences of numbers / string, so that the test harness can easily detect
corruptions, out-of-order events or gaps.

After running each test, the `stress_test` binary reads back the trace and
performs a bunch of checks:

- Checks that the number of sequences is exactly equal to #processes x #threads.
- Checks that each sequence has all the expected packets in the right sequence
- Checks the payload and correctness of proto nesting of each trace packet.
- Reports CPU/Memory/Context-switch numbers for the service and producer
  processes.

Each test config is isolated from the others. All processes are killed and
re-spawned for each test.

The stdout/err of each process is saved in a dedicated /tmp/ folder, as well as
the resulting trace.

## Building and running the test

```bash
# This will recursively build traced, perfetto and stress_producer.
ninja -C out/default stress_test

out/default/stress_test
```

will output:

```txt
[307.909] stress_test.cc:116      Saving test results in /tmp/perfetto-ltIBJgA0

===============================================================
Config: simple
===============================================================
Metric               Expected   Actual
------               --------   ------
#Errors              0          0
Duration [ms]        3000       3109
Num threads          1          1
Num packets          1000       1001
Trace size [KB]      168        170
Svc RSS [MB]         4          2
Prod RSS [MB]        ---        1
Svc CPU [ms]         ---        10
Prod CPU [ms]        ---        32
Svc #ctxswitch       ---        103 / 20
Prod #ctxswitch      ---        1022 / 1

===============================================================
Config: bursts
===============================================================
Metric               Expected   Actual
------               --------   ------
#Errors              0          0
Duration [ms]        2000       2381
Num threads          10         10
Num packets          2675       20021
Trace size [KB]      449        11063
Svc RSS [MB]         32         17
Prod RSS [MB]        ---        1
Svc CPU [ms]         ---        98
Prod CPU [ms]        ---        17
Svc #ctxswitch       ---        704 / 1327
Prod #ctxswitch      ---        421 / 1
```

```bash
$ ls -Rlh /tmp/perfetto-ltIBJgA0
total 0
drwxr-xr-x  16 primiano  wheel   512B  5 Aug 09:16 bursts
drwxr-xr-x   9 primiano  wheel   288B  5 Aug 09:16 simple
drwxr-xr-x  38 primiano  wheel   1.2K  5 Aug 09:16 the_storm

/tmp/perfetto-ltIBJgA0/bursts:
total 22752
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 errors.log
-rw-r--r--  1 primiano  wheel   180B  5 Aug 09:16 perfetto.log
-rw-r--r--  1 primiano  wheel   441B  5 Aug 09:16 producer.0.log
...
-rw-r--r--  1 primiano  wheel   441B  5 Aug 09:16 producer.9.log
-rw-------  1 primiano  wheel    11M  5 Aug 09:16 trace
-rw-r--r--  1 primiano  wheel   407B  5 Aug 09:16 traced.log

/tmp/perfetto-ltIBJgA0/simple:
total 400
srwxr-xr-x  1 primiano  wheel     0B  5 Aug 09:16 consumer.sock
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 errors.log
-rw-r--r--  1 primiano  wheel   178B  5 Aug 09:16 perfetto.log
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 producer.0.log
srwxr-xr-x  1 primiano  wheel     0B  5 Aug 09:16 producer.sock
-rw-------  1 primiano  wheel   167K  5 Aug 09:16 trace
-rw-r--r--  1 primiano  wheel   406B  5 Aug 09:16 traced.log

/tmp/perfetto-ltIBJgA0/the_storm:
total 524432
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 errors.log
-rw-r--r--  1 primiano  wheel   184B  5 Aug 09:16 perfetto.log
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 producer.0.log
...
-rw-r--r--  1 primiano  wheel     0B  5 Aug 09:16 producer.127.log
-rw-------  1 primiano  wheel   248M  5 Aug 09:16 trace
-rw-r--r--  1 primiano  wheel   408B  5 Aug 09:16 traced.log
```

## TODOs

The following scenarios requires more coverage:

- Nested messages.
- Force losses and check that the last_dropped flag is consistent.
- Flushes and scraping.
- Report data losses in the test output.
- Multibuffer scenarios.
- write_into_file=true.
- Vary page size, smb size.
