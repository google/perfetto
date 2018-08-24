// Copyright (C) 2018 The Android Open Source Project
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

import {IRawQueryArgs, RawQueryResult, TraceProcessor} from '../common/protos';
import {TimeSpan} from '../common/time';

/**
 * Abstract interface of a trace proccessor.
 * This class is wrapper for multiple proto services defined in:
 * //protos/perfetto/trace_processor/*
 * For each service ("FooService") Engine will have abstract getter
 * ("fooService") which returns a protobufjs rpc.Service object for
 * the given service.
 *
 * Engine also defines helpers for the most common service methods
 * (e.g. rawQuery).
 */
export abstract class Engine {
  abstract readonly id: string;

  /**
   * Push trace data into the engine. The engine is supposed to automatically
   * figure out the type of the trace (JSON vs Protobuf).
   */
  abstract parse(data: Uint8Array): void;

  /*
   * The RCP interface to call service methods defined in trace_processor.proto.
   */
  abstract get rpc(): TraceProcessor;

  /**
   * Send a raw SQL query to the engine.
   */
  rawQuery(args: IRawQueryArgs): Promise<RawQueryResult> {
    return this.rpc.rawQuery(args);
  }

  async rawQueryOneRow(sqlQuery: string): Promise<number[]> {
    const result = await this.rawQuery({sqlQuery});
    const res: number[] = [];
    result.columns.map(c => res.push(+c.longValues![0]));
    return res;
  }

  // TODO(hjd): Maybe we should cache result? But then Engine must be
  // streaming aware.
  async getNumberOfCpus(): Promise<number> {
    const result = await this.rawQuery({
      sqlQuery: 'select count(distinct(cpu)) as cpuCount from sched;',
    });
    return +result.columns[0].longValues![0];
  }

  // TODO: This should live in code that's more specific to chrome, instead of
  // in engine.
  async getNumberOfProcesses(): Promise<number> {
    const result = await this.rawQuery({
      sqlQuery: 'select count(distinct(upid)) from thread;',
    });
    return +result.columns[0].longValues![0];
  }

  async getTraceTimeBounds(): Promise<TimeSpan> {
    const maxQuery = 'select max(ts) from (select max(ts) as ts from sched ' +
        'union all select max(ts) as ts from slices)';
    const minQuery = 'select min(ts) from (select min(ts) as ts from sched ' +
        'union all select min(ts) as ts from slices)';
    const start = (await this.rawQueryOneRow(minQuery))[0];
    const end = (await this.rawQueryOneRow(maxQuery))[0];
    return new TimeSpan(start / 1e9, end / 1e9);
  }
}

export interface EnginePortAndId {
  id: string;
  port: MessagePort;
}