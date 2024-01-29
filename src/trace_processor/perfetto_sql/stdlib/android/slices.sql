--
-- Copyright 2023 The Android Open Source Project
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


CREATE PERFETTO FUNCTION _remove_lambda_name(
-- Raw slice name containing at least one "$"
  name STRING)
-- Removes everything after the first "$"
RETURNS STRING AS
SELECT
    substr($name, 0, instr($name, "$"))
END;

-- Some slice names have params in them. This functions removes them to make it
-- possible to aggregate by name.
-- Some examples are:
--  - Lock/monitor contention slices. The name includes where the lock
--    contention is in the code. That part is removed.
--  - DrawFrames/ooFrame. The name also includes the frame number.
--  - Apk/oat/dex loading: The name of the apk is removed
CREATE PERFETTO FUNCTION android_standardize_slice_name(
  -- The raw slice name.
  name STRING)
-- Simplified name.
RETURNS STRING AS
SELECT
  CASE
    WHEN $name GLOB "monitor contention with*" THEN "monitor contention with <...>"
    WHEN $name GLOB "SuspendThreadByThreadId*" THEN "SuspendThreadByThreadId <...>"
    WHEN $name GLOB "LoadApkAssetsFd*" THEN "LoadApkAssetsFd <...>"
    WHEN $name GLOB "relayoutWindow*" THEN "relayoutWindow <...>"
    WHEN $name GLOB "android.os.Handler: kotlinx.coroutines*" THEN "CoroutineContinuation"
    WHEN $name GLOB "Choreographer#doFrame*" THEN "Choreographer#doFrame"
    WHEN $name GLOB "DrawFrames*" THEN "DrawFrames"
    WHEN $name GLOB "/data/app*.apk" THEN "APK load"
    WHEN $name GLOB "OpenDexFilesFromOat*" THEN "OpenDexFilesFromOat"
    WHEN $name GLOB "Open oat file*" THEN "Open oat file"
    WHEN $name GLOB "GC: Wait For*" THEN "Garbage Collector"
    -- E.g. Lock contention on thread list lock (owner tid: 1665)
    -- To: Lock contention on thread list lock <...>
    WHEN $name GLOB "Lock contention on* (*" THEN substr($name, 0, instr($name, "(")) || "<...>"
    -- Top level handlers slices heuristics:
        -- E.g. android.os.Handler: com.android.systemui.qs.external.TileServiceManager$1
        -- To: Handler: com.android.systemui.qs.external.TileServiceManager
    WHEN $name GLOB "*Handler: *$*" THEN _remove_lambda_name(substr($name, instr($name, "Handler:")))
        -- E.g. : android.view.ViewRootImpl$ViewRootHandler: com.android.systemui.someClass$enableMarquee$1
        -- To: Handler: android.view.ViewRootImpl
    WHEN $name GLOB "*.*.*: *$*" THEN "Handler: " || _remove_lambda_name(substr($name, ": "))
        -- E.g.: android.os.AsyncTask$InternalHandler: #1
        -- To: Handler: android.os.AsyncTask
    WHEN $name GLOB "*.*$*: #*" THEN "Handler: " || _remove_lambda_name($name)
    ELSE $name
  END;
