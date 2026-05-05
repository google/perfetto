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
import {Trace} from '../public/trace';
import {QueryFlamegraph, QueryFlamegraphMetric} from './query_flamegraph';
import {FlamegraphState} from '../widgets/flamegraph';

export interface FlamegraphPanelAttrs {
  readonly trace: Trace;
  readonly metrics?: ReadonlyArray<QueryFlamegraphMetric>;
  readonly state?: FlamegraphState;
  readonly onStateChange: (state: FlamegraphState) => void;
  // Identity-compared: a new array re-creates the inner QueryFlamegraph.
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
