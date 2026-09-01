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

// Hash-based router (#!/query, #!/settings). Not m.route(): it bypasses the
// raf scheduler and breaks portal-based popups.

export function getCurrentRoute(): string {
  const hash = window.location.hash;
  if (hash.startsWith('#!')) {
    const route = hash.slice(2);
    return route || '/';
  }
  return '/';
}

export function setRoute(route: string): void {
  window.location.hash = '!' + route;
}

export function initRouter(): void {
  window.addEventListener('hashchange', () => {
    m.redraw();
  });
}
