--
-- Copyright 2021 The Android Open Source Project
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

DROP VIEW IF EXISTS rx_packets;
CREATE VIEW rx_packets AS
  SELECT
    ts,
    RTRIM(name, " Received KB") AS dev,
    EXTRACT_ARG(arg_set_id, 'cpu') AS cpu,
    EXTRACT_ARG(arg_set_id, 'len') AS len
  FROM counter c
  LEFT JOIN counter_track t
    ON c.track_id = t.id
  WHERE name GLOB "* Received KB"
  ORDER BY ts DESC;

DROP VIEW IF EXISTS device_total_ingress_traffic;
CREATE VIEW device_total_ingress_traffic AS
  SELECT
    dev,
    MIN(ts) AS start_ts,
    MAX(ts) AS end_ts,
    IIF((MAX(ts) - MIN(ts)) > 10000000, MAX(ts)-MIN(ts), 10000000) AS interval,
    count(1) AS packets,
    SUM(len) AS bytes
  FROM rx_packets
  GROUP BY dev;

DROP VIEW IF EXISTS device_per_core_ingress_traffic;
CREATE VIEW device_per_core_ingress_traffic AS
  SELECT
    dev,
    AndroidNetworkMetric_CorePacketStatistic(
      'id', cpu,
      'packet_statistic', AndroidNetworkMetric_PacketStatistic(
        'packets', count(1),
        'bytes', SUM(len),
        'first_packet_timestamp_ns', MIN(ts),
        'last_packet_timestamp_ns', MAX(ts),
        'interval_ns', IIF((MAX(ts)-MIN(ts))>10000000, MAX(ts)-MIN(ts), 10000000),
        'data_rate_kbps', (SUM(len)*8)/(IIF((MAX(ts)-MIN(ts))>10000000, MAX(ts)-MIN(ts), 10000000)/1e9)/1024
      )
    ) AS proto
  FROM rx_packets
  GROUP BY dev, cpu;

DROP VIEW IF EXISTS device_ingress_traffic_statistic;
CREATE VIEW device_ingress_traffic_statistic AS
  SELECT
    AndroidNetworkMetric_NetDevice(
      'name', dev,
      'rx', AndroidNetworkMetric_Rx(
        'total', AndroidNetworkMetric_PacketStatistic(
          'packets', packets,
          'bytes', bytes,
          'first_packet_timestamp_ns', start_ts,
          'last_packet_timestamp_ns', end_ts,
          'interval_ns', interval,
          'data_rate_kbps', (bytes*8)/(interval/1e9)/1024
        ),
        'core', (
          SELECT
            RepeatedField(proto)
          FROM device_per_core_ingress_traffic
          WHERE device_per_core_ingress_traffic.dev = device_total_ingress_traffic.dev
        )
      )
    ) AS proto
  FROM device_total_ingress_traffic
  ORDER BY dev;

DROP VIEW IF EXISTS android_netperf_output;
CREATE VIEW android_netperf_output AS
  SELECT AndroidNetworkMetric(
    'net_devices', (
      SELECT
        RepeatedField(proto)
      FROM device_ingress_traffic_statistic
    )
  );

