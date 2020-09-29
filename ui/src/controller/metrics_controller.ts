// Copyright (C) 2020 The Android Open Source Project
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

import {Actions} from '../common/actions';
import {Engine, QueryError} from '../common/engine';
import {iter, STR} from '../common/query_iterator';

import {Controller} from './controller';
import {globals} from './globals';

export class MetricsController extends Controller<'main'> {
  private engine: Engine;
  private currentlyRunningMetric?: string;

  constructor(args: {engine: Engine}) {
    super('main');
    this.engine = args.engine;
    this.setup().finally(() => {
      this.run();
    });
  }

  private async getMetricNames() {
    const metrics = [];
    const it = iter(
        {
          name: STR,
        },
        await this.engine.query('select name from trace_metrics'));
    for (; it.valid(); it.next()) {
      metrics.push(it.row.name);
    }
    return metrics;
  }

  private async setup() {
    const metrics = await this.getMetricNames();
    globals.dispatch(Actions.setAvailableMetrics({metrics}));
  }

  private async computeMetric(name: string) {
    if (name === this.currentlyRunningMetric) return;
    this.currentlyRunningMetric = name;
    try {
      const metricResult = await this.engine.computeMetric([name]);
      globals.publish(
          'MetricResult',
          {name, resultString: metricResult.metricsAsPrototext});
    } catch (e) {
      if (e instanceof QueryError) {
        // Reroute error to be displated differently when metric is run through
        // metric page.
        globals.publish('MetricResult', {name, error: e.message});
      } else {
        throw e;
      }
    }
    globals.dispatch(Actions.resetMetricRequest({name}));
    this.currentlyRunningMetric = undefined;
  }

  run() {
    const {requestedMetric} = globals.state.metrics;
    if (!requestedMetric) return;
    this.computeMetric(requestedMetric);
  }
}
