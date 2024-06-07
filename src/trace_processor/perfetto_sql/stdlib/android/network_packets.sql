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

-- Android network packet events (from android.network_packets data source).
CREATE PERFETTO VIEW android_network_packets(
  -- Timestamp in nanoseconds.
  ts INT,
  -- Duration (non-zero only in aggregate events)
  dur INT,
  -- The track name (interface and direction)
  track_name STRING,
  -- Traffic package source (or uid=$X if not found)
  package_name STRING,
  -- Traffic interface name (linux interface name)
  iface STRING,
  -- Traffic direction ('Transmitted' or 'Received')
  direction STRING,
  -- Number of packets in this event
  packet_count INT,
  -- Number of bytes in this event (wire size)
  packet_length INT,
  -- Transport used for traffic in this event
  packet_transport STRING,
  -- TCP flags used by tcp frames in this event
  packet_tcp_flags INT,
  -- The Android traffic tag of the network socket
  socket_tag STRING,
  -- The Linux user id of the network socket
  socket_uid INT,
  -- The local port number (for udp or tcp only)
  local_port INT,
  -- The remote port number (for udp or tcp only)
  remote_port INT,
  -- 1-byte ICMP type identifier.
  packet_icmp_type INT,
  -- 1-byte ICMP code identifier.
  packet_icmp_code INT,
  -- Packet's tcp flags bitmask (e.g. FIN=0x1, SYN=0x2).
  packet_tcp_flags_int INT,
  -- Packet's socket tag as an integer.
  socket_tag_int INT
) AS
SELECT
  ts,
  dur,
  category AS track_name,
  name AS package_name,
  iface,
  direction,
  packet_count,
  packet_length,
  packet_transport,
  -- For backwards compatibility, the _str suffixed flags (which the ui shows)
  -- are exposed without suffix, and the integer fields get suffix instead.
  packet_tcp_flags_str AS packet_tcp_flags,
  packet_tcp_flags AS packet_tcp_flags_int,
  socket_tag_str AS socket_tag,
  socket_tag AS socket_tag_int,
  socket_uid,
  local_port,
  remote_port,
  packet_icmp_type,
  packet_icmp_code
FROM __intrinsic_android_network_packets;
