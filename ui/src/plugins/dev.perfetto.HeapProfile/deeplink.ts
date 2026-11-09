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

import {ProfileType} from './common';

export interface HeapProfileDeeplinkArgs {
  readonly process?: string;
  readonly type?: string;
  readonly metric?: string;
}

export function resolveFlamegraphMetricName(
  profileType: ProfileType,
  metric: string,
): string | undefined {
  switch (profileType) {
    case ProfileType.NATIVE_HEAP_PROFILE:
      switch (metric) {
        case 'unreleased_size':
          return 'Unreleased Malloc Size';
        case 'unreleased_count':
          return 'Unreleased Malloc Count';
        case 'total_size':
          return 'Total Malloc Size';
        case 'total_count':
          return 'Total Malloc Count';
        default:
          return undefined;
      }
    case ProfileType.GENERIC_HEAP_PROFILE:
      switch (metric) {
        case 'unreleased_size':
          return 'Unreleased Size';
        case 'unreleased_count':
          return 'Unreleased Count';
        case 'total_size':
          return 'Total Size';
        case 'total_count':
          return 'Total Count';
        default:
          return undefined;
      }
    case ProfileType.JAVA_HEAP_SAMPLES:
      switch (metric) {
        case 'total_size':
          return 'Total Allocation Size';
        case 'total_count':
          return 'Total Allocation Count';
        default:
          return undefined;
      }
    default:
      return undefined;
  }
}

export function parseHeapProfileDeeplinkArgs(
  args: Record<string, unknown>,
): HeapProfileDeeplinkArgs | undefined {
  const process =
    typeof args.process === 'string' && args.process !== ''
      ? args.process
      : undefined;
  const rawType =
    typeof args.type === 'string' && args.type !== '' ? args.type : undefined;
  const type =
    rawType === undefined
      ? undefined
      : rawType.startsWith('heap_profile:')
        ? rawType
        : `heap_profile:${rawType}`;
  const metric =
    typeof args.metric === 'string' && args.metric !== ''
      ? args.metric
      : undefined;

  if (process === undefined && type === undefined && metric === undefined) {
    return undefined;
  }
  return {
    ...(process !== undefined && {process}),
    ...(type !== undefined && {type}),
    ...(metric !== undefined && {metric}),
  };
}
