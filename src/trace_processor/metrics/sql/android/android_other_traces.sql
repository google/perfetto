--
-- Copyright 2022 The Android Open Source Project
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
--

DROP VIEW IF EXISTS android_other_traces_view;
CREATE VIEW android_other_traces_view AS
SELECT
  ts,
  dur,
  SUBSTR(slice.name, 15) AS uuid,
  'Finalize' AS event_type
FROM slice
JOIN track
  ON track.name = 'OtherTraces' AND slice.track_id = track.id
WHERE
  slice.name GLOB 'finalize-uuid-*';

DROP VIEW IF EXISTS android_other_traces_event;
CREATE VIEW android_other_traces_event AS
SELECT
  'slice' AS track_type,
  'Other Traces' AS track_name,
  ts,
  dur,
  event_type || ' ' || uuid AS slice_name
FROM android_other_traces_view;

DROP VIEW IF EXISTS android_other_traces_output;
CREATE VIEW android_other_traces_output AS
SELECT AndroidOtherTracesMetric(
    'finalized_traces_uuid', (
      SELECT RepeatedField(uuid)
      FROM android_other_traces_view
      WHERE event_type = 'Finalize')
  );
