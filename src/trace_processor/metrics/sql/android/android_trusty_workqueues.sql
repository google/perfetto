-- Gather the `nop_work_func` slices and the CPU they each ran on and use that
-- information to generate a metric that displays just the Trusty workqueue
-- events grouped by CPU.
DROP VIEW IF EXISTS android_trusty_workqueues_event;
CREATE PERFETTO VIEW android_trusty_workqueues_event AS
SELECT
  'slice' AS track_type,
  name AS slice_name,
  ts,
  dur,
  'Cpu ' || EXTRACT_ARG(arg_set_id, 'cpu') AS track_name,
  'Trusty Workqueues' AS group_name
FROM slice
WHERE slice.name GLOB 'nop_work_func*';

-- Generate the final metric output. This is empty because we're only using the
-- metric to generate custom tracks, and so don't have any aggregate data to
-- generate.
DROP VIEW IF EXISTS android_trusty_workqueues_output;
CREATE PERFETTO VIEW android_trusty_workqueues_output AS
SELECT AndroidTrustyWorkqueues();
