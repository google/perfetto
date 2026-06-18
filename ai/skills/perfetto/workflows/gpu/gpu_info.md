# GPU inventory — what GPUs (and machines) is this trace from?

Before any GPU analysis, know what hardware the trace describes: how many GPUs,
which vendor/model/architecture each is, and — for multi-machine captures — which
machine each belongs to. Vendor and architecture in particular decide which
vendor-specific analysis applies (e.g. NVIDIA vs AMD counter sets). **Do not
assume a single GPU or a single machine**: a trace can contain several GPUs, and
a multi-machine capture records a host plus remote machines, each with its own
GPUs.

This information comes from the `gpu` table, which trace_processor builds from
`GpuInfo` trace packets
([gpu\_info.proto](/protos/perfetto/trace/system_info/gpu_info.proto)).

If the user has not yet loaded a trace into `trace_processor`, follow
`../../infra-references/querying.md` first, then come back here.

## Enumerate the GPUs

```bash
trace_processor query --query-file scripts/gpu_info.sql TRACE_FILE
```

Columns: `machine_id`, `is_host`, `ugpu`, `gpu_index`, `vendor`, `name`,
`model`, `architecture`, `uuid`, `pci_bdf`. One row per GPU.

> No rows means the trace has no `GpuInfo` packet — GPU activity (slices /
> counters) may still be present, but vendor/model/architecture are unknown.

## The two GPU identifiers — don't confuse them

- **`ugpu` — host-unique GPU id.** Unique across the *whole* trace, every
  machine included. **This is the join key**: counters (`gpu_counter_track`),
  slices (via `gpu_track`), and the frequency track all reference `ugpu`. Always
  scope and join per `ugpu`.
- **`gpu_index` — the 0-based GPU index within its machine.** *Not* unique across
  machines: machine A's GPU 0 and machine B's GPU 0 are different devices. Use it
  for display, never as a cross-trace key.

For a single-machine trace `ugpu == gpu_index`; for multi-machine they diverge,
which is exactly why downstream analysis keys on `ugpu`.

## Machines

`machine_id` links to the `machine` table; `machine_id = 0` (`is_host = 1`) is
the host/local machine, non-zero ids are remote machines. To identify a machine
further (OS, arch, RAM, CPU count), join the `machine` table:

```sql
SELECT g.ugpu, g.name AS gpu, g.machine_id, m.sysname, m.arch, m.num_cpus
FROM gpu AS g
LEFT JOIN machine AS m ON m.id = g.machine_id
ORDER BY g.machine_id, g.gpu;
```

## Using it

- **Pick the vendor / architecture** for vendor-specific analysis from the
  `vendor` / `architecture` columns (authoritative — read them rather than
  guessing from counter names).
- **Enumerate, then scope.** When a trace has more than one GPU, run the
  per-GPU analyses once per `ugpu` (or filter to the GPU of interest), and label
  results with `name` + `machine_id` so multiple devices stay distinguishable.
- **Extra vendor fields.** `GpuInfo` carries optional `extra_info` key/value
  pairs; they land in the `gpu.arg_set_id` and can be read with
  `EXTRACT_ARG(arg_set_id, '<key>')`.

## Reference

- GPU data sources: <https://perfetto.dev/docs/data-sources/gpu>
- `gpu` table (built from `GpuInfo`): `ugpu` (host-unique), `gpu` (per-machine
  index), `vendor`, `model`, `architecture`, `uuid`, `pci_bdf`, `machine_id`.
- Multi-machine tracing:
  <https://perfetto.dev/docs/deployment/multi-machine-architecture>
