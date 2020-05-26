# System calls

On Linux and Android (userdebug builds only) Perfetto can keep track of system
calls.

Right now only the syscall number is recorded in the trace, the arguments are
not stored to limit the trace size overhead.

At import time, the Trace Processor uses an internal syscall mapping table,
currently supporting x86, x86_64, ArmEabi, aarch32 and aarch64. These tables are
generated through the
[`extract_linux_syscall_tables`](/tools/extract_linux_syscall_tables) script.

## UI

At the UI level system calls are shown inlined with the per-thread slice tracks:

![](/docs/images/syscalls.png "System calls in the thread tracks")

## SQL

At the SQL level, syscalls are no different than any other userspace slice
event. They get interleaved in the per-thread slice stack and can be easily
filtered by looking for the 'sys_' prefix:

```sql
select ts, dur, t.name as thread, s.name, depth from slices as s
left join thread_track as tt on s.track_id = tt.id
left join thread as t on tt.utid = t.utid
where s.name like 'sys_%'
```

ts | dur | thread | name 
---|-----|--------|------
856325324372751 | 439867648 | s.nexuslauncher | sys_epoll_pwait
856325324376970 | 990 | FpsThrottlerThr | sys_recvfrom
856325324378376 | 2657 | surfaceflinger | sys_ioctl
856325324419574 | 1250 | android.anim.lf | sys_recvfrom
856325324428168 | 27344 | android.anim.lf | sys_ioctl
856325324451345 | 573 | FpsThrottlerThr | sys_getuid

## TraceConfig

```protobuf
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "raw_syscalls/sys_enter"
            ftrace_events: "raw_syscalls/sys_exit"
        }
    }
}
```
