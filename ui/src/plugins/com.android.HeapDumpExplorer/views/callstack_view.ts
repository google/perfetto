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

import {AsyncLimiter} from '../../../base/async_limiter';
import {Monitor} from '../../../base/monitor';

interface CallstackViewAttrs {
  readonly trace: Trace;
  readonly dump: HeapDump;
  readonly state: FlamegraphState | undefined;
  readonly onStateChange: (state: FlamegraphState) => void;
}

export class CallstackView implements m.ClassComponent<CallstackViewAttrs> {
  private oomeData?: OomeData;
  private oomeDataLoaded = false;
  private cachedMetrics?: ReadonlyArray<QueryFlamegraphMetric>;
  private cachedKey?: string;
  private errorMsg?: string;
  private readonly limiter = new AsyncLimiter();
  private monitor?: Monitor;

  async loadErrorMsg(trace: Trace, ts: bigint) {
    this.errorMsg = await loadOomeErrorMsg(trace.engine, Time.fromRaw(ts));
    m.redraw();
  }

  view({attrs}: m.Vnode<CallstackViewAttrs>) {
    this.monitor ??= new Monitor([() => attrs.dump]);
    if (this.monitor.ifStateChanged()) {
      this.oomeData = undefined;
      this.oomeDataLoaded = false;
      const dump = attrs.dump;
      this.limiter.schedule(async () => {
        try {
          this.oomeData = await getOome(attrs.trace.engine, dump);
        } catch {
          this.oomeData = undefined;
        } finally {
          this.oomeDataLoaded = true;
          m.redraw();
        }
      });
    }

    if (!this.oomeDataLoaded) {
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

    if (this.oomeData === undefined) {
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

    const upid = this.oomeData.upid;
    const ts = this.oomeData.ts;
    const key = `${upid}:${ts}`;
    if (this.cachedMetrics === undefined || key !== this.cachedKey) {
      this.cachedMetrics = buildOomeCallstackMetrics(Time.fromRaw(ts));
      this.cachedKey = key;
      this.errorMsg = undefined;
      this.loadErrorMsg(attrs.trace, ts);
    }
    const metrics = this.cachedMetrics;

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
        this.errorMsg &&
          m(
            'div',
            {style: {padding: '8px', fontSize: '14px', color: '#ff4081'}},
            this.errorMsg,
          ),
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics,
          state,
          onStateChange: attrs.onStateChange,
        }),
      ),
    );
  }
}
