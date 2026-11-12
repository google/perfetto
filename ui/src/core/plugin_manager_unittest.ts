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

import type {App} from '../public/app';
import type {Trace} from '../public/trace';
import {AppImpl} from './app_impl';
import {createFakeTraceImpl} from './fake_trace_impl';
import {getOwner} from './ownership';
import {PluginManagerImpl} from './plugin_manager';

describe('PluginManagerImpl', () => {
  let nextPluginId = 0;

  // Registers a new plugin, activates it and loads a trace, so that the hooks
  // run with the proxies that plugins get. Returns the plugin's ID.
  async function runPlugin(hooks: {
    onActivate?: (app: App) => void;
    onTraceLoad?: (trace: Trace) => void;
  }): Promise<string> {
    const id = `test.Plugin${nextPluginId++}`;
    const trace = createFakeTraceImpl();
    class Plugin {
      static readonly id = id;
      static onActivate(app: App) {
        hooks.onActivate?.(app);
      }
      async onTraceLoad(ctx: Trace) {
        hooks.onTraceLoad?.(ctx);
      }
    }
    const plugins = new PluginManagerImpl([id]);
    plugins.registerPlugin(Plugin);
    plugins.activatePlugins(AppImpl.instance);
    await plugins.onTraceLoad(trace);
    return id;
  }

  function findMenuItem(text: string) {
    return AppImpl.instance.sidebar.menuItems
      .valuesAsArray()
      .find((item) => item.text === text);
  }

  function findSidePanelTab(uri: string) {
    return AppImpl.instance.sidePanel.registeredTabs.find(
      (tab) => tab.uri === uri,
    );
  }

  describe('app proxy', () => {
    test('records the plugin as the owner of its commands', async () => {
      const id = await runPlugin({
        onActivate: (app) =>
          app.commands.registerCommand({
            id: 'test.AppCommand',
            name: 'App command',
            callback: () => {},
          }),
      });
      const cmd = AppImpl.instance.commands.getCommand('test.AppCommand');
      expect(getOwner(cmd!)).toBe(id);
    });

    test('passes a class instance command on unchanged', async () => {
      class MyCommand {
        readonly id = 'test.ClassCommand';
        readonly name = 'Class command';
        calls = 0;
        callback() {
          this.calls++;
        }
      }
      const cmd = new MyCommand();
      const id = await runPlugin({
        onActivate: (app) => app.commands.registerCommand(cmd),
      });
      const commands = AppImpl.instance.commands;
      expect(commands.getCommand('test.ClassCommand')).toBe(cmd);
      expect(getOwner(cmd)).toBe(id);
      await commands.runCommand('test.ClassCommand');
      expect(cmd.calls).toBe(1);
    });

    test('records the plugin as the owner of its sidebar items', async () => {
      const id = await runPlugin({
        onActivate: (app) =>
          app.sidebar.addMenuItem({
            section: 'support',
            text: 'App item',
            action: () => {},
          }),
      });
      expect(getOwner(findMenuItem('App item')!)).toBe(id);
    });

    test('records the plugin as the owner of its side panel tabs', async () => {
      const id = await runPlugin({
        onActivate: (app) =>
          app.sidePanel.registerTab({
            uri: 'test#AppTab',
            title: 'App tab',
            render: () => '',
          }),
      });
      expect(getOwner(findSidePanelTab('test#AppTab')!)).toBe(id);
    });

    test('records the plugin as the owner of its omnibox modes', async () => {
      const id = await runPlugin({
        onActivate: (app) =>
          app.omnibox.registerMode({
            trigger: '~',
            placeholder: 'App mode',
            onSubmit: () => {},
          }),
      });
      const mode = AppImpl.instance.omnibox.registeredModes.get('~');
      expect(getOwner(mode!)).toBe(id);
    });
  });

  describe('trace proxy', () => {
    // Change detector for com.google.LiveStream, an out-of-tree plugin managed
    // by xldrx@. It shows running jobs as a growing trace. The core has no API
    // to grow a trace, so the plugin writes traceInfo.end, which needs the
    // trace proxy that plugins get to return the trace's own traceInfo, not a
    // copy. If this changes on purpose, delete this test and tell us, so the
    // plugin can adapt.
    test("gives plugins the trace's own traceInfo", async () => {
      const trace = createFakeTraceImpl();
      let seen: unknown;
      class Plugin {
        static readonly id = 'test.Plugin';
        async onTraceLoad(ctx: Trace) {
          seen = ctx.traceInfo;
        }
      }

      const plugins = new PluginManagerImpl([Plugin.id]);
      plugins.registerPlugin(Plugin);
      plugins.activatePlugins(AppImpl.instance);
      await plugins.onTraceLoad(trace);
      expect(seen).toBe(trace.traceInfo);
    });

    test('records the plugin as the owner of its commands', async () => {
      const id = await runPlugin({
        onTraceLoad: (trace) =>
          trace.commands.registerCommand({
            id: 'test.TraceCommand',
            name: 'Trace command',
            callback: () => {},
          }),
      });
      const cmd = AppImpl.instance.commands.getCommand('test.TraceCommand');
      expect(getOwner(cmd!)).toBe(id);
    });

    test('records the plugin as the owner of its sidebar items', async () => {
      const id = await runPlugin({
        onTraceLoad: (trace) =>
          trace.sidebar.addMenuItem({
            section: 'current_trace',
            text: 'Trace item',
            action: () => {},
          }),
      });
      expect(getOwner(findMenuItem('Trace item')!)).toBe(id);
    });

    test('records the plugin as the owner of its side panel tabs', async () => {
      const id = await runPlugin({
        onTraceLoad: (trace) =>
          trace.sidePanel.registerTab({
            uri: 'test#TraceTab',
            title: 'Trace tab',
            render: () => '',
          }),
      });
      expect(getOwner(findSidePanelTab('test#TraceTab')!)).toBe(id);
    });

    test('records the plugin as the owner of its omnibox modes', async () => {
      const id = await runPlugin({
        onTraceLoad: (trace) =>
          trace.omnibox.registerMode({
            trigger: '^',
            placeholder: 'Trace mode',
            onSubmit: () => {},
          }),
      });
      const mode = AppImpl.instance.omnibox.registeredModes.get('^');
      expect(getOwner(mode!)).toBe(id);
    });

    test('records the plugin as the owner of its bottom tabs', async () => {
      const id = await runPlugin({
        onTraceLoad: (trace) =>
          trace.tabs.registerTab({
            uri: 'test#BottomTab',
            content: {render: () => '', getTitle: () => 'Bottom tab'},
          }),
      });
      const tab = AppImpl.instance.trace!.tabs.resolveTab('test#BottomTab');
      expect(getOwner(tab!)).toBe(id);
    });

    test('records the plugin as the owner of its selection tabs', async () => {
      const content = {isLoading: false, content: ''};
      const id = await runPlugin({
        onTraceLoad: (trace) => {
          trace.selection.registerSelectionTab({
            id: 'test.SelectionTab',
            name: 'Selection tab',
            render: () => content,
          });
          trace.selection.registerAreaSelectionTab({
            id: 'test.AreaTab',
            name: 'Area tab',
            render: () => content,
          });
          trace.selection.registerTrackEventSelectionTab({
            id: 'test.TrackEventTab',
            name: 'Track event tab',
            render: () => content,
          });
        },
      });
      const tabs = AppImpl.instance.trace!.selection.selectionTabs;
      expect(tabs.map((tab) => [tab.id, getOwner(tab)])).toEqual([
        ['test.SelectionTab', id],
        ['test.AreaTab', id],
        ['test.TrackEventTab', id],
      ]);
    });

    test('records the plugin as the owner of its status bar items', async () => {
      const item = {renderItem: () => ({label: 'Status'})};
      const id = await runPlugin({
        onTraceLoad: (trace) => trace.statusbar.registerItem(item),
      });
      const items = AppImpl.instance.trace!.statusbar.statusBarItems;
      expect(items).toContain(item);
      expect(getOwner(item)).toBe(id);
    });
  });
});
