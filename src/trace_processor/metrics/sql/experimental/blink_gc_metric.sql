-- Maps non-aggregated Blink GC events in timeline to telemetry friendly
-- names.
--
-- This includes the old style or the new naming scheme one which only occur on
-- the main thread.
DROP VIEW IF EXISTS blink_non_aggregated_gc_event_name;
CREATE VIEW blink_non_aggregated_gc_event_name AS
SELECT
  'BlinkGC.AtomicPauseMarkEpilogue' AS name,
  'blink-gc-atomic-pause-mark-epilogue' AS old_event_name,
  'blink:gc:main_thread:cycle:full:atomic:mark:epilogue' AS new_event_name
UNION ALL
SELECT
  'BlinkGC.AtomicPauseMarkPrologue',
  'blink-gc-atomic-pause-mark-prologue',
  'blink:gc:main_thread:cycle:full:atomic:mark:prologue'
UNION ALL
SELECT
  'BlinkGC.AtomicPauseMarkRoots',
  'blink-gc-atomic-pause-mark-roots',
  'blink:gc:main_thread:cycle:full:atomic:mark:roots'
UNION ALL
SELECT
  'BlinkGC.IncrementalMarkingStartMarking',
  'blink-gc-incremental-start',
  'blink:gc:main_thread:cycle:full:incremental:mark:start'
UNION ALL
SELECT
  'BlinkGC.IncrementalMarkingStep',
  'blink-gc-incremental-step',
  'blink:gc:main_thread:cycle:full:incremental:mark:step'
UNION ALL
SELECT
  'BlinkGC.UnifiedMarkingStep',
  'blink-gc-unified-marking-by-v8',
  'unified:gc:main_thread:cycle:full:mark:step'
UNION ALL
SELECT
  'BlinkGC.CompleteSweep',
  'blink-gc-complete-sweep',
  'blink:gc:main_thread:cycle:full:sweep:complete'
UNION ALL
SELECT
  'BlinkGC.LazySweepInIdle',
  'blink-gc-sweep-task-foreground',
  'blink:gc:main_thread:cycle:full:sweep:idle'
UNION ALL
SELECT
  'BlinkGC.LazySweepOnAllocation',
  'blink-gc-sweep-allocation',
  'blink:gc:main_thread:cycle:full:sweep:on_allocation'
UNION ALL
SELECT
  'BlinkGC.AtomicPauseSweepAndCompact' AS name,
  'blink-gc-atomic-pause-sweep-and-compact' AS old_event_name,
  'blink:gc:main_thread:cycle:full:atomic:sweep:compact' AS new_event_name;

-- Get all the slices we care about. These are ones that start with V8.GC or
-- BlinkGC. If you need more you need to modify the where clause for
-- blink_gc_cpu_slice.
DROP TABLE IF EXISTS blink_gc_cpu_slice;
CREATE TABLE blink_gc_cpu_slice AS
SELECT
  CASE WHEN dur != 0 THEN cpuDurNs / 1e6 ELSE 0.0 END AS cpuDurMs,
  *
FROM (
  SELECT
    COALESCE(EXTRACT_ARG(arg_set_id, 'debug.forced'), FALSE)
    -- This subquery replaces
    -- metrics.v8.utils.isForcedGarbageCollectionEvent(event)
    OR (
      SELECT
        id
      FROM ANCESTOR_SLICE(slice.id) AS ancestor
      WHERE ancestor.name = 'V8.GCLowMemoryNotification' LIMIT 1
    ) IS NOT NULL AS forced,
    -- upid replaces pid, because its more fool proof ensuring uniqueness.
    thread.upid || ':' || EXTRACT_ARG(arg_set_id, 'debug.epoch') AS epoch,
    slice.thread_dur AS cpuDurNs,
    slice.*
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread ON thread_track.utid = thread.id
  WHERE
    slice.dur >= 0 AND (
      slice.name GLOB "V8.GC*" OR (slice.name GLOB "BlinkGC*" AND NOT forced)
    )
);

-- This grabs all the single events for "BlinkGC.*", and restricts to only
-- forced events.
DROP TABLE IF EXISTS blink_slice;
CREATE TABLE blink_slice AS
SELECT
  event_name.old_event_name AS blink_non_aggregated_gc_event_name,
  event_name.new_event_name AS blink_non_aggregated_gc_events_new_name,
  blink_gc_cpu_slice.*
FROM
  blink_gc_cpu_slice LEFT JOIN
  blink_non_aggregated_gc_event_name AS event_name ON
    event_name.name = blink_gc_cpu_slice.name
WHERE
  blink_gc_cpu_slice.name GLOB "BlinkGC*" AND NOT forced;

-- This groups all the events by name and epoch for from "blink_slice" for easy
-- access.
DROP TABLE IF EXISTS blink_per_epoch_slice;
CREATE TABLE blink_per_epoch_slice AS
SELECT
  name,
  epoch,
  blink_non_aggregated_gc_event_name,
  blink_non_aggregated_gc_events_new_name,
  SUM(cpuDurMs) AS cpuDurPerEpochMs
FROM blink_slice
GROUP BY 1, 2, 3, 4;

-- All events that should be summed up to 'blink-gc-mark-roots'.
DROP VIEW IF EXISTS blink_top_gc_roots_marking_event;
CREATE VIEW blink_top_gc_roots_marking_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.VisitRoots'
);

-- All events that should be summed up to
-- 'blink-gc-atomic-pause-mark-transitive-closure'.
DROP VIEW IF EXISTS blink_gc_atomic_pause_transitive_closure_event;
CREATE VIEW blink_gc_atomic_pause_transitive_closure_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.AtomicPauseMarkTransitiveClosure'
);

-- All events that should be summed up to 'blink-gc-mark-transitive-closure'.
DROP VIEW IF EXISTS blink_gc_foreground_marking_transitive_closure_event;
CREATE VIEW
blink_gc_foreground_marking_transitive_closure_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.AtomicPauseMarkTransitiveClosure',
  'BlinkGC.IncrementalMarkingStep',
  'BlinkGC.UnifiedMarkingStep'
);

-- Names of Blink GC foreground marking events in timeline.
DROP VIEW IF EXISTS blink_top_gc_foreground_marking_event;
CREATE VIEW blink_top_gc_foreground_marking_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.AtomicPauseMarkEpilogue',
  'BlinkGC.AtomicPauseMarkPrologue',
  'BlinkGC.AtomicPauseMarkRoots',
  'BlinkGC.IncrementalMarkingStartMarking'
)
UNION ALL
SELECT * FROM blink_gc_foreground_marking_transitive_closure_event;

-- Names of Blink GC foreground marking events in timeline.
DROP VIEW IF EXISTS blink_gc_forced_foreground_marking_event;
CREATE VIEW blink_gc_forced_foreground_marking_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.AtomicPauseMarkEpilogue',
  'BlinkGC.AtomicPauseMarkPrologue',
  'BlinkGC.AtomicPauseMarkRoots',
  'BlinkGC.IncrementalMarkingStartMarking',
  'BlinkGC.MarkBailOutObjects',
  'BlinkGC.MarkFlushV8References',
  'BlinkGC.MarkFlushEphemeronPairs'
);

-- Names of Blink GC background marking events in timeline.
DROP VIEW IF EXISTS blink_top_gc_background_marking_event;
CREATE VIEW blink_top_gc_background_marking_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.ConcurrentMarkingStep'
);

-- Names of Blink GC foreground sweeping events in timeline.
DROP VIEW IF EXISTS blink_top_gc_foreground_sweeping_event;
CREATE VIEW blink_top_gc_foreground_sweeping_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.CompleteSweep',
  'BlinkGC.LazySweepInIdle',
  'BlinkGC.LazySweepOnAllocation'
);

-- Names of Blink GC background sweeping events in timeline.
DROP VIEW IF EXISTS blink_top_gc_background_sweeping_event;
CREATE VIEW blink_top_gc_background_sweeping_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.ConcurrentSweepingStep'
);

-- Names of all Blink Unified GC events in timeline.
DROP VIEW IF EXISTS blink_top_gc_event;
CREATE VIEW blink_top_gc_event AS
SELECT * FROM blink_slice WHERE name IN (
  SELECT name FROM blink_non_aggregated_gc_event_name
) OR name IN (
  SELECT name FROM blink_gc_atomic_pause_transitive_closure_event
);

-- All events that should be summed up to 'blink-gc-atomic-pause'. Note that
-- this events need to have an epoch counter in args.epoch.
DROP VIEW IF EXISTS atomic_pause_event;
CREATE VIEW atomic_pause_event AS
SELECT * FROM blink_slice WHERE name IN (
  'BlinkGC.AtomicPauseMarkEpilogue',
  'BlinkGC.AtomicPauseMarkPrologue',
  'BlinkGC.AtomicPauseMarkRoots',
  'BlinkGC.AtomicPauseMarkTransitiveClosure',
  'BlinkGC.AtomicPauseSweepAndCompact'
);

-- This is a more complex variable so benefits from additional comments so we
-- pull it out of the proto filling.
DROP VIEW IF EXISTS unified_gc_total;
CREATE VIEW unified_gc_total AS
SELECT
  *
FROM blink_gc_cpu_slice
WHERE (
  -- This subclause replaces
  -- metrics.v8.utils.isNotForcedTopGarbageCollectionEvent()

  -- These names are found in isTopGarbageCollectionEvent().
  name IN (
    'V8.GCCompactor',
    'V8.GCFinalizeMC',
    'V8.GCFinalizeMCReduceMemory',
    'V8.GCIncrementalMarking',
    'V8.GCIncrementalMarkingFinalize',
    'V8.GCIncrementalMarkingStart',
    'V8.GCPhantomHandleProcessingCallback',
    'V8.GCScavenger'
  ) AND (
    -- This replaces isForcedGarbageCollectionEvent.
    SELECT name FROM ANCESTOR_SLICE(blink_gc_cpu_slice.id) AS ancestor
    WHERE ancestor.name = 'V8.GCLowMemoryNotification'
    LIMIT 1
  ) IS NULL
) OR (
  -- This subclause replaces isNonNestedNonForcedBlinkGarbageCollectionEvent().
  name IN (
    -- This subquery replaces isNonForcedBlinkGarbageCollectionEvent().
    SELECT name FROM blink_top_gc_event
  ) AND (
    -- This subquery replaces metrics.v8.utils.isGarbageCollectionEvent().
    SELECT name FROM ANCESTOR_SLICE(blink_gc_cpu_slice.id) AS ancestor
    WHERE
      ancestor.name GLOB "V8.GC*"
      AND ancestor.name != 'V8.GCLowMemoryNotification'
    LIMIT 1
  ) IS NULL
);

-- This table name is just "file_name" + "_output" used by TBMv3 to know which
-- view to extract the proto BlinkGcMetric out of.
DROP VIEW IF EXISTS blink_gc_metric_output;
CREATE VIEW blink_gc_metric_output AS
SELECT BlinkGcMetric(
  'blink_gc_atomic_pause_mark_epilogue',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE
      blink_non_aggregated_gc_event_name = 'blink-gc-atomic-pause-mark-epilogue'
  ),
  'blink_gc_main_thread_cycle_full_atomic_mark_epilogue',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:atomic:mark:epilogue'
  ),
  'blink_gc_atomic_pause_mark_prologue',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE
      blink_non_aggregated_gc_event_name
      = 'blink-gc-atomic-pause-mark-prologue'
  ),
  'blink_gc_main_thread_cycle_full_atomic_mark_prologue',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:atomic:mark:prologue'
  ),
  'blink_gc_atomic_pause_mark_roots',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE
      blink_non_aggregated_gc_event_name = 'blink-gc-atomic-pause-mark-roots'
  ),
  'blink_gc_main_thread_cycle_full_atomic_mark_roots',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:atomic:mark:roots'
  ),
  'blink_gc_atomic_pause_sweep_and_compact',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE
      blink_non_aggregated_gc_event_name
      = 'blink-gc-atomic-pause-sweep-and-compact'
  ),
  'blink_gc_main_thread_cycle_full_atomic_sweep_compact',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:atomic:sweep:compact'
  ),
  'blink_gc_complete_sweep',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-complete-sweep'
  ),
  'blink_gc_main_thread_cycle_full_sweep_complete',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:sweep:complete'
  ),
  'blink_gc_incremental_start',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-incremental-start'
  ),
  'blink_gc_main_thread_cycle_full_incremental_mark_start',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:incremental:mark:start'
  ),
  'blink_gc_incremental_step',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-incremental-step'
  ),
  'blink_gc_main_thread_cycle_full_incremental_mark_step',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:incremental:mark:step'
  ),
  'blink_gc_sweep_allocation',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-sweep-allocation'
  ),
  'blink_gc_main_thread_cycle_full_sweep_on_allocation',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:sweep:on_allocation'
  ),
  'blink_gc_sweep_task_foreground',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-sweep-task-foreground'
  ),
  'blink_gc_main_thread_cycle_full_sweep_idle',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'blink:gc:main_thread:cycle:full:sweep:idle'
  ),
  'blink_gc_unified_marking_by_v8',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_slice
    WHERE blink_non_aggregated_gc_event_name = 'blink-gc-unified-marking-by-v8'
  ),
  'unified_gc_main_thread_cycle_full_mark_step',
  (
    SELECT
      RepeatedField(cpuDurPerEpochMs)
    FROM blink_per_epoch_slice
    WHERE
      blink_non_aggregated_gc_events_new_name
      = 'unified:gc:main_thread:cycle:full:mark:step'
  ),
  'blink_gc_atomic_pause',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM atomic_pause_event
  ),
  'blink_gc_main_thread_cycle_full_atomic',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM atomic_pause_event
      GROUP BY epoch
    )
  ),
  'blink_gc_atomic_pause_mark_transitive_closure',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_gc_atomic_pause_transitive_closure_event
  ),
  'blink_gc_main_thread_cycle_full_atomic_mark_transitive_closure',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_gc_atomic_pause_transitive_closure_event
      GROUP BY epoch
    )
  ),
  'blink_gc_total',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_event
  ),
  'blink_gc_main_thread_cycle_full',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_event
      GROUP BY epoch
    )
  ),
  'blink_gc_mark_roots',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_roots_marking_event
  ),
  'blink_gc_main_thread_cycle_full_mark_roots',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_roots_marking_event
      GROUP BY epoch
    )
  ),
  'blink_gc_mark_transitive_closure',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_gc_foreground_marking_transitive_closure_event
  ),
  'blink_gc_main_thread_cycle_full_mark_transitive_closure',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_gc_foreground_marking_transitive_closure_event
      GROUP BY epoch
    )
  ),
  'blink_gc_mark_foreground',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_foreground_marking_event
  ),
  'blink_gc_main_thread_cycle_full_mark',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_foreground_marking_event
      GROUP BY epoch
    )
  ),
  'blink_gc_mark_foreground_forced',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_gc_forced_foreground_marking_event
  ),
  'blink_gc_main_thread_cycle_full_mark_forced',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_gc_forced_foreground_marking_event
      GROUP BY epoch
    )
  ),
  'blink_gc_mark_background',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_background_marking_event
  ),
  'blink_gc_concurrent_thread_cycle_full_mark',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_background_marking_event
      GROUP BY epoch
    )
  ),
  'blink_gc_sweep_foreground',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_foreground_sweeping_event
  ),
  'blink_gc_main_thread_cycle_full_sweep',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_foreground_sweeping_event
      GROUP BY epoch
    )
  ),
  'blink_gc_sweep_background',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM blink_top_gc_background_sweeping_event
  ),
  'blink_gc_concurrent_thread_cycle_full_sweep',
  (
    SELECT RepeatedField(val) FROM (
      SELECT
        SUM(cpuDurMs) AS val
      FROM blink_top_gc_background_sweeping_event
      GROUP BY epoch
    )
  ),
  'unified_gc_total',
  (
    SELECT
      RepeatedField(cpuDurMs)
    FROM unified_gc_total
  )
);
