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

import {nodeRegistry} from './node_registry';
import {TestNode} from './nodes/dev/test_node';

export function registerDevNodes() {
  nodeRegistry.register('test_source', {
    name: 'Test Source',
    description: 'A source for testing purposes.',
    icon: 'bug_report',
    type: 'source',
    factory: (state) => new TestNode(state),
    devOnly: true,
  });
}
