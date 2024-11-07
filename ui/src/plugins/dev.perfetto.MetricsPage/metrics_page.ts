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

import m from 'mithril';
import {
  error,
  isError,
  isPending,
  pending,
  Result,
  success,
} from '../../base/result';
import {MetricVisualisation} from '../../public/plugin';
import {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {VegaView} from '../../widgets/vega_view';
import {PageWithTraceAttrs} from '../../public/page';
import {assertExists} from '../../base/logging';
import {Trace} from '../../public/trace';

type Format = 'json' | 'prototext' | 'proto';
const FORMATS: Format[] = ['json', 'prototext', 'proto'];

async function getMetrics(engine: Engine): Promise<string[]> {
  const metrics: string[] = [];
  const metricsResult = await engine.query('select name from trace_metrics');
  for (const it = metricsResult.iter({name: STR}); it.valid(); it.next()) {
    metrics.push(it.name);
  }
  return metrics;
}

async function getMetric(
  engine: Engine,
  metric: string,
  format: Format,
): Promise<string> {
  const result = await engine.computeMetric([metric], format);
  if (result instanceof Uint8Array) {
    return `Uint8Array<len=${result.length}>`;
  } else {
    return result;
  }
}

class MetricsController {
  private readonly trace: Trace;
  private readonly engine: Engine;
  private _metrics: string[];
  private _selected?: string;
  private _result: Result<string>;
  private _format: Format;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private _json: any;

  constructor(trace: Trace) {
    this.trace = trace;
    this.engine = trace.engine.getProxy('MetricsPage');
    this._metrics = [];
    this._result = success('');
    this._json = {};
    this._format = 'json';
    getMetrics(this.engine).then((metrics) => {
      this._metrics = metrics;
    });
  }

  get metrics(): string[] {
    return this._metrics;
  }

  get visualisations(): MetricVisualisation[] {
    return this.trace.plugins
      .metricVisualisations()
      .filter((v) => v.metric === this.selected);
  }

  set selected(metric: string | undefined) {
    if (this._selected === metric) {
      return;
    }
    this._selected = metric;
    this.update();
  }

  get selected(): string | undefined {
    return this._selected;
  }

  set format(format: Format) {
    if (this._format === format) {
      return;
    }
    this._format = format;
    this.update();
  }

  get format(): Format {
    return this._format;
  }

  get result(): Result<string> {
    return this._result;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  get resultAsJson(): any {
    return this._json;
  }

  private update() {
    const selected = this._selected;
    const format = this._format;
    if (selected === undefined) {
      this._result = success('');
      this._json = {};
    } else {
      this._result = pending();
      this._json = {};
      getMetric(this.engine, selected, format)
        .then((result) => {
          if (this._selected === selected && this._format === format) {
            this._result = success(result);
            if (format === 'json') {
              this._json = JSON.parse(result);
            }
          }
        })
        .catch((e) => {
          if (this._selected === selected && this._format === format) {
            this._result = error(e);
            this._json = {};
          }
        })
        .finally(() => {
          this.trace.scheduleFullRedraw();
        });
    }
    this.trace.scheduleFullRedraw();
  }
}

interface MetricResultAttrs {
  result: Result<string>;
}

class MetricResultView implements m.ClassComponent<MetricResultAttrs> {
  view({attrs}: m.CVnode<MetricResultAttrs>) {
    const result = attrs.result;
    if (isPending(result)) {
      return m(Spinner);
    }

    if (isError(result)) {
      return m('pre.metric-error', result.error);
    }

    return m('pre', result.data);
  }
}

interface MetricPickerAttrs {
  controller: MetricsController;
}

class MetricPicker implements m.ClassComponent<MetricPickerAttrs> {
  view({attrs}: m.CVnode<MetricPickerAttrs>) {
    const {controller} = attrs;
    return m(
      '.metrics-page-picker',
      m(
        Select,
        {
          value: controller.selected,
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.selected = (e.target as HTMLSelectElement).value;
          },
        },
        controller.metrics.map((metric) =>
          m(
            'option',
            {
              value: metric,
              key: metric,
            },
            metric,
          ),
        ),
      ),
      m(
        Select,
        {
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.format = (e.target as HTMLSelectElement).value as Format;
          },
        },
        FORMATS.map((f) => {
          return m('option', {
            selected: controller.format === f,
            key: f,
            value: f,
            label: f,
          });
        }),
      ),
    );
  }
}

interface MetricVizViewAttrs {
  visualisation: MetricVisualisation;
  data: unknown;
}

class MetricVizView implements m.ClassComponent<MetricVizViewAttrs> {
  view({attrs}: m.CVnode<MetricVizViewAttrs>) {
    return m(
      '',
      m(VegaView, {
        spec: attrs.visualisation.spec,
        data: {
          metric: attrs.data,
        },
      }),
    );
  }
}

export class MetricsPage implements m.ClassComponent<PageWithTraceAttrs> {
  private controller?: MetricsController;

  oninit({attrs}: m.Vnode<PageWithTraceAttrs>) {
    this.controller = new MetricsController(attrs.trace);
  }

  view() {
    const controller = assertExists(this.controller);
    const json = controller.resultAsJson;
    return m(
      '.metrics-page',
      m(MetricPicker, {
        controller,
      }),
      controller.format === 'json' &&
        controller.visualisations.map((visualisation) => {
          let data = json;
          for (const p of visualisation.path) {
            data = data[p] ?? [];
          }
          return m(MetricVizView, {visualisation, data});
        }),
      m(MetricResultView, {result: controller.result}),
    );
  }
}
