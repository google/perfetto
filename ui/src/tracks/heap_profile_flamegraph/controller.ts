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
  Config,
  Data,
  HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND,
  HeapProfileFlamegraphKey,
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
    totalSize: callsite.totalSize
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

const EMPTY_KEY = 'empty';
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

  private flamegraphDatasets: Map<string, CallsiteInfo[]> = new Map();

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    this.start = start;
    this.end = end;
    this.resolution = resolution;

    return this.generateEmptyData();
  }

  private generateEmptyData() {
    const data: Data = {
      start: -1,
      end: -1,
      resolution: this.resolution,
      length: 0,
      flamegraph: [],
      key: EMPTY_KEY
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

        this.config.ts = selectedTs;
        this.config.upid = selectedUpid;
        this.config.expandedId = this.lastExpandedId;

        const key = `${selectedUpid};${selectedTs}`;

        // TODO(tneda): Prevent lots of flamegraph queries being queued if a
        // user clicks lots of the markers quickly.
        this.getFlamegraphData(key, selection.ts, selectedUpid)
            .then(flamegraphData => {
              if (flamegraphData !== undefined && selection &&
                  selection.kind === selectedKind &&
                  selection.id === selectedId && selection.ts === selectedTs) {
                this.prepareAndMergeCallsites(flamegraphData, key);
                globals.dispatch(Actions.updateTrackConfig(
                    {id: this.trackState.id, config: this.config}));
              }
            });
      } else if (this.config.expandedId !== this.lastExpandedId) {
        const key = `${this.config.upid};${this.lastSelectedTs}`;
        this.lastExpandedId = this.config.expandedId;
        this.getFlamegraphData(key, this.lastSelectedTs, this.config.upid)
            .then(flamegraphData => {
              this.prepareAndMergeCallsites(flamegraphData, key);
            });
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
      flamegraphData: CallsiteInfo[], key: string) {
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
        key,
        clickedCallsite: expandedCallsite
      }
    });
  }

  async getFlamegraphData(key: string, ts: number, upid: number):
      Promise<CallsiteInfo[]> {
    let currentData;
    if (this.flamegraphDatasets.has(key)) {
      currentData = this.flamegraphDatasets.get(key)!;
    } else {
      // Sending empty data to show Loading state before we get an actual
      // data.
      this.publishEmptyData();
      currentData = await this.getFlamegraphDataFromTables(ts, upid);
      this.flamegraphDatasets.set(key, currentData);
    }
    return currentData;
  }

  async getFlamegraphDataFromTables(ts: number, upid: number) {
    // Collecting data for drawing flagraph for selected heap profile.
    // Data needs to be in following format:
    // id, name, parent_id, depth, total_size

    // Creating unique names for views so we can reuse and not delete them
    // for each marker.
    const tableNameCallsiteNameSize =
        this.tableName(`callsite_with_name_and_size_${ts}`);
    const tableNameCallsiteHashNameSize =
        this.tableName(`callsite_hash_name_size_${ts}`);
    // Joining the callsite table with frame table then with alloc table to get
    // the size and name for each callsite.
    await this.query(
        // TODO(tneda|lalitm): get names from symbols to exactly replicate
        // pprof.
        `create view if not exists ${tableNameCallsiteNameSize} as
      select cs.id, parent_id, depth, IFNULL(symbols.name, fr.name) as name,
      SUM(IFNULL(size, 0)) as size
      from stack_profile_callsite cs
      join stack_profile_frame fr on cs.frame_id = fr.id
      inner join (SELECT symbol_set_id, FIRST_VALUE(name) OVER(PARTITION BY
        symbol_set_id) as name
      FROM stack_profile_symbol GROUP BY symbol_set_id) as symbols
        using(symbol_set_id)
      left join heap_profile_allocation alloc on alloc.callsite_id = cs.id and
      alloc.ts <= ${ts} and alloc.upid = ${upid} group by cs.id`);

    // Recursive query to compute the hash for each callsite based on names
    // rather than ids.
    // We get all the children of the row in question and emit a row with hash
    // equal hash(name, parent.hash). Roots without the parent will have -1 as
    // hash.  Slices will be merged into a big slice.
    await this.query(
        `create view if not exists ${tableNameCallsiteHashNameSize} as
      with recursive callsite_table_names(
        id, hash, name, size, parent_hash, depth) AS (
      select id, hash(name) as hash, name, size, -1, depth
      from ${tableNameCallsiteNameSize}
      where depth = 0
      UNION ALL
      SELECT cs.id, hash(cs.name, ctn.hash) as hash, cs.name, cs.size, ctn.hash,
      cs.depth
      FROM callsite_table_names ctn
      INNER JOIN ${tableNameCallsiteNameSize} cs ON ctn.id = cs.parent_id
      )
      SELECT hash, name, parent_hash, depth, SUM(size) as size
      FROM callsite_table_names
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
    const callsites = await this.query(
        `with recursive callsite_children(hash, name, parent_hash, depth, size)
        AS (
        select *
        from ${tableNameCallsiteHashNameSize}
        where size > 0
        union all
        select chns.hash, chns.name, chns.parent_hash, chns.depth, cc.size
        from ${tableNameCallsiteHashNameSize} chns
        inner join callsite_children cc on chns.hash = cc.parent_hash
        )
        SELECT hash, name, parent_hash, depth, SUM(size) as size
        from callsite_children
        group by hash
        order by depth, parent_hash, size desc, name`);
    const flamegraphData: CallsiteInfo[] = [];
    const hashToindex: Map<number, number> = new Map();
    for (let i = 0; i < callsites.numRecords; i++) {
      const hash = callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const parentHash = callsites.columns[2].longValues![i];
      const depth = callsites.columns[3].longValues![i];
      const totalSize = callsites.columns[4].longValues![i];
      const parentId =
          hashToindex.has(+parentHash) ? hashToindex.get(+parentHash)! : -1;
      hashToindex.set(+hash, i);
      // Instead of hash, we will store index of callsite in this original array
      // as an id of callsite. That way, we have quicker access to parent and it
      // will stay unique.
      flamegraphData.push(
          {id: i, totalSize: +totalSize, depth: +depth, parentId, name});
    }
    return flamegraphData;
  }
}

trackControllerRegistry.register(HeapProfileFlameraphTrackController);
