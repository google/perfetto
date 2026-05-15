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

import type m from 'mithril';
import type {Trace} from '../public/trace';
import {QueryFlamegraph, type QueryFlamegraphMetric} from './query_flamegraph';
import type {FlamegraphState} from '../widgets/flamegraph';

export interface FlamegraphPanelAttrs {
  readonly trace: Trace;

  // The metrics to render. Undefined shows a loading state.
  readonly metrics?: ReadonlyArray<QueryFlamegraphMetric>;

  // Caller-owned flamegraph state (filters, view, selected metric). When
  // `metrics` change, pass `Flamegraph.updateState(state, metrics)` to keep
  // the selected metric valid. Undefined shows an empty pending state.
  readonly state?: FlamegraphState;

  readonly onStateChange: (state: FlamegraphState) => void;

  // Perfetto tables / indices the metric SQL depends on. The panel forwards
  // them to the inner `QueryFlamegraph`, which disposes them along with
  // itself on unmount or when the array reference changes.
  readonly dependencies?: ReadonlyArray<AsyncDisposable>;
}

// Mithril wrapper around `QueryFlamegraph` that owns the inner instance's
// lifetime: it is created on first render and disposed on unmount or when
// `trace` / `dependencies` identity changes. Lets area-selection tabs and
// details panels render a flamegraph without managing `[Symbol.asyncDispose]`
// themselves.
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
