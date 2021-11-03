// Copyright (C) 2019 The Android Open Source Project
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

import {Engine} from '../common/engine';
import {
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
  DEFAULT_VIEWING_OPTION,
  expandCallsites,
  findRootSize,
  mergeCallsites,
  OBJECTS_ALLOCATED_KEY,
  OBJECTS_ALLOCATED_NOT_FREED_KEY,
  PERF_SAMPLES_KEY,
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY
} from '../common/flamegraph_util';
import {NUM, STR} from '../common/query_result';
import {CallsiteInfo, FlamegraphState} from '../common/state';
import {fromNs} from '../common/time';
import {FlamegraphDetails} from '../frontend/globals';
import {publishFlamegraphDetails} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

export interface FlamegraphControllerArgs {
  engine: Engine;
}
const MIN_PIXEL_DISPLAYED = 1;

class TablesCache {
  private engine: Engine;
  private cache: Map<string, string>;
  private prefix: string;
  private tableId: number;
  private cacheSizeLimit: number;

  constructor(engine: Engine, prefix: string) {
    this.engine = engine;
    this.cache = new Map<string, string>();
    this.prefix = prefix;
    this.tableId = 0;
    this.cacheSizeLimit = 10;
  }

  async getTableName(query: string): Promise<string> {
    let tableName = this.cache.get(query);
    if (tableName === undefined) {
      // TODO(hjd): This should be LRU.
      if (this.cache.size > this.cacheSizeLimit) {
        for (const name of this.cache.values()) {
          await this.engine.query(`drop table ${name}`);
        }
        this.cache.clear();
      }
      tableName = `${this.prefix}_${this.tableId++}`;
      await this.engine.query(
          `create temp table if not exists ${tableName} as ${query}`);
      this.cache.set(query, tableName);
    }
    return tableName;
  }
}

export class FlamegraphController extends Controller<'main'> {
  private flamegraphDatasets: Map<string, CallsiteInfo[]> = new Map();
  private lastSelectedFlamegraphState?: FlamegraphState;
  private requestingData = false;
  private queuedRequest = false;
  private flamegraphDetails: FlamegraphDetails = {};
  private cache: TablesCache;

  constructor(private args: FlamegraphControllerArgs) {
    super('main');
    this.cache = new TablesCache(args.engine, 'grouped_callsites');
  }

  run() {
    const selection = globals.state.currentFlamegraphState;
    if (!selection || !this.shouldRequestData(selection)) {
      return;
    }
    if (this.requestingData) {
      this.queuedRequest = true;
      return;
    }
    this.requestingData = true;

    this.assembleFlamegraphDetails(selection);
  }

  private async assembleFlamegraphDetails(selection: FlamegraphState) {
    const selectedFlamegraphState = {...selection};
    const flamegraphMetadata = await this.getFlamegraphMetadata(
        selection.type,
        selectedFlamegraphState.ts,
        selectedFlamegraphState.upid);
    if (flamegraphMetadata !== undefined) {
      Object.assign(this.flamegraphDetails, flamegraphMetadata);
    }

    // TODO(hjd): Clean this up.
    if (this.lastSelectedFlamegraphState &&
        this.lastSelectedFlamegraphState.focusRegex !== selection.focusRegex) {
      this.flamegraphDatasets.clear();
    }

    this.lastSelectedFlamegraphState = {...selection};

    const expandedId = selectedFlamegraphState.expandedCallsite ?
        selectedFlamegraphState.expandedCallsite.id :
        -1;
    const rootSize = selectedFlamegraphState.expandedCallsite === undefined ?
        undefined :
        selectedFlamegraphState.expandedCallsite.totalSize;

    const key = `${selectedFlamegraphState.upid};${selectedFlamegraphState.ts}`;

    try {
      const flamegraphData = await this.getFlamegraphData(
          key,
          selectedFlamegraphState.viewingOption ?
              selectedFlamegraphState.viewingOption :
              DEFAULT_VIEWING_OPTION,
          selection.ts,
          selectedFlamegraphState.upid,
          selectedFlamegraphState.type,
          selectedFlamegraphState.focusRegex);
      if (flamegraphData !== undefined && selection &&
          selection.kind === selectedFlamegraphState.kind &&
          selection.id === selectedFlamegraphState.id &&
          selection.ts === selectedFlamegraphState.ts) {
        const expandedFlamegraphData =
            expandCallsites(flamegraphData, expandedId);
        this.prepareAndMergeCallsites(
            expandedFlamegraphData,
            this.lastSelectedFlamegraphState.viewingOption,
            rootSize,
            this.lastSelectedFlamegraphState.expandedCallsite);
      }
    } finally {
      this.requestingData = false;
      if (this.queuedRequest) {
        this.queuedRequest = false;
        this.run();
      }
    }
  }

  private shouldRequestData(selection: FlamegraphState) {
    return selection.kind === 'FLAMEGRAPH_STATE' &&
        (this.lastSelectedFlamegraphState === undefined ||
         (this.lastSelectedFlamegraphState.id !== selection.id ||
          this.lastSelectedFlamegraphState.ts !== selection.ts ||
          this.lastSelectedFlamegraphState.type !== selection.type ||
          this.lastSelectedFlamegraphState.upid !== selection.upid ||
          this.lastSelectedFlamegraphState.viewingOption !==
              selection.viewingOption ||
          this.lastSelectedFlamegraphState.focusRegex !==
              selection.focusRegex ||
          this.lastSelectedFlamegraphState.expandedCallsite !==
              selection.expandedCallsite));
  }

  private prepareAndMergeCallsites(
      flamegraphData: CallsiteInfo[],
      viewingOption: string|undefined = DEFAULT_VIEWING_OPTION,
      rootSize?: number, expandedCallsite?: CallsiteInfo) {
    this.flamegraphDetails.flamegraph = mergeCallsites(
        flamegraphData, this.getMinSizeDisplayed(flamegraphData, rootSize));
    this.flamegraphDetails.expandedCallsite = expandedCallsite;
    this.flamegraphDetails.viewingOption = viewingOption;
    publishFlamegraphDetails(this.flamegraphDetails);
  }

  async getFlamegraphData(
      baseKey: string, viewingOption: string, ts: number, upid: number,
      type: string, focusRegex: string): Promise<CallsiteInfo[]> {
    let currentData: CallsiteInfo[];
    const key = `${baseKey}-${viewingOption}`;
    if (this.flamegraphDatasets.has(key)) {
      currentData = this.flamegraphDatasets.get(key)!;
    } else {
      // TODO(hjd): Show loading state.

      // Collecting data for drawing flamegraph for selected profile.
      // Data needs to be in following format:
      // id, name, parent_id, depth, total_size
      const tableName =
          await this.prepareViewsAndTables(ts, upid, type, focusRegex);
      currentData = await this.getFlamegraphDataFromTables(
          tableName, viewingOption, focusRegex);
      this.flamegraphDatasets.set(key, currentData);
    }
    return currentData;
  }

  async getFlamegraphDataFromTables(
      tableName: string, viewingOption = DEFAULT_VIEWING_OPTION,
      focusRegex: string) {
    let orderBy = '';
    let totalColumnName: 'cumulativeSize'|'cumulativeAllocSize'|
        'cumulativeCount'|'cumulativeAllocCount' = 'cumulativeSize';
    let selfColumnName: 'size'|'count' = 'size';
    // TODO(fmayer): Improve performance so this is no longer necessary.
    // Alternatively consider collapsing frames of the same label.
    const maxDepth = 100;
    switch (viewingOption) {
      case ALLOC_SPACE_MEMORY_ALLOCATED_KEY:
        orderBy = `where cumulative_alloc_size > 0 and depth < ${
            maxDepth} order by depth, parent_id,
            cumulative_alloc_size desc, name`;
        totalColumnName = 'cumulativeAllocSize';
        selfColumnName = 'size';
        break;
      case OBJECTS_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where cumulative_count > 0 and depth < ${
            maxDepth} order by depth, parent_id,
            cumulative_count desc, name`;
        totalColumnName = 'cumulativeCount';
        selfColumnName = 'count';
        break;
      case OBJECTS_ALLOCATED_KEY:
        orderBy = `where cumulative_alloc_count > 0 and depth < ${
            maxDepth} order by depth, parent_id,
            cumulative_alloc_count desc, name`;
        totalColumnName = 'cumulativeAllocCount';
        selfColumnName = 'count';
        break;
      case PERF_SAMPLES_KEY:
      case SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where cumulative_size > 0 and depth < ${
            maxDepth} order by depth, parent_id,
            cumulative_size desc, name`;
        totalColumnName = 'cumulativeSize';
        selfColumnName = 'size';
        break;
      default:
        break;
    }

    const callsites = await this.args.engine.query(`
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
        from ${tableName} ${orderBy}`);

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
      const highlighted = focusRegex !== '' &&
          name.toLocaleLowerCase().includes(focusRegex.toLocaleLowerCase());
      const parentId =
          hashToindex.has(+parentHash) ? hashToindex.get(+parentHash)! : -1;

      let location: string|undefined;
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
        location
      });
    }
    return flamegraphData;
  }

  private async prepareViewsAndTables(
      ts: number, upid: number, type: string,
      focusRegex: string): Promise<string> {
    // Creating unique names for views so we can reuse and not delete them
    // for each marker.
    let focusRegexConditional = '';
    if (focusRegex !== '') {
      const linkingWord = type === 'perf' ? 'and' : 'where';
      focusRegexConditional = `${linkingWord} focus_str = '${focusRegex}'`;
    }

    /*
     * TODO(octaviant) this branching should be eliminated for simplicity.
     */
    if (type === 'perf') {
      return this.cache.getTableName(
          `select id, name, map_name, parent_id, depth, cumulative_size,
          cumulative_alloc_size, cumulative_count, cumulative_alloc_count,
          size, alloc_size, count, alloc_count, source_file, line_number
          from experimental_flamegraph
          where profile_type = "${type}" and ts <= ${ts} and upid = ${upid} 
          ${focusRegexConditional}`);
    }
    return this.cache.getTableName(
        `select id, name, map_name, parent_id, depth, cumulative_size,
          cumulative_alloc_size, cumulative_count, cumulative_alloc_count,
          size, alloc_size, count, alloc_count, source_file, line_number
          from experimental_flamegraph(${ts}, ${upid}, '${type}') ${
            focusRegexConditional}`);
  }

  getMinSizeDisplayed(flamegraphData: CallsiteInfo[], rootSize?: number):
      number {
    const timeState = globals.state.frontendLocalState.visibleState;
    let width = (timeState.endSec - timeState.startSec) / timeState.resolution;
    // TODO(168048193): Remove screen size hack:
    width = Math.max(width, 800);
    if (rootSize === undefined) {
      rootSize = findRootSize(flamegraphData);
    }
    return MIN_PIXEL_DISPLAYED * rootSize / width;
  }

  async getFlamegraphMetadata(type: string, ts: number, upid: number) {
    // Don't do anything if selection of the marker stayed the same.
    if ((this.lastSelectedFlamegraphState !== undefined &&
         ((this.lastSelectedFlamegraphState.ts === ts &&
           this.lastSelectedFlamegraphState.upid === upid)))) {
      return undefined;
    }

    // Collecting data for more information about profile, such as:
    // total memory allocated, memory that is allocated and not freed.
    const result = await this.args.engine.query(
        `select pid from process where upid = ${upid}`);
    const pid = result.firstRow({pid: NUM}).pid;
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    return {ts: startTime, tsNs: ts, pid, upid, type};
  }
}
