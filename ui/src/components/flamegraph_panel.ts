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

// One-line wrapper around `QueryFlamegraph` that owns the inner instance's
// lifetime and disposes it on unmount. Callers stop allocating + leaking the
// inner class manually.

import m from 'mithril';
import {Trace} from '../public/trace';
import {QueryFlamegraph, QueryFlamegraphMetric} from './query_flamegraph';
import {FlamegraphState} from '../widgets/flamegraph';

export interface FlamegraphPanelAttrs {
  readonly trace: Trace;
  // undefined → loading state.
  readonly metrics?: ReadonlyArray<QueryFlamegraphMetric>;
  // Caller-owned. Pass `Flamegraph.updateState(state, metrics)` to re-seed
  // when metrics change. Undefined → flamegraph renders empty pending state.
  readonly state?: FlamegraphState;
  readonly onStateChange: (state: FlamegraphState) => void;
  // Perfetto tables / indices the metric SQL depends on. Identity-compared;
  // a new array reference re-creates the inner QueryFlamegraph (the prior
  // one is asynchronously disposed).
  readonly dependencies?: ReadonlyArray<AsyncDisposable>;
}

export class FlamegraphPanel implements m.ClassComponent<FlamegraphPanelAttrs> {
  private flamegraph?: QueryFlamegraph;
  private lastTrace?: Trace;
  private lastDeps?: ReadonlyArray<AsyncDisposable>;

  view({attrs}: m.CVnode<FlamegraphPanelAttrs>): m.Children {
    if (
      this.flamegraph === undefined ||
      this.lastTrace !== attrs.trace ||
      this.lastDeps !== attrs.dependencies
    ) {
      void this.flamegraph?.[Symbol.asyncDispose]();
      this.flamegraph = new QueryFlamegraph(attrs.trace, attrs.dependencies);
      this.lastTrace = attrs.trace;
      this.lastDeps = attrs.dependencies;
    }
    return this.flamegraph.render({
      metrics: attrs.metrics,
      state: attrs.state,
      onStateChange: attrs.onStateChange,
    });
  }

  async onremove(): Promise<void> {
    await this.flamegraph?.[Symbol.asyncDispose]();
    this.flamegraph = undefined;
  }
}
