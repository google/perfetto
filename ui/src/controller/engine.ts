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
  abstract get traceProcessor(): TraceProcessor;

  /**
   * Send a raw SQL query to the engine.
   */
  rawQuery(args: IRawQueryArgs): Promise<RawQueryResult> {
    return this.traceProcessor.rawQuery(args);
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

  // TODO(hjd): Maybe we should cache result? But then Engine must be
  // streaming aware.
  async getTraceTimeBounds(): Promise<[number, number]> {
    const result = await this.rawQuery({
      sqlQuery: 'select max(ts) as start, min(ts) as end from sched;',
    });
    const start = +result.columns[0].longValues![0];
    const end = +result.columns[1].longValues![0];
    return [start, end];
  }
}
