# Tracing in Background

This document describes how to run Perfetto in the background, allowing you to
disconnect from the device and collect the trace file later.

## Use Case

Let's say you want to start recording a long-running trace on an Android device
or a Linux server, then terminate your adb/ssh shell and come back later to stop
the tracing session and collect the trace file. This page shows you how to do
this while ensuring the trace remains intact.

To run tracing in the background, use the `--background-wait` argument with the
`perfetto` command. This will daemonize Perfetto (i.e., run it as a background
process) and print its process ID (PID).

NOTE: It's recommended to use `--background-wait` rather than `--background`, as
the former waits for all data sources to be started before exiting. This ensures
that no data is lost at the beginning of the trace.

## Usage

Start recording a trace using `tracebox` or `perfetto`.

```bash
perfetto -c config.cfg --txt -o trace.pftrace --background-wait
```

This will print the pid of the background perfetto process to stdout.

When you are ready to stop tracing, you need to send a `SIGINT` or `SIGTERM`
signal to the background Perfetto process. However, simply killing the process
creates a race condition: the `kill` command returns immediately, but Perfetto
may still be writing the final parts of the trace file to disk.

If you collect the file too soon, it may be incomplete. To prevent this, you
must wait for the `close_write` event on the trace file, which confirms that
Perfetto has finished writing and closed the file. You can achieve this using
platform-specific `inotify` tools.

<?tabs>

TAB: Linux

On Debian Linux we can use `inotifywait` from the `inotify-tools` package.

```bash
kill <pid> && inotifywait -e close_write trace.pftrace
```

TAB: Android

On Android we can use `inotifyd` from toybox.

```sh
kill <pid> && inotifyd - trace.pftrace:w | head -n0
```

</tabs?>
