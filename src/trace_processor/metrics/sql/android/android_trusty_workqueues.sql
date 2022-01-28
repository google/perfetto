-- Gather the `nop_work_func` slices and the CPU they each ran on and use that
-- information to generate a metric that displays just the Trusty workqueue
-- events grouped by CPU.
DROP VIEW IF EXISTS android_trusty_workqueues_event;
CREATE VIEW android_trusty_workqueues_event AS
SELECT
  'slice' as track_type,
  name as slice_name,
  ts,
  dur,
  'Cpu ' || EXTRACT_ARG(arg_set_id, 'cpu') as track_name,
  'Trusty Workqueues' as group_name
FROM slice
WHERE slice.name GLOB 'nop_work_func*';

-- Generate the final metric output. This is empty because we're only using the
-- metric to generate custom tracks, and so don't have any aggregate data to
-- generate.
DROP VIEW IF EXISTS android_trusty_workqueues_output;
CREATE VIEW android_trusty_workqueues_output AS
SELECT AndroidTrustyWorkqueues();
