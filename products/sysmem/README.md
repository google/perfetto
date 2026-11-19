# Sysmem

Sysmem shows where a whole device's memory goes, live: per-process usage,
page cache, swap and memory pressure. To debug the memory of one app, use
[Memscope](../memscope/) instead.

```sh
products/sysmem/run-dev-server   # UI dev server
products/sysmem/build            # UI build into out/sysmem_ui
```

The views live in the `dev.perfetto.Sysmem` UI plugin. It reuses
Memscope's device connection and live sampling, and from its process list
you can profile a single process.
