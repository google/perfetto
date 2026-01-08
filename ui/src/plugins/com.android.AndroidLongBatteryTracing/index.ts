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
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {PerfettoPlugin} from '../../public/plugin';
import {
  STR,
  LONG,
  UNKNOWN,
  SqlValue,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';

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

const DEFAULT_NETWORK_DATASET = new SourceDataset({
  src: `
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
    from diff where keep is null or keep
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

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

const TETHERING_DATASET = new SourceDataset({
  src: `
    with base as (
        select
            ts as ts_end,
            EXTRACT_ARG(arg_set_id, 'network_tethering_reported.duration_millis') * 1000000 as dur
        from track t join slice s on t.id = s.track_id
        where t.name = 'Statsd Atoms'
          and s.name = 'network_tethering_reported'
    )
    select ts_end - dur as ts, dur, 'Tethering' as name from base
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const NETWORK_SUMMARY = `
  create or replace perfetto table network_summary as
  with base as (
      select
          cast(ts / 5000000000 as int64) * 5000000000 AS ts,
          case
              when iface glob '*wlan*' then 'wifi'
              when iface glob '*rmnet*' then 'modem'
              else 'unknown'
          end as dev_type,
          package_name as pkg,
          sum(packet_length) AS value
      from android_network_packets
      where (iface glob '*wlan*' or iface glob '*rmnet*')
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

const SUSPEND_RESUME_DATASET = new SourceDataset({
  src: `
    SELECT
      ts,
      dur,
      'Suspended' AS name
    FROM android_suspend_state
    WHERE power_state = 'suspended'
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const THERMAL_THROTTLING_DATASET = new SourceDataset({
  src: `
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
    where severity != 'NONE'
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const KERNEL_WAKELOCKS_STATSD = `
  create or replace perfetto table kernel_wakelocks_statsd as
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

const KERNEL_WAKELOCKS_STATSD_SUMMARY = `
  select wakelock_name, max(value) as max_value
  from kernel_wakelocks_statsd
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

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLongBatteryTracing';
  static readonly dependencies = [StandardGroupsPlugin, SupportPlugin];

  private support(ctx: Trace) {
    return ctx.plugins.getPlugin(SupportPlugin);
  }

  async addBatteryStatsState(
    ctx: Trace,
    support: SupportPlugin,
    name: string,
    track: string,
    groupName: string,
    features: Set<string>,
  ) {
    if (!features.has(`track.${track}`)) {
      return;
    }
    await support.addSliceTrack(
      ctx,
      name,
      new SourceDataset({
        src: `
          SELECT
            ts,
            safe_dur AS dur,
            value_name AS name
          FROM android_battery_stats_state
          WHERE track_name = "${track}"
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
  }

  async addBatteryStatsEvent(
    ctx: Trace,
    support: SupportPlugin,
    name: string,
    track: string,
    groupName: string,
    features: Set<string>,
  ) {
    if (!features.has(`track.${track}`)) {
      return;
    }

    await support.addSliceTrack(
      ctx,
      name,
      new SourceDataset({
        src: `
          SELECT
            ts,
            safe_dur AS dur,
            str_value AS name
          FROM android_battery_stats_event_slices
          WHERE track_name = "${track}"
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
  }

  async addDeviceState(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (!features.has('track.battery_stats.*')) {
      return;
    }

    const groupName = 'Device State';
    const deviceStateGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'DEVICE_STATE');
    support.groups.set(groupName, deviceStateGroup);

    const query = (name: string, track: string) =>
      this.addBatteryStatsEvent(ctx, support, name, track, groupName, features);

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.battery_stats;`);
    await e.query(`INCLUDE PERFETTO MODULE android.suspend;`);
    await e.query(`INCLUDE PERFETTO MODULE android.battery.charging_states;`);
    await e.query(`INCLUDE PERFETTO MODULE android.battery.doze;`);
    await e.query(`INCLUDE PERFETTO MODULE android.screen_state;`);

    await support.addSliceTrack(
      ctx,
      'Screen state',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            screen_state AS name
          FROM android_screen_state
        `,
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
      'Charging',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            charging_state AS name
          FROM android_charging_states
        `,
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
      'Suspend / resume',
      SUSPEND_RESUME_DATASET,
      groupName,
    );

    await support.addSliceTrack(
      ctx,
      'Doze light state',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            light_idle_state AS name
          FROM android_light_idle_state
        `,
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
      'Doze deep state',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            deep_idle_state AS name
          FROM android_deep_idle_state
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );

    query('Top app', 'battery_stats.top');

    await support.addSliceTrack(
      ctx,
      'Long wakelocks',
      new SourceDataset({
        src: `
          SELECT
            -- Clamp start time to > 0 to avoid negative timestamps.
            MAX(0, ts - 60000000000) AS ts,
            -- The end time is (ts + safe_dur), so the duration is the original
            -- end time minus the clamped start time.
            (ts + safe_dur) - MAX(0, ts - 60000000000) AS dur,
            str_value AS name,
            package_name AS package
          FROM add_package_name!((
            SELECT
              *,
              int_value AS uid
            FROM android_battery_stats_event_slices
            WHERE track_name = "battery_stats.longwake"
          ))
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
          package: STR,
        },
      }),
      groupName,
    );

    query('Foreground apps', 'battery_stats.fg');

    if (
      features.has('atom.scheduled_job_state_changed') &&
      features.has('google3')
    ) {
      await e.query(`INCLUDE PERFETTO MODULE
         google3.wireless.android.telemetry.trace_extractor.modules.power.jobs;`);
      await support.addSliceTrack(
        ctx,
        'Jobs',
        new SourceDataset({
          src: `
            SELECT
              ts,
              dur,
              tag AS name,
              uid
            FROM jobs
          `,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            uid: UNKNOWN,
          },
        }),
        groupName,
      );
    } else {
      query('Jobs', 'battery_stats.job');
    }

    if (features.has('atom.thermal_throttling_severity_state_changed')) {
      await support.addSliceTrack(
        ctx,
        'Thermal throttling',
        THERMAL_THROTTLING_DATASET,
        groupName,
      );
    }
  }

  async addAtomCounters(ctx: Trace, support: SupportPlugin): Promise<void> {
    const e = ctx.engine;

    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.atom_counters_slices`,
    );

    const counters = await e.query(
      `select distinct ui_group, ui_name, ui_unit, counter_name
       from atom_counters
       where ui_name is not null`,
    );
    const countersIt = counters.iter({
      ui_group: STR,
      ui_name: STR,
      ui_unit: STR,
      counter_name: STR,
    });
    for (; countersIt.valid(); countersIt.next()) {
      const unit = countersIt.ui_unit;
      const opts =
        unit === '%'
          ? {yOverrideMaximum: 100, unit: '%'}
          : unit !== undefined
            ? {unit}
            : undefined;

      await support.addCounterTrack(
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

  async addAtomSlices(ctx: Trace, support: SupportPlugin): Promise<void> {
    const e = ctx.engine;

    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.atom_counters_slices`,
    );

    const sliceTracks = await e.query(
      `select distinct ui_group, ui_name, atom, field
       from atom_slices
       where ui_name is not null
       order by 1, 2, 3, 4`,
    );
    const slicesIt = sliceTracks.iter({
      atom: STR,
      ui_group: STR,
      ui_name: STR,
      field: STR,
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

      // Add schema entries for dynamic columns
      const argsSchema: Record<string, SqlValue> = {};
      for (const arg of args) {
        argsSchema[safeArg(arg)] = UNKNOWN;
      }

      await support.addSliceTrack(
        ctx,
        tracks.get(atom)!.ui_name,
        new SourceDataset({
          src: `
            SELECT
              ts,
              dur,
              slice_name as name,
              ${args.map((a) => argSql(a)).join(',')}
            FROM atom_slices
            WHERE atom = '${atom}'
            GROUP BY ts, dur, name
          `,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            ...argsSchema,
          },
        }),
        tracks.get(atom)!.ui_group,
      );
    }
  }

  async addNetworkSummary(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (!features.has('net.modem') && !features.has('net.wifi')) {
      return;
    }

    const groupName = 'Network Summary';

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.battery_stats;`);
    await e.query(`INCLUDE PERFETTO MODULE android.network_packets;`);
    await e.query(NETWORK_SUMMARY);
    await e.query(RADIO_TRANSPORT_TYPE);

    await support.addSliceTrack(
      ctx,
      'Default network',
      DEFAULT_NETWORK_DATASET,
      groupName,
    );

    if (features.has('atom.network_tethering_reported')) {
      await support.addSliceTrack(
        ctx,
        'Tethering',
        TETHERING_DATASET,
        groupName,
      );
    }
    if (features.has('net.wifi')) {
      await support.addCounterTrack(
        ctx,
        'Wifi total bytes',
        `select ts, sum(value) as value from network_summary where dev_type = 'wifi' group by 1`,
        groupName,
        {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
      );
      const result = await e.query(
        `select pkg, sum(value) from network_summary where dev_type='wifi' group by 1 order by 2 desc limit 10`,
      );
      const it = result.iter({pkg: STR});
      for (; it.valid(); it.next()) {
        await support.addCounterTrack(
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
      support,
      'Wifi interface',
      'battery_stats.wifi_radio',
      groupName,
      features,
    );
    this.addBatteryStatsState(
      ctx,
      support,
      'Wifi supplicant state',
      'battery_stats.wifi_suppl',
      groupName,
      features,
    );
    this.addBatteryStatsState(
      ctx,
      support,
      'Wifi strength',
      'battery_stats.wifi_signal_strength',
      groupName,
      features,
    );
    if (features.has('net.modem')) {
      await support.addCounterTrack(
        ctx,
        'Modem total bytes',
        `select ts, sum(value) as value from network_summary where dev_type = 'modem' group by 1`,
        groupName,
        {yDisplay: 'log', yRangeSharingKey: 'net_bytes', unit: 'byte'},
      );
      const result = await e.query(
        `select pkg, sum(value) from network_summary where dev_type='modem' group by 1 order by 2 desc limit 10`,
      );
      const it = result.iter({pkg: STR});
      for (; it.valid(); it.next()) {
        await support.addCounterTrack(
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
      support,
      'Cellular interface',
      'battery_stats.mobile_radio',
      groupName,
      features,
    );
    await support.addSliceTrack(
      ctx,
      'Cellular connection',
      new SourceDataset({
        src: `
          SELECT
            ts,
            dur,
            name
          FROM radio_transport
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
      groupName,
    );
    this.addBatteryStatsState(
      ctx,
      support,
      'Cellular strength',
      'battery_stats.phone_signal_strength',
      groupName,
      features,
    );
  }

  async addModemMintData(ctx: Trace, support: SupportPlugin): Promise<void> {
    const e = ctx.engine;
    const groupName = 'Modem Detail';

    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.modem_mint_metrics`,
    );

    const counters = await e.query(
      `select distinct name from pixel_modem_counters`,
    );
    const countersIt = counters.iter({name: STR});
    for (; countersIt.valid(); countersIt.next()) {
      await support.addCounterTrack(
        ctx,
        countersIt.name,
        `select ts, value from pixel_modem_counters where name = '${countersIt.name}'`,
        groupName,
      );
    }
    const slices = await e.query(
      `select distinct track_name from pixel_modem_slices`,
    );
    const slicesIt = slices.iter({track_name: STR});
    for (; slicesIt.valid(); slicesIt.next()) {
      await support.addSliceTrack(
        ctx,
        slicesIt.track_name,
        new SourceDataset({
          src: `
            SELECT
              ts,
              dur,
              slice_name as name
            FROM pixel_modem_slices
            WHERE track_name = '${slicesIt.track_name}'
          `,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
        }),
        groupName,
      );
    }
  }

  async addKernelWakelocks(ctx: Trace, support: SupportPlugin): Promise<void> {
    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.kernel_wakelocks;`);
    const result = await e.query(
      `SELECT DISTINCT name, type FROM android_kernel_wakelocks`,
    );
    const it = result.iter({name: STR, type: STR});
    for (; it.valid(); it.next()) {
      await support.addCounterTrack(
        ctx,
        it.name,
        `SELECT ts, dur, held_ratio * 100 AS value
         FROM android_kernel_wakelocks
         WHERE name = "${it.name}"`,
        'Kernel Wakelock Summary',
        {yRangeSharingKey: 'kernel_wakelock', unit: '%'},
      );
    }
  }

  async addKernelWakelocksStatsd(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (!features.has('atom.kernel_wakelock')) {
      return;
    }
    const groupName = 'Kernel Wakelock Summary (statsd)';

    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.suspend;`);
    await e.query(KERNEL_WAKELOCKS_STATSD);
    const result = await e.query(KERNEL_WAKELOCKS_STATSD_SUMMARY);
    const it = result.iter({wakelock_name: STR});
    for (; it.valid(); it.next()) {
      await support.addCounterTrack(
        ctx,
        `${it.wakelock_name} (statsd)`,
        `select ts, dur, value
         from kernel_wakelocks_statsd
         where wakelock_name = "${it.wakelock_name}"`,
        groupName,
        {yRangeSharingKey: 'kernel_wakelock_statsd', unit: '%'},
      );
    }
  }

  async addWakeups(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (!features.has('track.suspend_backoff')) {
      return;
    }

    const e = ctx.engine;
    const groupName = 'Wakeups';
    await e.query(`INCLUDE PERFETTO MODULE android.wakeups;`);
    const result = await e.query(`select
          item,
          sum(dur) as sum_dur
      from android_wakeups
      group by 1
      having sum_dur > 600e9`);
    const it = result.iter({item: STR});
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
                on_device_attribution,
                suspend_quality,
                backoff_state,
                backoff_reason,
                backoff_count,
                backoff_millis
            from android_wakeups`;
    const items = [];
    let labelOther = false;
    for (; it.valid(); it.next()) {
      labelOther = true;

      await support.addSliceTrack(
        ctx,
        `Wakeup ${it.item}`,
        new SourceDataset({
          src: `${sqlPrefix} WHERE item="${it.item}"`,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            item: UNKNOWN,
            type: UNKNOWN,
            raw_wakeup: UNKNOWN,
            on_device_attribution: UNKNOWN,
            suspend_quality: UNKNOWN,
            backoff_state: UNKNOWN,
            backoff_reason: UNKNOWN,
            backoff_count: UNKNOWN,
            backoff_millis: UNKNOWN,
          },
        }),
        groupName,
      );
      items.push(it.item);
    }

    await support.addSliceTrack(
      ctx,
      labelOther ? 'Other wakeups' : 'Wakeups',
      new SourceDataset({
        src: `${sqlPrefix} WHERE item NOT IN ('${items.join("','")}')`,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
          item: UNKNOWN,
          type: UNKNOWN,
          raw_wakeup: UNKNOWN,
          on_device_attribution: UNKNOWN,
          suspend_quality: UNKNOWN,
          backoff_state: UNKNOWN,
          backoff_reason: UNKNOWN,
          backoff_count: UNKNOWN,
          backoff_millis: UNKNOWN,
        },
      }),
      groupName,
    );
  }

  async addHighCpu(
    ctx: Trace,
    support: SupportPlugin,
    features: Set<string>,
  ): Promise<void> {
    if (!features.has('atom.cpu_cycles_per_uid_cluster')) {
      return;
    }
    const groupName = 'CPU per UID (major users, from statsd)';

    const e = ctx.engine;

    await e.query(HIGH_CPU);
    const result = await e.query(
      `select distinct pkg, cluster from high_cpu where value > 10 order by 1, 2`,
    );
    const it = result.iter({pkg: STR, cluster: STR});
    for (; it.valid(); it.next()) {
      await support.addCounterTrack(
        ctx,
        `${it.pkg} (${it.cluster})`,
        `select ts, value from high_cpu where pkg = "${it.pkg}" and cluster="${it.cluster}"`,
        groupName,
        {yOverrideMaximum: 100, unit: '%'},
      );
    }
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const support = this.support(ctx);
    const features = await support.features(ctx.engine);

    await ctx.engine.query(PACKAGE_LOOKUP);
    await this.addNetworkSummary(ctx, support, features);
    await this.addKernelWakelocks(ctx, support);
    await this.addKernelWakelocksStatsd(ctx, support, features);
    await this.addWakeups(ctx, support, features);
    await this.addDeviceState(ctx, support, features);
    await this.addHighCpu(ctx, support, features);

    if (features.has('google3')) {
      await this.addAtomCounters(ctx, support);
      await this.addAtomSlices(ctx, support);
      await this.addModemMintData(ctx, support);
    }
  }
}
