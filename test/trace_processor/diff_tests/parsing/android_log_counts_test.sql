--
-- Copyright 2019 The Android Open Source Project
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
SELECT count(*) AS cnt FROM android_logs UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE prio = 3 UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE prio > 4 UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE tag = 'screen_toggled' UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE tag GLOB '*_pss' UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE msg GLOB '*i2c?write*' OR msg GLOB '*I2C?Write*' UNION ALL
SELECT count(*) AS cnt FROM android_logs WHERE ts >= 1510113924391 AND ts < 1512610021879;
