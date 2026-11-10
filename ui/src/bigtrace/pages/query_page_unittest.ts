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

import {expect, test} from 'vitest';
import m from 'mithril';
import {QueryPage, queryRightSidebarToggleFn} from './query_page';

test('toggling the history sidebar keeps the corpus page mounted', () => {
  localStorage.clear();
  const root = document.createElement('div');
  document.body.appendChild(root);
  m.mount(root, {view: () => m(QueryPage)});
  const launcher = root.querySelector('.pf-bt-launcher');
  expect(launcher).not.toBeNull();

  const toggle = () => {
    queryRightSidebarToggleFn?.();
    m.redraw.sync();
  };
  toggle();
  expect(root.querySelector('.pf-query-history')).toBeNull();
  // Same node: the component was kept, so it didn't refetch.
  expect(root.querySelector('.pf-bt-launcher')).toBe(launcher);
  toggle();
  expect(root.querySelector('.pf-query-history')).not.toBeNull();
  expect(root.querySelector('.pf-bt-launcher')).toBe(launcher);

  m.mount(root, null);
  root.remove();
});
