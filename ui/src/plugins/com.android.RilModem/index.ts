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

const MODEM_RIL_CHANNELS_DATASET = new SourceDataset({
  src: `
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
    FROM Stage3
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
  },
});

const MODEM_CELL_RESELECTION_DATASET = new SourceDataset({
  src: `
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
    order by ts
  `,
  schema: {
    ts: LONG,
    dur: LONG_NULL,
    name: STR,
    raw_ril: UNKNOWN,
  },
});

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidRil';
  static readonly dependencies = [StandardGroupsPlugin, SupportPlugin];

  private support(ctx: Trace) {
    return ctx.plugins.getPlugin(SupportPlugin);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    const support = this.support(ctx);
    const features = await support.features(e);
    if (!features.has('track.ril')) {
      return;
    }

    const groupName = 'Modem Detail';

    const rilStrength = async (band: string, value: string) =>
      await support.addSliceTrack(
        ctx,
        `Modem signal strength ${band} ${value}`,
        new SourceDataset({
          src: `
            SELECT
              ts,
              dur,
              name
            FROM RilScreenOn
            WHERE band_name = '${band}' AND value_name = '${value}'
          `,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
        }),
        groupName,
      );

    await e.query(MODEM_RIL_STRENGTH);
    await e.query(MODEM_RIL_CHANNELS_PREAMBLE);

    await rilStrength('LTE', 'rsrp');
    await rilStrength('LTE', 'rssi');
    await rilStrength('NR', 'rsrp');
    await rilStrength('NR', 'rssi');

    await support.addSliceTrack(
      ctx,
      'Modem channel config',
      MODEM_RIL_CHANNELS_DATASET,
      groupName,
    );

    await support.addSliceTrack(
      ctx,
      'Modem cell reselection',
      MODEM_CELL_RESELECTION_DATASET,
      groupName,
    );
  }
}
