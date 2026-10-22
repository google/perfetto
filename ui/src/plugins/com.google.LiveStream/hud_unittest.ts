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
import {z} from 'zod';
import type {Setting} from '../../public/settings';
import {type LiveHudProps, LiveHudState, renderLiveStreamHud} from './hud';
import type {
  BUFFER_DELAYS,
  LiveSettings,
  RETENTIONS,
  UPDATE_INTERVALS,
} from './settings';
import type {LiveStatus} from './status';
import {TARGET} from './wire';

const WORKER = TARGET.parse({
  id: 'job-1/worker',
  short_name: 'worker',
  description: 'Training job',
  task_count: 8,
  task_index: 3,
});
const EVAL = TARGET.parse({
  id: 'job-1/eval',
  short_name: 'eval',
  link: 'https://example.com/eval',
});

// A setting kept in memory.
function memorySetting<T>(defaultValue: T): Setting<T> {
  let value = defaultValue;
  return {
    id: '',
    name: '',
    description: '',
    schema: z.custom<T>(),
    defaultValue,
    get isDefault() {
      return value === defaultValue;
    },
    get: () => value,
    set: (newValue: T) => {
      value = newValue;
    },
    reset: () => {
      value = defaultValue;
    },
    [Symbol.dispose]: () => {},
  };
}

let root: HTMLElement;
let hudState: LiveHudState;
let redraws: number;
let settings: LiveSettings;

beforeEach(() => {
  root = document.createElement('div');
  document.body.append(root);
  redraws = 0;
  hudState = new LiveHudState(() => redraws++);
  settings = {
    updateInterval: memorySetting<(typeof UPDATE_INTERVALS)[number]>('0.25'),
    bufferDelay: memorySetting<(typeof BUFFER_DELAYS)[number]>('5'),
    retention: memorySetting<(typeof RETENTIONS)[number]>('60'),
  };
});

afterEach(() => {
  hudState[Symbol.dispose]();
  m.render(root, null);
  root.remove();
  vi.useRealTimers();
});

// Renders the HUD row of a live stream of WORKER, with `overrides`. draw()
// renders it again, e.g. after a click.
function hud(overrides: Partial<LiveHudProps> = {}) {
  const props: LiveHudProps = {
    layout: 'row',
    status: 'live',
    error: '',
    targets: [WORKER, EVAL],
    target: WORKER,
    following: true,
    paused: false,
    settings,
    hudState,
    onOpenTarget: vi.fn(),
    onTargetMenuOpen: vi.fn(),
    onToggleFollow: vi.fn(),
    onTogglePause: vi.fn(),
    onReconnect: vi.fn(),
    onDownload: vi.fn(),
    onDismissError: vi.fn(),
    ...overrides,
  };
  const draw = () => m.render(root, renderLiveStreamHud(props));
  draw();
  return {props, draw};
}

function find<T extends Element = HTMLElement>(selector: string): T {
  const element = root.querySelector<T>(selector);
  if (element === null) throw new Error(`No ${selector}`);
  return element;
}

function text(selector: string): string {
  return find(selector).textContent ?? '';
}

function click(selector: string) {
  find(selector).click();
}

function texts(selector: string): string[] {
  return [...root.querySelectorAll(selector)].map((e) => e.textContent ?? '');
}

// The names in the target list.
function rows(): string[] {
  return texts('.pf-live-stream-hud__target-item-short');
}

function searchInput(): HTMLInputElement {
  return find<HTMLInputElement>('.pf-live-stream-hud__target-search-input');
}

function type(value: string) {
  const input = searchInput();
  input.value = value;
  input.dispatchEvent(new Event('input', {bubbles: true}));
}

// Presses `key` on `target`, by default the search box.
function press(key: string, target: Element = searchInput()) {
  target.dispatchEvent(new KeyboardEvent('keydown', {key, bubbles: true}));
}

// The name in the row highlighted with the arrow keys.
function highlighted(): string {
  return text(
    '.pf-live-stream-hud__target-item--highlighted ' +
      '.pf-live-stream-hud__target-item-short',
  );
}

test('shows the status and the controls', () => {
  const {props} = hud();

  expect(text('.pf-live-stream-hud__badge')).toBe('LIVE STREAM');
  expect(texts('.pf-live-stream-hud__btn')).toEqual([
    'Follow Live: ON [Space]',
    'Pause',
    '⋯',
  ]);
  expect(text('.pf-live-stream-hud__target-trigger-label')).toBe('worker/3');

  const [follow, pause] = root.querySelectorAll<HTMLElement>(
    '.pf-live-stream-hud__btn',
  );
  expect(follow.classList).toContain('pf-live-stream-hud__btn--active');
  follow.click();
  pause.click();
  expect(props.onToggleFollow).toHaveBeenCalledTimes(1);
  expect(props.onTogglePause).toHaveBeenCalledTimes(1);
});

test('shows a pause independently of following', () => {
  hud({status: 'paused', paused: true, following: false});

  expect(text('.pf-live-stream-hud__badge')).toBe('PAUSED');
  expect(texts('.pf-live-stream-hud__btn')).toEqual([
    'Follow Live: OFF [Space]',
    'Resume Live',
    '⋯',
  ]);
});

test('shows the target menu once the targets load', () => {
  hud({targets: []});

  expect(root.querySelector('.pf-live-stream-hud__target-dropdown')).toBeNull();
});

test('shows the first status at once, live and paused too', () => {
  let raw: LiveStatus = 'buffering';
  const sync = () => hudState.syncStatus(() => raw);

  expect(sync()).toEqual({status: 'buffering', changed: true});
  raw = 'live';
  expect(sync()).toEqual({status: 'live', changed: true});
  raw = 'paused';
  expect(sync()).toEqual({status: 'paused', changed: true});
  expect(sync()).toEqual({status: 'paused', changed: false});
});

test('shows other statuses once they last 1 s', () => {
  vi.useFakeTimers();
  let raw: LiveStatus = 'live';
  const sync = () => hudState.syncStatus(() => raw);
  sync();

  raw = 'error';
  expect(sync()).toEqual({status: 'live', changed: false});
  vi.advanceTimersByTime(999);
  expect(redraws).toBe(0);

  vi.advanceTimersByTime(1);
  expect(redraws).toBe(1);
  expect(sync()).toEqual({status: 'error', changed: false});
});

test('drops short hiccups', () => {
  vi.useFakeTimers();
  let raw: LiveStatus = 'live';
  const sync = () => hudState.syncStatus(() => raw);
  sync();

  raw = 'buffering';
  sync();
  vi.advanceTimersByTime(500);
  raw = 'live';
  expect(sync()).toEqual({status: 'live', changed: false});
  vi.advanceTimersByTime(1_000);

  expect(redraws).toBe(0);
});

test('pops out errors until dismissed, and other errors', () => {
  const {props, draw} = hud({status: 'error', error: 'Disk full'});
  expect(text('.pf-live-stream-hud__badge')).toBe('STREAM ERROR');
  expect(text('.pf-live-stream-hud__popout-title')).toBe('Stream Error');
  expect(text('.pf-live-stream-hud__popout-msg')).toBe('Disk full');

  click('.pf-live-stream-hud__popout-close');
  draw();
  expect(props.onDismissError).toHaveBeenCalledWith('Disk full');
  expect(root.querySelector('.pf-live-stream-hud__popout')).toBeNull();

  // A click on the badge shows it again.
  click('.pf-live-stream-hud__badge');
  draw();
  expect(text('.pf-live-stream-hud__popout-msg')).toBe('Disk full');

  click('.pf-live-stream-hud__popout-close');
  hud({status: 'error', error: 'Quota exceeded'});
  expect(text('.pf-live-stream-hud__popout-msg')).toBe('Quota exceeded');
});

test('shows a dismissed popout again on Enter on the badge', () => {
  const {draw} = hud({status: 'error', error: 'Disk full'});
  const badge = find('.pf-live-stream-hud__badge');
  expect(badge.getAttribute('role')).toBe('button');
  expect(badge.tabIndex).toBe(0);
  click('.pf-live-stream-hud__popout-close');
  draw();

  // Space stays the Follow hotkey.
  const hotkeys = vi.fn();
  document.addEventListener('keydown', hotkeys);
  press(' ', badge);
  draw();
  expect(root.querySelector('.pf-live-stream-hud__popout')).toBeNull();
  expect(hotkeys).toHaveBeenCalledTimes(1);

  // Enter isn't the timeline's, e.g. for the next search result.
  press('Enter', badge);
  draw();
  expect(text('.pf-live-stream-hud__popout-msg')).toBe('Disk full');
  expect(hotkeys).toHaveBeenCalledTimes(1);
  document.removeEventListener('keydown', hotkeys);
});

test('pops out buffering until dismissed or the status changes', () => {
  hudState.syncStatus(() => 'buffering');
  const {props, draw} = hud({status: 'buffering'});
  expect(text('.pf-live-stream-hud__popout-title')).toBe('Buffering');
  expect(text('.pf-live-stream-hud__popout-msg')).toBe(
    'Buffering live stream...',
  );

  click('.pf-live-stream-hud__popout-close');
  draw();
  expect(root.querySelector('.pf-live-stream-hud__popout')).toBeNull();
  expect(props.onDismissError).not.toHaveBeenCalled();

  hudState.syncStatus(() => 'live');
  hud({status: 'buffering'});
  expect(text('.pf-live-stream-hud__popout-title')).toBe('Buffering');
});

test('opens the target menu with the open target', () => {
  const {props, draw} = hud();

  click('.pf-live-stream-hud__target-trigger');
  draw();

  // Prefilled, so that another task is a backspace away.
  const input = searchInput();
  expect(input.value).toBe('worker/3');
  expect(document.activeElement).toBe(input);
  expect(input.selectionStart).toBe('worker/3'.length);
  expect(props.onTargetMenuOpen).toHaveBeenCalledTimes(1);

  // A combobox of the target list.
  const list = find('.pf-live-stream-hud__target-list');
  expect(list.getAttribute('role')).toBe('listbox');
  expect(input.getAttribute('role')).toBe('combobox');
  expect(input.getAttribute('aria-expanded')).toBe('true');
  expect(list.id).not.toBe('');
  expect(input.getAttribute('aria-controls')).toBe(list.id);

  // All targets until edited, the open one selected.
  expect(rows()).toEqual(['worker/3', 'eval']);
  const selected = find('.pf-live-stream-hud__target-item--selected');
  expect(selected.getAttribute('aria-selected')).toBe('true');
  expect(find<HTMLAnchorElement>('a').href).toBe('https://example.com/eval');

  // Descriptions show on hover only.
  expect(selected.title).toBe('Training job');
  expect(texts('.pf-live-stream-hud__target-item-desc')).toEqual([]);
});

test('names the only task of a job "<name>/0", and opens it', () => {
  const trainer = TARGET.parse({
    id: 'job-2/trainer',
    short_name: 'trainer',
    task_count: 1,
  });
  const {props, draw} = hud({targets: [WORKER, trainer], target: trainer});
  expect(text('.pf-live-stream-hud__target-trigger-label')).toBe('trainer/0');

  click('.pf-live-stream-hud__target-trigger');
  draw();
  expect(searchInput().value).toBe('trainer/0');

  press('Enter');
  expect(props.onOpenTarget).toHaveBeenCalledWith('job-2/trainer');
});

test('filters the targets once edited, and opens a typed task', () => {
  const {props, draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();

  type('worker/5');
  draw();
  expect(rows()).toEqual(['Switch to target: worker/5', 'worker/3']);
  expect(texts('.pf-live-stream-hud__target-item-desc')).toEqual([
    '\u200EPress Enter or click to switch\u200E',
  ]);
  expect(text('.pf-live-stream-hud__target-item-count')).toBe('Total count: 8');

  press('Enter');
  expect(props.onOpenTarget).toHaveBeenCalledWith('job-1/worker/5');
  expect(hudState.isTargetMenuOpen).toBe(false);
});

test('opens nothing for unknown targets', () => {
  const {props, draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();

  type('nope');
  draw();
  expect(rows()).toEqual([]);
  const go = find<HTMLButtonElement>('.pf-live-stream-hud__target-search-go');
  expect(go.disabled).toBe(true);

  press('Enter');
  expect(props.onOpenTarget).not.toHaveBeenCalled();
  expect(hudState.isTargetMenuOpen).toBe(true);
});

test('opens a clicked target', () => {
  const {props, draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();

  root
    .querySelectorAll<HTMLElement>('.pf-live-stream-hud__target-item')[1]
    .click();

  expect(props.onOpenTarget).toHaveBeenCalledWith('job-1/eval');
  expect(hudState.isTargetMenuOpen).toBe(false);
});

test('picks a listed target with the arrow keys, and opens it on Enter', () => {
  const {props, draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();

  // From the open target, up to the last row.
  press('ArrowDown');
  press('ArrowDown');
  draw();
  expect(highlighted()).toBe('eval');
  const row = find('.pf-live-stream-hud__target-item--highlighted');
  expect(searchInput().getAttribute('aria-activedescendant')).toBe(row.id);

  press('ArrowUp');
  draw();
  expect(highlighted()).toBe('worker/3');

  press('ArrowDown');
  press('Enter');
  expect(props.onOpenTarget).toHaveBeenCalledWith('job-1/eval');
  expect(hudState.isTargetMenuOpen).toBe(false);
});

test('starts the highlight at the edges, and drops it on input', () => {
  const {props, draw} = hud({target: undefined});
  click('.pf-live-stream-hud__target-trigger');
  draw();

  press('ArrowUp');
  draw();
  expect(highlighted()).toBe('eval');

  // Enter then opens the typed target.
  type('worker/5');
  draw();
  expect(
    root.querySelector('.pf-live-stream-hud__target-item--highlighted'),
  ).toBeNull();
  expect(searchInput().hasAttribute('aria-activedescendant')).toBe(false);
  press('Enter');
  expect(props.onOpenTarget).toHaveBeenCalledWith('job-1/worker/5');
});

test('opens the typed target on Enter once a refresh drops the highlighted one', () => {
  const {draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();
  press('ArrowDown');
  draw();
  expect(highlighted()).toBe('eval');

  const {props} = hud({targets: [WORKER]});
  press('Enter');
  expect(props.onOpenTarget).toHaveBeenCalledWith('job-1/worker');
});

test('closes the menus on Escape and on clicks outside them', () => {
  hudState.listen();
  const {draw} = hud();
  click('.pf-live-stream-hud__target-trigger');
  draw();

  // Clicks in the menu keep it open.
  searchInput().dispatchEvent(new MouseEvent('mousedown', {bubbles: true}));
  expect(hudState.isTargetMenuOpen).toBe(true);
  document.body.dispatchEvent(new MouseEvent('mousedown', {bubbles: true}));
  expect(hudState.isTargetMenuOpen).toBe(false);
  expect(redraws).toBe(1);

  // An Escape that closes a menu doesn't reach the timeline's hotkeys, e.g.
  // to deselect. Others do.
  const hotkeys = vi.fn();
  document.addEventListener('keydown', hotkeys);
  hudState.isMoreMenuOpen = true;
  press('Escape', document.body);
  expect(hudState.isMoreMenuOpen).toBe(false);
  expect(hotkeys).not.toHaveBeenCalled();
  press('Escape', document.body);
  expect(hotkeys).toHaveBeenCalledTimes(1);
  document.removeEventListener('keydown', hotkeys);

  // Disposing the state removes the listeners.
  hudState[Symbol.dispose]();
  hudState.isMoreMenuOpen = true;
  press('Escape', document.body);
  expect(hudState.isMoreMenuOpen).toBe(true);
});

test('runs the actions and changes the settings of the more menu', () => {
  const {props, draw} = hud();
  click('.pf-live-stream-hud__more-trigger');
  draw();

  expect(texts('.pf-live-stream-hud__more-item-label')).toEqual([
    'Reconnect',
    'Download Trace',
  ]);
  expect(texts('.pf-live-stream-hud__more-interval-label')).toEqual([
    'Poll Interval',
    'Buffer Window',
    'History',
  ]);
  const [poll, buffer, history] = root.querySelectorAll('select');
  const options = (select: HTMLSelectElement) =>
    [...select.options].map((o) => o.text);
  expect(options(poll)).toEqual(['0.25s', '0.5s', '1.0s', '2.0s']);
  expect(options(buffer)).toEqual(['0s', '1s', '2s', '5s', '10s', '15s']);
  expect(options(history)).toEqual(['2min', '10min', '1h', '4h']);
  expect([poll.value, buffer.value, history.value]).toEqual([
    '0.25',
    '5',
    '60',
  ]);

  buffer.value = '10';
  buffer.dispatchEvent(new Event('change'));
  expect(settings.bufferDelay.get()).toBe('10');

  click('.pf-live-stream-hud__more-item');
  expect(props.onReconnect).toHaveBeenCalledTimes(1);
  expect(hudState.isMoreMenuOpen).toBe(false);
});

test('lays out the panel: status and target, actions, settings', () => {
  hud({layout: 'panel'});

  const sections = root.querySelectorAll('.pf-live-stream-hud__section');
  expect(sections).toHaveLength(3);
  expect(sections[0].textContent).toBe('LIVE STREAMworker/3▾');
  expect(texts('.pf-live-stream-hud__btn')).toEqual([
    'Follow Live: ON [Space]',
    'Pause',
    '↻Reconnect',
    '⬇Download Trace',
  ]);
  expect(texts('.pf-live-stream-hud__section-title')).toEqual(['Settings']);
  expect(texts('.pf-live-stream-hud__more-interval-label')).toEqual([
    'Poll Interval',
    'Buffer Window',
    'History',
  ]);

  // No ⋯ menu: its items are in the open.
  expect(root.querySelector('.pf-live-stream-hud__more-trigger')).toBeNull();
});

test('runs the actions and changes the settings of the panel', () => {
  const {props} = hud({layout: 'panel'});

  const [follow, pause, reconnect, download] =
    root.querySelectorAll<HTMLElement>('.pf-live-stream-hud__btn');
  follow.click();
  pause.click();
  reconnect.click();
  download.click();
  expect(props.onToggleFollow).toHaveBeenCalledTimes(1);
  expect(props.onTogglePause).toHaveBeenCalledTimes(1);
  expect(props.onReconnect).toHaveBeenCalledTimes(1);
  expect(props.onDownload).toHaveBeenCalledTimes(1);

  const history = root.querySelectorAll('select')[2];
  history.value = '240';
  history.dispatchEvent(new Event('change'));
  expect(settings.retention.get()).toBe('240');
});

test('pops out errors in line in the panel, under the status', () => {
  hud({layout: 'panel', status: 'error', error: 'Disk full'});

  const status = find('.pf-live-stream-hud__section');
  expect(status.lastElementChild?.classList).toContain(
    'pf-live-stream-hud__popout',
  );
  expect(text('.pf-live-stream-hud__popout-msg')).toBe('Disk full');
});
