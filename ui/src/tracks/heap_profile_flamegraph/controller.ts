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

import {Actions} from '../../common/actions';
import {globals} from '../../controller/globals';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';
import {findRootSize} from '../../frontend/flamegraph';
import {CallsiteInfo} from '../../frontend/globals';

import {
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
  Config,
  Data,
  DEFAULT_VIEWING_OPTION,
  HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND,
  HeapProfileFlamegraphKey,
  OBJECTS_ALLOCATED_KEY,
  OBJECTS_ALLOCATED_NOT_FREED_KEY,
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY,
} from './common';

const MIN_PIXEL_DISPLAYED = 1;

export function expandCallsites(
    data: CallsiteInfo[], clickedCallsiteIndex: number): CallsiteInfo[] {
  if (clickedCallsiteIndex === -1) return data;
  const expandedCallsites: CallsiteInfo[] = [];
  if (clickedCallsiteIndex >= data.length || clickedCallsiteIndex < -1) {
    return expandedCallsites;
  }
  const clickedCallsite = data[clickedCallsiteIndex];
  expandedCallsites.unshift(clickedCallsite);
  // Adding parents
  let parentId = clickedCallsite.parentId;
  while (parentId > -1) {
    expandedCallsites.unshift(data[parentId]);
    parentId = data[parentId].parentId;
  }
  // Adding children
  const parents: number[] = [];
  parents.push(clickedCallsiteIndex);
  for (let i = clickedCallsiteIndex + 1; i < data.length; i++) {
    const element = data[i];
    if (parents.includes(element.parentId)) {
      expandedCallsites.push(element);
      parents.push(element.id);
    }
  }
  return expandedCallsites;
}

// Merge callsites that have approximately width less than
// MIN_PIXEL_DISPLAYED. All small callsites in the same depth and with same
// parent will be merged to one callsite with size of the biggest merged
// callsite.
export function mergeCallsites(data: CallsiteInfo[], minSizeDisplayed: number) {
  const mergedData: CallsiteInfo[] = [];
  const mergedCallsites: Map<number, number> = new Map();
  for (let i = 0; i < data.length; i++) {
    // When a small callsite is found, it will be merged with other small
    // callsites of the same depth. So if the current callsite has already been
    // merged we can skip it.
    if (mergedCallsites.has(data[i].id)) {
      continue;
    }
    const copiedCallsite = copyCallsite(data[i]);
    copiedCallsite.parentId =
        getCallsitesParentHash(copiedCallsite, mergedCallsites);

    // If current callsite is small, find other small callsites with same depth
    // and parent and merge them into the current one, marking them as merged.
    if (copiedCallsite.totalSize <= minSizeDisplayed && i + 1 < data.length) {
      let j = i + 1;
      let nextCallsite = data[j];
      while (j < data.length && copiedCallsite.depth === nextCallsite.depth) {
        if (copiedCallsite.parentId ===
                getCallsitesParentHash(nextCallsite, mergedCallsites) &&
            nextCallsite.totalSize <= minSizeDisplayed) {
          copiedCallsite.totalSize += nextCallsite.totalSize;
          mergedCallsites.set(nextCallsite.id, copiedCallsite.id);
        }
        j++;
        nextCallsite = data[j];
      }
    }
    mergedData.push(copiedCallsite);
  }
  return mergedData;
}

function copyCallsite(callsite: CallsiteInfo): CallsiteInfo {
  return {
    id: callsite.id,
    parentId: callsite.parentId,
    depth: callsite.depth,
    name: callsite.name,
    totalSize: callsite.totalSize,
    mapping: callsite.mapping,
    selfSize: callsite.selfSize
  };
}

function getCallsitesParentHash(
    callsite: CallsiteInfo, map: Map<number, number>): number {
  return map.has(callsite.parentId) ? map.get(callsite.parentId)! :
                                      callsite.parentId;
}

function getMinSizeDisplayed(
    flamegraphData: CallsiteInfo[], rootSize?: number): number {
  const timeState = globals.state.frontendLocalState.visibleState;
  const width = (timeState.endSec - timeState.startSec) / timeState.resolution;
  if (rootSize === undefined) {
    rootSize = findRootSize(flamegraphData);
  }
  return MIN_PIXEL_DISPLAYED * rootSize / width;
}

class HeapProfileFlameraphTrackController extends
    TrackController<Config, Data> {
  static readonly kind = HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND;
  private start = 0;
  private end = 0;
  private resolution = 0;
  private length = 0;
  private lastSelectedTs?: number;
  private lastSelectedId?: number;
  private lastExpandedId?: number;
  private lastViewingOption?: string;

  private flamegraphDatasets: Map<string, CallsiteInfo[]> = new Map();

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    this.start = start;
    this.end = end;
    this.resolution = resolution;

    return this.generateEmptyData();
  }

  private generateEmptyData(): Data {
    const data: Data = {
      start: -1,
      end: -1,
      resolution: this.resolution,
      length: 0,
      flamegraph: []
    };
    return data;
  }

  run() {
    const selection = globals.state.currentHeapProfileFlamegraph;

    if (selection && selection.kind === 'HEAP_PROFILE_FLAMEGRAPH') {
      if (this.lastSelectedId !== selection.id ||
          this.lastSelectedTs !== selection.ts) {
        const selectedId = selection.id;
        const selectedUpid = selection.upid;
        const selectedKind = selection.kind;
        const selectedTs = selection.ts;
        // If we opened new heap profile, we don't want to show it expanded, but
        // if we are opening trace for the first time with existing state (ie.
        // via link), we want to show it expanded.
        this.lastExpandedId = !this.lastSelectedTs && this.config.expandedId ?
            this.config.expandedId :
            -1;
        this.lastSelectedId = selection.id;
        this.lastSelectedTs = selection.ts;
        this.lastViewingOption = this.config.viewingOption ?
            this.config.viewingOption :
            DEFAULT_VIEWING_OPTION;

        this.config.ts = selectedTs;
        this.config.upid = selectedUpid;
        this.config.expandedId = this.lastExpandedId;

        const key = `${selectedUpid};${selectedTs}`;

        // TODO(tneda): Prevent lots of flamegraph queries being queued if a
        // user clicks lots of the markers quickly.
        this.getFlamegraphData(
                key, this.lastViewingOption, selection.ts, selectedUpid)
            .then(flamegraphData => {
              if (flamegraphData !== undefined && selection &&
                  selection.kind === selectedKind &&
                  selection.id === selectedId && selection.ts === selectedTs) {
                this.prepareAndMergeCallsites(
                    flamegraphData, this.lastViewingOption);
                globals.dispatch(Actions.updateTrackConfig(
                    {id: this.trackState.id, config: this.config}));
              }
            });
      } else if (
          this.config.expandedId &&
          this.config.expandedId !== this.lastExpandedId) {
        const key = `${this.config.upid};${this.lastSelectedTs}`;
        this.lastExpandedId = this.config.expandedId;
        this.getFlamegraphData(
                key,
                this.config.viewingOption,
                this.lastSelectedTs,
                this.config.upid)
            .then(flamegraphData => {
              this.prepareAndMergeCallsites(flamegraphData, key);
            });
      } else if (this.config.viewingOption !== this.lastViewingOption) {
        const key = `${this.config.upid};${this.lastSelectedTs}`;
        this.lastViewingOption = this.config.viewingOption;
        this.config.expandedId = -1;
        this.getFlamegraphData(
                key,
                this.config.viewingOption,
                this.lastSelectedTs,
                this.config.upid)
            .then(flamegraphData => {
              this.prepareAndMergeCallsites(
                  flamegraphData, this.lastViewingOption);
            });
        globals.dispatch(Actions.updateTrackConfig(
            {id: this.trackState.id, config: this.config}));
      }
    } else {
      globals.publish(
          'TrackData', {id: HeapProfileFlamegraphKey, data: undefined});
    }
  }

  private publishEmptyData() {
    globals.publish(
        'TrackData',
        {id: HeapProfileFlamegraphKey, data: this.generateEmptyData()});
  }

  private prepareAndMergeCallsites(
      flamegraphData: CallsiteInfo[],
      viewingOption: string|undefined = DEFAULT_VIEWING_OPTION) {
    const expandedFlamegraphData =
        expandCallsites(flamegraphData, this.config.expandedId);
    const expandedCallsite = this.config.expandedId === -1 ?
        undefined :
        flamegraphData[this.config.expandedId];

    const rootSize =
        expandedCallsite === undefined ? undefined : expandedCallsite.totalSize;

    const mergedFlamegraphData = mergeCallsites(
        expandedFlamegraphData,
        getMinSizeDisplayed(expandedFlamegraphData, rootSize));

    globals.publish('TrackData', {
      id: HeapProfileFlamegraphKey,
      data: {
        start: this.start,
        end: this.end,
        resolution: this.resolution,
        length: this.length,
        flamegraph: mergedFlamegraphData,
        clickedCallsite: expandedCallsite,
        viewingOption
      }
    });
  }


  async getFlamegraphData(
      baseKey: string, viewingOption: string, ts: number,
      upid: number): Promise<CallsiteInfo[]> {
    let currentData: CallsiteInfo[];
    const key = `${baseKey}-${viewingOption}`;
    if (this.flamegraphDatasets.has(key)) {
      currentData = this.flamegraphDatasets.get(key)!;
    } else {
      // Sending empty data to show Loading state before we get an actual
      // data.
      this.publishEmptyData();

      // Collecting data for drawing flamegraph for selected heap profile.
      // Data needs to be in following format:
      // id, name, parent_id, depth, total_size
      const tableName = await this.prepareViewsAndTables(ts, upid);
      currentData =
          await this.getFlamegraphDataFromTables(tableName, viewingOption);
      this.flamegraphDatasets.set(key, currentData);
    }
    return currentData;
  }

  async getFlamegraphDataFromTables(
      tableName: string, viewingOption = DEFAULT_VIEWING_OPTION) {
    let orderBy = '';
    let sizeIndex = 4;
    switch (viewingOption) {
      case SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where size > 0 order by depth, parent_hash, size desc, name`;
        sizeIndex = 4;
        break;
      case ALLOC_SPACE_MEMORY_ALLOCATED_KEY:
        orderBy =
            `where alloc_size > 0 order by depth, parent_hash, alloc_size desc,
            name`;
        sizeIndex = 5;
        break;
      case OBJECTS_ALLOCATED_NOT_FREED_KEY:
        orderBy =
            `where count > 0 order by depth, parent_hash, count desc, name`;
        sizeIndex = 6;
        break;
      case OBJECTS_ALLOCATED_KEY:
        orderBy = `where alloc_count > 0 order by depth, parent_hash,
            alloc_count desc, name`;
        sizeIndex = 7;
        break;
      default:
        break;
    }

    const callsites = await this.query(
        `SELECT hash, name, parent_hash, depth, size, alloc_size, count,
        alloc_count, map_name, self_size from ${tableName} ${orderBy}`);

    const flamegraphData: CallsiteInfo[] = new Array();
    const hashToindex: Map<number, number> = new Map();
    for (let i = 0; i < callsites.numRecords; i++) {
      const hash = callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const parentHash = callsites.columns[2].longValues![i];
      const depth = +callsites.columns[3].longValues![i];
      const totalSize = +callsites.columns[sizeIndex].longValues![i];
      const mapping = callsites.columns[8].stringValues![i];
      const selfSize = +callsites.columns[9].longValues![i];
      const parentId =
          hashToindex.has(+parentHash) ? hashToindex.get(+parentHash)! : -1;
      hashToindex.set(+hash, i);
      // Instead of hash, we will store index of callsite in this original array
      // as an id of callsite. That way, we have quicker access to parent and it
      // will stay unique.
      flamegraphData.push(
          {id: i, totalSize, depth, parentId, name, selfSize, mapping});
    }
    return flamegraphData;
  }

  private async prepareViewsAndTables(ts: number, upid: number):
      Promise<string> {
    // Creating unique names for views so we can reuse and not delete them
    // for each marker.
    const tableNameCallsiteNameSize =
        this.tableName(`callsite_with_name_and_size_${ts}`);
    const tableNameCallsiteHashNameSize =
        this.tableName(`callsite_hash_name_size_${ts}`);
    const tableNameGroupedCallsitesForFlamegraph =
        this.tableName(`grouped_callsites_for_flamegraph${ts}`);
    // Joining the callsite table with frame table then with alloc table to get
    // the size and name for each callsite.
    // TODO(tneda): Make frame name nullable in the trace processor for
    // consistency with the other columns.
    await this.query(`create view if not exists ${tableNameCallsiteNameSize} as
         select id, parent_id, depth, IFNULL(DEMANGLE(name), name) as name,
            map_name, size, alloc_size, count, alloc_count from (
         select cs.id as id, parent_id, depth,
            coalesce(symbols.name,
                case when fr.name != '' then fr.name else map.name end) as name,
            map.name as map_name,
            SUM(IFNULL(size, 0)) as size,
            SUM(IFNULL(size, 0)) as size,
            SUM(case when size > 0 then size else 0 end) as alloc_size,
            SUM(IFNULL(count, 0)) as count,
            SUM(case when count > 0 then count else 0 end) as alloc_count
         from stack_profile_callsite cs
         join stack_profile_frame fr on cs.frame_id = fr.id
         join stack_profile_mapping map on fr.mapping = map.id
         inner join (
              select symbol_set_id, FIRST_VALUE(name) OVER(PARTITION BY
                symbol_set_id) as name
              from stack_profile_symbol GROUP BY symbol_set_id
            ) as symbols using(symbol_set_id)
         left join heap_profile_allocation alloc on alloc.callsite_id = cs.id
         and alloc.ts <= ${ts} and alloc.upid = ${upid} group by cs.id)`);

    // Recursive query to compute the hash for each callsite based on names
    // rather than ids.
    // We get all the children of the row in question and emit a row with hash
    // equal hash(name, parent.hash). Roots without the parent will have -1 as
    // hash.  Slices will be merged into a big slice.
    await this.query(
        `create view if not exists ${tableNameCallsiteHashNameSize} as
        with recursive callsite_table_names(
          id, hash, name, map_name, size, alloc_size, count, alloc_count,
          parent_hash, depth) AS (
        select id, hash(name) as hash, name, map_name, size, alloc_size, count,
          alloc_count, -1, depth
        from ${tableNameCallsiteNameSize}
        where depth = 0
        union all
        select cs.id, hash(cs.name, ctn.hash) as hash, cs.name, cs.map_name,
          cs.size, cs.alloc_size, cs.count, cs.alloc_count, ctn.hash, cs.depth
        from callsite_table_names ctn
        inner join ${tableNameCallsiteNameSize} cs ON ctn.id = cs.parent_id
        )
        select hash, name, map_name, parent_hash, depth, SUM(size) as size,
          SUM(case when alloc_size > 0 then alloc_size else 0 end)
            as alloc_size, SUM(count) as count,
          SUM(case when alloc_count > 0 then alloc_count else 0 end)
            as alloc_count
        from callsite_table_names
        group by hash`);

    // Recursive query to compute the cumulative size of each callsite.
    // Base case: We get all the callsites where the size is non-zero.
    // Recursive case: We get the callsite which is the parent of the current
    //  callsite(in terms of hashes) and emit a row with the size of the current
    //  callsite plus all the info of the parent.
    // Grouping: For each callsite, our recursive table has n rows where n is
    //  the number of descendents with a non-zero self size. We need to group on
    //  the hash and sum all the sizes to get the cumulative size for each
    //  callsite hash.
    await this.query(`create temp table if not exists ${
        tableNameGroupedCallsitesForFlamegraph}
        as with recursive callsite_children(
          hash, name, map_name, parent_hash, depth, size, alloc_size, count,
          alloc_count, self_size, self_alloc_size, self_count, self_alloc_count)
        as (
        select hash, name, map_name, parent_hash, depth, size, alloc_size,
          count, alloc_count, size as self_size, alloc_size as self_alloc_size,
          count as self_count, alloc_count as self_alloc_count
        from ${tableNameCallsiteHashNameSize}
        union all
        select chns.hash, chns.name, chns.map_name, chns.parent_hash,
          chns.depth, cc.size, cc.alloc_size, cc.count, cc.alloc_count,
          chns.size, chns.alloc_size, chns.count, chns.alloc_count
        from ${tableNameCallsiteHashNameSize} chns
        inner join callsite_children cc on chns.hash = cc.parent_hash
        )
        select hash, name, map_name, parent_hash, depth, SUM(size) as size,
          SUM(case when alloc_size > 0 then alloc_size else 0 end)
            as alloc_size, SUM(count) as count,
          SUM(case when alloc_count > 0 then alloc_count else 0 end) as
            alloc_count,
          self_size, self_alloc_size, self_count, self_alloc_count
        from callsite_children
        group by hash`);
    return tableNameGroupedCallsitesForFlamegraph;
  }
}

trackControllerRegistry.register(HeapProfileFlameraphTrackController);
