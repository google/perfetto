-- Copyright 2024 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

INCLUDE PERFETTO MODULE slices.with_context;

-- This module defines tables with information about Android input pipeline
-- steps. The trace needs to be recorded with the 'view' atrace category.

-- On Android, input goes through the following path before getting to Chrome:
--  * InputReader thread (part of Android system_server)
--  * InputDispatcher thread (part of Android system_server)
--  * Browser Main thread (Chromium/Chrome)

-- In traces, each of these three steps have slices which are implicitly linked
-- together by an input id (part of slice name) assigned by the Android system.

-- The following queries correlate the three steps mentioned above
-- with the rest of the `LatencyInfo.Flow` pipeline.

-- DeliverInputEvent is the third step in the input pipeline.
-- It is responsible for routing the input events within browser process.
CREATE PERFETTO PIPELINE chrome_deliver_android_input_event (
  -- Timestamp.
  ts TIMESTAMP,
  -- Touch move processing duration.
  dur DURATION,
  -- Utid.
  utid LONG,
  -- Input id (assigned by the system, used by InputReader and InputDispatcher)
  android_input_id STRING
) MATERIALIZED AS
FROM thread_slice AS slice
|> WHERE slice.name GLOB 'deliverInputEvent*'
|> SELECT
     slice.ts,
     slice.dur,
     slice.utid,
     substr(substr(name, instr(name, 'id=')), 4) AS android_input_id;

-- Collects information about input reader, input dispatcher and
-- DeliverInputEvent steps for the given Android input id.
CREATE PERFETTO PIPELINE chrome_android_input (
  -- Input id.
  android_input_id STRING,
  -- Input reader step start timestamp.
  input_reader_processing_start_ts TIMESTAMP,
  -- Input reader step end timestamp.
  input_reader_processing_end_ts TIMESTAMP,
  -- Input reader step utid.
  input_reader_utid LONG,
  -- Input dispatcher step start timestamp.
  input_dispatcher_processing_start_ts TIMESTAMP,
  -- Input dispatcher step end timestamp.
  input_dispatcher_processing_end_ts TIMESTAMP,
  -- Input dispatcher step utid.
  input_dispatcher_utid LONG,
  -- DeliverInputEvent step start timestamp.
  deliver_input_event_start_ts TIMESTAMP,
  -- DeliverInputEvent step end timestamp.
  deliver_input_event_end_ts TIMESTAMP,
  -- DeliverInputEvent step utid.
  deliver_input_event_utid LONG
) MATERIALIZED AS
-- InputReader is the first step in the input pipeline.
-- It is responsible for reading the input events from the system_server
-- process and sending them to the InputDispatcher (which then sends them
-- to the browser process).
SUBPIPELINE input_reader_step AS (
  FROM thread_slice AS slice
  |> WHERE name GLOB 'UnwantedInteractionBlocker::notifyMotion*'
  -- Get the substring that starts with 'id=', remove the 'id=' and remove the
  -- trailing ')'. 'id=0x344bb0f9)' ->  '0x344bb0f9'
  |> SELECT
       ts,
       dur,
       id,
       trim(substr(substr(name, instr(name, 'id=')), 4), ')') AS android_input_id,
       utid
)
-- InputDispatcher is the second step in the input pipeline.
-- It is responsible for dispatching the input events to the browser process.
SUBPIPELINE input_dispatcher_step AS (
  FROM thread_slice AS slice
  |> WHERE name GLOB 'prepareDispatchCycleLocked*chrome*'
  |> SELECT
       ts,
       dur,
       id,
       trim(substr(substr(name, instr(name, 'id=')), 4), ')') AS android_input_id,
       utid
)
FROM input_reader_step
|> LEFT JOIN input_dispatcher_step USING (android_input_id)
|> LEFT JOIN chrome_deliver_android_input_event USING (android_input_id)
|> SELECT
     input_reader_step.android_input_id,
     input_reader_step.ts AS input_reader_processing_start_ts,
     input_reader_step.ts + input_reader_step.dur AS input_reader_processing_end_ts,
     input_reader_step.utid AS input_reader_utid,
     input_dispatcher_step.ts AS input_dispatcher_processing_start_ts,
     input_dispatcher_step.ts + input_dispatcher_step.dur AS input_dispatcher_processing_end_ts,
     input_dispatcher_step.utid AS input_dispatcher_utid,
     chrome_deliver_android_input_event.ts AS deliver_input_event_start_ts,
     chrome_deliver_android_input_event.ts + chrome_deliver_android_input_event.dur AS deliver_input_event_end_ts,
     chrome_deliver_android_input_event.utid AS deliver_input_event_utid;
