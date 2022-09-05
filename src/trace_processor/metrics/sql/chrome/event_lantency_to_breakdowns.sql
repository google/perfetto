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
  EXTRACT_ARG(arg_set_id, "event_latency.event_type") as event_type
FROM slice
WHERE
  name = "EventLatency";

-- Select breakdowns related to EventLatencies from `event_latency` table.
DROP VIEW IF EXISTS event_latency_breakdowns;
CREATE VIEW event_latency_breakdowns
AS
SELECT
  slice.id as slice_id,
  slice.name as name,
  slice.dur as dur,
  slice.track_id as track_id,
  slice.ts as ts,
  event_latency.slice_id as event_latency_id,
  event_latency.track_id as event_latency_track_id,
  event_latency.ts as event_latency_ts,
  event_latency.dur as event_latency_dur,
  event_latency.event_type as event_type
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
  max(CASE WHEN name = "GenerationToRendererCompositor" THEN dur end) GenerationToRendererCompositorNs,
  max(CASE WHEN name = "GenerationToBrowserMain" THEN dur end) GenerationToBrowserMainNs,
  max(CASE WHEN name = "BrowserMainToRendererCompositor" THEN dur end) BrowserMainToRendererCompositorNs,
  max(CASE WHEN name = "RendererCompositorQueueingDelay" THEN dur end) RendererCompositorQueueingDelayNs,
  max(CASE WHEN name = "RendererCompositorProcessing" THEN dur end) RendererCompositorProcessingNs,
  max(CASE WHEN name = "RendererCompositorToMain" THEN dur end) RendererCompositorToMainNs,
  max(CASE WHEN name = "RendererMainProcessing" THEN dur end) RendererMainProcessingNs,

  max(CASE WHEN name = "ArrivedInRendererCompositorToTermination" THEN dur end) ArrivedInRendererCompositorToTerminationNs,
  max(CASE WHEN name = "RendererCompositorStartedToTermination" THEN dur end) RendererCompositorStartedToTerminationNs,
  max(CASE WHEN name = "RendererCompositorFinishedToTermination" THEN dur end) RendererCompositorFinishedToTerminationNs,
  max(CASE WHEN name = "RendererMainStartedToTermination" THEN dur end) RendererMainStartedToTerminationNs,
  max(CASE WHEN name = "RendererMainFinishedToTermination" THEN dur end) RendererMainFinishedToTerminationNs,

  max(CASE WHEN name = "BeginImplFrameToSendBeginMainFrame" THEN dur end) BeginImplFrameToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToSendBeginMainFrame" THEN dur end) RendererCompositorFinishedToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToBeginImplFrame" THEN dur end) RendererCompositorFinishedToBeginImplFrameNs,
  max(CASE WHEN name = "RendererCompositorFinishedToCommit" THEN dur end) RendererCompositorFinishedToCommitNs,
  max(CASE WHEN name = "RendererCompositorFinishedToEndCommit" THEN dur end) RendererCompositorFinishedToEndCommitNs,
  max(CASE WHEN name = "RendererCompositorFinishedToActivation" THEN dur end) RendererCompositorFinishedToActivationNs,
  max(CASE WHEN name = "RendererCompositorFinishedToEndActivate" THEN dur end) RendererCompositorFinishedToEndActivateNs,
  max(CASE WHEN name = "RendererCompositorFinishedToSubmitCompositorFrame" THEN dur end) RendererCompositorFinishedToSubmitCompositorFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToBeginImplFrame" THEN dur end) RendererMainFinishedToBeginImplFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToSendBeginMainFrame" THEN dur end) RendererMainFinishedToSendBeginMainFrameNs,
  max(CASE WHEN name = "RendererMainFinishedToCommit" THEN dur end) RendererMainFinishedToCommitNs,
  max(CASE WHEN name = "RendererMainFinishedToEndCommit" THEN dur end) RendererMainFinishedToEndCommitNs,
  max(CASE WHEN name = "RendererMainFinishedToActivation" THEN dur end) RendererMainFinishedToActivationNs,
  max(CASE WHEN name = "RendererMainFinishedToEndActivate" THEN dur end) RendererMainFinishedToEndActivateNs,
  max(CASE WHEN name = "RendererMainFinishedToSubmitCompositorFrame" THEN dur end) RendererMainFinishedToSubmitCompositorFrameNs,

  max(CASE WHEN name = "EndActivateToSubmitCompositorFrame" THEN dur end) EndActivateToSubmitCompositorFrameNs,
  max(CASE WHEN name = "SubmitCompositorFrameToPresentationCompositorFrame" THEN dur end) SubmitCompositorFrameToPresentationCompositorFrameNs,
  max(CASE WHEN name = "SendBeginMainFrameToCommit" THEN dur end) SendBeginMainFrameToCommitNs,
  max(CASE WHEN name = "Commit" THEN dur end) CommitNs,
  max(CASE WHEN name = "EndCommitToActivation" THEN dur end) EndCommitToActivationNs,
  max(CASE WHEN name = "Activation" THEN dur end) ActivationNs,
-- This column indicates whether there are unknown breakdowns.
-- Contains: NULL if there are no unknown breakdowns, otherwise a list of unknown breakdows.
  group_concat(InvalidNameOrNull(name), ', ') as unknown_stages_seen
FROM event_latency_breakdowns
GROUP BY event_latency_id;