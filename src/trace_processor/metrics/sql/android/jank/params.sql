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

-- (Deprecated) Table to store parameters that will be matched with CUJs using
-- the CUJ name.
--
-- Note that this list should not be used anymore: in recent android versions
-- each CUJ reports an instant event on their ui thread that can be used to
-- automatically find the thread id.
-- This is kept for compatibility with old android versions, as a fallback only.
-- TODO: b/358038927 - Remove this list in 2025
DROP TABLE IF EXISTS android_jank_cuj_param_set;
CREATE TABLE android_jank_cuj_param_set (cuj_name_glob STRING, main_thread_override STRING);
INSERT INTO android_jank_cuj_param_set (cuj_name_glob, main_thread_override)
VALUES
('SPLASHSCREEN_EXIT_ANIM', 'll.splashscreen'),
('SPLASHSCREEN_AVD', 'll.splashscreen'),
('ONE_HANDED_ENTER_TRANSITION::*', 'wmshell.main'),
('ONE_HANDED_EXIT_TRANSITION::*', 'wmshell.main'),
('PIP_TRANSITION::*', 'wmshell.main'),
('BACK_PANEL_ARROW', 'BackPanelUiThre');


-- Matches each CUJ with the right set of parameters.
DROP TABLE IF EXISTS android_jank_cuj_param;
CREATE PERFETTO TABLE android_jank_cuj_param AS
SELECT cuj_id, main_thread_override
FROM android_jank_cuj
LEFT JOIN android_jank_cuj_param_set ON cuj_name GLOB cuj_name_glob;
