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

import type {Engine} from '../../trace_processor/engine';

// A chunk of PerfettoSQL (tables computed once per trace, macros) defining
// part of what the stitched flamegraph runs on.
export interface StitchedSqlModule {
  readonly name: string;
  readonly sql: string;
  // Modules which must run before this one.
  readonly deps?: ReadonlyArray<StitchedSqlModule>;
}

// Runs stitched SQL modules lazily, at most once per trace: nothing runs at
// trace load, only when a stitched flamegraph first needs a module.
export class StitchedSqlModules {
  private readonly runs = new Map<string, Promise<unknown>>();

  constructor(private readonly engine: Engine) {}

  async ensure(modules: ReadonlyArray<StitchedSqlModule>): Promise<void> {
    for (const module of modules) {
      await this.ensure(module.deps ?? []);
      let run = this.runs.get(module.name);
      if (run === undefined) {
        run = this.engine.query(module.sql);
        // Let a failed run be retried rather than cache the failure.
        run.catch(() => this.runs.delete(module.name));
        this.runs.set(module.name, run);
      }
      await run;
    }
  }
}
