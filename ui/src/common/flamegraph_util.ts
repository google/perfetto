// Copyright (C) 2019 The Android Open Source Project
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

import {featureFlags} from '../core/feature_flags';
import {ProfileType} from './state';

export enum FlamegraphViewingOption {
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY = 'SPACE',
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY = 'ALLOC_SPACE',
  OBJECTS_ALLOCATED_NOT_FREED_KEY = 'OBJECTS',
  OBJECTS_ALLOCATED_KEY = 'ALLOC_OBJECTS',
  PERF_SAMPLES_KEY = 'PERF_SAMPLES',
  DOMINATOR_TREE_OBJ_SIZE_KEY = 'DOMINATED_OBJ_SIZE',
  DOMINATOR_TREE_OBJ_COUNT_KEY = 'DOMINATED_OBJ_COUNT',
}

interface ViewingOption {
  option: FlamegraphViewingOption;
  name: string;
}

export interface CallsiteInfo {
  id: number;
  parentId: number;
  depth: number;
  name?: string;
  totalSize: number;
  selfSize: number;
  mapping: string;
  merged: boolean;
  highlighted: boolean;
  location?: string;
}

const SHOW_HEAP_GRAPH_DOMINATOR_TREE_FLAG = featureFlags.register({
  id: 'showHeapGraphDominatorTree',
  name: 'Show heap graph dominator tree',
  description: 'Show dominated size and objects tabs in Java heap graph view.',
  defaultValue: true,
});

export function viewingOptions(profileType: ProfileType): Array<ViewingOption> {
  switch (profileType) {
    case ProfileType.PERF_SAMPLE:
      return [
        {
          option: FlamegraphViewingOption.PERF_SAMPLES_KEY,
          name: 'Samples',
        },
      ];
    case ProfileType.JAVA_HEAP_GRAPH:
      return [
        {
          option: FlamegraphViewingOption.SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY,
          name: 'Size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_NOT_FREED_KEY,
          name: 'Objects',
        },
      ].concat(
        SHOW_HEAP_GRAPH_DOMINATOR_TREE_FLAG.get()
          ? [
              {
                option: FlamegraphViewingOption.DOMINATOR_TREE_OBJ_SIZE_KEY,
                name: 'Dominated size',
              },
              {
                option: FlamegraphViewingOption.DOMINATOR_TREE_OBJ_COUNT_KEY,
                name: 'Dominated objects',
              },
            ]
          : [],
      );
    case ProfileType.HEAP_PROFILE:
      return [
        {
          option: FlamegraphViewingOption.SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY,
          name: 'Unreleased size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_NOT_FREED_KEY,
          name: 'Unreleased count',
        },
        {
          option: FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
          name: 'Total size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_KEY,
          name: 'Total count',
        },
      ];
    case ProfileType.NATIVE_HEAP_PROFILE:
      return [
        {
          option: FlamegraphViewingOption.SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY,
          name: 'Unreleased malloc size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_NOT_FREED_KEY,
          name: 'Unreleased malloc count',
        },
        {
          option: FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
          name: 'Total malloc size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_KEY,
          name: 'Total malloc count',
        },
      ];
    case ProfileType.JAVA_HEAP_SAMPLES:
      return [
        {
          option: FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
          name: 'Total allocation size',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_KEY,
          name: 'Total allocation count',
        },
      ];
    case ProfileType.MIXED_HEAP_PROFILE:
      return [
        {
          option: FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
          name: 'Total allocation size (malloc + java)',
        },
        {
          option: FlamegraphViewingOption.OBJECTS_ALLOCATED_KEY,
          name: 'Total allocation count (malloc + java)',
        },
      ];
    default:
      const exhaustiveCheck: never = profileType;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}

export function defaultViewingOption(
  profileType: ProfileType,
): FlamegraphViewingOption {
  return viewingOptions(profileType)[0].option;
}

export function expandCallsites(
  data: ReadonlyArray<CallsiteInfo>,
  clickedCallsiteIndex: number,
): ReadonlyArray<CallsiteInfo> {
  if (clickedCallsiteIndex === -1) return data;
  const expandedCallsites: CallsiteInfo[] = [];
  if (clickedCallsiteIndex >= data.length || clickedCallsiteIndex < -1) {
    return expandedCallsites;
  }
  const clickedCallsite = data[clickedCallsiteIndex];
  expandedCallsites.unshift(clickedCallsite);
  // Adding parents
  let parentId = clickedCallsite.parentId;
  while (parentId > -1) {
    expandedCallsites.unshift(data[parentId]);
    parentId = data[parentId].parentId;
  }
  // Adding children
  const parents: number[] = [];
  parents.push(clickedCallsiteIndex);
  for (let i = clickedCallsiteIndex + 1; i < data.length; i++) {
    const element = data[i];
    if (parents.includes(element.parentId)) {
      expandedCallsites.push(element);
      parents.push(element.id);
    }
  }
  return expandedCallsites;
}

// Merge callsites that have approximately width less than
// MIN_PIXEL_DISPLAYED. All small callsites in the same depth and with same
// parent will be merged to one with total size of all merged callsites.
export function mergeCallsites(
  data: ReadonlyArray<CallsiteInfo>,
  minSizeDisplayed: number,
) {
  const mergedData: CallsiteInfo[] = [];
  const mergedCallsites: Map<number, number> = new Map();
  for (let i = 0; i < data.length; i++) {
    // When a small callsite is found, it will be merged with other small
    // callsites of the same depth. So if the current callsite has already been
    // merged we can skip it.
    if (mergedCallsites.has(data[i].id)) {
      continue;
    }
    const copiedCallsite = copyCallsite(data[i]);
    copiedCallsite.parentId = getCallsitesParentHash(
      copiedCallsite,
      mergedCallsites,
    );

    let mergedAny = false;
    // If current callsite is small, find other small callsites with same depth
    // and parent and merge them into the current one, marking them as merged.
    if (copiedCallsite.totalSize <= minSizeDisplayed && i + 1 < data.length) {
      let j = i + 1;
      let nextCallsite = data[j];
      while (j < data.length && copiedCallsite.depth === nextCallsite.depth) {
        if (
          copiedCallsite.parentId ===
            getCallsitesParentHash(nextCallsite, mergedCallsites) &&
          nextCallsite.totalSize <= minSizeDisplayed
        ) {
          copiedCallsite.totalSize += nextCallsite.totalSize;
          mergedCallsites.set(nextCallsite.id, copiedCallsite.id);
          mergedAny = true;
        }
        j++;
        nextCallsite = data[j];
      }
      if (mergedAny) {
        copiedCallsite.name = '[merged]';
        copiedCallsite.merged = true;
      }
    }
    mergedData.push(copiedCallsite);
  }
  return mergedData;
}

function copyCallsite(callsite: CallsiteInfo): CallsiteInfo {
  return {
    id: callsite.id,
    parentId: callsite.parentId,
    depth: callsite.depth,
    name: callsite.name,
    totalSize: callsite.totalSize,
    mapping: callsite.mapping,
    selfSize: callsite.selfSize,
    merged: callsite.merged,
    highlighted: callsite.highlighted,
    location: callsite.location,
  };
}

function getCallsitesParentHash(
  callsite: CallsiteInfo,
  map: Map<number, number>,
): number {
  return map.has(callsite.parentId)
    ? +map.get(callsite.parentId)!
    : callsite.parentId;
}
export function findRootSize(data: ReadonlyArray<CallsiteInfo>) {
  let totalSize = 0;
  let i = 0;
  while (i < data.length && data[i].depth === 0) {
    totalSize += data[i].totalSize;
    i++;
  }
  return totalSize;
}
