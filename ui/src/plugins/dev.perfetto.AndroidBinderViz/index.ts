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

import {
  EngineProxy,
  MetricVisualisation,
  PluginContext,
  Store,
  TracePlugin,
} from '../../public';

const SPEC = `
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "width": "container",
  "height": 300,
  "description": ".",
  "data": {
    "name": "metric"
  },
  "mark": "bar",
  "encoding": {
    "x": {"field": "client_process", "type": "nominal"},
    "y": {"field": "client_dur", "aggregate": "max"}
  }
}
`;

interface State {}

class AndroidBinderVizPlugin implements TracePlugin {
  static migrate(_initialState: unknown): State {
    return {};
  }

  constructor(_store: Store<State>, _engine: EngineProxy) {
    // No-op
  }

  dispose(): void {
    // No-op
  }

  metricVisualisations(): MetricVisualisation[] {
    return [{
      metric: 'android_binder',
      spec: SPEC,
      path: ['android_binder', 'unaggregated_txn_breakdown'],
    }];
  }
}

export const plugin = {
  pluginId: 'dev.perfetto.AndroidBinderVizPlugin',
  activate(ctx: PluginContext) {
    ctx.registerTracePluginFactory(AndroidBinderVizPlugin);
  },
};
