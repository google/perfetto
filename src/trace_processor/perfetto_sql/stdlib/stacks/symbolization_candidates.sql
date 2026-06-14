--
-- Copyright 2025 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE stacks.cpu_profiling;

-- On Linux/Android perfetto profilers (heapprofd, traced_perf) record the
-- .note.gnu.build-id as build_id. Within Google, this can be used as a lookup
-- key for most cases, but not for Chrome/Webview. Chrome is special and stores
-- symbols indexed by "breadkpad module ID". Breakpad module ID can be derived
-- with the following formula:
--      base::StrCat({module_id->substr(6, 2), module_id->substr(4, 2),
--                    module_id->substr(2, 2), module_id->substr(0, 2),
--                    module_id->substr(10, 2), module_id->substr(8, 2),
--                    module_id->substr(14, 2), module_id->substr(12, 2),
--                    module_id->substr(16, 16), "0"});
-- See also https://source.chromium.org/chromium/chromium/src/+/main:services/tracing/public/cpp/stack_sampling/tracing_sampler_profiler.cc;l=603;drc=cba00174ca338153b9c4f0c31ddbabaac7dd38c7
-- Note that in SQL SUBSTR() indexes are 1-based, not 0 based.
CREATE PERFETTO FUNCTION _to_breakpad_module(
  -- mapping name
  mapping STRING,
  -- linker build ID
  build_id STRING
)
RETURNS STRING
AS
SELECT
  CASE
    WHEN (($mapping GLOB '*libmonochrome_64.so'
    OR $mapping GLOB '*libchrome.so'
    OR $mapping GLOB '*libmonochrome.so'
    OR $mapping GLOB '*libwebviewchromium.so'
    OR $mapping GLOB '*libchromium_android_linker.so')
    AND length($build_id) >= 40) THEN (substr($build_id, 7, 2)
    || substr($build_id, 5, 2)
    || substr($build_id, 3, 2)
    || substr($build_id, 1, 2)
    || substr($build_id, 11, 2)
    || substr($build_id, 9, 2)
    || substr($build_id, 15, 2)
    || substr($build_id, 13, 2)
    || substr($build_id, 17, 16)
    || '0')
    ELSE NULL
  END;

-- Enumerates modules and rel_pcs that have no associated symbol information, broken down by caller process.
CREATE PERFETTO PIPELINE _stacks_symbolization_candidates(
  -- The process which is using this module
  upid JOINID(process.id),
  -- The module mapping (usually path)
  module STRING,
  -- The linker-generated build_id
  build_id STRING,
  -- The address corresponding to the frame
  rel_pc LONG,
  -- For chrome / webview .so, the breakpad module id derived from the build_id.
  -- This is only populated for chrome-like modules.
  breakpad_module_id STRING
)
MATERIALIZED AS
SUBPIPELINE all_callsites AS (
  SUBPIPELINE hprof_callsites AS (
    FROM heap_profile_allocation
    |> SELECT DISTINCT upid, callsite_id
  )
  FROM cpu_profiling_samples
  |> JOIN thread USING (utid)
  |> SELECT DISTINCT upid, callsite_id
  |> UNION ALL (FROM hprof_callsites)
)
SUBPIPELINE all_frames AS (
  SUBPIPELINE self_frames AS (
    FROM all_callsites
    |> JOIN stack_profile_callsite
       ON stack_profile_callsite.id = all_callsites.callsite_id
    |> SELECT all_callsites.upid, frame_id
  )
  SUBPIPELINE joined_frames AS (
    -- experimental_ancestor_stack_profile_callsite is a lateral table-valued
    -- function (native intrinsic), kept as a host-SQL correlated join.
    FROM all_callsites, experimental_ancestor_stack_profile_callsite(all_callsites.callsite_id)
    |> SELECT all_callsites.upid, frame_id
    |> UNION ALL (FROM self_frames)
  )
  FROM joined_frames
  |> JOIN stack_profile_frame AS spf
     ON joined_frames.frame_id = spf.id
  |> WHERE spf.symbol_set_id IS NULL
  |> SELECT DISTINCT upid, spf.mapping AS mapping_id, spf.rel_pc
)
FROM all_frames
|> JOIN stack_profile_mapping AS spm
   ON all_frames.mapping_id = spm.id
|> WHERE spm.build_id != ''
|> SELECT
     all_frames.upid AS upid,
     spm.name AS module,
     spm.build_id AS build_id,
     all_frames.rel_pc AS rel_pc,
     _to_breakpad_module(spm.name, spm.build_id) AS breakpad_module_id;
