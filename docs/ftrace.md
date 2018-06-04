# Perfetto <-> Ftrace interoperability

*** note
**This doc is WIP**, stay tuned.
<!-- TODO(primiano): write ftrace doc. -->
***

This doc should:
- Describe the ftrace trace_pipe_raw -> protobuf translation.
- Describe how we deal with kernel ABI (in)stability and ftrace fields changing
  over kernel versions (we process `event/**/format files on-device`).
- Describe how to generate ftrace protos (`tools/pull_ftrace_format_files.py`,
  `tools/udate_protos.py`)
- Describe how session multiplexing works.
- Describe the page-by-page scheduling algorithm that uses vmsplice()

Code lives in [/src/traced/probes/ftrace](/src/traced/probes/ftrace/).

From https://android-review.googlesource.com/c/platform/external/perfetto/+/603793/
```

  main thread                           [drain] [unblock]
                                        /:              |
                            post .-----' :              |
                                /        :              v
  worker #0  [splice ...] [wakeup] [block ............] [splice]
                                         :
  worker #1  [splice ...]     [wakeup] [block ........] [splice]
                                         :
  worker #2  [splice ..........................................]
                                         :
                                         :
                                    drain period (100ms)

In other words, the splice(2) system call is used to move data from
the raw kernel ftrace pipe into an intermediate pipe at a page
granularity. This call allows every per-cpu worker to sleep until there
is at least one page of data available.

When a worker wakes up, it will attempt to move as many pages as
possible to its staging pipe (up to 64K, depending on the
system's pipe buffer size) in a non-blocking way. After this, it
will notify the main thread that data is available. This notification
will block the calling worker until the main thread has drained the
data.

When at least one worker has woken up, we schedule a drain operation
on the main thread for the next drain period (every 100ms by default).
The drain operation parses ftrace data from the staging pipes of
every worker having pending data. After this, each waiting worker is
allowed to issue another call to splice(), restarting the cycle.
```
