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

import m from 'mithril';

export function panel(
  title: string,
  subtitle: string | undefined,
  body: m.Children,
): m.Children {
  return m(
    '.pf-memento-panel',
    m(
      '.pf-memento-panel__header',
      m('h2', title),
      subtitle !== undefined && m('p', subtitle),
    ),
    m('.pf-memento-panel__body', body),
  );
}

export function formatKb(kb: number): string {
  if (kb < 1024) return `${kb.toLocaleString()} KB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / (1024 * 1024)).toFixed(1)} GB`;
}
