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
CREATE VIEW v1 AS SELECT tag, count(*) FROM android_logs GROUP BY tag ORDER BY 2 DESC LIMIT 5;

CREATE VIEW v2 AS SELECT tag, count(*) FROM android_logs GROUP BY tag ORDER BY 2 ASC LIMIT 5;

CREATE VIEW v3 AS
SELECT tag, count(*)
FROM android_logs
WHERE msg GLOB '*wakelock*' OR msg GLOB '*Wakelock*' OR msg GLOB '*WakeLock*' OR msg GLOB '*wakeLock*'
GROUP BY tag;

CREATE VIEW v4 AS SELECT msg, 1 FROM android_logs LIMIT 10;

SELECT * FROM v1 UNION ALL
SELECT '-----', 0 UNION ALL
SELECT * FROM v2 UNION ALL
SELECT '-----', 0 UNION ALL
SELECT * FROM v3 UNION ALL
SELECT '-----', 0 UNION ALL
SELECT * FROM v4;
