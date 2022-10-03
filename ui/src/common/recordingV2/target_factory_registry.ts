// Copyright (C) 2022 The Android Open Source Project
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

import {Registry} from '../registry';

import {RecordingTargetV2, TargetFactory} from './recording_interfaces_v2';

export class TargetFactoryRegistry extends Registry<TargetFactory> {
  listTargets(): RecordingTargetV2[] {
    const targets: RecordingTargetV2[] = [];
    for (const factory of this.registry.values()) {
      for (const target of factory.listTargets()) {
        targets.push(target);
      }
    }
    return targets;
  }

  listTargetFactories(): TargetFactory[] {
    return Array.from(this.registry.values());
  }

  listRecordingProblems(): string[] {
    const recordingProblems: string[] = [];
    for (const factory of this.registry.values()) {
      for (const recordingProblem of factory.listRecordingProblems()) {
        recordingProblems.push(recordingProblem);
      }
    }
    return recordingProblems;
  }
}

export const targetFactoryRegistry = new TargetFactoryRegistry((f) => {
  return f.kind;
});
