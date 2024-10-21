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

INCLUDE PERFETTO MODULE android.frames.timeline;
INCLUDE PERFETTO MODULE slices.with_context;

CREATE PERFETTO TABLE _input_message_sent
AS
SELECT
  STR_SPLIT(STR_SPLIT(slice.name, '=', 3), ')', 0) AS event_type,
  STR_SPLIT(STR_SPLIT(slice.name, '=', 2), ',', 0) AS event_seq,
  STR_SPLIT(STR_SPLIT(slice.name, '=', 1), ',', 0) AS event_channel,
  thread.tid,
  thread.name AS thread_name,
  process.pid,
  process.name AS process_name,
  slice.ts,
  slice.dur,
  slice.track_id
FROM slice
JOIN thread_track
  ON thread_track.id = slice.track_id
JOIN thread
  USING (utid)
JOIN process
  USING (upid)
WHERE slice.name GLOB 'sendMessage(*'
order by event_seq;

CREATE PERFETTO TABLE _input_message_received
AS
SELECT
  STR_SPLIT(STR_SPLIT(slice.name, '=', 3), ')', 0) AS event_type,
  STR_SPLIT(STR_SPLIT(slice.name, '=', 2), ',', 0) AS event_seq,
  STR_SPLIT(STR_SPLIT(slice.name, '=', 1), ',', 0) AS event_channel,
  thread.tid,
  thread.name AS thread_name,
  process.pid,
  process.name AS process_name,
  slice.ts,
  slice.dur,
  slice.track_id
FROM slice
JOIN thread_track
  ON thread_track.id = slice.track_id
JOIN thread
  USING (utid)
JOIN process
  USING (upid)
WHERE slice.name GLOB 'receiveMessage(*'
ORDER BY event_seq;

CREATE PERFETTO TABLE _input_read_time
AS
SELECT
  name,
  STR_SPLIT(STR_SPLIT(name, '=', 1), ')', 0) AS input_event_id,
  ts as read_time
FROM slice
WHERE name GLOB 'UnwantedInteractionBlocker::notifyMotion*';

CREATE PERFETTO TABLE _event_seq_to_input_event_id
AS
SELECT
  STR_SPLIT(STR_SPLIT(send_message_slice.name, '=', 2), ',', 0) AS event_seq,
  STR_SPLIT(STR_SPLIT(send_message_slice.name, '=', 1), ',', 0) AS event_channel,
  STR_SPLIT(STR_SPLIT(enqeue_slice.name, '=', 2), ')', 0) AS input_event_id,
  thread_slice.thread_name
FROM slice send_message_slice
JOIN slice publish_slice
  ON send_message_slice.parent_id = publish_slice.id
JOIN slice start_dispatch_slice
  ON publish_slice.parent_id = start_dispatch_slice.id
JOIN slice enqeue_slice
  ON start_dispatch_slice.parent_id = enqeue_slice.id
JOIN thread_slice
  ON send_message_slice.id = thread_slice.id
WHERE send_message_slice.name GLOB 'sendMessage(*' AND thread_slice.thread_name = 'InputDispatcher';

CREATE PERFETTO TABLE _input_event_id_to_android_frame
AS
SELECT
  STR_SPLIT(deliver_input_slice.name, '=', 3) AS input_event_id,
  STR_SPLIT(STR_SPLIT(dispatch_input_slice.name, '_', 1), ' ', 0) AS event_action,
  dispatch_input_slice.ts AS consume_time,
  dispatch_input_slice.ts + dispatch_input_slice.dur AS finish_time,
  thread_slice.utid,
  thread_slice.process_name AS process_name,
  (
    SELECT
      android_frames.frame_id
    FROM android_frames
    WHERE android_frames.ts > dispatch_input_slice.ts
    LIMIT 1
  ) as frame_id,
  (
    SELECT
      android_frames.ts
    FROM android_frames
    WHERE android_frames.ts > dispatch_input_slice.ts
    LIMIT 1
  ) as ts,
  (
    SELECT
      _input_message_received.event_channel
    FROM _input_message_received
    WHERE _input_message_received.ts < deliver_input_slice.ts
      AND _input_message_received.track_id = deliver_input_slice.track_id
    ORDER BY _input_message_received.ts DESC
    LIMIT 1
  ) as event_channel
FROM slice deliver_input_slice
JOIN slice dispatch_input_slice
  ON deliver_input_slice.parent_id = dispatch_input_slice.id
JOIN thread_slice
  ON deliver_input_slice.id = thread_slice.id
WHERE deliver_input_slice.name GLOB 'deliverInputEvent src=*';

CREATE PERFETTO TABLE _app_frame_to_surface_flinger_frame
AS
SELECT
  app.surface_frame_token as app_surface_frame_token,
  surface_flinger.ts as surface_flinger_ts,
  surface_flinger.dur as surface_flinger_dur,
  app.ts as app_ts,
  app.present_type,
  app.upid
FROM actual_frame_timeline_slice surface_flinger
JOIN actual_frame_timeline_slice app
  ON surface_flinger.display_frame_token = app.display_frame_token
  AND surface_flinger.id != app.id
WHERE surface_flinger.surface_frame_token = 0 AND app.present_type != 'Dropped Frame';

CREATE PERFETTO TABLE _first_non_dropped_frame_after_input
AS
SELECT
  _input_read_time.input_event_id,
  _input_read_time.read_time,
  (
    SELECT
      surface_flinger_ts + surface_flinger_dur
    FROM _app_frame_to_surface_flinger_frame sf_frames
    WHERE sf_frames.app_ts > _input_event_id_to_android_frame.ts
    LIMIT 1
  ) AS present_time,
  (
    SELECT
      app_surface_frame_token
    FROM _app_frame_to_surface_flinger_frame sf_frames
    WHERE sf_frames.app_ts > _input_event_id_to_android_frame.ts
    LIMIT 1
  ) as frame_id,
  event_seq,
  event_action
FROM _input_event_id_to_android_frame
RIGHT JOIN _event_seq_to_input_event_id
  ON _input_event_id_to_android_frame.input_event_id = _event_seq_to_input_event_id.input_event_id
  AND _input_event_id_to_android_frame.event_channel = _event_seq_to_input_event_id.event_channel
JOIN _input_read_time
  ON _input_read_time.input_event_id = _event_seq_to_input_event_id.input_event_id;

-- All input events with round trip latency breakdown. Input delivery is socket based and every
-- input event sent from the OS needs to be ACK'ed by the app. This gives us 4 subevents to measure
-- latencies between:
-- 1. Input dispatch event sent from OS.
-- 2. Input dispatch event received in app.
-- 3. Input ACK event sent from app.
-- 4. Input ACk event received in OS.
CREATE PERFETTO TABLE android_input_events (
  -- Duration from input dispatch to input received.
  dispatch_latency_dur INT,
  -- Duration from input received to input ACK sent.
  handling_latency_dur INT,
  -- Duration from input ACK sent to input ACK recieved.
  ack_latency_dur INT,
  -- Duration from input dispatch to input event ACK received.
  total_latency_dur INT,
  -- Duration from input read to frame present time. Null if an input event has no associated frame event.
  end_to_end_latency_dur INT,
  -- Tid of thread receiving the input event.
  tid INT,
  -- Name of thread receiving the input event.
  thread_name STRING,
  -- Pid of process receiving the input event.
  pid INT,
  -- Name of process receiving the input event.
  process_name STRING,
  -- Input event type. See InputTransport.h: InputMessage#Type
  event_type STRING,
  -- Input event action.
  event_action STRING,
  -- Input event sequence number, monotonically increasing for an event channel and pid.
  event_seq STRING,
  -- Input event channel name.
  event_channel STRING,
  -- Unique identifier for the input event.
  input_event_id STRING,
  -- Timestamp input event was read by InputReader.
  read_time INT,
  -- Thread track id of input event dispatching thread.
  dispatch_track_id INT,
  -- Timestamp input event was dispatched.
  dispatch_ts INT,
  -- Duration of input event dispatch.
  dispatch_dur INT,
  -- Thread track id of input event receiving thread.
  receive_track_id INT,
  -- Timestamp input event was received.
  receive_ts INT,
  -- Duration of input event receipt.
  receive_dur INT,
  -- Vsync Id associated with the input. Null if an input event has no associated frame event.
  frame_id INT
  )
AS
WITH dispatch AS MATERIALIZED (
  SELECT * FROM _input_message_sent
  WHERE thread_name = 'InputDispatcher'
  ORDER BY event_seq, event_channel
),
receive AS MATERIALIZED (
  SELECT
    *,
    REPLACE(event_channel, '(client)', '(server)') AS dispatch_event_channel
  FROM _input_message_received
  WHERE event_type NOT IN ('0x2', 'FINISHED')
  ORDER BY event_seq, dispatch_event_channel
),
finish AS MATERIALIZED (
  SELECT
    *,
    REPLACE(event_channel, '(client)', '(server)') AS dispatch_event_channel
  FROM _input_message_sent
  WHERE thread_name != 'InputDispatcher'
  ORDER BY event_seq, dispatch_event_channel
),
finish_ack AS MATERIALIZED(
  SELECT * FROM _input_message_received
  WHERE event_type IN ('0x2', 'FINISHED')
  ORDER BY event_seq, event_channel
)
SELECT
  receive.ts - dispatch.ts AS dispatch_latency_dur,
  finish.ts - receive.ts AS handling_latency_dur,
  finish_ack.ts - finish.ts AS ack_latency_dur,
  finish_ack.ts - dispatch.ts AS total_latency_dur,
  frame.present_time - frame.read_time AS end_to_end_latency_dur,
  finish.tid AS tid,
  finish.thread_name AS thread_name,
  finish.pid AS pid,
  finish.process_name AS process_name,
  dispatch.event_type,
  frame.event_action,
  dispatch.event_seq,
  dispatch.event_channel,
  frame.input_event_id,
  frame.read_time,
  dispatch.track_id AS dispatch_track_id,
  dispatch.ts AS dispatch_ts,
  dispatch.dur AS dispatch_dur,
  receive.ts AS receive_ts,
  receive.dur AS receive_dur,
  receive.track_id AS receive_track_id,
  frame.frame_id
FROM dispatch
JOIN receive
  ON
    receive.dispatch_event_channel = dispatch.event_channel
    AND dispatch.event_seq = receive.event_seq
JOIN finish
  ON
    finish.dispatch_event_channel = dispatch.event_channel
    AND dispatch.event_seq = finish.event_seq
JOIN finish_ack
  ON
    finish_ack.event_channel = dispatch.event_channel
    AND dispatch.event_seq = finish_ack.event_seq
LEFT JOIN _first_non_dropped_frame_after_input frame
  ON frame.event_seq = dispatch.event_seq;

-- Key events processed by the Android framework (from android.input.inputevent data source).
CREATE PERFETTO VIEW android_key_events(
  -- ID of the trace entry
  id INT,
  -- The randomly-generated ID associated with each input event processed
  -- by Android Framework, used to track the event through the input pipeline
  event_id INT,
  -- The timestamp of when the input event was processed by the system
  ts INT,
  -- Details of the input event parsed from the proto message
  arg_set_id INT
) AS
SELECT
  id,
  event_id,
  ts,
  arg_set_id
FROM __intrinsic_android_key_events;

-- Motion events processed by the Android framework (from android.input.inputevent data source).
CREATE PERFETTO VIEW android_motion_events(
  -- ID of the trace entry
  id INT,
  -- The randomly-generated ID associated with each input event processed
  -- by Android Framework, used to track the event through the input pipeline
  event_id INT,
  -- The timestamp of when the input event was processed by the system
  ts INT,
  -- Details of the input event parsed from the proto message
  arg_set_id INT
) AS
SELECT
  id,
  event_id,
  ts,
  arg_set_id
FROM __intrinsic_android_motion_events;

-- Input event dispatching information in Android (from android.input.inputevent data source).
CREATE PERFETTO VIEW android_input_event_dispatch(
  -- ID of the trace entry
  id INT,
  -- Event ID of the input event that was dispatched
  event_id INT,
  -- Extra args parsed from the proto message
  arg_set_id INT,
  -- Vsync ID that identifies the state of the windows during which the dispatch decision was made
  vsync_id INT,
  -- Window ID of the window receiving the event
  window_id INT
) AS
SELECT
  id,
  event_id,
  arg_set_id,
  vsync_id,
  window_id
FROM __intrinsic_android_input_event_dispatch;
