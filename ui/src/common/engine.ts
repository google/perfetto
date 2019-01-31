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

import {RawQueryResult, TraceProcessor} from './protos';
import {TimeSpan} from './time';

/**
 * Abstract interface of a trace proccessor.
 * This class is wrapper for multiple proto services defined in:
 * //protos/perfetto/trace_processor/*
 * For each service ("FooService") Engine will have abstract getter
 * ("fooService") which returns a protobufjs rpc.Service object for
 * the given service.
 *
 * Engine also defines helpers for the most common service methods
 * (e.g. query).
 */
export abstract class Engine {
  abstract readonly id: string;

  /**
   * Push trace data into the engine. The engine is supposed to automatically
   * figure out the type of the trace (JSON vs Protobuf).
   */
  abstract parse(data: Uint8Array): void;

  /**
   * Notify the engine no more data is coming.
   */
  abstract notifyEof(): void;

  /*
   * The RCP interface to call service methods defined in trace_processor.proto.
   */
  abstract get rpc(): TraceProcessor;

  /**
   * Shorthand for sending a SQL query to the engine.
   * Exactly the same as engine.rpc.rawQuery({rawQuery});
   */
  query(sqlQuery: string): Promise<RawQueryResult> {
    const timeQueuedNs = Math.floor(performance.now() * 1e6);
    return this.rpc.rawQuery({sqlQuery, timeQueuedNs});
  }

  async queryOneRow(query: string): Promise<number[]> {
    const result = await this.query(query);
    const res: number[] = [];
    result.columns.map(c => res.push(+c.longValues![0]));
    return res;
  }

  // TODO(hjd): Maybe we should cache result? But then Engine must be
  // streaming aware.
  async getNumberOfCpus(): Promise<number> {
    const result =
        await this.query('select count(distinct(cpu)) as cpuCount from sched;');
    return +result.columns[0].longValues![0];
  }

  // TODO: This should live in code that's more specific to chrome, instead of
  // in engine.
  async getNumberOfProcesses(): Promise<number> {
    const result = await this.query('select count(*) from process;');
    return +result.columns[0].longValues![0];
  }

  async getTraceTimeBounds(): Promise<TimeSpan> {
    const query = `select start_ts, end_ts from trace_bounds`;
    const res = (await this.queryOneRow(query));
    return new TimeSpan(res[0] / 1e9, res[1] / 1e9);
  }
}
