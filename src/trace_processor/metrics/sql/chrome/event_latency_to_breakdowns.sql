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
-- Creates metric with info about breakdowns and jank for GestureScrollBegin and GestureScrollUpdate.

-- Select EventLatency events.
DROP VIEW IF EXISTS event_latency;
CREATE VIEW event_latency
AS
SELECT
  *,
  EXTRACT_ARG(arg_set_id, "event_latency.event_type") AS event_type
FROM slice
WHERE
  name = "EventLatency";

-- Select breakdowns related to EventLatencies from `event_latency` table.
DROP VIEW IF EXISTS event_latency_breakdowns;
CREATE VIEW event_latency_breakdowns
AS
SELECT
  slice.id AS slice_id,
  slice.name AS name,
  slice.dur AS dur,
  slice.track_id AS track_id,
  slice.ts AS ts,
  event_latency.slice_id AS event_latency_id,
  event_latency.track_id AS event_latency_track_id,
  event_latency.ts AS event_latency_ts,
  event_latency.dur AS event_latency_dur,
  event_latency.event_type AS event_type
FROM slice JOIN event_latency
  ON slice.parent_id = event_latency.slice_id;

-- The function takes a breakdown name and checks if the breakdown name is known or not.
SELECT CREATE_FUNCTION(
  'InvalidNameOrNull(name STRING)',
  -- Returns the input breakdown name if it's an unknown breakdown, NULL otherwise.
  'STRING',
  'SELECT
    CASE
      WHEN
      $name not in (
        "GenerationToBrowserMain", "GenerationToRendererCompositor",
        "BrowserMainToRendererCompositor", "RendererCompositorQueueingDelay",
        "RendererCompositorToMain", "RendererCompositorProcessing",
        "RendererMainProcessing", "EndActivateToSubmitCompositorFrame",
        "SubmitCompositorFrameToPresentationCompositorFrame",
        "ArrivedInRendererCompositorToTermination",
        "RendererCompositorStartedToTermination",
        "RendererMainFinishedToTermination",
        "RendererCompositorFinishedToTermination",
        "RendererMainStartedToTermination",
        "RendererCompositorFinishedToBeginImplFrame",
        "RendererCompositorFinishedToCommit",
        "RendererCompositorFinishedToEndCommit",
        "RendererCompositorFinishedToActivation",
        "RendererCompositorFinishedToEndActivate", 
        "RendererCompositorFinishedToSubmitCompositorFrame",
        "RendererMainFinishedToBeginImplFrame",
        "RendererMainFinishedToSendBeginMainFrame",
        "RendererMainFinishedToCommit", "RendererMainFinishedToEndCommit",
        "RendererMainFinishedToActivation", "RendererMainFinishedToEndActivate",
        "RendererMainFinishedToSubmitCompositorFrame",
        "BeginImplFrameToSendBeginMainFrame",
        "RendererCompositorFinishedToSendBeginMainFrame",
        "SendBeginMainFrameToCommit", "Commit",
        "EndCommitToActivation", "Activation")
        THEN $name
      ELSE NULL
    END'
);

-- Creates a view where each row contains information about one EventLatency event. Columns are duration of breakdowns.
-- In the result it will be something like this:
-- | event_latency_id | event_latency_ts | event_latency_dur | event_type       | GenerationToBrowserMainNs | BrowserMainToRendererCompositorNs |...|
-- |------------------|------------------|-------------------|------------------|----------------------------|------------------------------------|---|
-- | 123              | 1661947470       | 20                | 1234567          | 30                         | 50                                 |   |
DROP VIEW IF EXISTS event_latency_to_breakdowns;
CREATE VIEW event_latency_to_breakdowns
AS
SELECT
  event_latency_id,
  event_latency_track_id,
  event_latency_ts,
  event_latency_dur,
  event_type,
  max(CASE WHEN name = "GenerationToRendererCompositor" THEN dur END) AS GenerationToRendererCompositorNs,
  max(CASE WHEN name = "GenerationToBrowserMain" THEN dur END) AS GenerationToBrowserMainNs,
  max(CASE WHEN name = "BrowserMainToRendererCompositor" THEN dur END) AS BrowserMainToRendererCompositorNs,
  max(CASE WHEN name = "RendererCompositorQueueingDelay" THEN dur END) AS RendererCompositorQueueingDelayNs,
  max(CASE WHEN name = "RendererCompositorProcessing" THEN dur END) AS RendererCompositorProcessingNs,
  max(CASE WHEN name = "RendererCompositorToMain" THEN dur END) AS RendererCompositorToMainNs,
  max(CASE WHEN name = "RendererMainProcessing" THEN dur END) AS RendererMainProcessingNs,

  max(CASE WHEN name = "ArrivedInRendererCompositorToTermination" THEN dur END) AS ArrivedInRendererCompositorToTerminationNs,
  max(CASE WHEN name = "RendererCompositorStartedToTermination" THEN dur END) AS RendererCompositorStartedToTerminationNs,
  max(CASE WHEN name = "RendererCompositorFinishedToTermination" THEN dur END) AS RendererCompositorFinishedToTerminationNs,
  max(CASE WHEN name = "RendererMainStartedToTermination" THEN dur END) AS RendererMainStartedToTerminationNs,
  max(CASE WHEN name = "RendererMainFinishedToTermination" THEN dur END) AS RendererMainFinishedToTerminationNs,

  max(CASE WHEN name = "BeginImplFrameToSendBeginMainFrame" THEN dur END) AS BeginImplFrameToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToSendBeginMainFrame" THEN dur END) AS RendererCompositorFinishedToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToBeginImplFrame" THEN dur END) AS RendererCompositorFinishedToBeginImplFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToCommit" THEN dur END) AS RendererCompositorFinishedToCommitNs,
  max(CASE WHEN name = "RendererCompositorFinishedToEndCommit" THEN dur END) AS RendererCompositorFinishedToEndCommitNs,
  max(CASE WHEN name = "RendererCompositorFinishedToActivation" THEN dur END) AS RendererCompositorFinishedToActivationNs,
  max(CASE WHEN name = "RendererCompositorFinishedToEndActivate" THEN dur END) AS RendererCompositorFinishedToEndActivateNs,
  max(CASE WHEN name = "RendererCompositorFinishedToSubmitCompositorFrame" THEN dur END) AS RendererCompositorFinishedToSubmitCompositorFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToBeginImplFrame" THEN dur END) AS RendererMainFinishedToBeginImplFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToSendBeginMainFrame" THEN dur END) AS RendererMainFinishedToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToCommit" THEN dur END) AS RendererMainFinishedToCommitNs,
  max(CASE WHEN name = "RendererMainFinishedToEndCommit" THEN dur END) AS RendererMainFinishedToEndCommitNs,
  max(CASE WHEN name = "RendererMainFinishedToActivation" THEN dur END) AS RendererMainFinishedToActivationNs,
  max(CASE WHEN name = "RendererMainFinishedToEndActivate" THEN dur END) AS RendererMainFinishedToEndActivateNs,
  max(CASE WHEN name = "RendererMainFinishedToSubmitCompositorFrame" THEN dur END) AS RendererMainFinishedToSubmitCompositorFrameNs,

  max(CASE WHEN name = "EndActivateToSubmitCompositorFrame" THEN dur END) AS EndActivateToSubmitCompositorFrameNs,
  max(CASE WHEN name = "SubmitCompositorFrameToPresentationCompositorFrame" THEN dur END) AS SubmitCompositorFrameToPresentationCompositorFrameNs,
  max(CASE WHEN name = "SendBeginMainFrameToCommit" THEN dur END) AS SendBeginMainFrameToCommitNs,
  max(CASE WHEN name = "Commit" THEN dur END) AS CommitNs,
  max(CASE WHEN name = "EndCommitToActivation" THEN dur END) AS EndCommitToActivationNs,
  max(CASE WHEN name = "Activation" THEN dur END) AS ActivationNs,
  -- This column indicates whether there are unknown breakdowns.
  -- Contains: NULL if there are no unknown breakdowns, otherwise a list of unknown breakdows.
  group_concat(InvalidNameOrNull(name), ', ') AS unknown_stages_seen
FROM event_latency_breakdowns
GROUP BY event_latency_id;
