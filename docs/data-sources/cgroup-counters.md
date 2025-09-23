# Cgroup Counters

This document describes the cgroup statistics tracking feature in Perfetto, which enables monitoring of Linux cgroup subsystem resource usage for specific task groups.

## Overview

Cgroup tracking allows you to monitor resource usage for specific task groups, which is particularly useful for performance analysis in Android systems. By polling cgroup statistics, you can track:

- **CPU Usage**: CPU time allocation between foreground/background apps
- **Memory Usage**: Memory consumption patterns for different task groups  
- **I/O Statistics**: Disk read/write activity categorized by task groups

## Supported Cgroup Counters

### CPU Statistics (cpu.stat)
- `CGROUP_CPU_USAGE_USEC`: Total CPU usage time in microseconds
- `CGROUP_CPU_USER_USEC`: User-mode CPU time in microseconds
- `CGROUP_CPU_SYSTEM_USEC`: Kernel-mode CPU time in microseconds
- `CGROUP_CPU_NR_PERIODS`: Number of CFS scheduling periods
- `CGROUP_CPU_NR_THROTTLED`: Number of times throttled
- `CGROUP_CPU_THROTTLED_USEC`: Time spent throttled in microseconds

### Memory Statistics (memory.stat)
- `CGROUP_MEMORY_ANON`: Anonymous memory pages
- `CGROUP_MEMORY_FILE`: File cache pages
- `CGROUP_MEMORY_ACTIVE_ANON`: Active anonymous memory
- `CGROUP_MEMORY_INACTIVE_ANON`: Inactive anonymous memory
- `CGROUP_MEMORY_ACTIVE_FILE`: Active file cache
- `CGROUP_MEMORY_INACTIVE_FILE`: Inactive file cache
- `CGROUP_MEMORY_PGFAULT`: Page fault count
- `CGROUP_MEMORY_PGMAJFAULT`: Major page fault count

### Memory Limits
- `CGROUP_MEMORY_CURRENT`: Current memory usage
- `CGROUP_MEMORY_MAX`: Memory usage limit
- `CGROUP_MEMORY_SWAP_CURRENT`: Current swap usage
- `CGROUP_MEMORY_SWAP_MAX`: Swap usage limit

### I/O Statistics (io.stat)
- `CGROUP_IO_RBYTES`: Bytes read
- `CGROUP_IO_WBYTES`: Bytes written
- `CGROUP_IO_RIOS`: Read operations count
- `CGROUP_IO_WIOS`: Write operations count
- `CGROUP_IO_DBYTES`: Discarded bytes
- `CGROUP_IO_DIOS`: Discard operations count

## Configuration

Add the following to your trace config:

```protobuf
data_sources: {
    config {
        name: "linux.sys_stats"
        sys_stats_config {
            # Poll cgroup stats every 100ms
            cgroup_period_ms: 100
            
            # Cgroup paths to monitor
            cgroup_paths: "/sys/fs/cgroup/cpu/top-app"
            cgroup_paths: "/sys/fs/cgroup/memory/foreground"
            cgroup_paths: "/sys/fs/cgroup/cpu/background"
            
            # Counters to collect
            cgroup_counters: CGROUP_CPU_USAGE_USEC
            cgroup_counters: CGROUP_MEMORY_ANON
            cgroup_counters: CGROUP_IO_RBYTES
        }
    }
}
```

## Common Android Cgroup Paths

- `/sys/fs/cgroup/cpu/top-app`: Foreground applications
- `/sys/fs/cgroup/cpu/foreground`: Foreground services
- `/sys/fs/cgroup/cpu/background`: Background tasks
- `/sys/fs/cgroup/cpu/system-background`: System background tasks
- `/sys/fs/cgroup/memory/top-app`: Foreground app memory group
- `/sys/fs/cgroup/memory/foreground`: Foreground service memory group
- `/sys/fs/cgroup/memory/background`: Background task memory group

## Use Cases

1. **App Performance Analysis**: Compare resource usage between foreground and background apps
2. **System Optimization**: Identify task groups with highest resource consumption
3. **Memory Leak Detection**: Monitor memory usage trends for specific app groups
4. **I/O Performance Analysis**: Analyze disk activity patterns across different task groups

## Implementation Details

- All polling periods must be ≥ 10ms to avoid excessive CPU usage
- If no cgroup paths are specified, Android default paths are used
- Supports both cgroup v1 and v2 filesystem formats
- Statistics include full cgroup paths for analysis-time differentiation

## Performance Impact

Cgroup polling overhead is minimal:
- File reading: < 0.5ms per cgroup path
- Parsing and injection: < 0.1ms per counter
- Recommended polling interval: 100-1000ms depending on analysis needs
