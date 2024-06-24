// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m, {Vnode} from 'mithril';

import {findRef} from '../base/dom_utils';
import {assertExists, assertTrue} from '../base/logging';
import {Duration, time} from '../base/time';
import {Actions} from '../common/actions';
import {
  CallsiteInfo,
  FlamegraphViewingOption,
  defaultViewingOption,
  expandCallsites,
  findRootSize,
  mergeCallsites,
  viewingOptions,
} from '../common/legacy_flamegraph_util';
import {ProfileType} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {Button} from '../widgets/button';
import {Icon} from '../widgets/icon';
import {Modal, ModalAttrs} from '../widgets/modal';
import {Popup} from '../widgets/popup';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';

import {Flamegraph, NodeRendering} from './legacy_flamegraph';
import {globals} from './globals';
import {debounce} from './rate_limiters';
import {Router} from './router';
import {ButtonBar} from '../widgets/button';
import {DurationWidget} from './widgets/duration';
import {DetailsShell} from '../widgets/details_shell';
import {Intent} from '../widgets/common';
import {Engine, NUM, STR} from '../public';
import {Monitor} from '../base/monitor';
import {arrayEquals} from '../base/array_utils';
import {getCurrentTrace} from './sidebar';
import {convertTraceToPprofAndDownload} from './trace_converter';
import {AsyncLimiter} from '../base/async_limiter';
import {FlamegraphCache} from '../core/flamegraph_cache';

const HEADER_HEIGHT = 30;

export function profileType(s: string): ProfileType {
  if (isProfileType(s)) {
    return s;
  }
  if (s.startsWith('heap_profile')) {
    return ProfileType.HEAP_PROFILE;
  }
  throw new Error('Unknown type ${s}');
}

function isProfileType(s: string): s is ProfileType {
  return Object.values(ProfileType).includes(s as ProfileType);
}

function getFlamegraphType(type: ProfileType) {
  switch (type) {
    case ProfileType.HEAP_PROFILE:
    case ProfileType.MIXED_HEAP_PROFILE:
    case ProfileType.NATIVE_HEAP_PROFILE:
    case ProfileType.JAVA_HEAP_SAMPLES:
      return 'native';
    case ProfileType.JAVA_HEAP_GRAPH:
      return 'graph';
    case ProfileType.PERF_SAMPLE:
      return 'perf';
    default:
      const exhaustiveCheck: never = type;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}

const HEAP_GRAPH_DOMINATOR_TREE_VIEWING_OPTIONS = [
  FlamegraphViewingOption.DOMINATOR_TREE_OBJ_SIZE_KEY,
  FlamegraphViewingOption.DOMINATOR_TREE_OBJ_COUNT_KEY,
] as const;

export type HeapGraphDominatorTreeViewingOption =
  (typeof HEAP_GRAPH_DOMINATOR_TREE_VIEWING_OPTIONS)[number];

export function isHeapGraphDominatorTreeViewingOption(
  option: FlamegraphViewingOption,
): option is HeapGraphDominatorTreeViewingOption {
  return (
    HEAP_GRAPH_DOMINATOR_TREE_VIEWING_OPTIONS as readonly FlamegraphViewingOption[]
  ).includes(option);
}

const MIN_PIXEL_DISPLAYED = 1;

function toSelectedCallsite(c: CallsiteInfo | undefined): string {
  if (c !== undefined && c.name !== undefined) {
    return c.name;
  }
  return '(none)';
}

const RENDER_SELF_AND_TOTAL: NodeRendering = {
  selfSize: 'Self',
  totalSize: 'Total',
};
const RENDER_OBJ_COUNT: NodeRendering = {
  selfSize: 'Self objects',
  totalSize: 'Subtree objects',
};

export interface FlamegraphSelectionParams {
  readonly profileType: ProfileType;
  readonly upids: number[];
  readonly start: time;
  readonly end: time;
}

interface FlamegraphDetailsPanelAttrs {
  cache: FlamegraphCache;
  selection: FlamegraphSelectionParams;
}

interface FlamegraphResult {
  queryResults: ReadonlyArray<CallsiteInfo>;
  incomplete: boolean;
  renderResults?: ReadonlyArray<CallsiteInfo>;
}

interface FlamegraphState {
  selection: FlamegraphSelectionParams;
  viewingOption: FlamegraphViewingOption;
  focusRegex: string;
  result?: FlamegraphResult;
  selectedCallsites: Readonly<{
    [key: string]: CallsiteInfo | undefined;
  }>;
}

export class LegacyFlamegraphDetailsPanel
  implements m.ClassComponent<FlamegraphDetailsPanelAttrs>
{
  private undebouncedFocusRegex = '';
  private updateFocusRegexDebounced = debounce(() => {
    if (this.state === undefined) {
      return;
    }
    this.state.focusRegex = this.undebouncedFocusRegex;
    raf.scheduleFullRedraw();
  }, 20);

  private flamegraph: Flamegraph = new Flamegraph([]);
  private queryLimiter = new AsyncLimiter();

  private state?: FlamegraphState;
  private queryMonitor = new Monitor([
    () => this.state?.selection,
    () => this.state?.focusRegex,
    () => this.state?.viewingOption,
  ]);
  private selectedCallsitesMonitor = new Monitor([
    () => this.state?.selection,
    () => this.state?.focusRegex,
  ]);
  private renderResultMonitor = new Monitor([
    () => this.state?.result?.queryResults,
    () => this.state?.selectedCallsites,
  ]);

  view({attrs}: Vnode<FlamegraphDetailsPanelAttrs>) {
    if (attrs.selection === undefined) {
      this.state = undefined;
    } else if (
      attrs.selection.profileType !== this.state?.selection.profileType ||
      attrs.selection.start !== this.state.selection.start ||
      attrs.selection.end !== this.state.selection.end ||
      !arrayEquals(attrs.selection.upids, this.state.selection.upids)
    ) {
      this.state = {
        selection: attrs.selection,
        focusRegex: '',
        viewingOption: defaultViewingOption(attrs.selection.profileType),
        selectedCallsites: {},
      };
    }
    if (this.state === undefined) {
      return m(
        '.details-panel',
        m('.details-panel-heading', m('h2', `Flamegraph Profile`)),
      );
    }

    if (this.queryMonitor.ifStateChanged()) {
      this.state.result = undefined;
      const state = this.state;
      this.queryLimiter.schedule(() => {
        return LegacyFlamegraphDetailsPanel.fetchQueryResults(
          assertExists(this.getCurrentEngine()),
          attrs.cache,
          state,
        );
      });
    }

    if (this.selectedCallsitesMonitor.ifStateChanged()) {
      this.state.selectedCallsites = {};
    }

    if (
      this.renderResultMonitor.ifStateChanged() &&
      this.state.result !== undefined
    ) {
      const selected = this.state.selectedCallsites[this.state.viewingOption];
      const expanded = expandCallsites(
        this.state.result.queryResults,
        selected?.id ?? -1,
      );
      this.state.result.renderResults = mergeCallsites(
        expanded,
        LegacyFlamegraphDetailsPanel.getMinSizeDisplayed(
          expanded,
          selected?.totalSize,
        ),
      );
    }

    let height: number | undefined;
    if (this.state.result?.renderResults !== undefined) {
      this.flamegraph.updateDataIfChanged(
        this.nodeRendering(),
        this.state.result.renderResults,
        this.state.selectedCallsites[this.state.viewingOption],
      );
      height = this.flamegraph.getHeight() + HEADER_HEIGHT;
    } else {
      height = undefined;
    }

    return m(
      '.flamegraph-profile',
      this.maybeShowModal(),
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m(
            'div.title',
            this.getTitle(),
            this.state.selection.profileType ===
              ProfileType.MIXED_HEAP_PROFILE &&
              m(
                Popup,
                {
                  trigger: m(Icon, {icon: 'warning'}),
                },
                m(
                  '',
                  {style: {width: '300px'}},
                  'This is a mixed java/native heap profile, free()s are not visualized. To visualize free()s, remove "all_heaps: true" from the config.',
                ),
              ),
            ':',
          ),
          description: this.getViewingOptionButtons(),
          buttons: [
            m(
              'div.selected',
              `Selected function: ${toSelectedCallsite(
                this.state.selectedCallsites[this.state.viewingOption],
              )}`,
            ),
            m(
              'div.time',
              `Snapshot time: `,
              m(DurationWidget, {
                dur: this.state.selection.end - this.state.selection.start,
              }),
            ),
            m('input[type=text][placeholder=Focus]', {
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                this.undebouncedFocusRegex = target.value;
                this.updateFocusRegexDebounced();
              },
              // Required to stop hot-key handling:
              onkeydown: (e: Event) => e.stopPropagation(),
            }),
            (this.state.selection.profileType ===
              ProfileType.NATIVE_HEAP_PROFILE ||
              this.state.selection.profileType ===
                ProfileType.JAVA_HEAP_SAMPLES) &&
              m(Button, {
                icon: 'file_download',
                intent: Intent.Primary,
                onclick: () => {
                  this.downloadPprof();
                  raf.scheduleFullRedraw();
                },
              }),
          ],
        },
        m(
          '.flamegraph-content',
          this.state.result === undefined
            ? m(
                '.loading-container',
                m(
                  EmptyState,
                  {
                    icon: 'bar_chart',
                    title: 'Computing graph ...',
                    className: 'flamegraph-loading',
                  },
                  m(Spinner, {easing: true}),
                ),
              )
            : m(`canvas[ref=canvas]`, {
                style: `height:${height}px; width:100%`,
                onmousemove: (e: MouseEvent) => {
                  const {offsetX, offsetY} = e;
                  this.flamegraph.onMouseMove({x: offsetX, y: offsetY});
                  raf.scheduleFullRedraw();
                },
                onmouseout: () => {
                  this.flamegraph.onMouseOut();
                  raf.scheduleFullRedraw();
                },
                onclick: (e: MouseEvent) => {
                  if (
                    this.state === undefined ||
                    this.state.result === undefined
                  ) {
                    return;
                  }
                  const {offsetX, offsetY} = e;
                  const cs = {...this.state.selectedCallsites};
                  cs[this.state.viewingOption] = this.flamegraph.onMouseClick({
                    x: offsetX,
                    y: offsetY,
                  });
                  this.state.selectedCallsites = cs;
                  raf.scheduleFullRedraw();
                },
              }),
        ),
      ),
    );
  }

  private getTitle(): string {
    const state = assertExists(this.state);
    switch (state.selection.profileType) {
      case ProfileType.MIXED_HEAP_PROFILE:
        return 'Mixed heap profile';
      case ProfileType.HEAP_PROFILE:
        return 'Heap profile';
      case ProfileType.NATIVE_HEAP_PROFILE:
        return 'Native heap profile';
      case ProfileType.JAVA_HEAP_SAMPLES:
        return 'Java heap samples';
      case ProfileType.JAVA_HEAP_GRAPH:
        return 'Java heap graph';
      case ProfileType.PERF_SAMPLE:
        return 'Profile';
      default:
        throw new Error('unknown type');
    }
  }

  private nodeRendering(): NodeRendering {
    const state = assertExists(this.state);
    const profileType = state.selection.profileType;
    switch (profileType) {
      case ProfileType.JAVA_HEAP_GRAPH:
        if (
          state.viewingOption ===
            FlamegraphViewingOption.OBJECTS_ALLOCATED_NOT_FREED_KEY ||
          state.viewingOption ===
            FlamegraphViewingOption.DOMINATOR_TREE_OBJ_COUNT_KEY
        ) {
          return RENDER_OBJ_COUNT;
        } else {
          return RENDER_SELF_AND_TOTAL;
        }
      case ProfileType.MIXED_HEAP_PROFILE:
      case ProfileType.HEAP_PROFILE:
      case ProfileType.NATIVE_HEAP_PROFILE:
      case ProfileType.JAVA_HEAP_SAMPLES:
      case ProfileType.PERF_SAMPLE:
        return RENDER_SELF_AND_TOTAL;
      default:
        const exhaustiveCheck: never = profileType;
        throw new Error(`Unhandled case: ${exhaustiveCheck}`);
    }
  }

  private getViewingOptionButtons(): m.Children {
    const ret = [];
    const state = assertExists(this.state);
    for (const {option, name} of viewingOptions(state.selection.profileType)) {
      ret.push(
        m(Button, {
          label: name,
          active: option === state.viewingOption,
          onclick: () => {
            const state = assertExists(this.state);
            state.viewingOption = option;
            raf.scheduleFullRedraw();
          },
        }),
      );
    }
    return m(ButtonBar, ret);
  }

  onupdate({dom}: m.VnodeDOM<FlamegraphDetailsPanelAttrs>) {
    const canvas = findRef(dom, 'canvas');
    if (canvas === null || !(canvas instanceof HTMLCanvasElement)) {
      return;
    }
    if (!this.state?.result?.renderResults) {
      return;
    }
    canvas.width = canvas.offsetWidth * devicePixelRatio;
    canvas.height = canvas.offsetHeight * devicePixelRatio;

    const ctx = canvas.getContext('2d');
    if (ctx === null) {
      return;
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.save();
    ctx.scale(devicePixelRatio, devicePixelRatio);
    const {offsetWidth: width, offsetHeight: height} = canvas;
    const unit =
      this.state.viewingOption ===
        FlamegraphViewingOption.SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY ||
      this.state.viewingOption ===
        FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY ||
      this.state.viewingOption ===
        FlamegraphViewingOption.DOMINATOR_TREE_OBJ_SIZE_KEY
        ? 'B'
        : '';
    this.flamegraph.draw(ctx, width, height, 0, 0, unit);
    ctx.restore();
  }

  private static async fetchQueryResults(
    engine: Engine,
    cache: FlamegraphCache,
    state: FlamegraphState,
  ) {
    const table = await LegacyFlamegraphDetailsPanel.prepareViewsAndTables(
      engine,
      cache,
      state,
    );
    const queryResults =
      await LegacyFlamegraphDetailsPanel.getFlamegraphDataFromTables(
        engine,
        table,
        state.viewingOption,
        state.focusRegex,
      );

    let incomplete = false;
    if (state.selection.profileType === ProfileType.JAVA_HEAP_GRAPH) {
      const it = await engine.query(`
        select value from stats
        where severity = 'error' and name = 'heap_graph_non_finalized_graph'
      `);
      incomplete = it.firstRow({value: NUM}).value > 0;
    }
    state.result = {
      queryResults,
      incomplete,
    };
    raf.scheduleFullRedraw();
  }

  private static async prepareViewsAndTables(
    engine: Engine,
    cache: FlamegraphCache,
    state: FlamegraphState,
  ): Promise<string> {
    const flamegraphType = getFlamegraphType(state.selection.profileType);
    if (state.selection.profileType === ProfileType.PERF_SAMPLE) {
      let upid: string;
      let upidGroup: string;
      if (state.selection.upids.length > 1) {
        upid = `NULL`;
        upidGroup = `'${this.serializeUpidGroup(state.selection.upids)}'`;
      } else {
        upid = `${state.selection.upids[0]}`;
        upidGroup = `NULL`;
      }
      return cache.getTableName(
        engine,
        `
          select
            id,
            name,
            map_name,
            parent_id,
            depth,
            cumulative_size,
            cumulative_alloc_size,
            cumulative_count,
            cumulative_alloc_count,
            size,
            alloc_size,
            count,
            alloc_count,
            source_file,
            line_number
          from experimental_flamegraph(
            '${flamegraphType}',
            NULL,
            '>=${state.selection.start},<=${state.selection.end}',
            ${upid},
            ${upidGroup},
            '${state.focusRegex}'
          )
        `,
      );
    }
    if (
      state.selection.profileType === ProfileType.JAVA_HEAP_GRAPH &&
      isHeapGraphDominatorTreeViewingOption(state.viewingOption)
    ) {
      assertTrue(state.selection.start == state.selection.end);
      return cache.getTableName(
        engine,
        await this.loadHeapGraphDominatorTreeQuery(
          engine,
          cache,
          state.selection.upids[0],
          state.selection.start,
        ),
      );
    }
    assertTrue(state.selection.start == state.selection.end);
    return cache.getTableName(
      engine,
      `
        select
          id,
          name,
          map_name,
          parent_id,
          depth,
          cumulative_size,
          cumulative_alloc_size,
          cumulative_count,
          cumulative_alloc_count,
          size,
          alloc_size,
          count,
          alloc_count,
          source_file,
          line_number
        from experimental_flamegraph(
          '${flamegraphType}',
          ${state.selection.start},
          NULL,
          ${state.selection.upids[0]},
          NULL,
          '${state.focusRegex}'
        )
      `,
    );
  }

  private static async loadHeapGraphDominatorTreeQuery(
    engine: Engine,
    cache: FlamegraphCache,
    upid: number,
    timestamp: time,
  ) {
    const outputTableName = `heap_graph_type_dominated_${upid}_${timestamp}`;
    const outputQuery = `SELECT * FROM ${outputTableName}`;
    if (cache.hasQuery(outputQuery)) {
      return outputQuery;
    }

    await engine.query(`
      INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

      -- heap graph dominator tree with objects as nodes and all relavant
      -- object self stats and dominated stats
      CREATE PERFETTO TABLE _heap_graph_object_dominated AS
      SELECT
      node.id,
      node.idom_id,
      node.dominated_obj_count,
      node.dominated_size_bytes + node.dominated_native_size_bytes AS dominated_size,
      node.depth,
      obj.type_id,
      obj.root_type,
      obj.self_size + obj.native_size AS self_size
      FROM heap_graph_dominator_tree node
      JOIN heap_graph_object obj USING(id)
      WHERE obj.upid = ${upid} AND obj.graph_sample_ts = ${timestamp}
      -- required to accelerate the recursive cte below
      ORDER BY idom_id;

      -- calculate for each object node in the dominator tree the
      -- HASH(path of type_id's from the super root to the object)
      CREATE PERFETTO TABLE _dominator_tree_path_hash AS
      WITH RECURSIVE _tree_visitor(id, path_hash) AS (
        SELECT
          id,
          HASH(
            CAST(type_id AS TEXT) || '-' || IFNULL(root_type, '')
          ) AS path_hash
        FROM _heap_graph_object_dominated
        WHERE depth = 1
        UNION ALL
        SELECT
          child.id,
          HASH(CAST(parent.path_hash AS TEXT) || '/' || CAST(type_id AS TEXT)) AS path_hash
        FROM _heap_graph_object_dominated child
        JOIN _tree_visitor parent ON child.idom_id = parent.id
      )
      SELECT * from _tree_visitor
      ORDER BY id;

      -- merge object nodes with the same path into one "class type node", so the
      -- end result is a tree where nodes are identified by their types and the
      -- dominator relationships are preserved.
      CREATE PERFETTO TABLE ${outputTableName} AS
      SELECT
        map.path_hash as id,
        COALESCE(cls.deobfuscated_name, cls.name, '[NULL]') || IIF(
          node.root_type IS NOT NULL,
          ' [' || node.root_type || ']', ''
        ) AS name,
        IFNULL(parent_map.path_hash, -1) AS parent_id,
        node.depth - 1 AS depth,
        sum(dominated_size) AS cumulative_size,
        -1 AS cumulative_alloc_size,
        sum(dominated_obj_count) AS cumulative_count,
        -1 AS cumulative_alloc_count,
        '' as map_name,
        '' as source_file,
        -1 as line_number,
        sum(self_size) AS size,
        count(*) AS count
      FROM _heap_graph_object_dominated node
      JOIN _dominator_tree_path_hash map USING(id)
      LEFT JOIN _dominator_tree_path_hash parent_map ON node.idom_id = parent_map.id
      JOIN heap_graph_class cls ON node.type_id = cls.id
      GROUP BY map.path_hash, name, parent_id, depth, map_name, source_file, line_number;

      -- These are intermediates and not needed
      DROP TABLE _heap_graph_object_dominated;
      DROP TABLE _dominator_tree_path_hash;
    `);

    return outputQuery;
  }

  private static async getFlamegraphDataFromTables(
    engine: Engine,
    tableName: string,
    viewingOption: FlamegraphViewingOption,
    focusRegex: string,
  ) {
    let orderBy = '';
    let totalColumnName:
      | 'cumulativeSize'
      | 'cumulativeAllocSize'
      | 'cumulativeCount'
      | 'cumulativeAllocCount' = 'cumulativeSize';
    let selfColumnName: 'size' | 'count' = 'size';
    // TODO(fmayer): Improve performance so this is no longer necessary.
    // Alternatively consider collapsing frames of the same label.
    const maxDepth = 100;
    switch (viewingOption) {
      case FlamegraphViewingOption.ALLOC_SPACE_MEMORY_ALLOCATED_KEY:
        orderBy = `where cumulative_alloc_size > 0 and depth < ${maxDepth} order by depth, parent_id,
            cumulative_alloc_size desc, name`;
        totalColumnName = 'cumulativeAllocSize';
        selfColumnName = 'size';
        break;
      case FlamegraphViewingOption.OBJECTS_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where cumulative_count > 0 and depth < ${maxDepth} order by depth, parent_id,
            cumulative_count desc, name`;
        totalColumnName = 'cumulativeCount';
        selfColumnName = 'count';
        break;
      case FlamegraphViewingOption.OBJECTS_ALLOCATED_KEY:
        orderBy = `where cumulative_alloc_count > 0 and depth < ${maxDepth} order by depth, parent_id,
            cumulative_alloc_count desc, name`;
        totalColumnName = 'cumulativeAllocCount';
        selfColumnName = 'count';
        break;
      case FlamegraphViewingOption.PERF_SAMPLES_KEY:
      case FlamegraphViewingOption.SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where cumulative_size > 0 and depth < ${maxDepth} order by depth, parent_id,
            cumulative_size desc, name`;
        totalColumnName = 'cumulativeSize';
        selfColumnName = 'size';
        break;
      case FlamegraphViewingOption.DOMINATOR_TREE_OBJ_COUNT_KEY:
        orderBy = `where depth < ${maxDepth} order by depth,
          cumulativeCount desc, name`;
        totalColumnName = 'cumulativeCount';
        selfColumnName = 'count';
        break;
      case FlamegraphViewingOption.DOMINATOR_TREE_OBJ_SIZE_KEY:
        orderBy = `where depth < ${maxDepth} order by depth,
          cumulativeSize desc, name`;
        totalColumnName = 'cumulativeSize';
        selfColumnName = 'size';
        break;
      default:
        const exhaustiveCheck: never = viewingOption;
        throw new Error(`Unhandled case: ${exhaustiveCheck}`);
        break;
    }

    const callsites = await engine.query(`
      SELECT
        id as hash,
        IFNULL(IFNULL(DEMANGLE(name), name), '[NULL]') as name,
        IFNULL(parent_id, -1) as parentHash,
        depth,
        cumulative_size as cumulativeSize,
        cumulative_alloc_size as cumulativeAllocSize,
        cumulative_count as cumulativeCount,
        cumulative_alloc_count as cumulativeAllocCount,
        map_name as mapping,
        size,
        count,
        IFNULL(source_file, '') as sourceFile,
        IFNULL(line_number, -1) as lineNumber
      from ${tableName}
      ${orderBy}
    `);

    const flamegraphData: CallsiteInfo[] = [];
    const hashToindex: Map<number, number> = new Map();
    const it = callsites.iter({
      hash: NUM,
      name: STR,
      parentHash: NUM,
      depth: NUM,
      cumulativeSize: NUM,
      cumulativeAllocSize: NUM,
      cumulativeCount: NUM,
      cumulativeAllocCount: NUM,
      mapping: STR,
      sourceFile: STR,
      lineNumber: NUM,
      size: NUM,
      count: NUM,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const hash = it.hash;
      let name = it.name;
      const parentHash = it.parentHash;
      const depth = it.depth;
      const totalSize = it[totalColumnName];
      const selfSize = it[selfColumnName];
      const mapping = it.mapping;
      const highlighted =
        focusRegex !== '' &&
        name.toLocaleLowerCase().includes(focusRegex.toLocaleLowerCase());
      const parentId = hashToindex.has(+parentHash)
        ? hashToindex.get(+parentHash)!
        : -1;

      let location: string | undefined;
      if (/[a-zA-Z]/i.test(it.sourceFile)) {
        location = it.sourceFile;
        if (it.lineNumber !== -1) {
          location += `:${it.lineNumber}`;
        }
      }

      if (depth === maxDepth - 1) {
        name += ' [tree truncated]';
      }
      // Instead of hash, we will store index of callsite in this original array
      // as an id of callsite. That way, we have quicker access to parent and it
      // will stay unique:
      hashToindex.set(hash, i);

      flamegraphData.push({
        id: i,
        totalSize,
        depth,
        parentId,
        name,
        selfSize,
        mapping,
        merged: false,
        highlighted,
        location,
      });
    }
    return flamegraphData;
  }

  private async downloadPprof() {
    if (this.state === undefined) {
      return;
    }
    const engine = this.getCurrentEngine();
    if (engine === undefined) {
      return;
    }
    try {
      assertTrue(
        this.state.selection.upids.length === 1,
        'Native profiles can only contain one pid.',
      );
      const pid = await engine.query(
        `select pid from process where upid = ${this.state.selection.upids[0]}`,
      );
      const trace = await getCurrentTrace();
      convertTraceToPprofAndDownload(
        trace,
        pid.firstRow({pid: NUM}).pid,
        this.state.selection.start,
      );
    } catch (error) {
      throw new Error(`Failed to get current trace ${error}`);
    }
  }

  private maybeShowModal() {
    const state = assertExists(this.state);
    if (state.result?.incomplete === undefined || !state.result.incomplete) {
      return undefined;
    }
    if (globals.state.flamegraphModalDismissed) {
      return undefined;
    }
    return m(Modal, {
      title: 'The flamegraph is incomplete',
      vAlign: 'TOP',
      content: m(
        'div',
        'The current trace does not have a fully formed flamegraph',
      ),
      buttons: [
        {
          text: 'Show the errors',
          primary: true,
          action: () => Router.navigate('#!/info'),
        },
        {
          text: 'Skip',
          action: () => {
            globals.dispatch(Actions.dismissFlamegraphModal({}));
            raf.scheduleFullRedraw();
          },
        },
      ],
    } as ModalAttrs);
  }

  private static getMinSizeDisplayed(
    flamegraphData: ReadonlyArray<CallsiteInfo>,
    rootSize?: number,
  ): number {
    const timeState = globals.state.frontendLocalState.visibleState;
    const dur = globals.stateVisibleTime().duration;
    // TODO(stevegolton): Does this actually do what we want???
    let width = Duration.toSeconds(dur / timeState.resolution);
    // TODO(168048193): Remove screen size hack:
    width = Math.max(width, 800);
    if (rootSize === undefined) {
      rootSize = findRootSize(flamegraphData);
    }
    return (MIN_PIXEL_DISPLAYED * rootSize) / width;
  }

  private static serializeUpidGroup(upids: number[]) {
    return new Array(upids).join();
  }

  private getCurrentEngine() {
    const engineId = globals.getCurrentEngine()?.id;
    if (engineId === undefined) return undefined;
    return globals.engines.get(engineId);
  }
}
