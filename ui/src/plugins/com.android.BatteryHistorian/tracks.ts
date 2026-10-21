// Copyright (C) 2026 The Android Open Source Project
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

// The layout of the Battery Historian workspace: which battery stats tracks
// it shows, in which order, under which labels, and the SQL behind each.
// Kept free of UI code so it can be unit tested.

/** A counter plotted on the battery chart. */
export interface ChartCounterSpec {
  // The counter track's name in trace processor.
  readonly track: string;
  readonly name: string;
  readonly unit: string;
  // Multiplies the stored value into `unit`.
  readonly scale?: number;
}

// The battery chart, in the order its rows are drawn.
export const CHART_COUNTERS: ReadonlyArray<ChartCounterSpec> = [
  {track: 'batt.capacity_pct', name: 'Battery level', unit: '%'},
  {track: 'BatteryTemperature', name: 'Temperature', unit: '°C'},
  {track: 'batt.voltage_uv', name: 'Voltage', unit: 'V', scale: 1e-6},
  {track: 'batt.charge_uah', name: 'Charge', unit: 'mAh', scale: 1e-3},
];

/**
 * A row of the timeline, drawn as a slice track:
 * - 'flag': a battery stats state which is on or off, e.g. screen. Shows a
 *   slice, named after the row, while it is on.
 * - 'enum': a battery stats state with several values, e.g. signal strength.
 *   Shows a slice per value, except the values in `hide`.
 * - 'event': battery stats events, e.g. jobs. Paired start (+) and finish (-)
 *   events become slices and the others instants, named after their tag.
 * - 'sql': anything else, from a query returning ts, dur and name.
 */
export type RowSpec =
  | {
      readonly kind: 'flag';
      readonly name: string;
      readonly track: string;
    }
  | {
      readonly kind: 'enum';
      readonly name: string;
      readonly track: string;
      readonly hide?: ReadonlyArray<string>;
    }
  | {
      readonly kind: 'event';
      readonly name: string;
      readonly track: string;
    }
  | {
      readonly kind: 'sql';
      readonly name: string;
      // The track that must exist for the row to be shown.
      readonly track: string;
      readonly query: string;
    };

export interface GroupSpec {
  readonly name: string;
  readonly rows: ReadonlyArray<RowSpec>;
}

// A counter mapped to names, e.g. PlugType 1 to 'AC'. Every change of value
// starts a slice lasting until the next change.
function counterStatesQuery(
  track: string,
  names: ReadonlyArray<readonly [number, string]>,
  hide: ReadonlyArray<number> = [],
): string {
  const cases = names
    .map(([value, name]) => `WHEN ${value} THEN '${name}'`)
    .join(' ');
  const hidden =
    hide.length > 0 ? `WHERE value NOT IN (${hide.join(', ')})` : '';
  return `
    SELECT ts, dur, name FROM (
      SELECT
        c.ts,
        lead(c.ts, 1, trace_end()) OVER (ORDER BY c.ts) - c.ts AS dur,
        cast_int!(c.value) AS value,
        CASE cast_int!(c.value) ${cases}
          ELSE cast_string!(cast_int!(c.value))
        END AS name
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name = '${track}'
    ) ${hidden}`;
}

const CPU_RUNNING_QUERY = `
  SELECT
    s.ts,
    s.safe_dur AS dur,
    ifnull(
      (
        SELECT str_split(w.name, '"', 1)
        FROM slice w
        JOIN track wt ON w.track_id = wt.id
        WHERE wt.name = 'battery_stats.wakeup_reason' AND w.ts = s.ts
        LIMIT 1
      ),
      'CPU running'
    ) AS name
  FROM android_battery_stats_state s
  WHERE s.track_name = 'battery_stats.running' AND s.value = 1`;

const USERSPACE_WAKELOCKS_QUERY = `
  SELECT s.ts, s.dur, s.name
  FROM slice s
  JOIN track t ON s.track_id = t.id
  WHERE t.name = 'WakeLocks'`;

// The groups of rows under the battery chart, in the order of Battery
// Historian's timeline. Rows for tracks the trace doesn't have are skipped.
export const GROUPS: ReadonlyArray<GroupSpec> = [
  {
    name: 'Power',
    rows: [
      {
        kind: 'sql',
        name: 'Plugged',
        track: 'PlugType',
        query: counterStatesQuery(
          'PlugType',
          [
            [0, 'Unplugged'],
            [1, 'AC'],
            [2, 'USB'],
            [4, 'Wireless'],
            [8, 'Dock'],
          ],
          [0],
        ),
      },
      {kind: 'flag', name: 'Charging on', track: 'battery_stats.charging'},
      {
        kind: 'sql',
        name: 'Battery status',
        track: 'BatteryStatus',
        query: counterStatesQuery('BatteryStatus', [
          [1, 'Unknown'],
          [2, 'Charging'],
          [3, 'Discharging'],
          [4, 'Not charging'],
          [5, 'Full'],
        ]),
      },
      {
        kind: 'sql',
        name: 'Battery health',
        track: 'BatteryHealth',
        query: counterStatesQuery('BatteryHealth', [
          [1, 'Unknown'],
          [2, 'Good'],
          [3, 'Overheat'],
          [4, 'Dead'],
          [5, 'Over voltage'],
          [6, 'Failure'],
          [7, 'Cold'],
        ]),
      },
      {
        kind: 'flag',
        name: 'Power save mode',
        track: 'battery_stats.power_save',
      },
      {
        kind: 'enum',
        name: 'Doze',
        track: 'battery_stats.device_idle',
        hide: ['off'],
      },
    ],
  },
  {
    name: 'System',
    rows: [
      {
        kind: 'sql',
        name: 'CPU running',
        track: 'battery_stats.running',
        query: CPU_RUNNING_QUERY,
      },
      {
        kind: 'event',
        name: 'Kernel wakeup reasons',
        track: 'battery_stats.wakeup_reason',
      },
      {
        kind: 'sql',
        name: 'Userspace wakelocks',
        track: 'WakeLocks',
        query: USERSPACE_WAKELOCKS_QUERY,
      },
      {kind: 'event', name: 'Long wakelocks', track: 'battery_stats.longwake'},
      {kind: 'flag', name: 'Screen on', track: 'battery_stats.screen'},
      {kind: 'flag', name: 'Screen doze', track: 'battery_stats.screen_doze'},
      {kind: 'enum', name: 'Brightness', track: 'battery_stats.brightness'},
      {kind: 'event', name: 'Screen wake', track: 'battery_stats.screenwake'},
      {kind: 'event', name: 'Top app', track: 'battery_stats.top'},
      {kind: 'event', name: 'Foreground', track: 'battery_stats.fg'},
      {kind: 'event', name: 'Jobs', track: 'battery_stats.job'},
      {kind: 'event', name: 'Syncs', track: 'battery_stats.sync'},
      {kind: 'event', name: 'Alarms', track: 'battery_stats.alarm'},
      {kind: 'event', name: 'App wakeups', track: 'battery_stats.wakeupap'},
      {
        kind: 'event',
        name: 'Temp allowlist',
        track: 'battery_stats.tmpwhitelist',
      },
      {kind: 'event', name: 'Device active', track: 'battery_stats.active'},
      {kind: 'event', name: 'Package active', track: 'battery_stats.pkgactive'},
      {kind: 'event', name: 'Package installs', track: 'battery_stats.pkginst'},
      {
        kind: 'event',
        name: 'Package uninstalls',
        track: 'battery_stats.pkgunin',
      },
    ],
  },
  {
    name: 'Cellular',
    rows: [
      {
        kind: 'flag',
        name: 'Mobile radio active',
        track: 'battery_stats.mobile_radio',
      },
      {
        kind: 'enum',
        name: 'Mobile signal strength',
        track: 'battery_stats.phone_signal_strength',
      },
      {
        kind: 'enum',
        name: 'Network type',
        track: 'battery_stats.data_conn',
      },
      {kind: 'enum', name: 'Phone state', track: 'battery_stats.phone_state'},
      {kind: 'flag', name: 'Phone call', track: 'battery_stats.phone_in_call'},
      {
        kind: 'flag',
        name: 'Phone scanning',
        track: 'battery_stats.phone_scanning',
      },
      {
        kind: 'flag',
        name: 'High TX power',
        track: 'battery_stats.cellular_high_tx_power',
      },
      {kind: 'event', name: 'Connectivity', track: 'battery_stats.conn'},
    ],
  },
  {
    name: 'Wi-Fi & Bluetooth',
    rows: [
      {kind: 'flag', name: 'Wi-Fi on', track: 'battery_stats.wifi'},
      {
        kind: 'flag',
        name: 'Wi-Fi running',
        track: 'battery_stats.wifi_running',
      },
      {kind: 'flag', name: 'Wi-Fi radio', track: 'battery_stats.wifi_radio'},
      {kind: 'flag', name: 'Wi-Fi scan', track: 'battery_stats.wifi_scan'},
      {
        kind: 'enum',
        name: 'Wi-Fi signal strength',
        track: 'battery_stats.wifi_signal_strength',
      },
      {
        kind: 'enum',
        name: 'Wi-Fi supplicant',
        track: 'battery_stats.wifi_suppl',
      },
      {
        kind: 'flag',
        name: 'Wi-Fi full lock',
        track: 'battery_stats.wifi_full_lock',
      },
      {
        kind: 'flag',
        name: 'Wi-Fi multicast',
        track: 'battery_stats.wifi_multicast',
      },
      {kind: 'flag', name: 'Bluetooth', track: 'battery_stats.bluetooth'},
      {kind: 'flag', name: 'BLE scanning', track: 'battery_stats.ble_scan'},
    ],
  },
  {
    name: 'Location, media & sensors',
    rows: [
      {kind: 'flag', name: 'GPS', track: 'battery_stats.gps'},
      {
        kind: 'enum',
        name: 'GPS signal quality',
        track: 'battery_stats.gps_signal_quality',
      },
      {kind: 'flag', name: 'Sensor', track: 'battery_stats.sensor'},
      {kind: 'flag', name: 'Audio', track: 'battery_stats.audio'},
      {kind: 'flag', name: 'Video', track: 'battery_stats.video'},
      {kind: 'flag', name: 'Camera', track: 'battery_stats.camera'},
      {kind: 'flag', name: 'Flashlight', track: 'battery_stats.flashlight'},
    ],
  },
];

function sqlString(s: string): string {
  return `'${s.replace(/'/g, "''")}'`;
}

/** The query (ts, dur, name) behind a row. */
export function rowQuery(row: RowSpec): string {
  const track = sqlString(row.track);
  switch (row.kind) {
    case 'flag':
      return `
        SELECT ts, safe_dur AS dur, ${sqlString(row.name)} AS name
        FROM android_battery_stats_state
        WHERE track_name = ${track} AND value != 0`;
    case 'enum': {
      const hide = row.hide ?? [];
      const hidden =
        hide.length > 0
          ? `AND value_name NOT IN (${hide.map(sqlString).join(', ')})`
          : '';
      return `
        SELECT ts, safe_dur AS dur, value_name AS name
        FROM android_battery_stats_state
        WHERE track_name = ${track} ${hidden}`;
    }
    case 'event':
      // Events are named like '+job=10123:"com.example/.Job"'.
      return `
        SELECT ts, safe_dur AS dur, str_value AS name
        FROM android_battery_stats_event_slices
        WHERE track_name = ${track}
        UNION ALL
        SELECT s.ts, 0 AS dur, str_split(s.name, '"', 1) AS name
        FROM slice s
        JOIN track t ON s.track_id = t.id
        WHERE t.name = ${track} AND substr(s.name, 1, 1) NOT IN ('+', '-')`;
    case 'sql':
      return row.query;
  }
}

/** The groups, keeping only the rows whose track is in `tracks`. */
export function availableGroups(
  tracks: ReadonlySet<string>,
): ReadonlyArray<GroupSpec> {
  return GROUPS.map((group) => ({
    name: group.name,
    rows: group.rows.filter((row) => tracks.has(row.track)),
  })).filter((group) => group.rows.length > 0);
}

/** Every track name the workspace can show, to look up in the trace. */
export function wantedTracks(): string[] {
  const names = new Set<string>(CHART_COUNTERS.map((c) => c.track));
  for (const group of GROUPS) {
    for (const row of group.rows) {
      names.add(row.track);
    }
  }
  return [...names];
}
