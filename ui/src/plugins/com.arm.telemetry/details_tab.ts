// Copyright (C) 2025 The Android Open Source Project
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

import {AsyncLimiter} from '../../base/async_limiter';
import m from 'mithril';
import {Spinner} from '../../widgets/spinner';
import {time} from '../../base/time';
import {VegaView} from '../../components/widgets/vega_view';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {Section} from '../../widgets/section';
import {EngineProxy} from '../../trace_processor/engine';

export type ArmTelemetryDetailsTabConfigTableDesc = {
  query: string;
  name: string;
  cpu: number;
  metric: string;
};

export type ArmTelemetryDetailsTabConfig = {
  tables: ArmTelemetryDetailsTabConfigTableDesc[];
  start: time;
  end: time;
};

type TabData = {
  stacked: {
    [keyof: string]: {
      // stack name
      [keyof: number]: {
        // cpu index
        metrics: string[];
        spec: string;
        data: {};
      };
    };
  };
};

export class ArmTelemetryDetailsTab {
  private readonly queryLimiter = new AsyncLimiter();
  private initialized: boolean = false;
  private data: TabData | undefined;

  constructor(
    private readonly engine: EngineProxy,
    private readonly config: ArmTelemetryDetailsTabConfig,
  ) {}

  render() {
    if (!this.initialized) {
      this.queryLimiter.schedule(async () => {
        this.data = undefined;
        await this.fetchData();
      });
      this.initialized = true;
    }

    if (this.data === undefined) {
      return m(Spinner);
    }
    return m(
      'div',
      Object.entries(this.data.stacked).map(([name, cpus]) => {
        return Object.entries(cpus).map(([cpu, desc]) => {
          return m(
            Section,
            {title: `${name} - cpu ${cpu}`},
            m(VegaView, {
              spec: desc.spec,
              engine: this.engine,
              data: desc.data,
            }),
          );
        });
      }),
    );
  }

  async fetchData() {
    const data: TabData = {
      stacked: {},
    };

    const addStack = (stackName: string, metrics: string[]) => {
      for (const table of this.config.tables) {
        if (metrics.includes(table.metric)) {
          if (data.stacked[stackName] === undefined) {
            data.stacked[stackName] = {};
          }
          if (data.stacked[stackName][table.cpu] === undefined) {
            data.stacked[stackName][table.cpu] = {
              metrics: [],
              data: {},
              spec: '',
            };
          }
          data.stacked[stackName][table.cpu].metrics.push(table.metric);
        }
      }
    };

    // Hardcoded stacks
    // FIXME: Should be deduced from the methodology data when applicable
    addStack('slots_usage', [
      'spec_slot',
      'frontend_bound_unknown_slot',
      'frontend_flow_bound_slot',
      'frontend_flush_bound_slot',
      'frontend_core_unknown_bound_slot',
      'frontend_mem_tlb_bound_slot',
      'frontend_cache_l1i_bound_slot',
      'frontend_cache_l2i_bound_slot',
      'frontend_mem_bound_unknown_slot',
      'backend_bound_unknown_slot',
      'backend_core_rename_bound_slot',
      'backend_core_unknown_bound_slot',
      'backend_mem_store_bound_slot',
      'backend_mem_tlb_bound_slot',
      'backend_cache_l1d_bound_slot',
      'backend_cache_l2d_bound_slot',
    ]);

    addStack('top_down', [
      'retiring',
      'frontend_bound',
      'backend_bound',
      'bad_speculation',
    ]);

    addStack('frontend_bound', ['frontend_core_bound', 'frontend_mem_bound']);

    addStack('frontend_core_bound', [
      'frontend_core_flush_bound',
      'frontend_core_flow_bound',
    ]);

    addStack('frontend_mem_bound', [
      'frontend_mem_cache_bound',
      'frontend_mem_tlb_bound',
    ]);

    addStack('frontend_mem_cache_bound', [
      'frontend_cache_l1i_bound',
      'frontend_cache_l2i_bound',
    ]);

    addStack('backend_bound', ['backend_core_bound', 'backend_mem_bound']);

    addStack('backend_core_bound', ['backend_core_rename_bound']);

    addStack('backend_mem_bound', [
      'backend_mem_cache_bound',
      'backend_mem_tlb_bound',
      'backend_mem_store_bound',
    ]);

    addStack('backend_mem_cache_bound', [
      'backend_cache_l1d_bound',
      'backend_cache_l2d_bound',
    ]);

    // Apply "stack" on each individual metric
    // FIXME: Create dedicated vizualization spec for individual metrics
    this.config.tables.forEach((table) => {
      if (data.stacked[table.metric] === undefined) {
        addStack(table.metric, [table.metric]);
      }
    });

    for (const [_name, cpus] of Object.entries(data.stacked)) {
      for (const [cpu, desc] of Object.entries(cpus)) {
        desc.spec = this.makeVegaSpec(desc.metrics);
        desc.data = {
          metrics: await this.getVegaMetricsData(Number(cpu), desc.metrics),
          threads: await this.getVegaThreadSummaryData(
            Number(cpu),
            desc.metrics,
          ),
        };
      }
    }

    this.data = data;
  }

  makeVegaSpec(tables: string[]) {
    const spec = {
      $schema: 'https://vega.github.io/schema/vega-lite/v5.json',
      padding: 5,
      params: [
        {
          name: 'minProcessDur',
          value: 1,
          bind: {
            input: 'range',
            min: 0,
            max: 100,
          },
        },
      ],
      vconcat: [
        {
          width: 1125,
          height: tables.length > 4 ? 400 : 200,
          data: {
            name: 'metrics',
          },
          transform: [
            {
              fold: tables,
              as: ['category', 'value'],
            },
          ],
          layer: [
            {
              mark: {type: 'area', interpolate: 'step-after'},
              encoding: {
                x: {field: 'ts', type: 'quantitative', title: 'time (ms)'},
                y: {
                  field: 'value',
                  type: 'quantitative',
                  stack: 'zero',
                  title: '%',
                },
                color: {
                  field: 'category',
                  type: 'nominal',
                  title: 'Metric',
                  scale: {
                    scheme: tables.length > 10 ? 'tableau20' : 'tableau10',
                  },
                },
              },
              selection: {
                zoom_x: {type: 'interval', bind: 'scales', encodings: ['x']},
              },
            },
            {
              mark: {type: 'rule', color: 'black'},
              encoding: {
                x: {field: 'ts', type: 'quantitative', title: 'time (ms)'},
                opacity: {
                  condition: {value: 0.1, param: 'hover', empty: false},
                  value: 0,
                },
                tooltip: tables.reduce<
                  [{field: string; type: string; title: string}]
                >(
                  (acc, table) => {
                    acc.push({
                      field: table,
                      type: 'quantitative',
                      title: table,
                    });
                    return acc;
                  },
                  [{field: 'ts', type: 'quantitative', title: 'time (ms)'}],
                ),
              },
              params: [
                {
                  name: 'hover',
                  select: {
                    type: 'point',
                    fields: ['ts'],
                    nearest: true,
                    on: 'pointermove',
                    clear: 'pointerout',
                  },
                },
              ],
            },
          ],
        },
        {
          data: {
            name: 'threads',
          },
          transform: [
            {
              calculate:
                "datum.process_name === null ? 'idle' : datum.process_name",
              as: 'process_name',
            },
            {
              joinaggregate: [
                {
                  op: 'sum',
                  field: 'dur',
                  as: 'process_dur',
                },
              ],
              groupby: ['process_name'],
            },
            {
              filter: 'datum.process_dur > minProcessDur',
            },
          ],
          hconcat: [
            {
              width: 500,
              transform: [
                {
                  fold: tables,
                  as: ['metric_name', 'raw_metric'],
                },
                {
                  calculate:
                    '(datum.raw_metric * datum.dur) / datum.process_dur',
                  as: 'metric',
                },
                {
                  calculate: tables.reduce<string>((acc, current, idx) => {
                    // writing from right to left.
                    // If the value is not found, return null
                    return (
                      `datum.metric_name === '${current}' ? ${idx} : ` + acc
                    );
                  }, 'null'),
                  as: 'metricOrder',
                },
              ],
              encoding: {
                y: {
                  field: 'process_name',
                  type: 'nominal',
                  sort: {
                    field: 'dur',
                    op: 'sum',
                    order: 'descending',
                  },
                  title: 'Process',
                },
              },
              layer: [
                {
                  mark: {
                    type: 'bar',
                  },
                  encoding: {
                    x: {
                      aggregate: 'sum',
                      field: 'metric',
                      type: 'quantitative',
                      title: '%',
                      stack: 'zero',
                    },
                    color: {
                      field: 'metric_name',
                      type: 'nominal',
                      title: 'metric',
                      scale: {
                        scheme: tables.length > 10 ? 'tableau20' : 'tableau10',
                      },
                    },
                    order: {
                      field: 'metricOrder',
                    },
                    tooltip: [
                      {title: 'Process', field: 'process_name'},
                      {title: 'Metric', field: 'metric_name'},
                      {title: 'Value', field: 'metric', aggregate: 'sum'},
                    ],
                  },
                },
                {
                  mark: {type: 'text', opacity: 0.9, color: 'white'},
                  encoding: {
                    x: {
                      aggregate: 'sum',
                      field: 'metric',
                      type: 'quantitative',
                      title: '%',
                      stack: 'zero',
                      bandPosition: 0.5,
                    },
                    text: {
                      aggregate: 'sum',
                      field: 'metric',
                      type: 'quantitative',
                      bandPosition: 0,
                      format: '.1f',
                    },
                    detail: {
                      field: 'metric_name',
                      type: 'nominal',
                      scale: {range: ['white']},
                      legend: null,
                    },
                    order: {
                      field: 'metricOrder',
                    },
                  },
                },
              ],
            },
            {
              width: 500,
              mark: {type: 'bar'},
              encoding: {
                y: {
                  field: 'process_name',
                  title: 'Process',
                  type: 'nominal',
                  sort: {
                    field: 'dur',
                    op: 'sum',
                    order: 'descending',
                  },
                  axis: null,
                },
                x: {
                  field: 'dur',
                  type: 'quantitative',
                  scale: {type: 'linear'},
                  title: 'Duration (ms)',
                },
                color: {
                  field: 'thread_name',
                  title: 'Thread',
                  scale: {scheme: 'category20b'},
                },
                order: {field: 'dur', sort: 'descending'},
                tooltip: [
                  {title: 'Process', field: 'process_name'},
                  {title: 'Thread', field: 'thread_name'},
                  {title: 'Duration (ms)', field: 'dur'},
                ],
              },
            },
          ],
          resolve: {scale: {color: 'independent', x: 'independent'}},
        },
      ],
      resolve: {
        scale: {color: 'independent', x: 'independent', size: 'independent'},
      },
    };

    return JSON.stringify(spec);
  }

  async getVegaMetricsData(cpu: number, tables: string[]) {
    const qPromise = this.engine.query(`
      SELECT
        (${tables[0]}.ts - ${this.config.start}) / 1000000.0 AS ts,
        ${tables.slice(1).reduce<string>((acc, table) => {
          return acc + `${table}.value AS ${table},`;
        }, '')}
        ${tables[0]}.value as ${tables[0]}
      FROM ${tables[0]}
        ${tables.slice(1).reduce<string>((acc, table) => {
          return (
            acc + `LEFT JOIN ${table} ON ${table}.ROWID = ${tables[0]}.ROWID `
          );
        }, '')}
      WHERE ${tables[0]}.cpu=${cpu} AND
        ${tables[0]}.ts >= ${this.config.start} AND
        ${tables[0]}.ts <= ${this.config.end}
      ORDER BY ts;
    `);

    const rowSpec = tables.reduce<{[keyof: string]: number}>(
      (acc, current) => {
        acc[current] = NUM;
        return acc;
      },
      {ts: NUM},
    );

    const q = await qPromise;
    const result = new Array(q.numRows());
    for (let it = q.iter(rowSpec), row = 0; it.valid(); it.next(), row++) {
      result[row] = tables.reduce<{[keyof: string]: number}>(
        (acc, current) => {
          acc[current] = it[current];
          return acc;
        },
        {ts: it.ts},
      );
    }
    return result;
  }

  async getVegaThreadSummaryData(cpu: number, tables: string[]) {
    const qPromise = this.engine.query(`
      SELECT
        SUM(${tables[0]}.dur) / 1000000.0 AS dur,
        ${tables.reduce<string>((acc, table) => {
          return (
            acc +
            `SUM(${table}.value * ${table}.dur) / SUM(${table}.dur) AS ${table},`
          );
        }, '')}
        thread.name AS thread_name,
        process.name AS process_name
      FROM ${tables[0]}
        ${tables.slice(1).reduce<string>((acc, table) => {
          return (
            acc + `LEFT JOIN ${table} ON ${table}.ROWID = ${tables[0]}.ROWID `
          );
        }, '')}
        LEFT JOIN thread USING (utid)
        LEFT JOIN process USING (upid)
      WHERE ${tables[0]}.cpu=${cpu} AND
        ${tables[0]}.ts >= ${this.config.start} AND
        ${tables[0]}.ts <= ${this.config.end}
      GROUP BY ${tables[0]}.cpu, ${tables[0]}.utid
    `);

    const rowSpec = tables.reduce<{[keyof: string]: number | string | null}>(
      (acc, current) => {
        acc[current] = NUM;
        return acc;
      },
      {dur: NUM, thread_name: STR_NULL, process_name: STR_NULL},
    );

    const q = await qPromise;
    const result = new Array(q.numRows());
    for (let it = q.iter(rowSpec), row = 0; it.valid(); it.next(), row++) {
      result[row] = tables.reduce<{[keyof: string]: number | string | null}>(
        (acc, current) => {
          acc[current] = it[current];
          return acc;
        },
        {
          dur: it.dur,
          thread_name: it.thread_name,
          process_name: it.process_name,
        },
      );
    }
    return result;
  }

  // Filter the table in entry:
  // - retain slice in the timestamp range
  // - remove slices where the CPU is idle
  getFilteredTableQuery(tableQuery: string) {
    return `
      SELECT
        value,
        dur,
        utid
      FROM ${tableQuery}
      WHERE
        utid != 0 AND
        ts >= ${this.config.start} AND
        ts <= ${this.config.end}
    `;
  }
}
