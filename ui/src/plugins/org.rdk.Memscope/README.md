## org.rdk.Memscope Plugin

This is a fork of the [dev.perfetto.Memscope](../dev.perfetto.Memscope) plugin to
add MessageChannel connection methods and remove the android ones.

The reason to fork it, is because the `dev.perfetto.Memscope` plugin had a
dependency on the `dev.perfetto.RecordTrace2` plugin which we removed.  But
it's a bit of a hack.
