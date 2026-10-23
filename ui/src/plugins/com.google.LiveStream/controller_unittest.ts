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
import type {App} from '../../public/app';
import type {Command} from '../../public/commands';
import type {SidePanelTabDescriptor} from '../../public/side_panel';
import {
  hudPlacement,
  LiveController,
  type LiveHudPlacement,
  type LiveSessionModel,
} from './controller';
import {PLUGIN_ID} from './opener';
import type {LiveStatus} from './status';
import {TARGET, type Target} from './wire';

const WORKER = {id: 'job-1/worker', short_name: 'worker'};
const EVAL = {id: 'job-1/eval', short_name: 'eval', task_count: 4};
const TARGET_ARG = `${PLUGIN_ID}:target`;
const FOLLOW_COMMAND = 'com.google.LiveStream#ToggleFollowLive';
const HUD_TAB = 'com.google.LiveStream#hud';
const STATUS_ATTRIBUTE = 'data-live-stream-status';

// Answers target requests with WORKER and EVAL, and seed requests with a
// seed, 503 for `unavailable` targets, or 502 with the error of `failing`
// ones. Fails all requests while `unreachable`.
class FakeServer {
  // Targets of the requests.
  readonly targetRequests: string[] = [];
  readonly seedRequests: URLSearchParams[] = [];
  readonly unavailable = new Set<string>();
  readonly failing = new Map<string, string>();
  unreachable = false;

  readonly fetch = async (input: RequestInfo | URL): Promise<Response> => {
    if (this.unreachable) throw new TypeError('Failed to fetch');
    const [route, query] = String(input).split('?');
    const params = new URLSearchParams(query);
    const target = params.get('target') ?? '';
    if (route === 'api/targets') {
      this.targetRequests.push(target);
      return new Response(
        JSON.stringify({
          targets: [WORKER, EVAL],
          selected: findTarget(target),
          page_title: 'Live',
        }),
      );
    }
    this.seedRequests.push(params);
    if (this.unavailable.has(target)) {
      return new Response('No data yet', {status: 503});
    }
    const error = this.failing.get(target);
    if (error !== undefined) {
      return new Response(JSON.stringify({error}), {status: 502});
    }
    return new Response(new ArrayBuffer(8));
  };

  seedTargets(): string[] {
    return this.seedRequests.map((r) => r.get('target') ?? '');
  }
}

// The target of `spec`, e.g. "job-1/eval/2" for task 2 of EVAL.
function findTarget(spec: string) {
  if (spec === WORKER.id) return WORKER;
  const match = spec.match(/^job-1\/eval\/(\d)$/);
  if (match === null) return undefined;
  return {...EVAL, id: spec, task_index: Number(match[1])};
}

// A session of the trace of `target`.
function fakeModel(target: Target, status: LiveStatus = 'live') {
  return {
    state: () => ({status, target, paused: false}),
    toggleFollow: vi.fn(),
    togglePause: vi.fn(),
    download: vi.fn(),
    refreshTargets: vi.fn(),
    dismissError: vi.fn(),
  } satisfies LiveSessionModel;
}

// An app with what LiveController uses. Like AppImpl, opening a trace closes
// the current one, then may fail.
function fakeApp() {
  // The registered side panel tabs, and the uris of showTab() calls.
  const tabs: SidePanelTabDescriptor[] = [];
  const shownTabs: string[] = [];
  const commands = new Map<string, Command>();
  // The page URL when each trace was opened.
  const openedUrls: string[] = [];
  const state = {
    closeTrace: () => {},
    openError: undefined as Error | undefined,
    theme: 'light',
  };
  const scheduleFullRedraw = vi.fn();
  const openTraceFromBuffer = vi.fn(
    async (_args: {
      readonly title: string;
      readonly pluginArgs: Record<string, unknown>;
    }) => {
      openedUrls.push(window.location.href);
      state.closeTrace();
      if (state.openError !== undefined) throw state.openError;
    },
  );
  const app = {
    settings: {
      register: (setting: {defaultValue: unknown}) => ({
        get: () => setting.defaultValue,
        set: () => {},
      }),
      // The app's settings.
      get: (id: string) =>
        id === 'theme' ? {get: () => state.theme} : undefined,
    },
    sidePanel: {
      registerTab: (tab: SidePanelTabDescriptor) => {
        tabs.push(tab);
        return {[Symbol.dispose]: () => tabs.splice(tabs.indexOf(tab), 1)};
      },
      showTab: (uri: string) => shownTabs.push(uri),
    },
    commands: {
      // Like the core's registry, ids are unique.
      registerCommand: (command: Command) => {
        if (commands.has(command.id)) throw new Error(`Twice ${command.id}`);
        commands.set(command.id, command);
        return {[Symbol.dispose]: () => commands.delete(command.id)};
      },
    },
    raf: {scheduleFullRedraw},
    openTraceFromBuffer,
  } as unknown as App;
  return {
    app,
    state,
    tabs,
    shownTabs,
    commands,
    openedUrls,
    openTraceFromBuffer,
    scheduleFullRedraw,
  };
}

let server: FakeServer;
let root: HTMLElement;
// The callbacks of the ResizeObservers, which jsdom lacks, for resize().
let resizeCallbacks: Array<() => void>;

beforeEach(() => {
  vi.useFakeTimers({
    toFake: [
      'setTimeout',
      'clearTimeout',
      'setInterval',
      'clearInterval',
      'performance',
    ],
  });
  server = new FakeServer();
  vi.stubGlobal('fetch', vi.fn(server.fetch));
  resizeCallbacks = [];
  vi.stubGlobal(
    'ResizeObserver',
    class {
      constructor(callback: () => void) {
        resizeCallbacks.push(callback);
      }
      observe() {}
    },
  );
  const url = new URL(window.location.href);
  url.searchParams.delete(TARGET_ARG);
  window.history.replaceState(null, '', url);
  root = document.createElement('div');
  document.body.append(root);
});

afterEach(() => {
  m.render(root, null);
  root.remove();
  document.querySelectorAll('.pf-live-stream-hud-root').forEach((row) => {
    m.mount(row, null);
    row.remove();
  });
  document.body.style.paddingTop = '';
  document.documentElement.removeAttribute(STATUS_ATTRIBUTE);
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

function urlTarget(): string | null {
  return new URL(window.location.href).searchParams.get(TARGET_ARG);
}

// The status that the page's <html> shows.
function pageStatus(): string | null {
  return document.documentElement.getAttribute(STATUS_ATTRIBUTE);
}

// The root of the HUD's top row.
function topRow(): HTMLElement {
  const row = document.querySelector('.pf-live-stream-hud-root');
  if (!(row instanceof HTMLElement)) throw new Error('No top row');
  return row;
}

// Calls back the ResizeObservers, as layout would.
function resize() {
  resizeCallbacks.forEach((callback) => callback());
}

function setup(placement?: LiveHudPlacement) {
  const fake = fakeApp();
  const controller = new LiveController(fake.app, placement);

  // Renders the HUD and returns its container.
  const render = (): Element => {
    if (placement === 'top') {
      m.redraw.sync();
      return topRow();
    }
    m.render(root, fake.tabs[0]!.render());
    return root;
  };

  // Renders the HUD and returns what it shows.
  const hud = () => {
    const shown = render();
    const text = (selector: string) =>
      shown.querySelector(selector)?.textContent ?? undefined;
    return {
      badge: text('.pf-live-stream-hud__badge'),
      popout: text('.pf-live-stream-hud__popout-msg'),
      target: text('.pf-live-stream-hud__target-trigger-label'),
      note: text('.pf-live-stream-hud__note'),
    };
  };

  // Clicks the element of `selector` in the HUD.
  const click = (selector: string) => {
    const element = render().querySelector(selector);
    if (!(element instanceof HTMLElement)) throw new Error(`No ${selector}`);
    element.click();
  };

  // Clicks the HUD's button that reads `label`, e.g. "Reconnect".
  const press = (label: string) => {
    const buttons = render().querySelectorAll('button');
    const button = Array.from(buttons).find((b) =>
      b.textContent?.includes(label),
    );
    if (button === undefined) throw new Error(`No ${label} button`);
    button.click();
  };

  return {
    controller,
    hud,
    click,
    press,
    runCommand: (id: string) => fake.commands.get(id)?.callback(),
    // The plugin args of the latest trace.
    openedArgs: () =>
      fake.openTraceFromBuffer.mock.lastCall?.[0].pluginArgs[PLUGIN_ID],
    ...fake,
  };
}

// Sets up a page that shows the trace of WORKER.
async function openWorker(placement?: LiveHudPlacement) {
  const page = setup(placement);
  page.controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(0);
  const model = fakeModel(TARGET.parse(WORKER));
  const attachment = page.controller.attach(model);
  page.state.closeTrace = () => attachment[Symbol.dispose]();
  return {...page, model};
}

test('shows buffering while the first trace opens, then its session', async () => {
  const {controller, hud, openTraceFromBuffer, openedArgs} = setup();

  controller.open(WORKER.id);
  expect(hud()).toMatchObject({
    badge: 'BUFFERING',
    popout: 'Buffering live stream...',
  });

  await vi.advanceTimersByTimeAsync(0);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(1);
  expect(openTraceFromBuffer.mock.lastCall?.[0].title).toBe('Live: worker');
  expect(openedArgs()).toMatchObject({target: WORKER});
  expect(urlTarget()).toBe(WORKER.id);
  // With the history of the History setting.
  expect(server.seedRequests[0]!.get('history_sec')).toBe('3600');

  controller.attach(fakeModel(TARGET.parse(WORKER)));
  expect(hud()).toEqual({
    badge: 'LIVE STREAM',
    popout: undefined,
    target: 'worker',
    note: undefined,
  });
});

test('shows the HUD in a side panel tab from page open', () => {
  const {tabs, shownTabs, hud} = setup();

  expect(tabs).toMatchObject([
    {uri: HUD_TAB, title: 'Live Stream', icon: 'sensors'},
  ]);
  expect(shownTabs).toEqual([HUD_TAB]);
  expect(hud().badge).toBe('BUFFERING');
  expect(root.querySelector('.pf-live-stream-hud--panel')).not.toBeNull();
});

test('keeps the tab while a target switch replaces the trace', async () => {
  const {controller, tabs, shownTabs, hud} = await openWorker();

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(0);
  controller.attach(fakeModel(TARGET.parse(findTarget('job-1/eval/2'))));

  expect(tabs).toHaveLength(1);
  // Shown at page open only: a panel that the user closed stays closed.
  expect(shownTabs).toEqual([HUD_TAB]);
  expect(hud()).toMatchObject({badge: 'LIVE STREAM', target: 'eval/2'});
});

test('reads the HUD placement from route argument hud', () => {
  expect(hudPlacement('top')).toBe('top');
  expect(hudPlacement('panel')).toBe('panel');
  expect(hudPlacement(undefined)).toBe('panel');
  // Other values.
  expect(hudPlacement('row')).toBe('panel');
  expect(hudPlacement(true)).toBe('panel');
});

test('shows the HUD in a row above the app with hud=top', async () => {
  const {controller, tabs, shownTabs, hud} = setup('top');
  const row = topRow();

  expect(tabs).toEqual([]);
  expect(shownTabs).toEqual([]);
  expect(document.body.firstElementChild).toBe(row);
  expect(row.querySelector('.pf-live-stream-hud--row')).not.toBeNull();
  expect(hud().badge).toBe('BUFFERING');

  // The app starts below the row.
  vi.spyOn(row, 'getBoundingClientRect').mockReturnValue({
    height: 42.5,
  } as DOMRect);
  resize();
  expect(document.body.style.paddingTop).toBe('42.5px');

  // Kept while a target switch replaces the trace.
  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(0);
  controller.attach(fakeModel(TARGET.parse(WORKER)));
  expect(hud()).toMatchObject({badge: 'LIVE STREAM', target: 'worker'});
  expect(topRow()).toBe(row);
});

test('keeps the top row first in the page, in the app theme', () => {
  const {hud, state} = setup('top');
  const row = topRow();
  expect(row.querySelector('.pf-theme-provider--light')).not.toBeNull();

  state.theme = 'dark';
  hud();
  expect(row.querySelector('.pf-theme-provider--dark')).not.toBeNull();

  // E.g. once something replaced the body's children.
  document.body.replaceChildren(root);
  expect(hud().badge).toBe('BUFFERING');
  expect(document.body.firstElementChild).toBe(row);
});

test('hides the top row while another trace is open', async () => {
  const {controller, hud, state} = await openWorker('top');

  state.closeTrace();
  controller.showOtherTrace();
  expect(hud()).toMatchObject({badge: undefined, note: undefined});
  expect(topRow().querySelector('.pf-live-stream-hud')).toBeNull();
});

test('shows an error while the server is unreachable', async () => {
  server.unreachable = true;
  const {controller, hud, openTraceFromBuffer} = setup();

  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(0);
  // Once it lasts 1 s.
  expect(hud().badge).toBe('BUFFERING');
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud().badge).toBe('STREAM ERROR');
  expect(hud().popout).toBe('Cannot reach the live server: Failed to fetch');

  // Retries 1 s, then 2 s later.
  server.unreachable = false;
  await vi.advanceTimersByTimeAsync(2_000);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(1);
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud().badge).toBe('BUFFERING');
});

test('waits for the first data of a target as buffering', async () => {
  server.unavailable.add(WORKER.id);
  const {controller, hud, openTraceFromBuffer} = setup();

  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(5_000);

  expect(server.seedRequests.length).toBeGreaterThan(1);
  expect(hud().badge).toBe('BUFFERING');
  expect(openTraceFromBuffer).not.toHaveBeenCalled();
});

test('shows why the server fails to reach a target, and retries', async () => {
  const failing = 'Waiting for the first snapshot: Unreachable';
  server.failing.set(WORKER.id, failing);
  const {controller, hud, openTraceFromBuffer} = setup();

  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(0);
  // Once it lasts 1 s.
  expect(hud().badge).toBe('BUFFERING');
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud()).toMatchObject({badge: 'STREAM ERROR', popout: failing});

  // Retries 1 s, then 2 s later.
  server.failing.delete(WORKER.id);
  await vi.advanceTimersByTimeAsync(2_000);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(1);
});

test('redraws when only the error changes, e.g. the reason to fail', () => {
  const {controller, scheduleFullRedraw} = setup();
  const model = fakeModel(TARGET.parse(WORKER), 'error');
  const shown = {error: 'Unreachable'};
  controller.attach({...model, state: () => ({...model.state(), ...shown})});
  controller.refreshStatus();
  scheduleFullRedraw.mockClear();

  controller.refreshStatus();
  expect(scheduleFullRedraw).not.toHaveBeenCalled();

  shown.error = 'Connection refused';
  controller.refreshStatus();
  expect(scheduleFullRedraw).toHaveBeenCalledTimes(1);
});

test('shows a failed first open until the next open', async () => {
  const {controller, hud, click, press} = setup();

  controller.open('job-2/unknown');
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud()).toMatchObject({
    badge: 'STREAM ERROR',
    popout:
      'Cannot open job-2/unknown: The live server has no target "job-2/unknown"',
  });

  // Dismissing hides the popout only.
  click('.pf-live-stream-hud__popout-close');
  expect(hud()).toMatchObject({badge: 'STREAM ERROR', popout: undefined});

  press('Reconnect');
  expect(server.targetRequests).toEqual(['job-2/unknown', 'job-2/unknown']);
});

test('shows a failed switch over the open trace until dismissed', async () => {
  const {controller, hud, click, model} = await openWorker();

  controller.open('job-2/unknown');
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud()).toMatchObject({badge: 'STREAM ERROR', target: 'worker'});
  expect(hud().popout).toContain('Cannot open job-2/unknown');
  expect(urlTarget()).toBe(WORKER.id);

  click('.pf-live-stream-hud__popout-close');
  expect(hud()).toMatchObject({badge: 'LIVE STREAM', popout: undefined});
  expect(model.dismissError).not.toHaveBeenCalled();
});

test('shows a trace that fails to load as an error', async () => {
  const {controller, hud, state} = await openWorker();
  state.openError = new Error('Bad seed');

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(1_000);

  expect(hud()).toMatchObject({
    badge: 'STREAM ERROR',
    popout: 'Cannot open job-1/eval/2: Bad seed',
    target: 'eval/2',
  });
});

test('opens another target in place, with its URL', async () => {
  const {controller, openTraceFromBuffer, openedArgs, openedUrls} =
    await openWorker();

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(0);

  expect(openTraceFromBuffer).toHaveBeenCalledTimes(2);
  expect(openedArgs()).toMatchObject({target: {id: 'job-1/eval/2'}});
  expect(urlTarget()).toBe('job-1/eval/2');
  // Before the trace changed.
  expect(new URL(openedUrls[1]!).searchParams.get(TARGET_ARG)).toBe(
    'job-1/eval/2',
  );
});

test('a new open replaces a pending one', async () => {
  server.unavailable.add('job-1/eval/2');
  const {controller, openTraceFromBuffer, openedArgs} = await openWorker();

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(5_000);
  expect(urlTarget()).toBe(WORKER.id);

  controller.open('job-1/eval/3');
  await vi.advanceTimersByTimeAsync(0);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(2);
  expect(openedArgs()).toMatchObject({target: {id: 'job-1/eval/3'}});

  // The first one stopped retrying.
  const seedRequests = server.seedRequests.length;
  await vi.advanceTimersByTimeAsync(60_000);
  expect(server.seedRequests.length).toBe(seedRequests);
});

test('reconnects to the pending target, else the open one', async () => {
  server.unavailable.add('job-1/eval/2');
  const {controller, click, press, openTraceFromBuffer, openedArgs} =
    await openWorker();
  const reconnect = () => press('Reconnect');

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(5_000);
  reconnect();
  expect(server.targetRequests.at(-1)).toBe('job-1/eval/2');

  // Picking the open target cancels the switch.
  click('.pf-live-stream-hud__target-trigger');
  click('.pf-live-stream-hud__target-item');
  await vi.advanceTimersByTimeAsync(0);
  reconnect();
  await vi.advanceTimersByTimeAsync(0);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(2);
  expect(openedArgs()).toMatchObject({target: WORKER});
});

test('picking the open target cancels a pending switch', async () => {
  server.unavailable.add('job-1/eval/2');
  const {controller, hud, click, openTraceFromBuffer} = await openWorker();
  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(5_000);
  expect(hud().target).toBe('eval/2');

  // The row of WORKER, the open target.
  click('.pf-live-stream-hud__target-trigger');
  click('.pf-live-stream-hud__target-item');
  const seedRequests = server.seedRequests.length;
  await vi.advanceTimersByTimeAsync(60_000);

  expect(server.seedRequests.length).toBe(seedRequests);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(1);
  expect(hud().badge).toBe('LIVE STREAM');
  expect(urlTarget()).toBe(WORKER.id);
});

test('reopens unless another open is pending', async () => {
  server.unavailable.add('job-1/eval/2');
  const {controller, openTraceFromBuffer, openedArgs} = await openWorker();

  controller.open('job-1/eval/2');
  await vi.advanceTimersByTimeAsync(0);
  controller.reopen(WORKER.id, true);
  await vi.advanceTimersByTimeAsync(5_000);
  expect(server.targetRequests).toEqual([WORKER.id, 'job-1/eval/2']);

  // A failed open is no longer pending.
  controller.open('job-2/unknown');
  await vi.advanceTimersByTimeAsync(0);
  controller.reopen(WORKER.id, true);
  await vi.advanceTimersByTimeAsync(0);
  expect(openTraceFromBuffer).toHaveBeenCalledTimes(2);
  expect(openedArgs()).toMatchObject({target: WORKER, epochRetry: true});
});

test('toggles following with Space, also between sessions', () => {
  const {controller, runCommand} = setup();

  runCommand(FOLLOW_COMMAND);
  expect(controller.following).toBe(false);

  const model = fakeModel(TARGET.parse(WORKER));
  controller.attach(model);
  runCommand(FOLLOW_COMMAND);
  expect(model.toggleFollow).toHaveBeenCalledTimes(1);
});

test('hides the HUD and frees Space while another trace is open', async () => {
  const {controller, hud, commands, state} = await openWorker();

  // E.g. a trace opened from a file. The tab stays, and says why.
  state.closeTrace();
  controller.showOtherTrace();
  expect(hud()).toMatchObject({
    badge: undefined,
    note: 'Live stream controls are off while another trace is open.',
  });
  expect(commands.has(FOLLOW_COMMAND)).toBe(false);

  // Until a live trace opens.
  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(1_000);
  expect(hud().badge).toBe('BUFFERING');
  controller.attach(fakeModel(TARGET.parse(WORKER)));
  expect(hud()).toMatchObject({badge: 'LIVE STREAM', note: undefined});
  expect(commands.has(FOLLOW_COMMAND)).toBe(true);
});

test('shows the status on the page, also while the HUD is closed', async () => {
  server.unreachable = true;
  // The HUD never renders, as in a closed side panel.
  const {controller} = setup();

  controller.open(WORKER.id);
  await vi.advanceTimersByTimeAsync(0);
  // Once it lasts 1 s, as in the HUD.
  expect(pageStatus()).toBe('buffering');
  await vi.advanceTimersByTimeAsync(1_000);
  expect(pageStatus()).toBe('error');

  // Retries 1 s, then 2 s later.
  server.unreachable = false;
  await vi.advanceTimersByTimeAsync(2_000);
  controller.attach(fakeModel(TARGET.parse(WORKER), 'paused'));
  expect(pageStatus()).toBe('paused');
});

test('sets the page status only when it changes', async () => {
  const {controller, hud} = await openWorker();
  const observer = new MutationObserver(() => {});
  observer.observe(document.documentElement, {attributes: true});

  hud();
  controller.refreshStatus();
  expect(observer.takeRecords()).toEqual([]);
  expect(pageStatus()).toBe('live');
  observer.disconnect();
});

test('removes the page status while another trace is open', async () => {
  const {controller, state} = await openWorker();

  state.closeTrace();
  controller.showOtherTrace();
  expect(pageStatus()).toBeNull();
  // Also once the closed trace's buffering would show.
  await vi.advanceTimersByTimeAsync(1_000);
  expect(pageStatus()).toBeNull();

  controller.open(WORKER.id);
  expect(pageStatus()).toBe('buffering');
});
