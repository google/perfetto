-- GPU inventory — every GPU in the trace, across every machine.
--
-- A trace can describe more than one GPU, and more than one machine (a
-- multi-machine setup records a host plus one or more remote machines, each
-- with its own GPUs). This lists them all so downstream analysis can scope to a
-- specific device. The data comes from the `gpu` table, which trace_processor
-- builds from GpuInfo trace packets (protos/perfetto/trace/system_info/
-- gpu_info.proto).
--
-- Two GPU identifiers exist and must not be confused:
--   ugpu  : host-unique GPU id. Unique across the WHOLE trace (all machines).
--           This is the join key — counters, slices and tracks reference ugpu.
--   gpu_index : the 0-based GPU index WITHIN its machine (from the `gpu` table's
--           `gpu` column). NOT unique across machines (machine A's index 0 and
--           machine B's index 0 are different devices).
-- machine_id links to the `machine` table; machine_id 0 is the host/local
-- machine, non-zero ids are remote machines.
--
-- No parameters; operates on the whole trace. One row per GPU. Columns:
--   machine_id   : owning machine (0 = host/local, non-zero = remote).
--   is_host      : 1 if this GPU is on the host machine, else 0.
--   ugpu         : host-unique GPU id — use this to join counters/slices/tracks.
--   gpu_index    : 0-based GPU index within its machine (not trace-unique).
--   vendor       : e.g. "NVIDIA", "AMD", "Qualcomm" (drives vendor-specific
--                  analysis; may be NULL if the producer did not report it).
--   name         : e.g. "NVIDIA A100-SXM4-80GB", "Adreno 740".
--   model        : product identifier.
--   architecture : e.g. "Ampere", "RDNA 3".
--   uuid         : 16-byte device UUID, hex-encoded (NULL if not reported).
--   pci_bdf      : PCI bus location domain:bus:device.function (NULL if absent).
--
-- If this returns no rows the trace carries no GpuInfo packet; GPU activity may
-- still be present (slices/counters), but vendor/model/architecture are unknown.

SELECT
  g.machine_id,
  g.machine_id = 0 AS is_host,
  g.ugpu,
  g.gpu AS gpu_index,
  g.vendor,
  g.name,
  g.model,
  g.architecture,
  g.uuid,
  g.pci_bdf
FROM gpu AS g
ORDER BY
  g.machine_id,
  g.gpu;
