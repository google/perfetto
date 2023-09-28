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
--
-- @column ts                  Timestamp in nanoseconds.
-- @column dur                 Duration (non-zero only in aggregate events)
-- @column track_name          The track name (interface and direction)
-- @column package_name        Traffic package source (or uid=$X if not found)
-- @column iface               Traffic interface name (linux interface name)
-- @column direction           Traffic direction ('Transmitted' or 'Received')
-- @column packet_count        Number of packets in this event
-- @column packet_length       Number of bytes in this event (wire size)
-- @column packet_transport    Transport used for traffic in this event
-- @column packet_tcp_flags    TCP flags used by tcp frames in this event
-- @column socket_tag          The Android traffic tag of the network socket
-- @column socket_uid          The Linux user id of the network socket
-- @column local_port          The local port number (for udp or tcp only)
-- @column remote_port         The remote port number (for udp or tcp only)
CREATE VIEW android_network_packets AS
SELECT
  ts,
  dur,
  track.name AS track_name,
  slice.name AS package_name,
  str_split(track.name, ' ', 0) AS iface,
  str_split(track.name, ' ', 1) AS direction,
  ifnull(extract_arg(arg_set_id, 'packet_count'), 1) AS packet_count,
  extract_arg(arg_set_id, 'packet_length') AS packet_length,
  extract_arg(arg_set_id, 'packet_transport') AS packet_transport,
  extract_arg(arg_set_id, 'packet_tcp_flags') AS packet_tcp_flags,
  extract_arg(arg_set_id, 'socket_tag') AS socket_tag,
  extract_arg(arg_set_id, 'socket_uid') AS socket_uid,
  extract_arg(arg_set_id, 'local_port') AS local_port,
  extract_arg(arg_set_id, 'remote_port') AS remote_port,
  extract_arg(arg_set_id, 'packet_icmp_type') AS packet_icmp_type,
  extract_arg(arg_set_id, 'packet_icmp_code') AS packet_icmp_code
FROM slice
JOIN track
  ON slice.track_id = track.id
WHERE (track.name GLOB '* Transmitted' OR
       track.name GLOB '* Received');
