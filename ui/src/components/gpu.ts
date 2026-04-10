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

import {maybeMachineLabel} from '../public/utils';

const MAX_GPUS_PER_MACHINE = 256;

export class Gpu {
  constructor(
    readonly ugpu: number,
    readonly gpu: number,
    readonly machine: number,
    readonly name?: string,
  ) {}

  get displayName(): string {
    return this.name ? this.name : `GPU ${this.gpu}`;
  }

  public maybeMachineLabel(): string {
    return maybeMachineLabel(this.machine);
  }

  // Sort order for deterministic track ordering: machine first (unbounded),
  // then gpu_id within a machine (small, bounded).
  get sortOrder(): number {
    return this.machine * MAX_GPUS_PER_MACHINE + this.gpu;
  }

  public toString(): string {
    return `${this.gpu}${this.maybeMachineLabel()}`;
  }
}
