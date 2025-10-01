// Copyright (C) 2025 The Android Open Source Project
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
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {PerfettoPlugin} from '../../public/plugin';
import {
  STR,
  LONG,
  UNKNOWN,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';

function bleScanDataset(condition: string) {
  return new SourceDataset({
    src: `
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
      select ts, dur, name from step2 where state = 'ON' and ${condition} and dur is not null
    `,
    schema: {
      ts: LONG,
      dur: LONG_NULL,
      name: STR,
    },
  });
}

const BLE_RESULTS_DATASET = new SourceDataset({
  src: `
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
    from step1
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const BT_A2DP_AUDIO_DATASET = new SourceDataset({
  src: `
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
    where playback_state = 'PLAYBACK_STATE_PLAYING'
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const BT_CONNS_ACL_DATASET = new SourceDataset({
  src: `
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
    where state != 'CONNECTION_STATE_DISCONNECTED' and dur is not null
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const BT_CONNS_SCO_DATASET = new SourceDataset({
  src: `
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
    where state != 'CONNECTION_STATE_DISCONNECTED' and dur is not null
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const BT_LINK_LEVEL_EVENTS_DATASET = new SourceDataset({
  src: `
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
    from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    direction: UNKNOWN,
    type: UNKNOWN,
    hci_cmd: UNKNOWN,
    hci_event: UNKNOWN,
    hci_ble_event: UNKNOWN,
    cmd_status: UNKNOWN,
    reason_code: UNKNOWN,
    metric_id: UNKNOWN,
  },
});

const BT_QUALITY_REPORTS_DATASET = new SourceDataset({
  src: `
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
    from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    quality_report_id: UNKNOWN,
    packet_types: UNKNOWN,
    connection_handle: UNKNOWN,
    connection_role: UNKNOWN,
    tx_power_level: UNKNOWN,
    rssi: UNKNOWN,
    snr: UNKNOWN,
    unused_afh_channel_count: UNKNOWN,
    afh_select_unideal_channel_count: UNKNOWN,
    lsto: UNKNOWN,
    connection_piconet_clock: UNKNOWN,
    retransmission_count: UNKNOWN,
    no_rx_count: UNKNOWN,
    nak_count: UNKNOWN,
    flow_off_count: UNKNOWN,
    buffer_overflow_bytes: UNKNOWN,
    buffer_underflow_bytes: UNKNOWN,
  },
});

const BT_RSSI_REPORTS_DATASET = new SourceDataset({
  src: `
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
    from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    connection_handle: UNKNOWN,
    hci_status: UNKNOWN,
    rssi: UNKNOWN,
    metric_id: UNKNOWN,
  },
});

const BT_CODE_PATH_COUNTER_DATASET = new SourceDataset({
  src: `
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
    from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    key: UNKNOWN,
    number: UNKNOWN,
  },
});

const BT_HAL_CRASHES_DATASET = new SourceDataset({
  src: `
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
    from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    metric_id: UNKNOWN,
    error_code: UNKNOWN,
    vendor_error_code: UNKNOWN,
  },
});

const BT_BYTES_DATASET = new SourceDataset({
  src: `
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
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

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
  static readonly id = 'com.android.Bluetooth';
  static readonly dependencies = [StandardGroupsPlugin, SupportPlugin];

  private support(ctx: Trace) {
    return ctx.plugins.getPlugin(SupportPlugin);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const support = this.support(ctx);
    const features = await support.features(ctx.engine);
    if (
      !Array.from(features.values()).some(
        (f) => f.startsWith('atom.bluetooth_') || f.startsWith('atom.ble_'),
      )
    ) {
      return;
    }

    const groupName = 'Bluetooth';
    await support.addSliceTrack(
      ctx,
      'BLE Scans (opportunistic)',
      bleScanDataset('opportunistic'),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'BLE Scans (filtered)',
      bleScanDataset('filtered'),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'BLE Scans (unfiltered)',
      bleScanDataset('not filtered'),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'BLE Scan Results',
      BLE_RESULTS_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'Connections (ACL)',
      BT_CONNS_ACL_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'Connections (SCO)',
      BT_CONNS_SCO_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'Link-level Events',
      BT_LINK_LEVEL_EVENTS_DATASET,
      groupName,
    );

    await support.addSliceTrack(
      ctx,
      'A2DP Audio',
      BT_A2DP_AUDIO_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'Bytes Transferred (L2CAP/RFCOMM)',
      BT_BYTES_DATASET,
      groupName,
    );
    await ctx.engine.query(BT_ACTIVITY);
    await support.addCounterTrack(
      ctx,
      'ACL Classic Active Count',
      'select ts, dur, acl_active_count as value from bt_activity',
      groupName,
    );
    await support.addCounterTrack(
      ctx,
      'ACL Classic Sniff Count',
      'select ts, dur, acl_sniff_count as value from bt_activity',
      groupName,
    );
    await support.addCounterTrack(
      ctx,
      'ACL BLE Count',
      'select ts, dur, acl_ble_count as value from bt_activity',
      groupName,
    );
    await support.addCounterTrack(
      ctx,
      'Advertising Instance Count',
      'select ts, dur, advertising_count as value from bt_activity',
      groupName,
    );
    await support.addCounterTrack(
      ctx,
      'LE Scan Duty Cycle Maximum',
      'select ts, dur, le_scan_duty_cycle as value from bt_activity',
      groupName,
      {unit: '%'},
    );
    await support.addSliceTrack(
      ctx,
      'Inquiry Active',
      new SourceDataset({
        src: `SELECT
                ts,
                dur,
                'Active' as name
              FROM bt_activity
              WHERE inquiry_active`,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'SCO Active',
      new SourceDataset({
        src: `SELECT
                ts,
                dur,
                'Active' as name
              FROM bt_activity
              WHERE sco_active`,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'A2DP Active',
      new SourceDataset({
        src: `SELECT
                ts,
                dur,
                'Active' as name
              FROM bt_activity
              WHERE a2dp_active`,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'LE Audio Active',
      new SourceDataset({
        src: `SELECT
                ts,
                dur,
                'Active' as name
              FROM bt_activity
              WHERE le_audio_active`,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
    await support.addCounterTrack(
      ctx,
      'Controller Idle Time',
      'select ts, dur, controller_idle_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await support.addCounterTrack(
      ctx,
      'Controller TX Time',
      'select ts, dur, controller_tx_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await support.addCounterTrack(
      ctx,
      'Controller RX Time',
      'select ts, dur, controller_rx_pct as value from bt_activity',
      groupName,
      {yRangeSharingKey: 'bt_controller_time', unit: '%'},
    );
    await support.addSliceTrack(
      ctx,
      'Quality reports',
      BT_QUALITY_REPORTS_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'RSSI Reports',
      BT_RSSI_REPORTS_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'HAL Crashes',
      BT_HAL_CRASHES_DATASET,
      groupName,
    );
    await support.addSliceTrack(
      ctx,
      'Code Path Counter',
      BT_CODE_PATH_COUNTER_DATASET,
      groupName,
    );
  }
}
