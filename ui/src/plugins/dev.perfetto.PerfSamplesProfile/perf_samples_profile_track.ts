// Copyright (C) 2024 The Android Open Source Project
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

import m from 'mithril';
import {NUM} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {
  BaseSliceTrack,
  OnSliceClickArgs,
} from '../../frontend/base_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {NAMED_ROW, NamedRow} from '../../frontend/named_slice_track';
import {getColorForSample} from '../../public/lib/colorizer';
import {
  ProfileType,
  TrackEventDetails,
  TrackEventSelection,
} from '../../public/selection';
import {assertExists} from '../../base/logging';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../public/lib/query_flamegraph';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {time} from '../../base/time';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';

interface PerfSampleRow extends NamedRow {
  callsiteId: number;
}

abstract class BasePerfSamplesProfileTrack extends BaseSliceTrack<
  Slice,
  PerfSampleRow
> {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  protected getRowSpec(): PerfSampleRow {
    return {...NAMED_ROW, callsiteId: NUM};
  }

  protected rowToSlice(row: PerfSampleRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    const name = assertExists(row.name);
    const colorScheme = getColorForSample(row.callsiteId);
    return {...baseSlice, title: name, colorScheme};
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  onSliceClick(args: OnSliceClickArgs<Slice>): void {
    // TODO(stevegolton): Perhaps we could just move this to BaseSliceTrack?
    this.trace.selection.selectTrackEvent(this.uri, args.slice.id);
  }
}

export class ProcessPerfSamplesProfileTrack extends BasePerfSamplesProfileTrack {
  constructor(
    args: NewTrackArgs,
    private upid: number,
  ) {
    super(args);
  }

  getSqlSource(): string {
    return `
      select
        p.id,
        ts,
        0 as dur,
        0 as depth,
        'Perf Sample' as name,
        callsite_id as callsiteId
      from perf_sample p
      join thread using (utid)
      where upid = ${this.upid} and callsite_id is not null
      order by ts
    `;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const details = await super.getSelectionDetails(id);
    if (details === undefined) return undefined;
    return {
      ...details,
      upid: this.upid,
      profileType: ProfileType.PERF_SAMPLE,
    };
  }

  detailsPanel(sel: TrackEventSelection) {
    const upid = assertExists(sel.upid);
    const ts = sel.ts;

    const metrics = metricsFromTableOrSubquery(
      `
        (
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file,
            cast(line_number AS text) as line_number,
            self_count
          from _callstacks_for_callsites!((
            select p.callsite_id
            from perf_sample p
            join thread t using (utid)
            where p.ts >= ${ts}
              and p.ts <= ${ts}
              and t.upid = ${upid}
          ))
        )
      `,
      [
        {
          name: 'Perf Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      'include perfetto module linux.perf.samples',
      [{name: 'mapping_name', displayName: 'Mapping'}],
      [
        {
          name: 'source_file',
          displayName: 'Source File',
          mergeAggregation: 'ONE_OR_NULL',
        },
        {
          name: 'line_number',
          displayName: 'Line Number',
          mergeAggregation: 'ONE_OR_NULL',
        },
      ],
    );
    const serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(metrics),
    };
    const flamegraph = new QueryFlamegraph(this.trace, metrics, serialization);
    return {
      render: () => renderDetailsPanel(flamegraph, ts),
      serialization,
    };
  }
}

export class ThreadPerfSamplesProfileTrack extends BasePerfSamplesProfileTrack {
  constructor(
    args: NewTrackArgs,
    private utid: number,
  ) {
    super(args);
  }

  getSqlSource(): string {
    return `
      select
        p.id,
        ts,
        0 as dur,
        0 as depth,
        'Perf Sample' as name,
        callsite_id as callsiteId
      from perf_sample p
      where utid = ${this.utid} and callsite_id is not null
      order by ts
    `;
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const details = await super.getSelectionDetails(id);
    if (details === undefined) return undefined;
    return {
      ...details,
      utid: this.utid,
      profileType: ProfileType.PERF_SAMPLE,
    };
  }

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel {
    const utid = assertExists(sel.utid);
    const ts = sel.ts;

    const metrics = metricsFromTableOrSubquery(
      `
        (
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file,
            cast(line_number AS text) as line_number,
            self_count
          from _callstacks_for_callsites!((
            select p.callsite_id
            from perf_sample p
            where p.ts >= ${ts}
              and p.ts <= ${ts}
              and p.utid = ${utid}
          ))
        )
      `,
      [
        {
          name: 'Perf Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      'include perfetto module linux.perf.samples',
      [{name: 'mapping_name', displayName: 'Mapping'}],
      [
        {
          name: 'source_file',
          displayName: 'Source File',
          mergeAggregation: 'ONE_OR_NULL',
        },
        {
          name: 'line_number',
          displayName: 'Line Number',
          mergeAggregation: 'ONE_OR_NULL',
        },
      ],
    );
    const serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(metrics),
    };
    const flamegraph = new QueryFlamegraph(this.trace, metrics, serialization);
    return {
      render: () => renderDetailsPanel(flamegraph, ts),
      serialization,
    };
  }
}

function renderDetailsPanel(flamegraph: QueryFlamegraph, ts: time) {
  return m(
    '.flamegraph-profile',
    m(
      DetailsShell,
      {
        fillParent: true,
        title: m('.title', 'Perf Samples'),
        description: [],
        buttons: [
          m(
            'div.time',
            `First timestamp: `,
            m(Timestamp, {
              ts,
            }),
          ),
          m(
            'div.time',
            `Last timestamp: `,
            m(Timestamp, {
              ts,
            }),
          ),
        ],
      },
      flamegraph.render(),
    ),
  );
}
