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

// Entry point of the component test harness (component_harness.html). Mounts
// a single fixture, selected via ?fixture=<name>, in isolation.

// Keep this import first.
import '../base/static_initializers';
import '../assets/typefaces.scss';
import '../assets/common.scss';
import './harness.scss';
import m from 'mithril';
import {ThemeProvider} from '../frontend/theme_provider';
import {OverlayContainer} from '../widgets/overlay_container';
import {Switch} from '../widgets/switch';
import {
  type FixtureArgs,
  type HarnessWindow,
  collectFixtures,
} from './registry';

// Every *.fixture.ts under ui/src, discovered at build time. The literal
// pattern and options are required by Vite's static analysis. Only this
// bundle (built with --enable-component-harness) contains this glob, so
// fixtures never end up in the shipped UI.
const fixtures = collectFixtures(
  import.meta.glob('../**/*.fixture.ts', {eager: true, import: 'default'}),
);

function main() {
  const harnessWindow = window as unknown as HarnessWindow;
  harnessWindow.__events = [];
  harnessWindow.__harnessReady = false;

  const params = new URLSearchParams(location.search);
  const fixtureName = params.get('fixture');
  let theme: 'light' | 'dark' =
    params.get('theme') === 'dark' ? 'dark' : 'light';
  let showGuides = params.get('guides') !== '0';
  const args: Record<string, string> = {};
  for (const [key, value] of params) {
    if (key === 'fixture' || key === 'theme' || key === 'guides') continue;
    args[key] = value;
  }

  const fixture = fixtureName === null ? undefined : fixtures.get(fixtureName);
  if (fixtureName !== null && fixture === undefined) {
    harnessWindow.__harnessError = `Unknown fixture: '${fixtureName}'`;
  }
  const component =
    typeof fixture === 'function' ? fixture(args as FixtureArgs) : undefined;
  const tree = buildTree(Array.from(fixtures.keys()));
  // Links keep the current theme and guides toggle.
  const hrefFor = (name: string) => {
    const next = new URLSearchParams({fixture: name});
    if (theme === 'dark') next.set('theme', 'dark');
    if (!showGuides) next.set('guides', '0');
    return `?${next}`;
  };
  const updateSearchParam = (key: string, value: string | undefined) => {
    const next = new URLSearchParams(location.search);
    if (value !== undefined) {
      next.set(key, value);
    } else {
      next.delete(key);
    }
    const qs = next.toString();
    history.replaceState(
      null,
      '',
      qs === '' ? location.pathname : `${location.pathname}?${qs}`,
    );
  };
  const setTheme = (nextTheme: 'light' | 'dark') => {
    theme = nextTheme;
    updateSearchParam('theme', nextTheme === 'dark' ? 'dark' : undefined);
  };
  const setShowGuides = (enabled: boolean) => {
    showGuides = enabled;
    updateSearchParam('guides', enabled ? undefined : '0');
  };

  // Called on every redraw: mithril vnodes must not be reused across renders.
  const content = (): m.Children =>
    component
      ? // harness.scss draws crop marks around the fixture's root element so
        // it's obvious where the component's edges are, without changing its
        // layout. The fixture must come first: the marks anchor to it.
        m('.harness-frame', [
          m(component),
          showGuides && [
            m('.harness-corner.harness-corner--tl'),
            m('.harness-corner.harness-corner--tr'),
            m('.harness-corner.harness-corner--bl'),
            m('.harness-corner.harness-corner--br'),
          ],
        ])
      : m(
          'p.harness-placeholder',
          fixtureName === null
            ? 'Pick a fixture on the left.'
            : `Unknown fixture: '${fixtureName}'. Pick one on the left.`,
        );

  m.mount(document.body, {
    view: () =>
      m(
        ThemeProvider,
        {theme},
        m(
          OverlayContainer,
          {fillHeight: true},
          m('.harness-layout', [
            // Tree of all fixtures, grouped by path, to help humans navigate.
            m('nav.harness-nav', [
              m(
                '.harness-nav__tree',
                renderTree(tree, (name) => ({
                  href: hrefFor(name),
                  active: name === fixtureName,
                })),
              ),
              m('.harness-nav__controls', [
                m(Switch, {
                  label: 'Dark mode',
                  checked: theme === 'dark',
                  onchange: (e: Event) =>
                    setTheme(
                      (e.target as HTMLInputElement).checked ? 'dark' : 'light',
                    ),
                }),
                m(Switch, {
                  label: 'Show guides',
                  checked: showGuides,
                  onchange: (e: Event) =>
                    setShowGuides((e.target as HTMLInputElement).checked),
                }),
              ]),
            ]),
            m('main#harness-root', content()),
          ]),
        ),
      ),
    oncreate: () => {
      harnessWindow.__harnessReady = true;
    },
  });
}

// A node in the fixture tree: either a group (a path segment) or a fixture.
interface TreeNode {
  readonly label: string;
  // The full fixture name, for leaves.
  readonly fixture?: string;
  readonly children: TreeNode[];
}

// Builds a tree from fixture names, splitting them on '/'. Insertion order is
// kept, so siblings appear in the order the fixtures were collected.
function buildTree(names: string[]): TreeNode[] {
  const root: TreeNode = {label: '', children: []};
  for (const name of names) {
    let node = root;
    const parts = name.split('/');
    parts.forEach((part, i) => {
      const isLeaf = i === parts.length - 1;
      let child = node.children.find(
        (c) => c.label === part && (c.fixture !== undefined) === isLeaf,
      );
      if (child === undefined) {
        child = {label: part, fixture: isLeaf ? name : undefined, children: []};
        node.children.push(child);
      }
      node = child;
    });
  }
  return root.children;
}

function renderTree(
  nodes: TreeNode[],
  link: (name: string) => {href: string; active: boolean},
): m.Children {
  return m(
    'ul.harness-tree',
    nodes.map((node) => {
      if (node.fixture !== undefined) {
        const {href, active} = link(node.fixture);
        return m(
          'li',
          m(
            'a.harness-tree__leaf',
            {href, className: active ? 'harness-tree__leaf--active' : ''},
            node.label,
          ),
        );
      }
      return m('li', [
        m('.harness-tree__group', node.label),
        renderTree(node.children, link),
      ]);
    }),
  );
}

main();
