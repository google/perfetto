--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- @module prelude.after_eof.tracks
-- Track infrastructure for organizing trace events.
--
-- This module provides the track concept and specialized track tables for
-- organizing events by thread, process, CPU, and GPU contexts.

INCLUDE PERFETTO MODULE prelude.after_eof.views;

-- Tracks are a fundamental concept in trace processor and represent a
-- "timeline" for events of the same type and with the same context. See
-- https://perfetto.dev/docs/analysis/trace-processor#tracks for a more
-- detailed explanation, with examples.
CREATE PERFETTO VIEW track(
  -- Unique identifier for this track. Identical to |track_id|, prefer using
  -- |track_id| instead.
  id ID,
  -- Name of the track; can be null for some types of tracks (e.g. thread
  -- tracks).
  name STRING,
  -- The type of a track indicates the type of data the track contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- type and a set of dimensions: type allow identifying a set of tracks
  -- with the same type of data within the whole universe of tracks while
  -- dimensions allow distinguishing between different tracks in that set.
  type STRING,
  -- The dimensions of the track which uniquely identify the track within a
  -- given `type`.
  --
  -- Join with the `args` table or use the `EXTRACT_ARG` helper function to
  -- expand the args.
  dimension_arg_set_id ARGSETID,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id JOINID(track.id),
  -- Generic key-value pairs containing extra information about the track.
  --
  -- Join with the `args` table or use the `EXTRACT_ARG` helper function to
  -- expand the args.
  source_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- An opaque key indicating that this track belongs to a group of tracks which
  -- are "conceptually" the same track.
  --
  -- Tracks in trace processor don't allow overlapping events to allow for easy
  -- analysis (i.e. SQL window functions, SPAN JOIN and other similar
  -- operators). However, in visualization settings (e.g. the UI), the
  -- distinction doesn't matter and all tracks with the same `track_group_id`
  -- should be merged together into a single logical "UI track".
  track_group_id LONG
)
AS
SELECT
  id,
  name,
  type,
  dimension_arg_set_id,
  parent_id,
  source_arg_set_id,
  machine_id,
  track_group_id
FROM __intrinsic_track;

-- Tracks which are associated to a single thread.
CREATE PERFETTO TABLE thread_track(
  -- Unique identifier for this thread track.
  id ID(track.id),
  -- Name of the track.
  name STRING,
  -- The type of a track indicates the type of data the track contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- type and a set of dimensions: type allow identifying a set of tracks
  -- with the same type of data within the whole universe of tracks while
  -- dimensions allow distinguishing between different tracks in that set.
  type STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id JOINID(track.id),
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- The utid that the track is associated with.
  utid JOINID(thread.id)
)
AS
SELECT
  t.id,
  t.name,
  t.type,
  t.parent_id,
  t.source_arg_set_id,
  t.machine_id,
  a.int_value AS utid
FROM __intrinsic_track AS t
JOIN args AS a
  ON t.dimension_arg_set_id = a.arg_set_id
WHERE
  t.event_type = 'slice'
  AND a.key = 'utid';

-- Tracks which are associated to a single process.
CREATE PERFETTO TABLE process_track(
  -- Unique identifier for this process track.
  id ID(track.id),
  -- Name of the track.
  name STRING,
  -- The type of a track indicates the type of data the track contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- type and a set of dimensions: type allow identifying a set of tracks
  -- with the same type of data within the whole universe of tracks while
  -- dimensions allow distinguishing between different tracks in that set.
  type STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id JOINID(track.id),
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- The upid that the track is associated with.
  upid JOINID(process.id)
)
AS
SELECT
  t.id,
  t.name,
  t.type,
  t.parent_id,
  t.source_arg_set_id,
  t.machine_id,
  a.int_value AS upid
FROM __intrinsic_track AS t
JOIN args AS a
  ON t.dimension_arg_set_id = a.arg_set_id
WHERE
  t.event_type = 'slice'
  AND a.key = 'upid';

-- Tracks which are associated to a single CPU.
CREATE PERFETTO TABLE cpu_track(
  -- Unique identifier for this cpu track.
  id ID(track.id),
  -- Name of the track.
  name STRING,
  -- The type of a track indicates the type of data the track contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- type and a set of dimensions: type allow identifying a set of tracks
  -- with the same type of data within the whole universe of tracks while
  -- dimensions allow distinguishing between different tracks in that set.
  type STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id JOINID(track.id),
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- The CPU that the track is associated with.
  cpu LONG
)
AS
SELECT
  t.id,
  t.name,
  t.type,
  t.parent_id,
  t.source_arg_set_id,
  t.machine_id,
  a.int_value AS cpu
FROM __intrinsic_track AS t
JOIN args AS a
  ON t.dimension_arg_set_id = a.arg_set_id
WHERE
  t.event_type = 'slice'
  AND a.key = 'cpu';

-- Table containing tracks which are loosely tied to a GPU.
--
-- NOTE: this table is deprecated due to inconsistency of it's design with
-- other track tables (e.g. not having a GPU column, mixing a bunch of different
-- tracks which are barely related). Please use the track table directly
-- instead.
CREATE PERFETTO TABLE gpu_track(
  -- Unique identifier for this cpu track.
  id ID(track.id),
  -- Name of the track.
  name STRING,
  -- The type of a track indicates the type of data the track contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- type and a set of dimensions: type allow identifying a set of tracks
  -- with the same type of data within the whole universe of tracks while
  -- dimensions allow distinguishing between different tracks in that set.
  type STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id JOINID(track.id),
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id ARGSETID,
  -- The dimensions of the track which uniquely identify the track within a
  -- given type.
  dimension_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- The source of the track. Deprecated.
  scope STRING,
  -- The description for the track.
  description STRING,
  -- The context id for the GPU this track is associated to.
  context_id LONG
)
AS
SELECT
  id,
  name,
  type,
  parent_id,
  source_arg_set_id,
  dimension_arg_set_id,
  machine_id,
  type AS scope,
  extract_arg(source_arg_set_id, 'description') AS description,
  extract_arg(dimension_arg_set_id, 'context_id') AS context_id
FROM __intrinsic_track
WHERE
  type IN (
    'drm_vblank',
    'drm_sched_ring',
    'drm_fence',
    'mali_mcu_state',
    'gpu_render_stage',
    'vulkan_events',
    'gpu_log',
    'graphics_frame_event'
  );

-- The *effective* dimensions of every track.
--
-- A dimension is a named, typed key/value which is part of a track's identity.
-- There is one vocabulary for all of them, regardless of whether the value
-- originated from a typed descriptor (e.g. `machine`, `gpu`), from import
-- context, or from a producer declaring it explicitly with
-- `TrackDescriptor.dimensions` (e.g. `rank` for a distributed training job).
--
-- Custom (producer-declared) dimensions are resolved onto every track which
-- inherits them: tracks of the same process/thread when they were declared on
-- a process/thread track, and `parent_id` descendants otherwise.
--
-- Note: this is a view rather than a table because the well known dimensions
-- below are synthesized on the fly for *every* track, and most consumers only
-- care about `is_well_known = 0`.
CREATE PERFETTO VIEW track_dimension(
  -- The track this dimension applies to.
  track_id JOINID(track.id),
  -- The canonical dimension name, e.g. 'rank' or 'machine'.
  name STRING,
  -- The value, for integer valued dimensions. Well known dimensions use trace
  -- processor identities here (e.g. `machine.id`, `upid`, `utid`).
  int_value LONG,
  -- The value, for string valued dimensions. Exactly one of `int_value` and
  -- `string_value` is non-null.
  string_value STRING,
  -- Optional human readable label for this *value* (e.g. a machine or process
  -- name). Presentation only: joins should use the canonical value above.
  display_name STRING,
  -- Whether this is a well known dimension, i.e. one which trace processor
  -- understands and whose value means the same thing across data sources.
  is_well_known BOOL
)
AS
-- Producer-declared custom dimensions, after inheritance was resolved.
SELECT track_id, name, int_value, string_value, display_name, 0 AS is_well_known
FROM __intrinsic_track_dimension
UNION ALL
-- Every track belongs to exactly one machine.
SELECT
  t.id AS track_id,
  'machine' AS name,
  m.id AS int_value,
  NULL AS string_value,
  m.name AS display_name,
  1 AS is_well_known
FROM __intrinsic_track AS t
JOIN machine AS m
  ON m.id = t.machine_id
UNION ALL
-- Tracks scoped to a single process, either directly or through their thread.
SELECT
  t.id AS track_id,
  'process' AS name,
  p.upid AS int_value,
  NULL AS string_value,
  p.name AS display_name,
  1 AS is_well_known
FROM __intrinsic_track AS t
JOIN process AS p
  ON p.upid
  = coalesce(t.upid, (SELECT th.upid FROM thread AS th WHERE th.utid = t.utid))
UNION ALL
-- Tracks scoped to a single thread.
SELECT
  t.id AS track_id,
  'thread' AS name,
  t.utid AS int_value,
  NULL AS string_value,
  th.name AS display_name,
  1 AS is_well_known
FROM __intrinsic_track AS t
JOIN thread AS th USING (utid)
UNION ALL
-- Tracks scoped to a single CPU or GPU. `ucpu`/`ugpu` are the canonical
-- (machine aware) identities; the raw numbers stay available through
-- `cpu.cpu` and `gpu.gpu`.
SELECT
  track_id,
  name,
  int_value,
  NULL AS string_value,
  NULL AS display_name,
  1 AS is_well_known
FROM (
  SELECT
    id AS track_id,
    'cpu' AS name,
    coalesce(
      extract_arg(dimension_arg_set_id, 'ucpu'),
      extract_arg(dimension_arg_set_id, 'cpu')
    ) AS int_value
  FROM __intrinsic_track
  UNION ALL
  SELECT
    id AS track_id,
    'gpu' AS name,
    coalesce(
      extract_arg(dimension_arg_set_id, 'ugpu'),
      extract_arg(dimension_arg_set_id, 'gpu')
    ) AS int_value
  FROM __intrinsic_track
)
WHERE
  int_value IS NOT NULL;

-- The effective dimensions of every process.
--
-- This is a projection of the same declarations as `track_dimension`, keyed by
-- `upid`: use it for event tables which carry their own `upid` (e.g.
-- `gpu_slice`) rather than inheriting it from the track.
CREATE PERFETTO VIEW process_dimension(
  -- The process this dimension applies to.
  upid JOINID(process.id),
  -- The canonical dimension name, e.g. 'rank'.
  name STRING,
  -- The value, for integer valued dimensions.
  int_value LONG,
  -- The value, for string valued dimensions.
  string_value STRING,
  -- Optional human readable label for this *value*.
  display_name STRING,
  -- Whether this is a well known dimension.
  is_well_known BOOL
)
AS
SELECT upid, name, int_value, string_value, display_name, 0 AS is_well_known
FROM __intrinsic_track_dimension_decl
WHERE
  upid IS NOT NULL
UNION ALL
SELECT
  p.upid,
  'machine' AS name,
  m.id AS int_value,
  NULL AS string_value,
  m.name AS display_name,
  1 AS is_well_known
FROM process AS p
JOIN machine AS m
  ON m.id = p.machine_id;

-- The effective dimensions of every thread: the dimensions declared on the
-- thread itself plus the ones it inherits from its process.
CREATE PERFETTO VIEW thread_dimension(
  -- The thread this dimension applies to.
  utid JOINID(thread.id),
  -- The canonical dimension name, e.g. 'rank'.
  name STRING,
  -- The value, for integer valued dimensions.
  int_value LONG,
  -- The value, for string valued dimensions.
  string_value STRING,
  -- Optional human readable label for this *value*.
  display_name STRING,
  -- Whether this is a well known dimension.
  is_well_known BOOL
)
AS
SELECT utid, name, int_value, string_value, display_name, 0 AS is_well_known
FROM __intrinsic_track_dimension_decl
WHERE
  utid IS NOT NULL
UNION ALL
SELECT
  t.utid,
  d.name,
  d.int_value,
  d.string_value,
  d.display_name,
  d.is_well_known
FROM thread AS t
JOIN process_dimension AS d USING (upid);
