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

import {v4 as uuidv4} from 'uuid';

import {Actions, DeferredAction} from '../../common/actions';
import {globals} from '../../frontend/globals';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
} from '../../public';
import {EngineProxy} from '../../trace_processor/engine';
import {createDebugCounterTrackActions} from '../../tracks/debug/counter_track';
import {createDebugSliceTrackActions} from '../../tracks/debug/slice_track';
import {NULL_TRACK_URI} from '../../tracks/null_track';

const DEFAULT_NETWORK = `
  with base as (
      select
          ts,
          substr(s.name, 6, 1) as conn
      from track t join slice s on t.id = s.track_id
      where t.name = 'battery_stats.conn'
          and s.name like '%"CONNECTED"'
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
      case conn when '1' then 'WiFi' when '0' then 'Modem' else conn end as name
  from diff where keep is null or keep`;

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
  drop table if exists network_summary;
  create table network_summary as
  with base as (
      select
          cast(s.ts / 5000000000 as int) * 5000000000 as ts,
          case
              when t.name glob '*wlan*' then 'wifi'
              when t.name glob '*rmnet*' then 'modem'
              else 'unknown'
          end as dev_type,
          lower(substr(t.name, instr(t.name, ' ') + 1, 1)) || 'x' as dir,
          sum(EXTRACT_ARG(arg_set_id, 'packet_length')) AS value
      from slice s join track t on s.track_id = t.id
      where (t.name glob '*Received' or t.name glob '*Transmitted')
      and (t.name glob '*wlan*' or t.name glob '*rmnet*')
      group by 1,2,3
  ),
  zeroes as (
      select
          ts,
          dev_type,
          dir,
          value
      from base
      union all
      select
          ts + 5000000000 as ts,
          dev_type,
          dir,
          0 as value
      from base
  ),
  final as (
      select
          ts,
          dev_type,
          dir,
          sum(value) as value
      from zeroes
      group by 1, 2, 3
  )
  select * from final where ts is not null`;

const MODEM_ACTIVITY_INFO = `
  drop table if exists modem_activity_info;
  create table modem_activity_info as
  with modem_raw as (
    select
      ts,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.timestamp_millis') as timestamp_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.sleep_time_millis') as sleep_time_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_idle_time_millis') as controller_idle_time_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_tx_time_pl0_millis') as controller_tx_time_pl0_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_tx_time_pl1_millis') as controller_tx_time_pl1_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_tx_time_pl2_millis') as controller_tx_time_pl2_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_tx_time_pl3_millis') as controller_tx_time_pl3_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_tx_time_pl4_millis') as controller_tx_time_pl4_millis,
      EXTRACT_ARG(arg_set_id, 'modem_activity_info.controller_rx_time_millis') as controller_rx_time_millis
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
      and s.name = 'modem_activity_info'
  ),
  deltas as (
      select
          timestamp_millis * 1000000 as ts,
          lead(timestamp_millis) over (order by ts) - timestamp_millis as dur_millis,
          lead(sleep_time_millis) over (order by ts) - sleep_time_millis as sleep_time_millis,
          lead(controller_idle_time_millis) over (order by ts) - controller_idle_time_millis as controller_idle_time_millis,
          lead(controller_tx_time_pl0_millis) over (order by ts) - controller_tx_time_pl0_millis as controller_tx_time_pl0_millis,
          lead(controller_tx_time_pl1_millis) over (order by ts) - controller_tx_time_pl1_millis as controller_tx_time_pl1_millis,
          lead(controller_tx_time_pl2_millis) over (order by ts) - controller_tx_time_pl2_millis as controller_tx_time_pl2_millis,
          lead(controller_tx_time_pl3_millis) over (order by ts) - controller_tx_time_pl3_millis as controller_tx_time_pl3_millis,
          lead(controller_tx_time_pl4_millis) over (order by ts) - controller_tx_time_pl4_millis as controller_tx_time_pl4_millis,
          lead(controller_rx_time_millis) over (order by ts) - controller_rx_time_millis as controller_rx_time_millis
      from modem_raw
  ),
  ratios as (
      select
          ts,
          100.0 * sleep_time_millis / dur_millis as sleep_time_ratio,
          100.0 * controller_idle_time_millis / dur_millis as controller_idle_time_ratio,
          100.0 * controller_tx_time_pl0_millis / dur_millis as controller_tx_time_pl0_ratio,
          100.0 * controller_tx_time_pl1_millis / dur_millis as controller_tx_time_pl1_ratio,
          100.0 * controller_tx_time_pl2_millis / dur_millis as controller_tx_time_pl2_ratio,
          100.0 * controller_tx_time_pl3_millis / dur_millis as controller_tx_time_pl3_ratio,
          100.0 * controller_tx_time_pl4_millis / dur_millis as controller_tx_time_pl4_ratio,
          100.0 * controller_rx_time_millis / dur_millis as controller_rx_time_ratio
      from deltas
  )
  select * from ratios where sleep_time_ratio is not null and sleep_time_ratio >= 0`;

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
  drop table if exists kernel_wakelocks;
  create table kernel_wakelocks as
  with step1 as (
    select
      ts,
      EXTRACT_ARG(arg_set_id, 'kernel_wakelock.name') as wakelock_name,
      EXTRACT_ARG(arg_set_id, 'kernel_wakelock.count') as count,
      EXTRACT_ARG(arg_set_id, 'kernel_wakelock.time_micros') as time_micros
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
      and s.name = 'kernel_wakelock'
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
      ifnull((select sum(dur) from suspend_slice_ s where s.ts > step2.ts and s.ts < step2.ts_end), 0) as suspended_dur,
      wakelock_name,
      count,
      wakelock_dur
    from step2
    where wakelock_dur is not null
      and wakelock_dur > 0
      and count >= 0
  ),
  step4 as (
    select
      ts,
      ts_end,
      suspended_dur,
      wakelock_name,
      count,
      1.0 * wakelock_dur / (ts_end - ts - suspended_dur) as ratio,
      wakelock_dur
    from step3
  )
  select
    ts,
    min(ratio, 1) * (ts_end - ts) as dur,
    wakelock_name,
    cast (100.0 * ratio as int) || '% (+' || count || ')' as name
    from step4
  where cast (100.0 * wakelock_dur / (ts_end - ts - suspended_dur) as int) > 1`;

const KERNEL_WAKELOCKS_SUMMARY = `
  select distinct wakelock_name
  from kernel_wakelocks
  where wakelock_name not in ('PowerManager.SuspendLockout', 'PowerManagerService.Display')
  order by 1;`;

const HIGH_CPU = `
  drop table if exists high_cpu;
  create table high_cpu as
  with base as (
    select
      ts,
      EXTRACT_ARG(arg_set_id, 'cpu_cycles_per_uid_cluster.uid') as uid,
      EXTRACT_ARG(arg_set_id, 'cpu_cycles_per_uid_cluster.cluster') as cluster,
      sum(EXTRACT_ARG(arg_set_id, 'cpu_cycles_per_uid_cluster.time_millis')) as time_millis
    from track t join slice s on t.id = s.track_id
    where t.name = 'Statsd Atoms'
      and s.name = 'cpu_cycles_per_uid_cluster'
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
  app_package_list as (
    select
      uid,
      group_concat(package_name) as package_name
    from package_list
    where uid >= 10000
    group by 1
  ),
  with_ratio as (
    select
      ts,
      100.0 * cpu_dur / dur as value,
      dur,
      case cluster when 0 then 'little' when 1 then 'mid' when 2 then 'big' else 'cl-' || cluster end as cluster,
      case
          when uid = 0 then 'AID_ROOT'
          when uid = 1000 then 'AID_SYSTEM_USER'
          when uid = 1001 then 'AID_RADIO'
          when uid = 1082 then 'AID_ARTD'
          when pl.package_name is null then 'uid=' || uid
          else pl.package_name
      end as pkg
    from with_windows left join app_package_list pl using(uid)
    where cpu_dur is not null
  ),
  with_zeros as (
      select ts, value, cluster, pkg
      from with_ratio
      union all
      select ts + dur as ts, 0 as value, cluster, pkg
      from with_ratio
  )
  select ts, sum(value) as value, cluster, pkg
  from with_zeros
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
    from suspend_slice_
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
  select ts, dur, name from step2 where state = 'ON' and ${
  condition} and dur is not null`;
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

const BT_RSSI_REPORTS_COLUMNS =
    ['connection_handle', 'hci_status', 'rssi', 'metric_id'];

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

function flatten<T>(arr: T[][]): T[] {
  return arr.reduce((a, b) => a.concat(b), []);
}

class AndroidLongBatteryTracing implements Plugin {
  onActivate(_: PluginContext): void {}

  async addSliceTrack(
    engine: EngineProxy, name: string, query: string, groupId: string,
    columns: string[] = []): Promise<DeferredAction<{}>> {
    const actions = await createDebugSliceTrackActions(
      engine,
      {
        sqlSource: query,
        columns: ['ts', 'dur', 'name', ...columns],
      },
      name,
      {ts: 'ts', dur: 'dur', name: 'name'},
      columns,
      {closeable: false, pinned: false},
    );
    if (actions.length > 1) {
      throw new Error();
    }
    const action = actions[0];
    action.args = {
      ...action.args,
      trackGroup: groupId,
    };
    return action;
  }

  async addCounterTrack(
    engine: EngineProxy, name: string, query: string,
    groupId: string): Promise<DeferredAction<{}>> {
    const actions = await createDebugCounterTrackActions(
      engine,
      {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
      name,
      {ts: 'ts', value: 'value'},
      {closeable: false, pinned: false},
    );
    if (actions.length > 1) {
      throw new Error();
    }
    const action = actions[0];
    action.args = {
      ...action.args,
      trackGroup: groupId,
    };
    return action;
  }

  async addNetworkSummary(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
    await e.query(NETWORK_SUMMARY);
    return await Promise.all([
      this.addSliceTrack(e, 'Default network', DEFAULT_NETWORK, groupId),
      this.addSliceTrack(e, 'Tethering', TETHERING, groupId),
      this.addCounterTrack(
        e,
        'Wifi bytes (logscale)',
        `select ts, ifnull(ln(sum(value)), 0) as value from network_summary where dev_type = 'wifi' group by 1`,
        groupId),
      this.addCounterTrack(
        e,
        'Wifi TX bytes (logscale)',
        `select ts, ifnull(ln(value), 0) as value from network_summary where dev_type = 'wifi' and dir = 'tx'`,
        groupId),
      this.addCounterTrack(
        e,
        'Wifi RX bytes (logscale)',
        `select ts, ifnull(ln(value), 0) as value from network_summary where dev_type = 'wifi' and dir = 'rx'`,
        groupId),
      this.addCounterTrack(
        e,
        'Modem bytes (logscale)',
        `select ts, ifnull(ln(sum(value)), 0) as value from network_summary where dev_type = 'modem' group by 1`,
        groupId),
      this.addCounterTrack(
        e,
        'Modem TX bytes (logscale)',
        `select ts, ifnull(ln(value), 0) as value from network_summary where dev_type = 'modem' and dir = 'tx'`,
        groupId),
      this.addCounterTrack(
        e,
        'Modem RX bytes (logscale)',
        `select ts, ifnull(ln(value), 0) as value from network_summary where dev_type = 'modem' and dir = 'rx'`,
        groupId),
    ]);
  }

  async addModemActivityInfo(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
    const query = (name: string, col: string): Promise<DeferredAction<{}>> =>
      this.addCounterTrack(
        e,
        name,
        `select ts, ${col}_ratio as value from modem_activity_info`,
        groupId);

    await e.query(MODEM_ACTIVITY_INFO);
    return await Promise.all([
      query('Modem sleep', 'sleep_time'),
      query('Modem controller idle', 'controller_idle_time'),
      query('Modem RX time', 'controller_rx_time'),
      query('Modem TX time power 0', 'controller_tx_time_pl0'),
      query('Modem TX time power 1', 'controller_tx_time_pl1'),
      query('Modem TX time power 2', 'controller_tx_time_pl2'),
      query('Modem TX time power 3', 'controller_tx_time_pl3'),
      query('Modem TX time power 4', 'controller_tx_time_pl4'),
    ]);
  }

  async addKernelWakelocks(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
    await e.query(KERNEL_WAKELOCKS);
    const result = await e.query(KERNEL_WAKELOCKS_SUMMARY);
    const it = result.iter({wakelock_name: 'str'});
    const actions: Promise<DeferredAction<{}>>[] = [];
    for (; it.valid(); it.next()) {
      actions.push(this.addSliceTrack(
        e,
        it.wakelock_name,
        `select ts, dur, name from kernel_wakelocks where wakelock_name = "${
          it.wakelock_name}"`,
        groupId));
    }
    return await Promise.all(actions);
  }

  async addWakeups(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
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
    const actions: Promise<DeferredAction<{}>>[] = [];
    for (; it.valid(); it.next()) {
      actions.push(this.addSliceTrack(
        e,
        `Wakeup ${it.item}`,
        `${sqlPrefix} where item="${it.item}"`,
        groupId,
        WAKEUPS_COLUMNS));
      items.push(it.item);
    }
    actions.push(this.addSliceTrack(
      e,
      'Other wakeups',
      `${sqlPrefix} where item not in ('${items.join('\',\'')}')`,
      groupId,
      WAKEUPS_COLUMNS));
    return await Promise.all(actions);
  }

  async addHighCpu(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
    await e.query(HIGH_CPU);
    const result = await e.query(
      `select distinct pkg, cluster from high_cpu where value > 10 order by 1, 2`);
    const it = result.iter({pkg: 'str', cluster: 'str'});
    const actions: Promise<DeferredAction<{}>>[] = [];
    for (; it.valid(); it.next()) {
      actions.push(this.addCounterTrack(
        e,
        `CPU (${it.cluster}): ${it.pkg}`,
        `select ts, value from high_cpu where pkg = "${
          it.pkg}" and cluster="${it.cluster}"`,
        groupId));
    }
    return await Promise.all(actions);
  }

  async addBluetooth(e: EngineProxy, groupId: string):
      Promise<DeferredAction<{}>[]> {
    return await Promise.all([
      this.addSliceTrack(
        e,
        'BLE Scans (opportunistic)',
        bleScanQuery('opportunistic'),
        groupId),
      this.addSliceTrack(
        e, 'BLE Scans (filtered)', bleScanQuery('filtered'), groupId),
      this.addSliceTrack(
        e, 'BLE Scans (unfiltered)', bleScanQuery('not filtered'), groupId),
      this.addSliceTrack(e, 'BLE Scan Results', BLE_RESULTS, groupId),
      this.addSliceTrack(e, 'Connections (ACL)', BT_CONNS_ACL, groupId),
      this.addSliceTrack(e, 'Connections (SCO)', BT_CONNS_SCO, groupId),
      this.addSliceTrack(
        e,
        'Link-level Events',
        BT_LINK_LEVEL_EVENTS,
        groupId,
        BT_LINK_LEVEL_EVENTS_COLUMNS),
      this.addSliceTrack(e, 'A2DP Audio', BT_A2DP_AUDIO, groupId),
      this.addSliceTrack(
        e,
        'Quality reports',
        BT_QUALITY_REPORTS,
        groupId,
        BT_QUALITY_REPORTS_COLUMNS),
      this.addSliceTrack(
        e, 'RSSI Reports', BT_RSSI_REPORTS, groupId, BT_RSSI_REPORTS_COLUMNS),
      this.addSliceTrack(
        e, 'HAL Crashes', BT_HAL_CRASHES, groupId, BT_HAL_CRASHES_COLUMNS),
      this.addSliceTrack(
        e,
        'Code Path Counter',
        BT_CODE_PATH_COUNTER,
        groupId,
        BT_CODE_PATH_COUNTER_COLUMNS),
    ]);
  }

  findGroupId(name: string) {
    for (const group of Object.values(globals.state.trackGroups)) {
      if (group.name === name) {
        return group.id;
      }
    }
    throw new Error(`No group ${name} found`);
  }

  addGroup(groupName: string): {id: string, actions: DeferredAction<{}>[]} {
    const summaryTrackKey = uuidv4();
    const groupUuid = uuidv4();

    return {
      id: groupUuid,
      actions: [
        Actions.addTrack({
          uri: NULL_TRACK_URI,
          key: summaryTrackKey,
          trackSortKey: PrimaryTrackSortKey.NULL_TRACK,
          name: groupName,
          trackGroup: undefined,
        }),
        Actions.addTrackGroup({
          summaryTrackKey,
          name: groupName,
          id: groupUuid,
          collapsed: true,
        }),
      ],
    };
  }

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'dev.perfetto.AndroidLongBatteryTracing#run',
      name: 'Add long battery tracing tracks',
      callback: async () => {
        const actions: DeferredAction<{}>[] = [];
        const addGroup = (name: string) => {
          const {id, actions: a} = this.addGroup(name);
          actions.push(...a);
          return id;
        };
        const miscGroupId = this.findGroupId('Misc Global Tracks');
        const networkId = addGroup('Network Summary');
        const wakelocksId = addGroup('Kernel Wakelocks');
        const wakeupsId = addGroup('Wakeups');
        const cpuId = addGroup('CPU');
        const btId = addGroup('Bluetooth');
        actions.push(await this.addSliceTrack(
          ctx.engine, 'Thermal throttling', THERMAL_THROTTLING, miscGroupId));

        const promises: Promise<DeferredAction<{}>[]>[] = [
          this.addNetworkSummary(ctx.engine, networkId),
          this.addModemActivityInfo(ctx.engine, networkId),
          this.addKernelWakelocks(ctx.engine, wakelocksId),
          this.addWakeups(ctx.engine, wakeupsId),
          this.addHighCpu(ctx.engine, cpuId),
          this.addBluetooth(ctx.engine, btId),
        ];
        const flattenedActions: DeferredAction<{}>[] =
            flatten(await Promise.all(promises));
        globals.dispatchMultiple([...actions, ...flattenedActions]);
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidLongBatteryTracing',
  plugin: AndroidLongBatteryTracing,
};
