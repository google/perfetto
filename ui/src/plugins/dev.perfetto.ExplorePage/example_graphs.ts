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

export interface ExampleGraph {
  name: string;
  description: string;
  jsonPath: string;
}

export const EXAMPLE_GRAPHS: ExampleGraph[] = [
  {
    name: 'Learning',
    description:
      'Interactive tutorial covering node docking, filtering, adding nodes, and multi-child workflows',
    jsonPath: 'assets/explore_page/examples/learning.json',
  },
  {
    name: 'Slice Analysis Pipeline',
    description:
      'Example data analysis of finding the total duration of specific process slices when any CPU was active ',
    jsonPath: 'assets/explore_page/examples/slices_example.json',
  },
];
