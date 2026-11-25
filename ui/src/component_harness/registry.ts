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

import type m from 'mithril';

// String args parsed from the harness page's query string.
export type FixtureArgs = Readonly<Record<string, string>>;

// A fixture returns the mithril component to mount, given the args.
export type Fixture = (args: FixtureArgs) => m.Component;

// What a *.fixture.ts file default-exports: its fixtures, keyed by a short
// name. The full fixture name is the file's path (relative to ui/src, minus
// the .fixture.ts suffix) followed by this key, e.g.
// widgets/popup.fixture.ts {scroll_container} ->
// 'widgets/popup/scroll_container'.
export type FixtureModule = Readonly<Record<string, Fixture>>;

// Shape of the globals the harness exposes to Playwright.
export interface HarnessWindow {
  // Events recorded by fixtures via record(), in order.
  __events: unknown[];
  // Set to true once the fixture has been mounted and first rendered.
  __harnessReady: boolean;
  // Set when the requested fixture could not be found.
  __harnessError?: string;
}

// Builds the name -> fixture map from the result of an import.meta.glob() of
// *.fixture.ts files. Glob keys are paths relative to the harness dir, e.g.
// '../widgets/popup.fixture.ts'.
export function collectFixtures(
  modules: Record<string, unknown>,
): Map<string, Fixture> {
  const fixtures = new Map<string, Fixture>();
  for (const path of Object.keys(modules).sort()) {
    const prefix = path.replace(/^\.\.\//, '').replace(/\.fixture\.ts$/, '');
    const module = modules[path] as FixtureModule;
    for (const [key, fixture] of Object.entries(module)) {
      fixtures.set(`${prefix}/${key}`, fixture);
    }
  }
  return fixtures;
}

// Records an event (e.g. a callback invocation) so tests can assert on it.
export function record(event: unknown) {
  (window as unknown as HarnessWindow).__events.push(event);
}
