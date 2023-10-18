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
--

CREATE OR REPLACE PERFETTO FUNCTION GET_EVENT_LATENCY_TABLE(event_name STRING)
RETURNS TABLE (latency LONG) AS
SELECT
  dur / 1e6 as latency
FROM
  slices
WHERE
  name = $event_name;

DROP VIEW IF EXISTS ad_services_metric_output;
CREATE VIEW ad_services_metric_output AS
SELECT
  AdServicesMetric(
    'ui_metric',
    (
      SELECT
        RepeatedField(
          AdServicesUiMetric('latency', latency)
        )
      FROM
        GET_EVENT_LATENCY_TABLE("NotificationTriggerEvent")
    ),
    'app_set_id_metric',
    (
      SELECT
        RepeatedField(
          AdServicesAppSetIdMetric(
            'latency', latency
          )
        )
      FROM
        GET_EVENT_LATENCY_TABLE("AdIdCacheEvent")
    ),
    'ad_id_metric',
    (
      SELECT
        RepeatedField(
          AdServicesAdIdMetric('latency', latency)
        )
      FROM
        GET_EVENT_LATENCY_TABLE("AppSetIdEvent")
    )
);
