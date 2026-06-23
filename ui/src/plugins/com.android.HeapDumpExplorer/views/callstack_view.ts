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

import m from 'mithril';
import type {Trace} from '../../../public/trace';
import {Time} from '../../../base/time';
import type {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {Flamegraph, type FlamegraphState} from '../../../widgets/flamegraph';
import {Stack} from '../../../widgets/stack';
import {EmptyState} from '../../../widgets/empty_state';

import {
  buildOomeCallstackMetrics,
  loadOomeErrorMsg,
} from '../../dev.perfetto.HeapProfile/oome_callstack_common';
import type {OomeData} from '../types';
import {getOome} from '../queries';
import type {HeapDump} from '../queries';

interface CallstackViewAttrs {
  readonly trace: Trace;
  readonly dump: HeapDump;
  readonly state: FlamegraphState | undefined;
  readonly onStateChange: (state: FlamegraphState) => void;
}

export const CallstackView: m.ClosureComponent<CallstackViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;
  let errorMsg: string | undefined;

  let cachedDump: HeapDump | undefined;
  let oomeData: OomeData | undefined;
  let isLoading = false;

  async function loadData(trace: Trace, dump: HeapDump) {
    isLoading = true;
    try {
      oomeData = await getOome(trace.engine, dump);
    } catch {
      oomeData = undefined;
    } finally {
      isLoading = false;
      m.redraw();
    }
  }

  async function loadErrorMsg(trace: Trace, ts: bigint) {
    errorMsg = await loadOomeErrorMsg(trace.engine, Time.fromRaw(ts));
  }

  return {
    view({attrs}) {
      if (attrs.dump !== cachedDump) {
        cachedDump = attrs.dump;
        oomeData = undefined;
        loadData(attrs.trace, attrs.dump);
      }

      if (isLoading) {
        return m(
          'div',
          {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
          m(FlamegraphPanel, {
            trace: attrs.trace,
            metrics: undefined,
            state: attrs.state,
            onStateChange: attrs.onStateChange,
          }),
        );
      }

      if (oomeData === undefined) {
        return m(
          EmptyState,
          {
            icon: 'data_array',
            title: 'Data is not available in this trace',
            fillHeight: true,
          },
          m(
            'div',
            'Callstacks in heap dumps are only available in Perfetto heap dumps collected on OutOfMemoryError and in recent versions of Android',
          ),
        );
      }

      const upid = oomeData.upid;
      const ts = oomeData.ts;
      const key = `${upid}:${ts}`;
      if (cachedMetrics === undefined || key !== cachedKey) {
        cachedMetrics = buildOomeCallstackMetrics(Time.fromRaw(ts));
        cachedKey = key;
        errorMsg = undefined;
        loadErrorMsg(attrs.trace, ts);
      }
      const metrics = cachedMetrics;

      let state = attrs.state;
      if (state === undefined) {
        state = Flamegraph.createDefaultState(metrics);
        attrs.onStateChange(state);
      }

      return m(
        'div',
        {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
        m(
          Stack,
          {orientation: 'vertical'},
          errorMsg &&
            m(
              'div',
              {style: {padding: '8px', fontSize: '14px', color: '#ff4081'}},
              errorMsg,
            ),
          m(FlamegraphPanel, {
            trace: attrs.trace,
            metrics,
            state,
            onStateChange: attrs.onStateChange,
          }),
        ),
      );
    },
  };
};
