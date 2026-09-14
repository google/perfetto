# Tracing in Background

This document describes how to run Perfetto in the background, allowing you to
disconnect from the device and collect the trace file later.

## Use Case

You can start a long-running trace on an Android device or Linux server, close
your adb/ssh shell, and return later to stop tracing and collect the trace file.

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

This prints the PID of the background Perfetto process to stdout.

To stop tracing, send a `SIGINT` or `SIGTERM` signal to the background Perfetto
process. However, killing the process creates a race condition: the `kill`
command returns immediately, but Perfetto may still be writing the final parts
of the trace file to disk.

If you collect the file too soon, it may be incomplete. To prevent this, you
must wait for the `close_write` event on the trace file, which confirms that
Perfetto has finished writing and closed the file. You can achieve this using
platform-specific `inotify` tools.

<?tabs>

TAB: Linux

On Debian Linux, use `inotifywait` from the `inotify-tools` package.

```bash
kill <pid> && inotifywait -e close_write trace.pftrace
```

TAB: Android

On Android, use `inotifyd` from toybox.

```sh
kill <pid> && inotifyd - trace.pftrace:w | head -n0
```

</tabs?>
