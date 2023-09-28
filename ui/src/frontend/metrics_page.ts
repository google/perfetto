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
} from '../base/result';
import {EngineProxy} from '../common/engine';
import {pluginManager, PluginManager} from '../common/plugins';
import {STR} from '../common/query_result';
import {raf} from '../core/raf_scheduler';
import {MetricVisualisation} from '../public';
import {Select} from '../widgets/select';
import {Spinner} from '../widgets/spinner';

import {globals} from './globals';
import {createPage} from './pages';
import {VegaView} from './widgets/vega_view';

type Format = 'json'|'prototext'|'proto';
const FORMATS: Format[] = ['json', 'prototext', 'proto'];

function getEngine(): EngineProxy|undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) {
    return undefined;
  }
  const engine = globals.engines.get(engineId)?.getProxy('MetricsPage');
  return engine;
}

async function getMetrics(engine: EngineProxy): Promise<string[]> {
  const metrics: string[] = [];
  const metricsResult = await engine.query('select name from trace_metrics');
  for (const it = metricsResult.iter({name: STR}); it.valid(); it.next()) {
    metrics.push(it.name);
  }
  return metrics;
}

async function getMetric(
    engine: EngineProxy, metric: string, format: Format): Promise<string> {
  const result = await engine.computeMetric([metric], format);
  if (result instanceof Uint8Array) {
    return `Uint8Array<len=${result.length}>`;
  } else {
    return result;
  }
}

class MetricsController {
  engine: EngineProxy;
  plugins: PluginManager;
  private _metrics: string[];
  private _selected?: string;
  private _result: Result<string>;
  private _format: Format;
  private _json: any;

  constructor(plugins: PluginManager, engine: EngineProxy) {
    this.plugins = plugins;
    this.engine = engine;
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
    return this.plugins.metricVisualisations().filter(
        (v) => v.metric === this.selected);
  }

  set selected(metric: string|undefined) {
    if (this._selected === metric) {
      return;
    }
    this._selected = metric;
    this.update();
  }

  get selected(): string|undefined {
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
            raf.scheduleFullRedraw();
          });
    }
    raf.scheduleFullRedraw();
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
        m(Select,
          {
            value: controller.selected,
            oninput: (e: Event) => {
              if (!e.target) return;
              controller.selected = (e.target as HTMLSelectElement).value;
            },
          },
          controller.metrics.map(
              (metric) =>
                  m('option',
                    {
                      value: metric,
                      key: metric,
                    },
                    metric))),
        m(
            Select,
            {
              oninput: (e: Event) => {
                if (!e.target) return;
                controller.format =
                    (e.target as HTMLSelectElement).value as Format;
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
  data: any;
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
};

class MetricPageContents implements m.ClassComponent {
  controller?: MetricsController;

  oncreate() {
    const engine = getEngine();
    if (engine !== undefined) {
      this.controller = new MetricsController(pluginManager, engine);
    }
  }

  view() {
    const controller = this.controller;
    if (controller === undefined) {
      return m('');
    }

    const json = controller.resultAsJson;

    return [
      m(MetricPicker, {
        controller,
      }),
      (controller.format === 'json') &&
          controller.visualisations.map((visualisation) => {
            let data = json;
            for (const p of visualisation.path) {
              data = data[p] ?? [];
            }
            return m(MetricVizView, {visualisation, data});
          }),
      m(MetricResultView, {result: controller.result}),
    ];
  }
}

export const MetricsPage = createPage({
  view() {
    return m('.metrics-page', m(MetricPageContents));
  },
});
