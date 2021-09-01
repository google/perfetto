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
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY
} from '../common/flamegraph_util';
import {NUM, STR} from '../common/query_result';
import {CallsiteInfo, HeapProfileFlamegraph} from '../common/state';
import {fromNs} from '../common/time';
import {HeapProfileDetails} from '../frontend/globals';
import {publishHeapProfileDetails} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

export interface HeapProfileControllerArgs {
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
          await this.engine.queryV2(`drop table ${name}`);
        }
        this.cache.clear();
      }
      tableName = `${this.prefix}_${this.tableId++}`;
      await this.engine.queryV2(
          `create temp table if not exists ${tableName} as ${query}`);
      this.cache.set(query, tableName);
    }
    return tableName;
  }
}

export class HeapProfileController extends Controller<'main'> {
  private flamegraphDatasets: Map<string, CallsiteInfo[]> = new Map();
  private lastSelectedHeapProfile?: HeapProfileFlamegraph;
  private requestingData = false;
  private queuedRequest = false;
  private heapProfileDetails: HeapProfileDetails = {};
  private cache: TablesCache;

  constructor(private args: HeapProfileControllerArgs) {
    super('main');
    this.cache = new TablesCache(args.engine, 'grouped_callsites');
  }

  run() {
    const selection = globals.state.currentHeapProfileFlamegraph;

    if (!selection) return;

    if (this.shouldRequestData(selection)) {
      if (this.requestingData) {
        this.queuedRequest = true;
      } else {
        this.requestingData = true;
        const selectedHeapProfile: HeapProfileFlamegraph =
            this.copyHeapProfile(selection);

        this.getHeapProfileMetadata(
                selection.type,
                selectedHeapProfile.ts,
                selectedHeapProfile.upid)
            .then(result => {
              if (result !== undefined) {
                Object.assign(this.heapProfileDetails, result);
              }

              // TODO(hjd): Clean this up.
              if (this.lastSelectedHeapProfile &&
                  this.lastSelectedHeapProfile.focusRegex !==
                      selection.focusRegex) {
                this.flamegraphDatasets.clear();
              }

              this.lastSelectedHeapProfile = this.copyHeapProfile(selection);

              const expandedId = selectedHeapProfile.expandedCallsite ?
                  selectedHeapProfile.expandedCallsite.id :
                  -1;
              const rootSize =
                  selectedHeapProfile.expandedCallsite === undefined ?
                  undefined :
                  selectedHeapProfile.expandedCallsite.totalSize;

              const key =
                  `${selectedHeapProfile.upid};${selectedHeapProfile.ts}`;

              this.getFlamegraphData(
                      key,
                      selectedHeapProfile.viewingOption ?
                          selectedHeapProfile.viewingOption :
                          DEFAULT_VIEWING_OPTION,
                      selection.ts,
                      selectedHeapProfile.upid,
                      selectedHeapProfile.type,
                      selectedHeapProfile.focusRegex)
                  .then(flamegraphData => {
                    if (flamegraphData !== undefined && selection &&
                        selection.kind === selectedHeapProfile.kind &&
                        selection.id === selectedHeapProfile.id &&
                        selection.ts === selectedHeapProfile.ts) {
                      const expandedFlamegraphData =
                          expandCallsites(flamegraphData, expandedId);
                      this.prepareAndMergeCallsites(
                          expandedFlamegraphData,
                          this.lastSelectedHeapProfile!.viewingOption,
                          rootSize,
                          this.lastSelectedHeapProfile!.expandedCallsite);
                    }
                  })
                  .finally(() => {
                    this.requestingData = false;
                    if (this.queuedRequest) {
                      this.queuedRequest = false;
                      this.run();
                    }
                  });
            });
      }
    }
  }

  private copyHeapProfile(heapProfile: HeapProfileFlamegraph):
      HeapProfileFlamegraph {
    return {
      kind: heapProfile.kind,
      id: heapProfile.id,
      upid: heapProfile.upid,
      ts: heapProfile.ts,
      type: heapProfile.type,
      expandedCallsite: heapProfile.expandedCallsite,
      viewingOption: heapProfile.viewingOption,
      focusRegex: heapProfile.focusRegex,
    };
  }

  private shouldRequestData(selection: HeapProfileFlamegraph) {
    return selection.kind === 'HEAP_PROFILE_FLAMEGRAPH' &&
        (this.lastSelectedHeapProfile === undefined ||
         (this.lastSelectedHeapProfile !== undefined &&
          (this.lastSelectedHeapProfile.id !== selection.id ||
           this.lastSelectedHeapProfile.ts !== selection.ts ||
           this.lastSelectedHeapProfile.type !== selection.type ||
           this.lastSelectedHeapProfile.upid !== selection.upid ||
           this.lastSelectedHeapProfile.viewingOption !==
               selection.viewingOption ||
           this.lastSelectedHeapProfile.focusRegex !== selection.focusRegex ||
           this.lastSelectedHeapProfile.expandedCallsite !==
               selection.expandedCallsite)));
  }

  private prepareAndMergeCallsites(
      flamegraphData: CallsiteInfo[],
      viewingOption: string|undefined = DEFAULT_VIEWING_OPTION,
      rootSize?: number, expandedCallsite?: CallsiteInfo) {
    const mergedFlamegraphData = mergeCallsites(
        flamegraphData, this.getMinSizeDisplayed(flamegraphData, rootSize));
    this.heapProfileDetails.flamegraph = mergedFlamegraphData;
    this.heapProfileDetails.expandedCallsite = expandedCallsite;
    this.heapProfileDetails.viewingOption = viewingOption;
    publishHeapProfileDetails(this.heapProfileDetails);
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

      // Collecting data for drawing flamegraph for selected heap profile.
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
      case SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where cumulative_size > 0 and depth < ${
            maxDepth} order by depth, parent_id,
            cumulative_size desc, name`;
        totalColumnName = 'cumulativeSize';
        selfColumnName = 'size';
        break;
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
      default:
        break;
    }

    const callsites = await this.args.engine.queryV2(`
        SELECT
        id as hash,
        IFNULL(DEMANGLE(name), name) as name,
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

    const flamegraphData: CallsiteInfo[] = new Array();
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
    let whereClause = '';
    if (focusRegex !== '') {
      whereClause = `where focus_str = '${focusRegex}'`;
    }

    return this.cache.getTableName(
        `select id, name, map_name, parent_id, depth, cumulative_size,
          cumulative_alloc_size, cumulative_count, cumulative_alloc_count,
          size, alloc_size, count, alloc_count, source_file, line_number
          from experimental_flamegraph(${ts}, ${upid}, '${type}') ${
            whereClause}`);
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

  async getHeapProfileMetadata(type: string, ts: number, upid: number) {
    // Don't do anything if selection of the marker stayed the same.
    if ((this.lastSelectedHeapProfile !== undefined &&
         ((this.lastSelectedHeapProfile.ts === ts &&
           this.lastSelectedHeapProfile.upid === upid)))) {
      return undefined;
    }

    // Collecting data for more information about heap profile, such as:
    // total memory allocated, memory that is allocated and not freed.
    const result = await this.args.engine.queryV2(
        `select pid from process where upid = ${upid}`);
    const pid = result.firstRow({pid: NUM}).pid;
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    return {ts: startTime, tsNs: ts, pid, upid, type};
  }
}
