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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import {Cpu} from './cpus';

export default class CpuPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Cpus';

  private _cpus: ReadonlyArray<Cpu> = [];

  async onTraceLoad(ctx: Trace): Promise<void> {
    this._cpus = await getCpus(ctx.engine);
  }

  get cpus(): ReadonlyArray<Cpu> {
    return this._cpus;
  }
}

// TODO(hjd): When streaming must invalidate this somehow.
async function getCpus(engine: Engine): Promise<Cpu[]> {
  const cpus: Cpu[] = [];
  const queryRes = await engine.query(
    `select ucpu, cpu, ifnull(machine_id, 0) as machine from cpu`,
  );
  for (
    const it = queryRes.iter({ucpu: NUM, cpu: NUM, machine: NUM});
    it.valid();
    it.next()
  ) {
    cpus.push(new Cpu(it.ucpu, it.cpu, it.machine));
  }
  return cpus;
}
