---
name: Camera DMA-BUF spike vs pre-existing buffer frees
tags: [android, memory, hard, workflow]
runs: 3
files:
  - src: "{repo}/test/data/camera-ion-mem-trace"
    dst: camera_trace.pftrace
---
I captured a Perfetto trace during camera capture: `./camera_trace.pftrace`.
A teammate summed `buf_size` in `android_dmabuf_allocs` and got about +97 MB,
so they think the camera leaked DMA-BUF memory. Did DMA-BUF memory really grow
that much by the end of the trace, or was the spike transient? Report the peak
and end device-wide DMA-BUF footprint, the actual net change across the trace,
and why the `android_dmabuf_allocs` sum disagrees.
