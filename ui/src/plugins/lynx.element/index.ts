// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {lynxPerfGlobals} from '../../frontend/lynx_perf/lynx_perf_globals';
import {LYNX_PERF_ELEMENT_PLUGIN_ID} from '../../frontend/lynx_perf/constants';
import {NUM, STR} from '../../trace_processor/query_result';
import {
  findDeeplyNestedNodesRecursively,
  findInvisibleNodesRecursively,
  findNonRenderingNodesRecursively,
  reConstructElementTree,
} from './utils';
import {LynxElement} from './types';
import ElementManager from './element_manager';
import {Engine} from '../../trace_processor/engine';
import {LynxElementTrack} from './element_track';
import {IssueSummary, IssueRank} from '../../frontend/lynx_perf/types';
import {getArgs} from '../../components/sql_utils/args';
import {asArgSetId} from '../../components/sql_utils/core_types';

/**
 * Lynx Element Performance Analysis Plugin
 * Tracks and analyzes Lynx element tree structures for performance issues,
 * including deeply nested nodes, invisible elements, and non-rendering nodes.
 */
export default class LynxElementPlugin implements PerfettoPlugin {
  static readonly id = LYNX_PERF_ELEMENT_PLUGIN_ID;
  /**
   * Tracks problematic element nodes by instance_id
   * Key: instance_id
   * Value: Set of problematic node IDs
   */
  private issueNodesMap: Map<number, Set<number>>;

  constructor() {
    this.issueNodesMap = new Map();
  }

  /**
   * Retrieves and updates screen dimensions from trace data
   * @param ctx - Trace context containing engine and storage
   */
  private async getScreenSize(ctx: Trace): Promise<void> {
    const engine = ctx.engine;
    const queryRes = await engine.query(
      `select arg_set_id as argSetId from slice where slice.name='UpdateScreenSize' limit 1`,
    );
    if (queryRes.numRows() > 0) {
      const row = queryRes.firstRow({
        argSetId: NUM,
      });
      const args = await getArgs(engine, asArgSetId(row.argSetId));
      let screenWidth = 0;
      let screenHeight = 0;
      args.forEach((arg) => {
        if (
          arg.key === 'debug.screen_width' ||
          arg.key === 'args.screen_width'
        ) {
          screenWidth = parseInt(arg.value as string);
        }
        if (
          arg.key === 'debug.screen_height' ||
          arg.key === 'args.screen_height'
        ) {
          screenHeight = parseInt(arg.value as string);
        }
      });
      ElementManager.updateScreenSize(screenWidth, screenHeight);
    }
  }

  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    // TODO: reset issue status in perf track when v49.x.
    lynxPerfGlobals.resetIssueStatus();

    await this.getScreenSize(ctx);
    const domIssues = await this.getIssueData(ctx.engine);
    if (domIssues.length > 0) {
      lynxPerfGlobals.appendPerformanceIssue(domIssues);
      const timeout = ctx.commands.hasCommand('lynx.PerformanceIssues#update')
        ? 0
        : 500;
      setTimeout(() => {
        ctx.commands.runCommand('lynx.PerformanceIssues#update');
      }, timeout);
    }

    ctx.tracks.registerTrack({
      uri: LYNX_PERF_ELEMENT_PLUGIN_ID,
      track: new LynxElementTrack(),
      title: 'Lynx Element Issues',
    });
  }

  /**
   * Retrieves and processes element tree data from trace
   * @param engine - Trace processor engine instance
   * @returns Array of detected performance issues
   */
  async getIssueData(engine: Engine): Promise<IssueSummary[]> {
    const queryRes = await engine.query(
      `select ts,id,dur,name, arg_set_id as argSetId from slice where slice.name='DumpElementTree' order by ts desc`,
    );
    const it = queryRes.iter({
      argSetId: NUM,
      ts: NUM,
      id: NUM,
      dur: NUM,
      name: STR,
    });
    const data: IssueSummary[] = [];

    this.issueNodesMap.clear();
    for (; it.valid(); it.next()) {
      const args = await getArgs(engine, asArgSetId(it.argSetId));
      const instanceIdArg = args.filter(
        (item) =>
          item.key === 'debug.instance_id' || item.key === 'args.instance_id',
      );
      const instanceId = instanceIdArg[0].value as number;
      if (!this.issueNodesMap.has(instanceId)) {
        this.issueNodesMap.set(instanceId, new Set());
      }
      const proceedSliceTs = await this.findPrecedingSliceTs(engine, it.id);
      args.forEach((arg) => {
        if (arg.key === 'debug.content' || arg.key === 'args.content') {
          const content = arg.value as string;
          const rootElementAbbr = JSON.parse(content);
          const rootElement = reConstructElementTree(
            rootElementAbbr,
            undefined,
          );
          const issueElements = this.findIssueElements(rootElement, instanceId);
          if (issueElements.length > 0) {
            ElementManager.setTraceIssueElements(it.id, issueElements);
            data.push({
              id: it.id,
              ts: proceedSliceTs > 0 ? proceedSliceTs : it.ts,
              description: `Performance issue detected in the Element tree, click for more details`,
              trackUri: LYNX_PERF_ELEMENT_PLUGIN_ID,
              issueRank: IssueRank.MODERATE,
            });
          }
        }
      });
    }
    return data;
  }

  /**
   * Finds timestamp of preceding slice in execution flow
   * @param engine - Trace processor engine
   * @param sliceId - ID of current slice
   * @returns Timestamp of preceding slice or -1 if not found
   */
  async findPrecedingSliceTs(engine: Engine, sliceId: number) {
    const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;

    select
      t1.ts as beginTs,
      t1.id as id
    from preceding_flow(${sliceId}) f
    join slice t1 on f.slice_out = t1.slice_id
    `;
    const result = await engine.query(query);
    if (result.numRows() > 0) {
      return result.firstRow({
        beginTs: NUM,
        id: NUM,
      }).beginTs;
    }
    return -1;
  }

  /**
   * Analyzes element tree for performance issues
   * @param root - Root element of the tree to analyze
   * @param instanceId - Unique identifier for this element tree instance
   * @returns Array of problematic elements
   * @remarks
   * Detects three types of issues:
   * 1. Deeply nested nodes
   * 2. Invisible nodes
   * 3. Non-rendering nodes
   */
  findIssueElements(root: LynxElement, instanceId: number): LynxElement[] {
    let deeplyNestedNodeList = findDeeplyNestedNodesRecursively(root, root);
    const issueNodes = this.issueNodesMap.get(instanceId) as Set<number>;

    // only filter top 5
    deeplyNestedNodeList = deeplyNestedNodeList
      .sort((a, b) => b.depth - a.depth)
      .slice(0, Math.min(5, deeplyNestedNodeList.length))
      .filter((item) => !issueNodes.has(item.id));

    if (deeplyNestedNodeList.length > 0) {
      deeplyNestedNodeList.forEach((item) => {
        issueNodes.add(item.id);
        item.rootElement = root;
      });
    }

    let invisibleNodeList = findInvisibleNodesRecursively(root, root);
    invisibleNodeList = invisibleNodeList.filter(
      (item) => !issueNodes.has(item.id),
    );
    if (invisibleNodeList.length > 0) {
      invisibleNodeList.forEach((item) => {
        issueNodes.add(item.id);
        item.rootElement = root;
      });
    }

    let overWrappedNodeList = findNonRenderingNodesRecursively(root, root);
    overWrappedNodeList = overWrappedNodeList.filter(
      (item) => !issueNodes.has(item.id),
    );
    if (overWrappedNodeList.length > 0) {
      overWrappedNodeList.forEach((item) => {
        issueNodes.add(item.id);
        item.rootElement = root;
      });
    }

    return deeplyNestedNodeList.concat(invisibleNodeList, overWrappedNodeList);
  }
}
