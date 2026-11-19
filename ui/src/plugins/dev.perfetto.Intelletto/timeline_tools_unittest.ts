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

import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Time} from '../../base/time';
import type {Note, SpanNote} from '../../public/note';
import type {Selection} from '../../public/selection';
import type {TabDescriptor} from '../../public/tab';
import {DurationPrecision, TimestampFormat} from '../../public/timeline';
import type {Trace} from '../../public/trace';
import type {Track, TrackFilterCriteria} from '../../public/track';
import {TrackNode, Workspace} from '../../public/workspace';
import {DrawerPanelVisibility} from '../../widgets/drawer_panel';
import {registerTimelineTools} from './timeline_tools';
import {ToolRegistry} from './tools';

function createFakeTrace() {
  const defaultWs = new Workspace('default');
  defaultWs.title = 'Default Workspace';
  defaultWs.userEditable = false;

  const allWorkspaces: Workspace[] = [defaultWs];
  let currentWs: Workspace = defaultWs;

  const registeredTracks = new Map<string, Track>();
  const filterCriteria: TrackFilterCriteria[] = [];
  const criteriaFilters = new Map<string, string[]>();
  const filters = {
    nameFilter: '',
    criteriaFilters,
    clearAll() {
      this.nameFilter = '';
      this.criteriaFilters.clear();
    },
    areFiltersSet() {
      return this.nameFilter !== '' || this.criteriaFilters.size > 0;
    },
  };

  let selection: Selection = {kind: 'empty'};
  let visibleWindow = HighPrecisionTimeSpan.fromTime(
    Time.fromRaw(0n),
    Time.fromRaw(10_000_000_000n),
  );
  let timezoneOverride = 'UTC';

  const notes = new Map<string, Note | SpanNote>();
  let nextNoteId = 0;

  let searchText = '';
  let resultIndex = -1;

  const tabs: TabDescriptor[] = [];
  const openTabs: string[] = [];
  let currentTabUri = 'current_selection';
  let tabPanelVisibility = DrawerPanelVisibility.COLLAPSED;

  const trace = {
    traceInfo: {
      traceTitle: 'test.pftrace',
      traceUrl: '',
      start: Time.fromRaw(0n),
      end: Time.fromRaw(10_000_000_000n),
      unixOffset: Time.ZERO,
      tzOffMin: 0,
      importErrors: 0,
      traceTypes: ['proto'],
      hasFtrace: true,
      uuid: 'uuid',
      cached: false,
      shareable: false,
      downloadable: false,
    },
    raf: {
      scheduleFullRedraw: vi.fn(),
      scheduleCanvasRedraw: vi.fn(),
      addCanvasRedrawCallback: vi.fn(),
    },
    scrollTo: vi.fn(),
    get currentWorkspace() {
      return currentWs;
    },
    get defaultWorkspace() {
      return defaultWs;
    },
    workspaces: {
      get currentWorkspace() {
        return currentWs;
      },
      get all() {
        return allWorkspaces;
      },
      createEmptyWorkspace(displayName: string, id?: string) {
        const ws = new Workspace(id);
        ws.title = displayName;
        allWorkspaces.push(ws);
        return ws;
      },
      removeWorkspace(ws: Workspace) {
        const idx = allWorkspaces.indexOf(ws);
        if (idx !== -1) allWorkspaces.splice(idx, 1);
        if (currentWs === ws) currentWs = defaultWs;
      },
      switchWorkspace(ws: Workspace) {
        currentWs = ws;
      },
    },
    tracks: {
      registerTrack(track: Track) {
        registeredTracks.set(track.uri, track);
      },
      getTrack(uri: string) {
        return registeredTracks.get(uri);
      },
      getAllTracks() {
        return Array.from(registeredTracks.values());
      },
      findTrack: vi.fn(),
      registerTrackFilterCriteria(c: TrackFilterCriteria) {
        filterCriteria.push(c);
      },
      get trackFilterCriteria() {
        return filterCriteria;
      },
      filters,
      registerOverlay: vi.fn(),
    },
    selection: {
      get selection() {
        return selection;
      },
      selectionTabs: [],
      clearSelection() {
        selection = {kind: 'empty'};
      },
      selectTrack(trackUri: string) {
        selection = {kind: 'track', trackUri};
      },
      selectTrackEvent: vi.fn(),
      selectSqlEvent: vi.fn(),
      selectArea: vi.fn(),
      scrollToSelection: vi.fn(),
      getTimeSpanOfSelection: vi.fn(),
      resolveSqlEvents: vi.fn(),
      registerSelectionTab: vi.fn(),
      registerAreaSelectionTab: vi.fn(),
      registerTrackEventSelectionTab: vi.fn(),
    },
    timeline: {
      get visibleWindow() {
        return visibleWindow;
      },
      setVisibleWindow(span: HighPrecisionTimeSpan) {
        visibleWindow = span;
      },
      pan(delta: number) {
        visibleWindow = visibleWindow.translate(delta);
      },
      zoom(factor: number, centerPoint = 0.5) {
        visibleWindow = visibleWindow.scale(factor, centerPoint, 10);
      },
      panIntoView: vi.fn(),
      panSpanIntoView: vi.fn(),
      getTimeAxisOrigin: () => Time.ZERO,
      toDomainTime: (ts: typeof Time.ZERO) => ts,
      timestampFormat: TimestampFormat.Timecode,
      durationPrecision: DurationPrecision.Full,
      customTimezoneOffset: 0,
      timezoneOverride: {
        get: () => timezoneOverride,
        set: (v: string) => {
          timezoneOverride = v;
        },
      },
    },
    notes: {
      get notes() {
        return notes;
      },
      getNote(id: string) {
        return notes.get(id);
      },
      addNote(args: {
        timestamp: typeof Time.ZERO;
        color?: string;
        text?: string;
        id?: string;
      }) {
        const id = args.id ?? `note_${++nextNoteId}`;
        notes.set(id, {
          ...args,
          noteType: 'DEFAULT',
          id,
          color: args.color ?? '#123456',
          text: args.text ?? '',
        });
        return id;
      },
      addSpanNote(args: {
        start: typeof Time.ZERO;
        end: typeof Time.ZERO;
        color?: string;
        text?: string;
        id?: string;
      }) {
        const id = args.id ?? `note_${++nextNoteId}`;
        notes.set(id, {
          ...args,
          noteType: 'SPAN',
          id,
          color: args.color ?? '#123456',
          text: args.text ?? '',
        });
        return id;
      },
      changeNote(id: string, args: {color?: string; text?: string}) {
        const note = notes.get(id);
        if (note === undefined) return;
        notes.set(id, {
          ...note,
          color: args.color ?? note.color,
          text: args.text ?? note.text,
        });
      },
      removeNote(id: string) {
        notes.delete(id);
      },
    },
    search: {
      registerSearchProvider: vi.fn(),
      search(text: string) {
        searchText = text;
        resultIndex = -1;
      },
      reset() {
        searchText = '';
        resultIndex = -1;
      },
      stepForward() {
        resultIndex++;
      },
      stepBackwards() {
        resultIndex--;
      },
      get searchText() {
        return searchText;
      },
      get searchInProgress() {
        return false;
      },
      get resultIndex() {
        return resultIndex;
      },
      get hasResults() {
        return false;
      },
      get searchResults() {
        return undefined;
      },
    },
    tabs: {
      registerTab(tab: TabDescriptor) {
        tabs.push(tab);
      },
      showTab(uri: string) {
        if (uri !== 'current_selection' && !openTabs.includes(uri)) {
          openTabs.push(uri);
        }
        currentTabUri = uri;
      },
      hideTab(uri: string) {
        const idx = openTabs.indexOf(uri);
        if (idx !== -1) openTabs.splice(idx, 1);
      },
      addDefaultTab: vi.fn(),
      get currentTabUri() {
        return currentTabUri;
      },
      get openTabsUri() {
        return openTabs;
      },
      get tabs() {
        return tabs;
      },
      get tabPanelVisibility() {
        return tabPanelVisibility;
      },
      setTabPanelVisibility(v: DrawerPanelVisibility) {
        tabPanelVisibility = v;
      },
      toggleTabPanelVisibility: vi.fn(),
    },
  } as unknown as Trace;

  return trace;
}

function setup() {
  const trace = createFakeTrace();
  const reg = new ToolRegistry();
  registerTimelineTools(reg, trace);

  const cpuGroup = new TrackNode({
    name: 'CPU',
    isSummary: true,
    collapsed: true,
  });
  const cpu0 = new TrackNode({name: 'CPU 0', uri: 'perfetto.Cpu#0'});
  const cpu1 = new TrackNode({name: 'CPU 1', uri: 'perfetto.Cpu#1'});
  cpuGroup.addChildLast(cpu0);
  cpuGroup.addChildLast(cpu1);

  const headlessContainer = new TrackNode({name: 'Hidden', headless: true});
  const surfaceFlinger = new TrackNode({
    name: 'SurfaceFlinger',
    uri: 'perfetto.Proc#sf',
    subtitle: 'pid 123',
    chips: ['GPU'],
  });
  headlessContainer.addChildLast(surfaceFlinger);

  trace.currentWorkspace.addChildLast(cpuGroup);
  trace.currentWorkspace.addChildLast(headlessContainer);

  trace.tracks.registerTrack({
    uri: 'perfetto.Cpu#0',
    description: 'CPU 0 scheduling',
    tags: {cpu: 0, kinds: ['cpu_sched']},
    renderer: {
      render: () => {},
    },
  });
  trace.tracks.registerTrack({
    uri: 'perfetto.Proc#sf',
    description: 'SurfaceFlinger process',
    tags: {kinds: ['process']},
    renderer: {
      render: () => {},
    },
  });

  return {trace, reg, cpuGroup, cpu0, cpu1, surfaceFlinger};
}

describe('timeline_tools', () => {
  test('list_tracks filters and skips headless containers', async () => {
    const {reg, cpuGroup} = setup();

    const all = JSON.parse(await reg.call('list_tracks', {}));
    expect(all.totalMatched).toBe(4);
    expect(all.tracks.map((t: {name: string}) => t.name)).toEqual([
      'CPU',
      'CPU 0',
      'CPU 1',
      'SurfaceFlinger',
    ]);

    const filtered = JSON.parse(
      await reg.call('list_tracks', {query: 'surface'}),
    );
    expect(filtered.totalMatched).toBe(1);
    expect(filtered.tracks[0].uri).toBe('perfetto.Proc#sf');

    const rootChildren = JSON.parse(
      await reg.call('list_tracks', {parentId: 'root'}),
    );
    expect(rootChildren.tracks.map((t: {name: string}) => t.name)).toEqual([
      'CPU',
      'SurfaceFlinger',
    ]);

    const cpuChildren = JSON.parse(
      await reg.call('list_tracks', {parentId: cpuGroup.id}),
    );
    expect(cpuChildren.tracks.map((t: {name: string}) => t.name)).toEqual([
      'CPU 0',
      'CPU 1',
    ]);
  });

  test('get_track_details returns descriptor metadata', async () => {
    const {reg} = setup();
    const details = JSON.parse(
      await reg.call('get_track_details', {trackUri: 'perfetto.Cpu#0'}),
    );
    expect(details.name).toBe('CPU 0');
    expect(details.description).toBe('CPU 0 scheduling');
    expect(details.tags).toEqual({cpu: 0, kinds: ['cpu_sched']});
  });

  test('pin_tracks and unpin_tracks avoid duplicates and support all', async () => {
    const {trace, reg} = setup();

    const pinRes1 = JSON.parse(
      await reg.call('pin_tracks', {
        trackUris: ['perfetto.Cpu#0', 'perfetto.Proc#sf', 'missing'],
      }),
    );
    expect(pinRes1).toEqual({
      pinned: ['perfetto.Cpu#0', 'perfetto.Proc#sf'],
      alreadyPinned: [],
      notFound: ['missing'],
    });
    expect(trace.currentWorkspace.pinnedTracks.length).toBe(2);

    // Pinning again does not duplicate
    const pinRes2 = JSON.parse(
      await reg.call('pin_tracks', {trackUris: ['perfetto.Cpu#0']}),
    );
    expect(pinRes2.alreadyPinned).toEqual(['perfetto.Cpu#0']);
    expect(trace.currentWorkspace.pinnedTracks.length).toBe(2);

    const pinnedOnly = JSON.parse(
      await reg.call('list_tracks', {pinnedOnly: true}),
    );
    expect(pinnedOnly.tracks.map((t: {uri: string}) => t.uri)).toEqual([
      'perfetto.Cpu#0',
      'perfetto.Proc#sf',
    ]);

    await reg.call('unpin_tracks', {trackUris: ['perfetto.Cpu#0']});
    expect(trace.currentWorkspace.pinnedTracks.length).toBe(1);

    await reg.call('unpin_tracks', {all: true});
    expect(trace.currentWorkspace.pinnedTracks.length).toBe(0);
  });

  test('set_tracks_expanded expands, collapses, and reveals ancestors', async () => {
    const {reg, cpuGroup} = setup();
    expect(cpuGroup.collapsed).toBe(true);

    await reg.call('set_tracks_expanded', {
      expanded: true,
      trackUris: ['perfetto.Cpu#0'],
      revealAncestors: true,
    });
    expect(cpuGroup.collapsed).toBe(false);

    await reg.call('set_tracks_expanded', {expanded: false, all: true});
    expect(cpuGroup.collapsed).toBe(true);
  });

  test('selection tools select_track, focus_selection, clear_selection', async () => {
    const {trace, reg} = setup();

    await reg.call('select_track', {trackUri: 'perfetto.Cpu#0'});
    expect(trace.selection.selection).toEqual({
      kind: 'track',
      trackUri: 'perfetto.Cpu#0',
    });

    await reg.call('focus_selection', {behavior: 'pan'});

    await reg.call('clear_selection', {});
    expect(trace.selection.selection.kind).toBe('empty');

    await expect(reg.call('focus_selection', {})).rejects.toThrow(
      'Nothing is currently selected.',
    );
  });

  test('viewport tools get_trace_info, pan_timeline, zoom_timeline, set_timeline_format', async () => {
    const {trace, reg} = setup();

    const info = JSON.parse(await reg.call('get_trace_info', {}));
    expect(info.start).toBe('0');
    expect(info.end).toBe('10000000000');
    expect(info.timestampFormat).toBe(TimestampFormat.Timecode);

    const before = JSON.parse(await reg.call('get_viewport', {}));
    const zoomed = JSON.parse(await reg.call('zoom_timeline', {factor: 0.5}));
    expect(zoomed.end - zoomed.start).toBeLessThan(before.end - before.start);

    const panned = JSON.parse(
      await reg.call('pan_timeline', {deltaNs: '1000000000'}),
    );
    expect(panned.start).toBeGreaterThan(zoomed.start);

    await reg.call('set_timeline_format', {
      timestampFormat: TimestampFormat.TraceNs,
      durationPrecision: DurationPrecision.HumanReadable,
    });
    expect(trace.timeline.timestampFormat).toBe(TimestampFormat.TraceNs);
    expect(trace.timeline.durationPrecision).toBe(
      DurationPrecision.HumanReadable,
    );
  });

  test('track filter tools get_track_filters and set_track_filters', async () => {
    const {trace, reg} = setup();
    trace.tracks.registerTrackFilterCriteria({
      name: 'Kind',
      options: [
        {key: 'cpu', label: 'CPU'},
        {key: 'gpu', label: 'GPU'},
      ],
      predicate: () => true,
    });

    const updated = JSON.parse(
      await reg.call('set_track_filters', {
        nameFilter: 'CPU',
        criteriaFilters: {Kind: ['cpu']},
      }),
    );
    expect(updated.nameFilter).toBe('CPU');
    expect(updated.criteriaFilters).toEqual({Kind: ['cpu']});

    await expect(
      reg.call('set_track_filters', {criteriaFilters: {Unknown: ['x']}}),
    ).rejects.toThrow('Unknown filter criterion');

    const cleared = JSON.parse(
      await reg.call('set_track_filters', {clearAll: true}),
    );
    expect(cleared.nameFilter).toBe('');
    expect(cleared.criteriaFilters).toEqual({});
  });

  test('note tools list, add, update, remove', async () => {
    const {reg} = setup();

    const p = JSON.parse(
      await reg.call('add_note', {timestamp: '1000', text: 'point'}),
    );
    const s = JSON.parse(
      await reg.call('add_note', {
        startTime: '2000',
        endTime: '5000',
        text: 'span',
      }),
    );
    expect(p.noteType).toBe('DEFAULT');
    expect(s.noteType).toBe('SPAN');

    await reg.call('update_note', {
      id: p.id,
      text: 'updated point',
      color: '#ff0000',
    });

    const notes = JSON.parse(await reg.call('list_notes', {}));
    expect(notes).toHaveLength(2);
    expect(notes[0].text).toBe('updated point');
    expect(notes[0].color).toBe('#ff0000');

    await reg.call('remove_note', {id: p.id});
    expect(JSON.parse(await reg.call('list_notes', {}))).toHaveLength(1);

    await reg.call('remove_note', {all: true});
    expect(JSON.parse(await reg.call('list_notes', {}))).toHaveLength(0);
  });

  test('workspace and track organization tools', async () => {
    const {trace, reg} = setup();

    // Default workspace is not editable
    await expect(reg.call('rename_workspace', {title: 'Nope'})).rejects.toThrow(
      'not editable',
    );

    const ws = JSON.parse(
      await reg.call('create_workspace', {title: 'Investigation'}),
    );
    expect(trace.workspaces.currentWorkspace.id).toBe(ws.id);

    await reg.call('rename_workspace', {title: 'Investigation 2'});
    expect(trace.workspaces.currentWorkspace.title).toBe('Investigation 2');

    // Switch back to default so we can copy tracks from it
    await reg.call('switch_workspace', {title: 'Default Workspace'});
    const copied = JSON.parse(
      await reg.call('copy_tracks_to_workspace', {
        trackUris: ['perfetto.Cpu#0', 'perfetto.Proc#sf'],
        targetWorkspaceId: ws.id,
        includeAncestors: true,
        switchTo: true,
      }),
    );
    expect(copied.copiedCount).toBe(2);
    expect(trace.workspaces.currentWorkspace.id).toBe(ws.id);

    const group = JSON.parse(
      await reg.call('create_track_group', {name: 'My Group'}),
    );
    await reg.call('move_track', {
      trackUri: 'perfetto.Proc#sf',
      targetParentId: group.id,
      position: 'first',
    });
    const sfInWs = trace.currentWorkspace.getTrackByUri('perfetto.Proc#sf');
    expect(sfInWs?.parent?.id).toBe(group.id);

    await reg.call('rename_track', {
      trackUri: 'perfetto.Proc#sf',
      name: 'SF Renamed',
    });
    expect(sfInWs?.name).toBe('SF Renamed');

    await reg.call('remove_tracks', {trackUris: ['perfetto.Proc#sf']});
    expect(
      trace.currentWorkspace.getTrackByUri('perfetto.Proc#sf'),
    ).toBeUndefined();

    await reg.call('switch_workspace', {title: 'Default Workspace'});
    await reg.call('remove_workspace', {workspaceId: ws.id});
    expect(trace.workspaces.all).toHaveLength(1);
  });

  test('search_timeline and tab drawer tools', async () => {
    const {trace, reg} = setup();

    trace.tabs.registerTab({
      uri: 'test.Tab',
      content: {
        getTitle: () => 'Test Tab',
        render: () => null,
      },
    });

    const tabsBefore = JSON.parse(await reg.call('list_tabs', {}));
    expect(tabsBefore.visibility).toBe('COLLAPSED');
    expect(tabsBefore.registeredTabs).toHaveLength(1);

    const tabRes = JSON.parse(
      await reg.call('set_tab_drawer', {
        showTabUri: 'test.Tab',
        visibility: 'VISIBLE',
      }),
    );
    expect(tabRes.currentTabUri).toBe('test.Tab');
    expect(tabRes.visibility).toBe('VISIBLE');
    expect(trace.tabs.tabPanelVisibility).toBe(DrawerPanelVisibility.VISIBLE);

    const searchRes = JSON.parse(
      await reg.call('search_timeline', {query: 'choreographer'}),
    );
    expect(searchRes.searchText).toBe('choreographer');

    const clearedSearch = JSON.parse(
      await reg.call('search_timeline', {query: ''}),
    );
    expect(clearedSearch.searchText).toBe('');
  });
});
