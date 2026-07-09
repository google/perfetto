--
-- Copyright 2020 The Android Open Source Project
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

-- Table to map any of the various chrome process names to a type (e.g. Browser,
-- Renderer, GPU Process, etc).
DROP VIEW IF EXISTS all_chrome_processes;
CREATE PERFETTO VIEW all_chrome_processes AS
SELECT upid, IFNULL(pt.string_value, '') AS process_type
FROM process
-- A process is a Chrome process if it has a chrome.process_type arg.
-- The value of the arg may be NULL.
-- All Chromium producers emit chrome_process field in their process track
-- descriptor when Chromium track event data source is enabled.
-- So this returns all processes in Chrome traces, and a subset of processes
-- in system traces.
JOIN args pt ON process.arg_set_id = pt.arg_set_id AND pt.key = 'chrome.process_type';

-- A view of all Chrome threads.
DROP VIEW IF EXISTS all_chrome_threads;
CREATE PERFETTO VIEW all_chrome_threads AS
SELECT utid, thread.upid, thread.name
FROM thread, all_chrome_processes
WHERE thread.upid = all_chrome_processes.upid;

-- Contains all the chrome processes from process with an extra column,
-- process_type.
DROP VIEW IF EXISTS chrome_process;
CREATE PERFETTO VIEW chrome_process AS
SELECT
  process.*,
  IIF(
    all_chrome_processes.process_type IN ("Sandboxed", "Privileged"),
    COALESCE(
      (
        SELECT SUBSTR(name, 3, LENGTH(name) - 6)
        FROM thread
        WHERE thread.upid = process.upid AND name GLOB "Cr*Main"
        LIMIT 1
      ),
      all_chrome_processes.process_type
    ),
    all_chrome_processes.process_type
  ) AS process_type
FROM process
JOIN all_chrome_processes ON process.upid = all_chrome_processes.upid;

-- Contains all the chrome threads from thread with an extra column,
-- canonical_name, that should contain a thread that's the same in both chrome
-- and system traces.
DROP VIEW IF EXISTS chrome_thread;

CREATE PERFETTO VIEW chrome_thread AS
SELECT thread.*,
  CASE
    WHEN thread.name GLOB "Cr*Main" THEN "CrProcessMain"
    WHEN thread.name IS NULL THEN "Unknown"
    ELSE thread.name
  END AS canonical_name
FROM thread
JOIN chrome_process ON thread.upid = chrome_process.upid;
