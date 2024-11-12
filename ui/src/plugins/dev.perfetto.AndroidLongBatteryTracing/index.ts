// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {Engine} from '../../trace_processor/engine';
import {createQuerySliceTrack} from '../../public/lib/tracks/query_slice_track';
import {CounterOptions} from '../../frontend/base_counter_track';
import {createQueryCounterTrack} from '../../public/lib/tracks/query_counter_track';
import {TrackNode} from '../../public/workspace';

interface ContainedTrace {
  uuid: string;
  subscription: string;
  trigger: string;
  // NB: these are millis.
  ts: number;
  dur: number;
}

const PACKAGE_LOOKUP = `
  create or replace perfetto table package_name_lookup as
  with installed as (
    select uid, string_agg(package_name, ',') as name
    from package_list
    where uid >= 10000
    group by 1
  ),
  system(uid, name) as (
    values
      (0, 'AID_ROOT'),
      (1000, 'AID_SYSTEM_USER'),
      (1001, 'AID_RADIO'),
      (1082, 'AID_ARTD')
  )
  select uid, name from installed
  union all
  select uid, name from system
  order by uid;

  -- Adds a "package_name" column by joining on "uid" from the source table.
  create or replace perfetto macro add_package_name(src TableOrSubquery) returns TableOrSubquery as (
    select A.*, ifnull(B.name, "uid=" || A.uid) as package_name
    from $src as A
    left join package_name_lookup as B
    on (B.uid = (A.uid % 100000))
  );
`;

const DEFAULT_NETWORK = `
  with base as (
      select
          ts,
          substr(s.name, 6) as conn
      from track t join slice s on t.id = s.track_id
      where t.name = 'battery_stats.conn'
  ),
  diff as (
      select
          ts,
          conn,
          conn != lag(conn) over (order by ts) as keep
      from base
  )
  select
      ts,
      ifnull(lead(ts) over (order by ts), (select end_ts from trace_bounds)) - ts as dur,
      case
        when conn like '-1:%' then 'Disconnected'
        when conn like '0:%' then 'Modem'
        when conn like '1:%' then 'WiFi'
        when conn like '4:%' then 'VPN'
        else conn
      end as name
  from diff where keep is null or keep`;

const RADIO_TRANSPORT_TYPE = `
  create or replace perfetto view radio_transport_data_conn as
  select ts, safe_dur AS dur, value_name as data_conn, value AS data_conn_val
  from android_battery_stats_state
  where track_name = "battery_stats.data_conn";

  create or replace perfetto view radio_transport_nr_state as
  select ts, safe_dur AS dur, value AS nr_state_val
  from android_battery_stats_state
  where track_name = "battery_stats.nr_state";

  drop table if exists radio_transport_join;
  create virtual table radio_transport_join
  using span_left_join(radio_transport_data_conn, radio_transport_nr_state);

  create or replace perfetto view radio_transport as
  select
    ts, dur,
    case data_conn_val
      -- On LTE with NR connected is 5G NSA.
      when 13 then iif(nr_state_val = 3, '5G (NSA)', data_conn)
      -- On NR with NR state present, is 5G SA.
      when 20 then iif(nr_state_val is null, '5G (SA or NSA)', '5G (SA)')
      else data_conn
    end as name
  from radio_transport_join;`;

const TETHERING = `
  with base as (
      select
          ts as ts_end,
          EXTRACT_ARG(arg_set_id, 'network_tethering_reported.duration_millis') * 1000000 as dur
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
        and s.name = 'network_tethering_reported'
  )
  select ts_end - dur as ts, dur, 'Tethering' as name from base`;

const NETWORK_SUMMARY = `
  create or replace perfetto table network_summary as
  with base as (
      select
          cast(ts / 5000000000 as int64) * 5000000000 AS ts,
          case
              when track_name glob '*wlan*' then 'wifi'
              when track_name glob '*rmnet*' then 'modem'
              else 'unknown'
          end as dev_type,
          package_name as pkg,
          sum(packet_length) AS value
      from android_network_packets
      where (track_name glob '*wlan*' or track_name glob '*rmnet*')
      group by 1,2,3
  ),
  zeroes as (
      select
          ts,
          dev_type,
          pkg,
          value
      from base
      union all
      select
          ts + 5000000000 as ts,
          dev_type,
          pkg,
          0 as value
      from base
  ),
  final as (
      select
          ts,
          dev_type,
          pkg,
          sum(value) as value
      from zeroes
      group by 1, 2, 3
  )
  select * from final where ts is not null`;

const MODEM_RIL_STRENGTH = `
  DROP VIEW IF EXISTS ScreenOn;
  CREATE VIEW ScreenOn AS
  SELECT ts, dur FROM (
      SELECT
          ts, value,
          LEAD(ts, 1, TRACE_END()) OVER (ORDER BY ts)-ts AS dur
      FROM counter, track ON (counter.track_id = track.id)
      WHERE track.name = 'ScreenState'
  ) WHERE value = 2;

  DROP VIEW IF EXISTS RilSignalStrength;
  CREATE VIEW RilSignalStrength AS
  With RilMessages AS (
      SELECT
          ts, slice.name,
          LEAD(ts, 1, TRACE_END()) OVER (ORDER BY ts)-ts AS dur
      FROM slice, track
      ON (slice.track_id = track.id)
      WHERE track.name = 'RIL'
        AND slice.name GLOB 'UNSOL_SIGNAL_STRENGTH*'
  ),
  BandTypes(band_ril, band_name) AS (
      VALUES ("CellSignalStrengthLte:", "LTE"),
              ("CellSignalStrengthNr:", "NR")
  ),
  ValueTypes(value_ril, value_name) AS (
      VALUES ("rsrp=", "rsrp"),
              ("rssi=", "rssi")
  ),
  Extracted AS (
      SELECT ts, dur, band_name, value_name, (
          SELECT CAST(SUBSTR(key_str, start_idx+1, end_idx-start_idx-1) AS INT64) AS value
          FROM (
              SELECT key_str, INSTR(key_str, "=") AS start_idx, INSTR(key_str, " ") AS end_idx
              FROM (
                  SELECT SUBSTR(band_str, INSTR(band_str, value_ril)) AS key_str
                  FROM (SELECT SUBSTR(name, INSTR(name, band_ril)) AS band_str)
              )
          )
      ) AS value
      FROM RilMessages
      JOIN BandTypes
      JOIN ValueTypes
  )
  SELECT
  ts, dur, band_name, value_name, value,
  value_name || "=" || IIF(value = 2147483647, "unknown", ""||value) AS name,
  ROW_NUMBER() OVER (ORDER BY ts) as id,
  DENSE_RANK() OVER (ORDER BY band_name, value_name) AS track_id
  FROM Extracted;

  DROP TABLE IF EXISTS RilScreenOn;
  CREATE VIRTUAL TABLE RilScreenOn
  USING SPAN_JOIN(RilSignalStrength PARTITIONED track_id, ScreenOn)`;

const MODEM_RIL_CHANNELS_PREAMBLE = `
  CREATE OR REPLACE PERFETTO FUNCTION EXTRACT_KEY_VALUE(source STRING, key_name STRING) RETURNS STRING AS
  SELECT SUBSTR(trimmed, INSTR(trimmed, "=")+1, INSTR(trimmed, ",") - INSTR(trimmed, "=") - 1)
  FROM (SELECT SUBSTR($source, INSTR($source, $key_name)) AS trimmed);`;

const MODEM_RIL_CHANNELS = `
  With RawChannelConfig AS (
      SELECT ts, slice.name AS raw_config
      FROM slice, track
      ON (slice.track_id = track.id)
      WHERE track.name = 'RIL'
      AND slice.name LIKE 'UNSOL_PHYSICAL_CHANNEL_CONFIG%'
  ),
  Attributes(attribute, attrib_name) AS (
      VALUES ("mCellBandwidthDownlinkKhz", "downlink"),
          ("mCellBandwidthUplinkKhz", "uplink"),
          ("mNetworkType", "network"),
          ("mBand", "band")
  ),
  Slots(idx, slot_name) AS (
      VALUES (0, "primary"),
          (1, "secondary 1"),
          (2, "secondary 2")
  ),
  Stage1 AS (
      SELECT *, IFNULL(EXTRACT_KEY_VALUE(STR_SPLIT(raw_config, "}, {", idx), attribute), "") AS name
      FROM RawChannelConfig
      JOIN Attributes
      JOIN Slots
  ),
  Stage2 AS (
      SELECT *, LAG(name) OVER (PARTITION BY idx, attribute ORDER BY ts) AS last_name
      FROM Stage1
  ),
  Stage3 AS (
      SELECT *, LEAD(ts, 1, TRACE_END()) OVER (PARTITION BY idx, attribute ORDER BY ts) - ts AS dur
      FROM Stage2 WHERE name != last_name
  )
  SELECT ts, dur, slot_name || "-" || attrib_name || "=" || name AS name
  FROM Stage3`;

const MODEM_CELL_RESELECTION = `
  with base as (
    select
        ts,
        s.name as raw_ril,
        ifnull(str_split(str_split(s.name, 'CellIdentityLte{', 1), ', operatorNames', 0),
            str_split(str_split(s.name, 'CellIdentityNr{', 1), ', operatorNames', 0)) as cell_id
    from track t join slice s on t.id = s.track_id
    where t.name = 'RIL' and s.name like '%DATA_REGISTRATION_STATE%'
  ),
  base2 as (
    select
        ts,
        raw_ril,
        case
            when cell_id like '%earfcn%' then 'LTE ' || cell_id
            when cell_id like '%nrarfcn%' then 'NR ' || cell_id
            when cell_id is null then 'Unknown'
            else cell_id
        end as cell_id
    from base
  ),
  base3 as (
    select ts, cell_id , lag(cell_id) over (order by ts) as lag_cell_id, raw_ril
    from base2
  )
  select ts, 0 as dur, cell_id as name, raw_ril
  from base3
  where cell_id != lag_cell_id
  order by ts`;

const SUSPEND_RESUME = `
  SELECT
    ts,
    dur,
    'Suspended' AS name
  FROM android_suspend_state
  WHERE power_state = 'suspended'`;

const SCREEN_STATE = `
  WITH _counter AS (
    SELECT counter.id, ts, 0 AS track_id, value
    FROM counter
    JOIN counter_track ON counter_track.id = counter.track_id
    WHERE name = 'ScreenState'
  )
  SELECT
    ts,
    dur,
    CASE value
      WHEN 1 THEN 'Screen off'
      WHEN 2 THEN 'Screen on'
      WHEN 3 THEN 'Always-on display (doze)'
      ELSE 'unknown'
    END AS name
  FROM counter_leading_intervals!(_counter)`;

// See DeviceIdleController.java for where these states come from and how
// they transition.
const DOZE_LIGHT = `
  WITH _counter AS (
    SELECT counter.id, ts, 0 AS track_id, value
    FROM counter
    JOIN counter_track ON counter_track.id = counter.track_id
    WHERE name = 'DozeLightState'
  )
  SELECT
    ts,
    dur,
    CASE value
      WHEN 0 THEN 'active'
      WHEN 1 THEN 'inactive'
      WHEN 4 THEN 'idle'
      WHEN 5 THEN 'waiting_for_network'
      WHEN 6 THEN 'idle_maintenance'
      WHEN 7 THEN 'override'
      ELSE 'unknown'
    END AS name
  FROM counter_leading_intervals!(_counter)`;

const DOZE_DEEP = `
  WITH _counter AS (
    SELECT counter.id, ts, 0 AS track_id, value
    FROM counter
    JOIN counter_track ON counter_track.id = counter.track_id
    WHERE name = 'DozeDeepState'
  )
  SELECT
    ts,
    dur,
    CASE value
      WHEN 0 THEN 'active'
      WHEN 1 THEN 'inactive'
      WHEN 2 THEN 'idle_pending'
      WHEN 3 THEN 'sensing'
      WHEN 4 THEN 'locating'
      WHEN 5 THEN 'idle'
      WHEN 6 THEN 'idle_maintenance'
      WHEN 7 THEN 'quick_doze_delay'
      ELSE 'unknown'
    END AS name
  FROM counter_leading_intervals!(_counter)`;

const CHARGING = `
  WITH _counter AS (
    SELECT counter.id, ts, 0 AS track_id, value
    FROM counter
    JOIN counter_track ON counter_track.id = counter.track_id
    WHERE name = 'BatteryStatus'
  )
  SELECT
    ts,
    dur,
    CASE value
      -- 0 and 1 are both unknown
      WHEN 2 THEN 'Charging'
      WHEN 3 THEN 'Discharging'
      -- special case when charger is present but battery isn't charging
      WHEN 4 THEN 'Not charging'
      WHEN 5 THEN 'Full'
      ELSE 'unknown'
    END AS name
  FROM counter_leading_intervals!(_counter)`;

const THERMAL_THROTTLING = `
  with step1 as (
      select
          ts,
          EXTRACT_ARG(arg_set_id, 'thermal_throttling_severity_state_changed.sensor_type') as sensor_type,
          EXTRACT_ARG(arg_set_id, 'thermal_throttling_severity_state_changed.sensor_name') as sensor_name,
          EXTRACT_ARG(arg_set_id, 'thermal_throttling_severity_state_changed.temperature_deci_celsius') / 10.0 as temperature_celcius,
          EXTRACT_ARG(arg_set_id, 'thermal_throttling_severity_state_changed.severity') as severity
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
      and s.name = 'thermal_throttling_severity_state_changed'
  ),
  step2 as (
      select
          ts,
          lead(ts) over (partition by sensor_type, sensor_name order by ts) - ts as dur,
          sensor_type,
          sensor_name,
          temperature_celcius,
          severity
      from step1
      where sensor_type not like 'TEMPERATURE_TYPE_BCL_%'
  )
  select
    ts,
    dur,
    case sensor_name
        when 'VIRTUAL-SKIN' then ''
        else sensor_name || ' is '
    end || severity || ' (' || temperature_celcius || 'C)' as name
  from step2
  where severity != 'NONE'`;

const KERNEL_WAKELOCKS = `
  create or replace perfetto table kernel_wakelocks as
  with kernel_wakelock_args as (
    select
      arg_set_id,
      min(iif(key = 'kernel_wakelock.name', string_value, null)) as wakelock_name,
      min(iif(key = 'kernel_wakelock.count', int_value, null)) as count,
      min(iif(key = 'kernel_wakelock.time_micros', int_value, null)) as time_micros
    from args
    where key in (
      'kernel_wakelock.name',
      'kernel_wakelock.count',
      'kernel_wakelock.time_micros'
    )
    group by 1
  ),
  interesting as (
    select wakelock_name
    from (
      select wakelock_name, max(time_micros)-min(time_micros) as delta_us
      from kernel_wakelock_args
      group by 1
    )
    -- Only consider wakelocks with over 1 second of time during the whole trace
    where delta_us > 1e6
  ),
  step1 as (
    select ts, wakelock_name, count, time_micros
    from kernel_wakelock_args
    join interesting using (wakelock_name)
    join slice using (arg_set_id)
  ),
  step2 as (
    select
      ts,
      wakelock_name,
      lead(ts) over (partition by wakelock_name order by ts) as ts_end,
      lead(count) over (partition by wakelock_name order by ts) - count as count,
      (lead(time_micros) over (partition by wakelock_name order by ts) - time_micros) * 1000 as wakelock_dur
    from step1
  ),
  step3 as (
    select
      ts,
      ts_end,
      ifnull((select sum(dur) from android_suspend_state s
              where power_state = 'suspended'
                and s.ts > step2.ts
                and s.ts < step2.ts_end), 0) as suspended_dur,
      wakelock_name,
      count,
      wakelock_dur
    from step2
    where wakelock_dur is not null
      and wakelock_dur >= 0
  )
  select
    ts,
    ts_end - ts as dur,
    wakelock_name,
    min(100.0 * wakelock_dur / (ts_end - ts - suspended_dur), 100) as value
  from step3`;

const KERNEL_WAKELOCKS_SUMMARY = `
  select wakelock_name, max(value) as max_value
  from kernel_wakelocks
  where wakelock_name not in ('PowerManager.SuspendLockout', 'PowerManagerService.Display')
  group by 1
  having max_value > 1
  order by 1;`;

const HIGH_CPU = `
  create or replace perfetto table high_cpu as
  with cpu_cycles_args AS (
    select
      arg_set_id,
      min(iif(key = 'cpu_cycles_per_uid_cluster.uid', int_value, null)) as uid,
      min(iif(key = 'cpu_cycles_per_uid_cluster.cluster', int_value, null)) as cluster,
      min(iif(key = 'cpu_cycles_per_uid_cluster.time_millis', int_value, null)) as time_millis
    from args
    where key in (
      'cpu_cycles_per_uid_cluster.uid',
      'cpu_cycles_per_uid_cluster.cluster',
      'cpu_cycles_per_uid_cluster.time_millis'
    )
    group by 1
  ),
  interesting AS (
    select uid, cluster
    from (
      select uid, cluster, max(time_millis)-min(time_millis) as delta_ms
      from cpu_cycles_args
      group by 1, 2
    )
    -- Only consider tracks with over 1 second of cpu during the whole trace
    where delta_ms > 1e3
  ),
  base as (
    select ts, uid, cluster, sum(time_millis) as time_millis
    from cpu_cycles_args
    join interesting using (uid, cluster)
    join slice using (arg_set_id)
    group by 1, 2, 3
  ),
  with_windows as (
    select
      ts,
      uid,
      cluster,
      lead(ts) over (partition by uid, cluster order by ts) - ts as dur,
      (lead(time_millis) over (partition by uid, cluster order by ts) - time_millis) * 1000000.0 as cpu_dur
    from base
  ),
  with_ratio as (
    select
      ts,
      iif(dur is null, 0, max(0, 100.0 * cpu_dur / dur)) as value,
      case cluster when 0 then 'little' when 1 then 'mid' when 2 then 'big' else 'cl-' || cluster end as cluster,
      package_name as pkg
    from add_package_name!(with_windows)
  )
  select ts, sum(value) as value, cluster, pkg
  from with_ratio
  group by 1, 3, 4`;

const WAKEUPS = `
  drop table if exists wakeups;
  create table wakeups as
  with wakeup_reason as (
      select
      ts,
      substr(i.name, 0, instr(i.name, ' ')) as id_timestamp,
      substr(i.name, instr(i.name, ' ') + 1) as raw_wakeup
      from track t join instant i on t.id = i.track_id
      where t.name = 'wakeup_reason'
  ),
  wakeup_attribution as (
      select
      substr(i.name, 0, instr(i.name, ' ')) as id_timestamp,
      substr(i.name, instr(i.name, ' ') + 1) as attribution
      from track t join instant i on t.id = i.track_id
      where t.name = 'wakeup_attribution'
  ),
  step1 as(
    select
      ts,
      raw_wakeup,
      attribution,
      null as raw_backoff
    from wakeup_reason r
      left outer join wakeup_attribution using(id_timestamp)
    union all
    select
      ts,
      null as raw_wakeup,
      null as attribution,
      i.name as raw_backoff
    from track t join instant i on t.id = i.track_id
    where t.name = 'suspend_backoff'
  ),
  step2 as (
    select
      ts,
      raw_wakeup,
      attribution,
      lag(raw_backoff) over (order by ts) as raw_backoff
    from step1
  ),
  step3 as (
    select
      ts,
      raw_wakeup,
      attribution,
      str_split(raw_backoff, ' ', 0) as suspend_quality,
      str_split(raw_backoff, ' ', 1) as backoff_state,
      str_split(raw_backoff, ' ', 2) as backoff_reason,
      cast(str_split(raw_backoff, ' ', 3) as int) as backoff_count,
      cast(str_split(raw_backoff, ' ', 4) as int) as backoff_millis,
      false as suspend_end
    from step2
    where raw_wakeup is not null
    union all
    select
      ts,
      null as raw_wakeup,
      null as attribution,
      null as suspend_quality,
      null as backoff_state,
      null as backoff_reason,
      null as backoff_count,
      null as backoff_millis,
      true as suspend_end
    from android_suspend_state
    where power_state = 'suspended'
  ),
  step4 as (
    select
      ts,
      case suspend_quality
        when 'good' then
          min(
            lead(ts, 1, ts + 5e9) over (order by ts) - ts,
            5e9
          )
        when 'bad' then backoff_millis * 1000000
        else 0
      end as dur,
      raw_wakeup,
      attribution,
      suspend_quality,
      backoff_state,
      backoff_reason,
      backoff_count,
      backoff_millis,
      suspend_end
    from step3
  ),
  step5 as (
    select
      ts,
      dur,
      raw_wakeup,
      attribution,
      suspend_quality,
      backoff_state,
      backoff_reason,
      backoff_count,
      backoff_millis
    from step4
    where not suspend_end
  ),
  step6 as (
    select
      ts,
      dur,
      raw_wakeup,
      attribution,
      suspend_quality,
      backoff_state,
      backoff_reason,
      backoff_count,
      backoff_millis,
      case
        when raw_wakeup like 'Abort: Pending Wakeup Sources: %' then 'abort_pending'
        when raw_wakeup like 'Abort: Last active Wakeup Source: %' then 'abort_last_active'
        when raw_wakeup like 'Abort: %' then 'abort_other'
        else 'normal'
      end as type,
      case
        when raw_wakeup like 'Abort: Pending Wakeup Sources: %' then substr(raw_wakeup, 32)
        when raw_wakeup like 'Abort: Last active Wakeup Source: %' then substr(raw_wakeup, 35)
        when raw_wakeup like 'Abort: %' then substr(raw_wakeup, 8)
        else raw_wakeup
      end as main,
      case
        when raw_wakeup like 'Abort: Pending Wakeup Sources: %' then ' '
        when raw_wakeup like 'Abort: %' then 'no delimiter needed'
        else ':'
      end as delimiter
    from step5
  ),
  step7 as (
    select
      ts,
      dur,
      raw_wakeup,
      attribution,
      suspend_quality,
      backoff_state,
      backoff_reason,
      backoff_count,
      backoff_millis,
      type,
      str_split(main, delimiter, 0) as item_0,
      str_split(main, delimiter, 1) as item_1,
      str_split(main, delimiter, 2) as item_2,
      str_split(main, delimiter, 3) as item_3
    from step6
  ),
  step8 as (
    select ts, dur, raw_wakeup, attribution, suspend_quality, backoff_state, backoff_reason, backoff_count, backoff_millis, type, item_0 as item from step7
    union all
    select ts, dur, raw_wakeup, attribution, suspend_quality, backoff_state, backoff_reason, backoff_count, backoff_millis, type, item_1 as item from step7 where item_1 is not null
    union all
    select ts, dur, raw_wakeup, attribution, suspend_quality, backoff_state, backoff_reason, backoff_count, backoff_millis, type, item_2 as item from step7 where item_2 is not null
    union all
    select ts, dur, raw_wakeup, attribution, suspend_quality, backoff_state, backoff_reason, backoff_count, backoff_millis, type, item_3 as item from step7 where item_3 is not null
  )
  select
    ts,
    dur,
    ts + dur as ts_end,
    raw_wakeup,
    attribution,
    suspend_quality,
    backoff_state,
    ifnull(backoff_reason, 'none') as backoff_reason,
    backoff_count,
    backoff_millis,
    type,
    case when type = 'normal' then ifnull(str_split(item, ' ', 1), item) else item end as item
  from step8`;

const WAKEUPS_COLUMNS = [
  'item',
  'type',
  'raw_wakeup',
  'attribution',
  'suspend_quality',
  'backoff_state',
  'backoff_reason',
  'backoff_count',
  'backoff_millis',
];

function bleScanQuery(condition: string) {
  return `
  with step1 as (
      select
          ts,
          extract_arg(arg_set_id, 'ble_scan_state_changed.attribution_node[0].tag') as name,
          extract_arg(arg_set_id, 'ble_scan_state_changed.is_opportunistic') as opportunistic,
          extract_arg(arg_set_id, 'ble_scan_state_changed.is_filtered') as filtered,
          extract_arg(arg_set_id, 'ble_scan_state_changed.state') as state
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
      and s.name = 'ble_scan_state_changed'
  ),
  step2 as (
      select
          ts,
          name,
          state,
          opportunistic,
          filtered,
          lead(ts) over (partition by name order by ts) - ts as dur
      from step1
  )
  select ts, dur, name from step2 where state = 'ON' and ${condition} and dur is not null`;
}

const BLE_RESULTS = `
  with step1 as (
      select
          ts,
          extract_arg(arg_set_id, 'ble_scan_result_received.attribution_node[0].tag') as name,
          extract_arg(arg_set_id, 'ble_scan_result_received.num_results') as num_results
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
      and s.name = 'ble_scan_result_received'
  )
  select
      ts,
      0 as dur,
      name || ' (' || num_results || ' results)' as name
  from step1`;

const BT_A2DP_AUDIO = `
  with step1 as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_a2dp_playback_state_changed.playback_state') as playback_state,
        EXTRACT_ARG(arg_set_id, 'bluetooth_a2dp_playback_state_changed.audio_coding_mode') as audio_coding_mode,
        EXTRACT_ARG(arg_set_id, 'bluetooth_a2dp_playback_state_changed.metric_id') as metric_id
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_a2dp_playback_state_changed'
  ),
  step2 as (
    select
        ts,
        lead(ts) over (partition by metric_id order by ts) - ts as dur,
        playback_state,
        audio_coding_mode,
        metric_id
    from step1
  )
  select
    ts,
    dur,
    audio_coding_mode as name
  from step2
  where playback_state = 'PLAYBACK_STATE_PLAYING'`;

const BT_CONNS_ACL = `
    with acl1 as (
        select
            ts,
            EXTRACT_ARG(arg_set_id, 'bluetooth_acl_connection_state_changed.state') as state,
            EXTRACT_ARG(arg_set_id, 'bluetooth_acl_connection_state_changed.transport') as transport,
            EXTRACT_ARG(arg_set_id, 'bluetooth_acl_connection_state_changed.metric_id') as metric_id
        from track t join slice s on t.id = s.track_id
        where t.name = 'Statsd Atoms'
        and s.name = 'bluetooth_acl_connection_state_changed'
    ),
    acl2 as (
        select
            ts,
            lead(ts) over (partition by metric_id, transport order by ts) - ts as dur,
            state,
            transport,
            metric_id
        from acl1
    )
    select
        ts,
        dur,
        'Device ' || metric_id ||
          ' (' || case transport when 'TRANSPORT_TYPE_BREDR' then 'Classic' when 'TRANSPORT_TYPE_LE' then 'BLE' end || ')' as name
    from acl2
    where state != 'CONNECTION_STATE_DISCONNECTED' and dur is not null`;

const BT_CONNS_SCO = `
  with sco1 as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_sco_connection_state_changed.state') as state,
        EXTRACT_ARG(arg_set_id, 'bluetooth_sco_connection_state_changed.codec') as codec,
        EXTRACT_ARG(arg_set_id, 'bluetooth_sco_connection_state_changed.metric_id') as metric_id
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_sco_connection_state_changed'
  ),
  sco2 as (
    select
        ts,
        lead(ts) over (partition by metric_id, codec order by ts) - ts as dur,
        state,
        codec,
        metric_id
    from sco1
  )
  select
    ts,
    dur,
    case state when 'CONNECTION_STATE_CONNECTED' then '' when 'CONNECTION_STATE_CONNECTING' then 'Connecting ' when 'CONNECTION_STATE_DISCONNECTING' then 'Disconnecting ' else 'unknown ' end ||
      'Device ' || metric_id || ' (' ||
      case codec when 'SCO_CODEC_CVSD' then 'CVSD' when 'SCO_CODEC_MSBC' then 'MSBC' end || ')' as name
  from sco2
  where state != 'CONNECTION_STATE_DISCONNECTED' and dur is not null`;

const BT_LINK_LEVEL_EVENTS = `
  with base as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.direction') as direction,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.type') as type,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.hci_cmd') as hci_cmd,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.hci_event') as hci_event,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.hci_ble_event') as hci_ble_event,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.cmd_status') as cmd_status,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.reason_code') as reason_code,
        EXTRACT_ARG(arg_set_id, 'bluetooth_link_layer_connection_event.metric_id') as metric_id
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_link_layer_connection_event'
  )
  select
    *,
    0 as dur,
    'Device '|| metric_id as name
  from base`;

const BT_LINK_LEVEL_EVENTS_COLUMNS = [
  'direction',
  'type',
  'hci_cmd',
  'hci_event',
  'hci_ble_event',
  'cmd_status',
  'reason_code',
  'metric_id',
];

const BT_QUALITY_REPORTS = `
  with base as (
      select
          ts,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.quality_report_id') as quality_report_id,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.packet_types') as packet_types,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.connection_handle') as connection_handle,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.connection_role') as connection_role,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.tx_power_level') as tx_power_level,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.rssi') as rssi,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.snr') as snr,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.unused_afh_channel_count') as unused_afh_channel_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.afh_select_unideal_channel_count') as afh_select_unideal_channel_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.lsto') as lsto,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.connection_piconet_clock') as connection_piconet_clock,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.retransmission_count') as retransmission_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.no_rx_count') as no_rx_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.nak_count') as nak_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.flow_off_count') as flow_off_count,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.buffer_overflow_bytes') as buffer_overflow_bytes,
          EXTRACT_ARG(arg_set_id, 'bluetooth_quality_report_reported.buffer_underflow_bytes') as buffer_underflow_bytes
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
      and s.name = 'bluetooth_quality_report_reported'
  )
  select
      *,
      0 as dur,
      'Connection '|| connection_handle as name
  from base`;

const BT_QUALITY_REPORTS_COLUMNS = [
  'quality_report_id',
  'packet_types',
  'connection_handle',
  'connection_role',
  'tx_power_level',
  'rssi',
  'snr',
  'unused_afh_channel_count',
  'afh_select_unideal_channel_count',
  'lsto',
  'connection_piconet_clock',
  'retransmission_count',
  'no_rx_count',
  'nak_count',
  'flow_off_count',
  'buffer_overflow_bytes',
  'buffer_underflow_bytes',
];

const BT_RSSI_REPORTS = `
  with base as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_device_rssi_reported.connection_handle') as connection_handle,
        EXTRACT_ARG(arg_set_id, 'bluetooth_device_rssi_reported.hci_status') as hci_status,
        EXTRACT_ARG(arg_set_id, 'bluetooth_device_rssi_reported.rssi') as rssi,
        EXTRACT_ARG(arg_set_id, 'bluetooth_device_rssi_reported.metric_id') as metric_id
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_device_rssi_reported'
  )
  select
    *,
    0 as dur,
    'Connection '|| connection_handle as name
  from base`;

const BT_RSSI_REPORTS_COLUMNS = [
  'connection_handle',
  'hci_status',
  'rssi',
  'metric_id',
];

const BT_CODE_PATH_COUNTER = `
  with base as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_code_path_counter.key') as key,
        EXTRACT_ARG(arg_set_id, 'bluetooth_code_path_counter.number') as number
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_code_path_counter'
  )
  select
    *,
    0 as dur,
    key as name
  from base`;

const BT_CODE_PATH_COUNTER_COLUMNS = ['key', 'number'];

const BT_HAL_CRASHES = `
  with base as (
      select
          ts,
          EXTRACT_ARG(arg_set_id, 'bluetooth_hal_crash_reason_reported.metric_id') as metric_id,
          EXTRACT_ARG(arg_set_id, 'bluetooth_hal_crash_reason_reported.error_code') as error_code,
          EXTRACT_ARG(arg_set_id, 'bluetooth_hal_crash_reason_reported.vendor_error_code') as vendor_error_code
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'
      and s.name = 'bluetooth_hal_crash_reason_reported'
  )
  select
      *,
      0 as dur,
      'Device ' || metric_id as name
  from base`;

const BT_HAL_CRASHES_COLUMNS = ['metric_id', 'error_code', 'vendor_error_code'];

const BT_BYTES = `
  with step1 as (
    select
        ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_bytes_transfer.uid') as uid,
        EXTRACT_ARG(arg_set_id, 'bluetooth_bytes_transfer.tx_bytes') as tx_bytes,
        EXTRACT_ARG(arg_set_id, 'bluetooth_bytes_transfer.rx_bytes') as rx_bytes
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_bytes_transfer'
  ),
  step2 as (
    select
        ts,
        lead(ts) over (partition by uid order by ts) - ts as dur,
        uid,
        lead(tx_bytes) over (partition by uid order by ts) - tx_bytes as tx_bytes,
        lead(rx_bytes) over (partition by uid order by ts) - rx_bytes as rx_bytes
    from step1
  ),
  step3 as (
    select
        ts,
        dur,
        uid % 100000 as uid,
        sum(tx_bytes) as tx_bytes,
        sum(rx_bytes) as rx_bytes
    from step2
    where tx_bytes >=0 and rx_bytes >=0
    group by 1,2,3
    having tx_bytes > 0 or rx_bytes > 0
  )
    select
        ts,
        dur,
        format("%s: TX %d bytes / RX %d bytes", package_name, tx_bytes, rx_bytes) as name
    from add_package_name!(step3)
`;

// See go/bt_system_context_report for reference on the bit-twiddling.
const BT_ACTIVITY = `
  create perfetto table bt_activity as
  with step1 as (
    select
        EXTRACT_ARG(arg_set_id, 'bluetooth_activity_info.timestamp_millis') * 1000000 as ts,
        EXTRACT_ARG(arg_set_id, 'bluetooth_activity_info.bluetooth_stack_state') as bluetooth_stack_state,
        EXTRACT_ARG(arg_set_id, 'bluetooth_activity_info.controller_idle_time_millis') * 1000000 as controller_idle_dur,
        EXTRACT_ARG(arg_set_id, 'bluetooth_activity_info.controller_tx_time_millis') * 1000000 as controller_tx_dur,
        EXTRACT_ARG(arg_set_id, 'bluetooth_activity_info.controller_rx_time_millis') * 1000000 as controller_rx_dur
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
    and s.name = 'bluetooth_activity_info'
  ),
  step2 as (
    select
        ts,
        lead(ts) over (order by ts) - ts as dur,
        bluetooth_stack_state,
        lead(controller_idle_dur) over (order by ts) - controller_idle_dur as controller_idle_dur,
        lead(controller_tx_dur) over (order by ts) - controller_tx_dur as controller_tx_dur,
        lead(controller_rx_dur) over (order by ts) - controller_rx_dur as controller_rx_dur
    from step1
  )
  select
    ts,
    dur,
    bluetooth_stack_state & 0x0000000F as acl_active_count,
    bluetooth_stack_state & 0x000000F0 >> 4 as acl_sniff_count,
    bluetooth_stack_state & 0x00000F00 >> 8 as acl_ble_count,
    bluetooth_stack_state & 0x0000F000 >> 12 as advertising_count,
    case bluetooth_stack_state & 0x000F0000 >> 16
      when 0 then 0
      when 1 then 5
      when 2 then 10
      when 3 then 25
      when 4 then 100
      else -1
    end as le_scan_duty_cycle,
    bluetooth_stack_state & 0x00100000 >> 20 as inquiry_active,
    bluetooth_stack_state & 0x00200000 >> 21 as sco_active,
    bluetooth_stack_state & 0x00400000 >> 22 as a2dp_active,
    bluetooth_stack_state & 0x00800000 >> 23 as le_audio_active,
    max(0, 100.0 * controller_idle_dur / dur) as controller_idle_pct,
    max(0, 100.0 * controller_tx_dur / dur) as controller_tx_pct,
    max(0, 100.0 * controller_rx_dur / dur) as controller_rx_pct
  from step2
`;

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AndroidLongBatteryTracing';
  private readonly groups = new Map<string, TrackNode>();

  private addTrack(ctx: Trace, track: TrackNode, groupName?: string): void {
    if (groupName) {
      const existingGroup = this.groups.get(groupName);
      if (existingGroup) {
        existingGroup.addChildInOrder(track);
      } else {
        const group = new TrackNode({title: groupName, isSummary: true});
        group.addChildInOrder(track);
        this.groups.set(groupName, group);
        ctx.workspace.addChildInOrder(group);
      }
    } else {
      ctx.workspace.addChildInOrder(track);
    }
  }

  async addSliceTrack(
    ctx: Trace,
    name: string,
    query: string,
    groupName?: string,
    columns: string[] = [],
  ) {
    const uri = `/long_battery_tracing_${name}`;
    const track = await createQuerySliceTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
        columns: ['ts', 'dur', 'name', ...columns],
      },
      argColumns: columns,
    });
    ctx.tracks.registerTrack({
      uri,
      title: name,
      track,
    });
    const trackNode = new TrackNode({uri, title: name});
    this.addTrack(ctx, trackNode, groupName);
  }

  async addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    groupName: string,
    options?: Partial<CounterOptions>,
  ) {
    const uri = `/long_battery_tracing_${name}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
      options,
    });
    ctx.tracks.registerTrack({
      uri,
      title: name,
      track,
    });
    const trackNode = new TrackNode({uri, title: name});
    this.addTrack(ctx, trackNode, groupName);
  }

  async addBatteryStatsState(
    ctx: Trace,
    name: string,
    track: string,
    groupName: string,
    features: Set<string>,
  ) {
    if (!features.has(`track.${track}`)) {
      return;
    }
    await this.addSliceTrack(
      ctx,
      name,
      `SELECT ts, safe_dur AS dur, value_name AS name
    FROM android_battery_stats_state
    WHERE track_name = "${track}"`,
      groupName,
    );
  }

  async addBatteryStatsEvent(
    ctx: Trace,
    name: string,
    track: string,
    groupName: string | undefined,
    features: Set<string>,
  ) {
    if (!features.has(`track.${track}`)) {
      return;
    }

    await this.addSliceTrack(
      ctx,
      name,
      `SELECT ts, safe_dur AS dur, str_value AS name
    FROM android_battery_stats_event_slices
    WHERE track_name = "${track}"`,
      groupName,
    );
  }

  async addDeviceState(ctx: Trace, features: Set<string>): Promise<void> {
    if (!features.has('track.battery_stats.*')) {
      return;
    }

    const query = (name: string, track: string) =>
      this.addBatteryStatsEvent(ctx, name, track, undefined, features);

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.battery_stats;`);
    await e.query(`INCLUDE PERFETTO MODULE android.suspend;`);
    await e.query(`INCLUDE PERFETTO MODULE counters.intervals;`);

    await this.addSliceTrack(ctx, 'Device State: Screen state', SCREEN_STATE);
    await this.addSliceTrack(ctx, 'Device State: Charging', CHARGING);
    await this.addSliceTrack(
      ctx,
      'Device State: Suspend / resume',
      SUSPEND_RESUME,
    );
    await this.addSliceTrack(ctx, 'Device State: Doze light state', DOZE_LIGHT);
    await this.addSliceTrack(ctx, 'Device State: Doze deep state', DOZE_DEEP);

    query('Device State: Top app', 'battery_stats.top');

    await this.addSliceTrack(
      ctx,
      'Device State: Long wakelocks',
      `SELECT
            ts - 60000000000 as ts,
            safe_dur + 60000000000 as dur,
            str_value AS name,
            package_name as package
        FROM add_package_name!((
          select *, int_value as uid
          from android_battery_stats_event_slices
          WHERE track_name = "battery_stats.longwake"
        ))`,
      undefined,
      ['package'],
    );

    query('Device State: Foreground apps', 'battery_stats.fg');
    query('Device State: Jobs', 'battery_stats.job');

    if (features.has('atom.thermal_throttling_severity_state_changed')) {
      await this.addSliceTrack(
        ctx,
        'Device State: Thermal throttling',
        THERMAL_THROTTLING,
      );
    }
  }

  async addAtomCounters(ctx: Trace): Promise<void> {
    const e = ctx.engine;

    try {
      await e.query(
        `INCLUDE PERFETTO MODULE
            google3.wireless.android.telemetry.trace_extractor.modules.atom_counters_slices`,
      );
    } catch (e) {
      return;
    }

    const counters = await e.query(
      `select distinct ui_group, ui_name, ui_unit, counter_name
       from atom_counters
       where ui_name is not null`,
    );
    const countersIt = counters.iter({
      ui_group: 'str',
      ui_name: 'str',
      ui_unit: 'str',
      counter_name: 'str',
    });
    for (; countersIt.valid(); countersIt.next()) {
      const unit = countersIt.ui_unit;
      const opts =
        unit === '%'
          ? {yOverrideMaximum: 100, unit: '%'}
          : unit !== undefined
            ? {unit}
            : undefined;

      await this.addCounterTrack(
        ctx,
        countersIt.ui_name,
        `select ts, ${unit === '%' ? 100.0 : 1.0} * counter_value as value
         from atom_counters
         where counter_name = '${countersIt.counter_name}'`,
        countersIt.ui_group,
        opts,
      );
    }
  }

  async addAtomSlices(ctx: Trace): Promise<void> {
    const e = ctx.engine;

    try {
      await e.query(
        `INCLUDE PERFETTO MODULE
            google3.wireless.android.telemetry.trace_extractor.modules.atom_counters_slices`,
      );
    } catch (e) {
      return;
    }

    const sliceTracks = await e.query(
      `select distinct ui_group, ui_name, atom, field
       from atom_slices
       where ui_name is not null
       order by 1, 2, 3, 4`,
    );
    const slicesIt = sliceTracks.iter({
      atom: 'str',
      ui_group: 'str',
      ui_name: 'str',
      field: 'str',
    });

    const tracks = new Map<
      string,
      {
        ui_group: string;
        ui_name: string;
      }
    >();
    const fields = new Map<string, string[]>();
    for (; slicesIt.valid(); slicesIt.next()) {
      const atom = slicesIt.atom;
      let args = fields.get(atom);
      if (args === undefined) {
        args = [];
        fields.set(atom, args);
      }
      args.push(slicesIt.field);
      tracks.set(atom, {
        ui_group: slicesIt.ui_group,
        ui_name: slicesIt.ui_name,
      });
    }

    for (const [atom, args] of fields) {
      function safeArg(arg: string) {
        return arg.replaceAll(/[[\]]/g, '').replaceAll(/\./g, '_');
      }

      // We need to make arg names compatible with SQL here because they pass through several
      // layers of SQL without being quoted in "".
      function argSql(arg: string) {
        return `max(case when field = '${arg}' then ifnull(string_value, int_value) end)
                as ${safeArg(arg)}`;
      }

      await this.addSliceTrack(
        ctx,
        tracks.get(atom)!.ui_name,
        `select ts, dur, slice_name as name, ${args.map((a) => argSql(a)).join(', ')}
         from atom_slices
         where atom = '${atom}'
         group by ts, dur, name`,
        tracks.get(atom)!.ui_group,
        args.map((a) => safeArg(a)),
      );
    }
  }

  async addNetworkSummary(ctx: Trace, features: Set<string>): Promise<void> {
    if (!features.has('net.modem') && !features.has('net.wifi')) {
      return;
    }

    const groupName = 'Network Summary';

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.battery_stats;`);
    await e.query(`INCLUDE PERFETTO MODULE android.network_packets;`);
    await e.query(NETWORK_SUMMARY);
    await e.query(RADIO_TRANSPORT_TYPE);

    await this.addSliceTrack(
      ctx,
      'Default network',
      DEFAULT_NETWORK,
      groupName,
    );

    if (features.has('atom.network_tethering_reported')) {
      await this.addSliceTrack(ctx, 'Tethering', TETHERING, groupName);
    }
    if (features.has('net.wifi')) {
      await this.addCounterTrack(
        ctx,
        'Wifi total bytes',
        `select ts, sum(value) as value from network_summary where dev_type = 'wifi' group by 1`,
        groupName,
        {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
      );
      const result = await e.query(
        `select pkg, sum(value) from network_summary where dev_type='wifi' group by 1 order by 2 desc limit 10`,
      );
      const it = result.iter({pkg: 'str'});
      for (; it.valid(); it.next()) {
        await this.addCounterTrack(
          ctx,
          `Top wifi: ${it.pkg}`,
          `select ts, value from network_summary where dev_type = 'wifi' and pkg = '${it.pkg}'`,
          groupName,
          {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
        );
      }
    }
    this.addBatteryStatsState(
      ctx,
      'Wifi interface',
      'battery_stats.wifi_radio',
      groupName,
      features,
    );
    this.addBatteryStatsState(
      ctx,
      'Wifi supplicant state',
      'battery_stats.wifi_suppl',
      groupName,
      features,
    );
    this.addBatteryStatsState(
      ctx,
      'Wifi strength',
      'battery_stats.wifi_signal_strength',
      groupName,
      features,
    );
    if (features.has('net.modem')) {
      await this.addCounterTrack(
        ctx,
        'Modem total bytes',
        `select ts, sum(value) as value from network_summary where dev_type = 'modem' group by 1`,
        groupName,
        {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
      );
      const result = await e.query(
        `select pkg, sum(value) from network_summary where dev_type='modem' group by 1 order by 2 desc limit 10`,
      );
      const it = result.iter({pkg: 'str'});
      for (; it.valid(); it.next()) {
        await this.addCounterTrack(
          ctx,
          `Top modem: ${it.pkg}`,
          `select ts, value from network_summary where dev_type = 'modem' and pkg = '${it.pkg}'`,
          groupName,
          {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
        );
      }
    }
    this.addBatteryStatsState(
      ctx,
      'Cellular interface',
      'battery_stats.mobile_radio',
      groupName,
      features,
    );
    await this.addSliceTrack(
      ctx,
      'Cellular connection',
      `select ts, dur, name from radio_transport`,
      groupName,
    );
    this.addBatteryStatsState(
      ctx,
      'Cellular strength',
      'battery_stats.phone_signal_strength',
      groupName,
      features,
    );
  }

  async addModemDetail(ctx: Trace, features: Set<string>): Promise<void> {
    const groupName = 'Modem Detail';
    if (features.has('track.ril')) {
      await this.addModemRil(ctx, groupName);
    }
    await this.addModemTeaData(ctx, groupName);
  }

  async addModemRil(ctx: Trace, groupName: string): Promise<void> {
    const rilStrength = async (band: string, value: string) =>
      await this.addSliceTrack(
        ctx,
        `Modem signal strength ${band} ${value}`,
        `SELECT ts, dur, name FROM RilScreenOn WHERE band_name = '${band}' AND value_name = '${value}'`,
        groupName,
      );

    const e = ctx.engine;

    await e.query(MODEM_RIL_STRENGTH);
    await e.query(MODEM_RIL_CHANNELS_PREAMBLE);

    await rilStrength('LTE', 'rsrp');
    await rilStrength('LTE', 'rssi');
    await rilStrength('NR', 'rsrp');
    await rilStrength('NR', 'rssi');

    await this.addSliceTrack(
      ctx,
      'Modem channel config',
      MODEM_RIL_CHANNELS,
      groupName,
    );

    await this.addSliceTrack(
      ctx,
      'Modem cell reselection',
      MODEM_CELL_RESELECTION,
      groupName,
      ['raw_ril'],
    );
  }

  async addModemTeaData(ctx: Trace, groupName: string): Promise<void> {
    const e = ctx.engine;

    try {
      await e.query(
        `INCLUDE PERFETTO MODULE
            google3.wireless.android.telemetry.trace_extractor.modules.modem_tea_metrics`,
      );
    } catch {
      return;
    }

    const counters = await e.query(
      `select distinct name from pixel_modem_counters`,
    );
    const countersIt = counters.iter({name: 'str'});
    for (; countersIt.valid(); countersIt.next()) {
      await this.addCounterTrack(
        ctx,
        countersIt.name,
        `select ts, value from pixel_modem_counters where name = '${countersIt.name}'`,
        groupName,
      );
    }
    const slices = await e.query(
      `select distinct track_name from pixel_modem_slices`,
    );
    const slicesIt = slices.iter({track_name: 'str'});
    for (; slicesIt.valid(); slicesIt.next()) {
      await this.addSliceTrack(
        ctx,
        slicesIt.track_name,
        `select ts, dur, slice_name as name from pixel_modem_slices
            where track_name = '${slicesIt.track_name}'`,
        groupName,
      );
    }
  }

  async addKernelWakelocks(ctx: Trace, features: Set<string>): Promise<void> {
    if (!features.has('atom.kernel_wakelock')) {
      return;
    }
    const groupName = 'Kernel Wakelock Summary';

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.suspend;`);
    await e.query(KERNEL_WAKELOCKS);
    const result = await e.query(KERNEL_WAKELOCKS_SUMMARY);
    const it = result.iter({wakelock_name: 'str'});
    for (; it.valid(); it.next()) {
      await this.addCounterTrack(
        ctx,
        it.wakelock_name,
        `select ts, dur, value from kernel_wakelocks where wakelock_name = "${it.wakelock_name}"`,
        groupName,
        {yRangeSharingKey: 'kernel_wakelock', unit: '%'},
      );
    }
  }

  async addWakeups(ctx: Trace, features: Set<string>): Promise<void> {
    if (!features.has('track.suspend_backoff')) {
      return;
    }

    const e = ctx.engine;
    const groupName = 'Wakeups';
    await e.query(`INCLUDE PERFETTO MODULE android.suspend;`);
    await e.query(WAKEUPS);
    const result = await e.query(`select
          item,
          sum(dur) as sum_dur
      from wakeups
      group by 1
      having sum_dur > 600e9`);
    const it = result.iter({item: 'str'});
    const sqlPrefix = `select
                ts,
                dur,
                item || case backoff_reason
                  when 'short' then ' (Short suspend backoff)'
                  when 'failed' then ' (Failed suspend backoff)'
                  else ''
                end as name,
                item,
                type,
                raw_wakeup,
                attribution,
                suspend_quality,
                backoff_state,
                backoff_reason,
                backoff_count,
                backoff_millis
            from wakeups`;
    const items = [];
    let labelOther = false;
    for (; it.valid(); it.next()) {
      labelOther = true;
      await this.addSliceTrack(
        ctx,
        `Wakeup ${it.item}`,
        `${sqlPrefix} where item="${it.item}"`,
        groupName,
        WAKEUPS_COLUMNS,
      );
      items.push(it.item);
    }
    await this.addSliceTrack(
      ctx,
      labelOther ? 'Other wakeups' : 'Wakeups',
      `${sqlPrefix} where item not in ('${items.join("','")}')`,
      groupName,
      WAKEUPS_COLUMNS,
    );
  }

  async addHighCpu(ctx: Trace, features: Set<string>): Promise<void> {
    if (!features.has('atom.cpu_cycles_per_uid_cluster')) {
      return;
    }
    const groupName = 'CPU per UID (major users)';

    const e = ctx.engine;

    await e.query(HIGH_CPU);
    const result = await e.query(
      `select distinct pkg, cluster from high_cpu where value > 10 order by 1, 2`,
    );
    const it = result.iter({pkg: 'str', cluster: 'str'});
    for (; it.valid(); it.next()) {
      await this.addCounterTrack(
        ctx,
        `CPU (${it.cluster}): ${it.pkg}`,
        `select ts, value from high_cpu where pkg = "${it.pkg}" and cluster="${it.cluster}"`,
        groupName,
        {yOverrideMaximum: 100, unit: '%'},
      );
    }
  }

  async addBluetooth(ctx: Trace, features: Set<string>): Promise<void> {
    if (
      !Array.from(features.values()).some(
        (f) => f.startsWith('atom.bluetooth_') || f.startsWith('atom.ble_'),
      )
    ) {
      return;
    }
    const groupName = 'Bluetooth';
    await this.addSliceTrack(
      ctx,
      'BLE Scans (opportunistic)',
      bleScanQuery('opportunistic'),
      groupName,
    );
    await this.addSliceTrack(
      ctx,
      'BLE Scans (filtered)',
      bleScanQuery('filtered'),
      groupName,
    );
    await this.addSliceTrack(
      ctx,
      'BLE Scans (unfiltered)',
      bleScanQuery('not filtered'),
      groupName,
    );
    await this.addSliceTrack(ctx, 'BLE Scan Results', BLE_RESULTS, groupName);
    await this.addSliceTrack(ctx, 'Connections (ACL)', BT_CONNS_ACL, groupName);
    await this.addSliceTrack(ctx, 'Connections (SCO)', BT_CONNS_SCO, groupName);
    await this.addSliceTrack(
      ctx,
      'Link-level Events',
      BT_LINK_LEVEL_EVENTS,
      groupName,
      BT_LINK_LEVEL_EVENTS_COLUMNS,
    );
    await this.addSliceTrack(ctx, 'A2DP Audio', BT_A2DP_AUDIO, groupName);
    await this.addSliceTrack(
      ctx,
      'Bytes Transferred (L2CAP/RFCOMM)',
      BT_BYTES,
      groupName,
    );
    await ctx.engine.query(BT_ACTIVITY);
    await this.addCounterTrack(
      ctx,
      'ACL Classic Active Count',
      'select ts, dur, acl_active_count as value from bt_activity',
      groupName,
    );
    await this.addCounterTrack(
      ctx,
      'ACL Classic Sniff Count',
      'select ts, dur, acl_sniff_count as value from bt_activity',
      groupName,
    );
    await this.addCounterTrack(
      ctx,
      'ACL BLE Count',
      'select ts, dur, acl_ble_count as value from bt_activity',
      groupName,
    );
    await this.addCounterTrack(
      ctx,
      'Advertising Instance Count',
      'select ts, dur, advertising_count as value from bt_activity',
      groupName,
    );
    await this.addCounterTrack(
      ctx,
      'LE Scan Duty Cycle Maximum',
      'select ts, dur, le_scan_duty_cycle as value from bt_activity',
      groupName,
      {unit: '%'},
    );
    await this.addSliceTrack(
      ctx,
      'Inquiry Active',
      "select ts, dur, 'Active' as name from bt_activity where inquiry_active",
      groupName,
    );
    await this.addSliceTrack(
      ctx,
      'SCO Active',
      "select ts, dur, 'Active' as name from bt_activity where sco_active",
      groupName,
    );
    await this.addSliceTrack(
      ctx,
      'A2DP Active',
      "select ts, dur, 'Active' as name from bt_activity where a2dp_active",
      groupName,
    );
    await this.addSliceTrack(
      ctx,
      'LE Audio Active',
      "select ts, dur, 'Active' as name from bt_activity where le_audio_active",
      groupName,
    );
    await this.addCounterTrack(
      ctx,
      'Controller Idle Time',
      'select ts, dur, controller_idle_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await this.addCounterTrack(
      ctx,
      'Controller TX Time',
      'select ts, dur, controller_tx_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await this.addCounterTrack(
      ctx,
      'Controller RX Time',
      'select ts, dur, controller_rx_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await this.addSliceTrack(
      ctx,
      'Quality reports',
      BT_QUALITY_REPORTS,
      groupName,
      BT_QUALITY_REPORTS_COLUMNS,
    );
    await this.addSliceTrack(
      ctx,
      'RSSI Reports',
      BT_RSSI_REPORTS,
      groupName,
      BT_RSSI_REPORTS_COLUMNS,
    );
    await this.addSliceTrack(
      ctx,
      'HAL Crashes',
      BT_HAL_CRASHES,
      groupName,
      BT_HAL_CRASHES_COLUMNS,
    );
    await this.addSliceTrack(
      ctx,
      'Code Path Counter',
      BT_CODE_PATH_COUNTER,
      groupName,
      BT_CODE_PATH_COUNTER_COLUMNS,
    );
  }

  async addContainedTraces(
    ctx: Trace,
    containedTraces: ContainedTrace[],
  ): Promise<void> {
    const bySubscription = new Map<string, ContainedTrace[]>();
    for (const trace of containedTraces) {
      if (!bySubscription.has(trace.subscription)) {
        bySubscription.set(trace.subscription, []);
      }
      bySubscription.get(trace.subscription)!.push(trace);
    }

    for (const [subscription, traces] of bySubscription) {
      await this.addSliceTrack(
        ctx,
        subscription,
        traces
          .map(
            (t) => `SELECT
          CAST(${t.ts} * 1e6 AS int) AS ts,
          CAST(${t.dur} * 1e6 AS int) AS dur,
          '${t.trigger === '' ? 'Trace' : t.trigger}' AS name,
          'http://go/trace-uuid/${t.uuid}' AS link
        `,
          )
          .join(' UNION ALL '),
        'Other traces',
        ['link'],
      );
    }
  }

  async findFeatures(e: Engine): Promise<Set<string>> {
    const features = new Set<string>();

    const addFeatures = async (q: string) => {
      const result = await e.query(q);
      const it = result.iter({feature: 'str'});
      for (; it.valid(); it.next()) {
        features.add(it.feature);
      }
    };

    await addFeatures(`
      select distinct 'atom.' || s.name as feature
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'`);

    await addFeatures(`
      select distinct
        case when name like '%wlan%' then 'net.wifi'
            when name like '%rmnet%' then 'net.modem'
            else 'net.other'
        end as feature
      from track
      where name like '%Transmitted' or name like '%Received'`);

    await addFeatures(`
      select distinct 'track.' || lower(name) as feature
      from track where name in ('RIL', 'suspend_backoff') or name like 'battery_stats.%'`);

    await addFeatures(`
      select distinct 'track.battery_stats.*' as feature
      from track where name like 'battery_stats.%'`);

    return features;
  }

  async addTracks(ctx: Trace): Promise<void> {
    const features: Set<string> = await this.findFeatures(ctx.engine);

    const containedTraces = (ctx.openerPluginArgs?.containedTraces ??
      []) as ContainedTrace[];

    await ctx.engine.query(PACKAGE_LOOKUP);
    await this.addNetworkSummary(ctx, features);
    await this.addBluetooth(ctx, features);
    await this.addAtomCounters(ctx);
    await this.addAtomSlices(ctx);
    await this.addModemDetail(ctx, features);
    await this.addKernelWakelocks(ctx, features);
    await this.addWakeups(ctx, features);
    await this.addDeviceState(ctx, features);
    await this.addHighCpu(ctx, features);
    await this.addContainedTraces(ctx, containedTraces);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addTracks(ctx);
  }
}
