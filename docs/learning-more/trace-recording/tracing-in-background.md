# Tracing in Background

This document describes run perfetto in the background and terminate tracing and
collect the trace file later.

## Use Case

Let's say you want to start recording a long running trace on an Android device
or a Linux server, then terminate your adb/ssh shell and come back some time
later to terminate the tracing session and collect the trace file, this page
shows you how to do this and make sure the trace stays in tact.

In order to run the tracing in the background, run `perfetto` / `tracebox` with
the `--background-wait` argument, which will daemonize perfetto and print the
pid of the daemonized process.

Note: It's recommended to use `--background-wait` rather than `--background` as
the former wait for all data sources to be started before exiting.

## Usage

Start recording a trace using `tracebox` or `perfetto`.

```bash
perfetto -c config.cfg --txt -o trace --background-wait
```

This will print the pid of the background perfetto process to stdout.

When we want to kill the process we need to send a `SIGINT` or `SIGTERM` to the
process pointed to by the returned pid. However, `kill` doesn't wait for the
trace file to be written properly so we need to manually wait for the trace file
to be closed to avoid taking an incomplete copy of it, which we can achieve with
various inotify tools (which are platform dependent).

<?tabs>

TAB: Linux

On Debian Linux we can use `inotifywait` from the `inotify-tools` package.

```bash
kill <pid> && inotifywait -e close_write ticker.pftrace
```

TAB: Android

On Android we can use `inotifyd` from toybox.

```sh
kill <pid> && inotifyd - ticker.pftrace:w | head -n0
```

</tabs?>
