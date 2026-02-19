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

export enum ProfileType {
  // malloc hook
  NATIVE_HEAP_PROFILE,
  // art allocations profiler
  JAVA_HEAP_SAMPLES,
  // heap dump ART plugin
  JAVA_HEAP_GRAPH,
  // Catch-all renderer for custom API implementations
  GENERIC_HEAP_PROFILE,
}

export interface ProfileDescriptor {
  type: ProfileType;
  label: string;
  // Not present for java heap graphs
  heapName?: string;
}

export function profileDescriptor(type: string): ProfileDescriptor {
  if (type === 'java_heap_graph') {
    return {
      type: ProfileType.JAVA_HEAP_GRAPH,
      label: 'Java heap dump',
    };
  }
  // libc.malloc heap_name introduced in aosp/1428871 (Sep 2020)
  if (type === 'heap_profile:libc.malloc') {
    return {
      type: ProfileType.NATIVE_HEAP_PROFILE,
      label: 'Native heap profile',
      heapName: 'libc.malloc',
    };
  }
  if (type === 'heap_profile:com.android.art') {
    return {
      type: ProfileType.JAVA_HEAP_SAMPLES,
      label: 'Java heap profile',
      heapName: 'com.android.art',
    };
  }
  if (type.startsWith('heap_profile:')) {
    const heapName = type.split(':')[1];
    return {
      type: ProfileType.GENERIC_HEAP_PROFILE,
      label: `Profile: ${heapName}`,
      heapName: heapName,
    };
  }
  throw new Error(`Unknown type ${type}`);
}
