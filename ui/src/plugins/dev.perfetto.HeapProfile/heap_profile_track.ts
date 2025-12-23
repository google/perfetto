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
import {Time, time} from '../../base/time';
import {SliceTrack} from '../../components/tracks/slice_track';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {FlamegraphState} from '../../widgets/flamegraph';
import {
  HeapProfileFlamegraphDetailsPanel,
  profileType,
  ProfileType,
} from './heap_profile_details_panel';
import {HeapProfileSampleFlamegraphDetailsPanel} from './heap_profile_sample_details_panel';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Spinner} from '../../widgets/spinner';

class HeapProfileSampleDetailsLoader implements TrackEventDetailsPanel {
  private panel?: HeapProfileSampleFlamegraphDetailsPanel;

  constructor(
    private readonly trace: Trace,
    private readonly ts: time,
    private readonly detailsPanelState: FlamegraphState | undefined,
    private readonly onDetailsPanelStateChange: (
      state: FlamegraphState,
    ) => void,
  ) {}

  async load() {
    const result = await this.trace.engine.query(`
      SELECT utid FROM heap_profile_sample WHERE ts = ${this.ts} LIMIT 1
    `);
    if (result.numRows() === 0) {
      return;
    }
    const utid = result.firstRow({utid: NUM}).utid;
    this.panel = new HeapProfileSampleFlamegraphDetailsPanel(
      this.trace,
      this.ts,
      utid,
      this.detailsPanelState,
      this.onDetailsPanelStateChange,
    );
    await this.panel.load();
  }

  render() {
    if (!this.panel) {
      return m(Spinner);
    }
    return this.panel.render();
  }
}

export function createHeapProfileTrack(
  trace: Trace,
  uri: string,
  tableName: string,
  upid: number,
  heapProfileIsIncomplete: boolean,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      src: tableName,
      schema: {
        ts: LONG,
        type: STR,
        id: NUM,
      },
      filter: {
        col: 'upid',
        eq: upid,
      },
    }),
    detailsPanel: (row) => {
      const ts = Time.fromRaw(row.ts);
      const type = profileType(row.type);

      // For individual heap profile samples, use the sample-specific details panel
      if (type === ProfileType.HEAP_PROFILE_SAMPLE) {
        return new HeapProfileSampleDetailsLoader(
          trace,
          ts,
          detailsPanelState,
          onDetailsPanelStateChange,
        );
      }

      // For snapshot-based profiles, use the original details panel
      return new HeapProfileFlamegraphDetailsPanel(
        trace,
        heapProfileIsIncomplete,
        upid,
        type,
        ts,
        detailsPanelState,
        onDetailsPanelStateChange,
      );
    },
    tooltip: (slice) => slice.row.type,
  });
}
