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

import {createTerminology, registerTerminology} from './index';

const openclTerminology = createTerminology('OpenCL', {
  gpu: {
    name: 'GPU',
    plural: 'GPUs',
    title: 'GPU',
    pluralTitle: 'GPUs',
  },
  thread: {
    name: 'work-item',
    plural: 'work-items',
    title: 'Work-Item',
    pluralTitle: 'Work-Items',
  },
  warp: {
    name: 'subgroup',
    plural: 'subgroups',
    title: 'Subgroup',
    pluralTitle: 'Subgroups',
  },
  block: {
    name: 'work-group',
    plural: 'work-groups',
    title: 'Work-Group',
    pluralTitle: 'Work-Groups',
  },
  grid: {
    name: 'NDRange',
    plural: 'NDRanges',
    title: 'NDRange',
    pluralTitle: 'NDRanges',
  },
  sm: {
    name: 'CU',
    plural: 'CUs',
    title: 'CU',
    pluralTitle: 'CUs',
  },
  streamingMultiprocessor: {
    name: 'compute unit',
    plural: 'compute units',
    title: 'Compute Unit',
    pluralTitle: 'Compute Units',
  },
  sharedMem: {
    name: 'local memory',
    plural: 'local memory',
    title: 'Local Memory',
    pluralTitle: 'Local Memory',
  },
  tensor: {
    name: 'matmul',
    plural: 'matmuls',
    title: 'Matmul',
    pluralTitle: 'Matmuls',
  },
});

export function registerOpenCLTerminology(): void {
  registerTerminology('opencl', openclTerminology);
}
