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

import {
  DurationNode,
  ZeroDurationNode,
  InstanceNode,
  MarkNode,
} from '../chart/node';
import {IChartConfig} from '../config';
import {
  TZeroDurationNodeData,
  TDurationNodeData,
  TMarkNodeData,
  TInstanceNodeData,
  TNode,
} from '../chart/types';
import {Group, GroupWithThread, Track, AsyncTrack} from '../chart/group';
import {TAsyncTraceEvent, TTraceEvent} from '../types';
import {
  isAsyncBeginTraceEvent,
  isAsyncEndTraceEvent,
  isAsyncTraceEvent,
  isBeginTraceEvent,
  isCompleteTraceEvent,
  isEndTraceEvent,
  isInstanceTraceEvent,
  isMarkTraceEvent,
  ETraceEventPhase,
} from '../utils';

function dataToNode(
  data: TTraceEvent[],
  config: IChartConfig,
  eventToNode: Map<TTraceEvent, TNode>,
  recordNode: (id: string, node: TNode) => void,
  initLevel = 1,
) {
  const {basis} = config;
  // for get the deep of the group
  let maxLevel = 1;
  // for generate node's  y location in chart
  let level = initLevel;
  // for traversal data, from start to end
  let idx = 0;
  // for check nested duration data
  const stack: {trace: TTraceEvent; level: number}[] = [];
  // for error info
  let leftCounts = 0;
  let rightCounts = 0;
  const updateMaxLevel = () => {
    maxLevel = Math.max(maxLevel, level);
  };

  const res: TNode[] = [];

  while (idx < data.length) {
    const cur = data[idx];
    idx += 1;
    const ts = Number(cur.ts) - (basis ?? 0);
    if (isBeginTraceEvent(cur) || isAsyncBeginTraceEvent(cur)) {
      stack.push({
        trace: cur,
        level,
      });
      level += 1;
      leftCounts += 1;
      continue;
    }

    if (isInstanceTraceEvent(cur)) {
      updateMaxLevel();
      const instanceData: TInstanceNodeData = {
        ts,
        name: cur.name,
        group: cur.pid,
        thread: cur.tid,
        _internal: {
          raw: [cur],
        },
      };
      const node = new InstanceNode(instanceData, level, config);
      recordNode(cur.name, node);
      eventToNode.set(cur, node);
      res.push(node);
      continue;
    }

    if (isMarkTraceEvent(cur)) {
      updateMaxLevel();
      const markData: TMarkNodeData = {
        ts,
        name: cur.name,
        group: cur.pid,
        thread: cur.tid,
        _internal: {
          raw: [cur],
        },
      };
      const node = new MarkNode(markData, level, config);
      recordNode(cur.name, node);
      eventToNode.set(cur, node);
      res.push(node);
      continue;
    }

    if (isCompleteTraceEvent(cur)) {
      updateMaxLevel();
      const completeData: TDurationNodeData = {
        ts,
        dur: cur.dur,
        name: cur.name,
        group: cur.pid,
        thread: cur.tid,
        _internal: {
          raw: [cur],
        },
      };
      const node = new DurationNode(completeData, level, config);
      recordNode(cur.name, node);
      eventToNode.set(cur, node);
      res.push(node);
      continue;
    }

    if (isEndTraceEvent(cur) || isAsyncEndTraceEvent(cur)) {
      const {trace: start, level: startLevel} = stack.pop() || {};
      if (!start || startLevel == undefined) {
        throw new Error(
          `No phase Start data matched with ${JSON.stringify(cur)}`,
        );
      }
      level = startLevel;
      rightCounts += 1;
      updateMaxLevel();
      if (start.name !== cur.name) {
        throw new Error(
          `Duration Events is not closure: begin -> ${JSON.stringify(
            start,
          )}, end -> ${JSON.stringify(cur)}`,
        );
      }
      if (Number(start.ts) === Number(cur.ts)) {
        const zeroDurationData: TZeroDurationNodeData = {
          ts,
          name: cur.name,
          group: cur.pid,
          thread: cur.tid,
          _internal: {
            raw: [start, cur],
          },
        };
        const node = new ZeroDurationNode(zeroDurationData, level, config);
        recordNode(cur.name, node);
        eventToNode.set(start, node);
        res.push(node);
      } else {
        const durationData: TDurationNodeData = {
          ts: Number(start.ts) - (basis ?? 0),
          dur: Number(cur.ts) - Number(start.ts),
          name: cur.name,
          group: cur.pid,
          thread: cur.tid,
          _internal: {
            raw: [start, cur],
          },
        };
        const node = new DurationNode(durationData, level, config);
        recordNode(cur.name, node);
        eventToNode.set(start, node);
        res.push(node);
      }
      continue;
    }
  }
  if (stack.length) {
    throw new Error(
      `Duration Events is not closure: ${JSON.stringify(
        rightCounts > leftCounts ? stack.reverse() : stack,
      )}`,
    );
  }
  const withMarkNodes = data.find(
    (event) => event.ph === ETraceEventPhase.MARK,
  );
  const withDurationNodes = data.find(
    (event) =>
      event.ph === ETraceEventPhase.BEGIN || event.ph === ETraceEventPhase.END,
  );
  if (withMarkNodes && withDurationNodes) {
    maxLevel += 1;
  }
  return {
    nodes: res,
    // for header label
    maxLevel: maxLevel + 1,
  };
}

function floorToNode(
  floor: TAsyncTraceEvent[][],
  config: IChartConfig,
  eventToNode: Map<TTraceEvent, TNode>,
  recordNode: (id: string, node: TNode) => void,
) {
  const {maxLevel: floorMaxLevel, nodes: floorNodes} = floor.reduce(
    (prev, cur, idx) => {
      const initLevel = idx === 0 ? 1 : prev.maxLevel + 1;
      const {nodes, maxLevel} = dataToNode(
        cur,
        config,
        eventToNode,
        recordNode,
        initLevel,
      );
      prev.nodes.push(...nodes);
      prev.maxLevel = maxLevel - 1;

      return prev;
    },
    {nodes: [], maxLevel: 0} as {nodes: TNode[]; maxLevel: number},
  );

  return {nodes: floorNodes, maxLevel: floorMaxLevel + 1};
}

function asyncDataToNode(
  data: TTraceEvent[],
  config: IChartConfig,
  eventToNode: Map<TTraceEvent, TNode>,
  recordNode: (id: string, node: TNode) => void,
) {
  const floor: TAsyncTraceEvent[][] = [];
  const map = new WeakMap();
  const {basis} = config;
  // for traversal data, from start to end
  let idx = 0;
  // for check nested duration data
  const stack: TAsyncTraceEvent[] = [];
  // for error info
  let leftCounts = 0;
  let rightCounts = 0;

  while (idx < data.length) {
    const cur = data[idx];
    idx += 1;
    const ts = Number(Number(cur.ts)) - (basis ?? 0);
    if (isAsyncBeginTraceEvent(cur)) {
      if (stack.length > 0) {
        const parent = stack[stack.length - 1];
        const floorIdx = map.get(parent);
        floor[floorIdx].push(cur);
        map.set(cur, floorIdx);
      } else {
        const floorIdx = floor.findIndex(
          (item) => ts > Number(item[item.length - 1].ts),
        );
        if (floorIdx > -1) {
          floor[floorIdx].push(cur);
          map.set(cur, floorIdx);
        } else {
          floor.push([cur]);
          map.set(cur, floor.length - 1);
        }
      }
      rightCounts += 1;
      stack.push(cur);
      continue;
    }

    if (isAsyncEndTraceEvent(cur)) {
      leftCounts += 1;
      const start = stack.pop();
      if (!start) {
        throw new Error(
          `No phase End data matched with ${JSON.stringify(cur)}`,
        );
      }
      const floorIdx = map.get(start);
      floor[floorIdx].push(cur);
      continue;
    }
  }
  if (stack.length) {
    throw new Error(
      `Duration Events is not closure: ${JSON.stringify(
        rightCounts > leftCounts ? stack.reverse() : stack,
      )}`,
    );
  }

  const res = floorToNode(floor, config, eventToNode, recordNode);
  return res;
}

/**
 * for async events sort by id and pid
 * for other events sort by tid and pid
 * @param data trace data
 * @returns
 */
function groupData(data: TTraceEvent[]) {
  return data.reduce(
    (prev, cur) => {
      const groupName = cur.pid.toString();
      if (prev[groupName] == null) {
        prev[groupName] = {};
      }
      if (isAsyncTraceEvent(cur)) {
        const trackName = cur.cat.toString();
        if (prev[groupName][trackName] != null) {
          prev[groupName][trackName].push(cur);
        } else {
          prev[groupName][trackName] = [cur];
        }
      } else {
        const trackName = cur.tid?.toString() || 'default';
        if (prev[groupName][trackName] != null) {
          prev[groupName][trackName].push(cur);
        } else {
          prev[groupName][trackName] = [cur];
        }
      }
      return prev;
    },
    {} as Record<string, Record<string, TTraceEvent[]>>,
  );
}

export function transform(
  data: TTraceEvent[],
  config: IChartConfig,
  eventToNode: Map<TTraceEvent, TNode>,
  recordNode: (id: string, node: TNode) => void,
) {
  const dataGrouped = groupData(data);
  let startLevel = 0;
  const groups = Object.entries(dataGrouped).reduce((prev, cur) => {
    const [pid, traceEventsData] = cur;
    if (
      Object.keys(traceEventsData).length === 1 &&
      Object.keys(traceEventsData)[0] === 'default' &&
      !isAsyncTraceEvent(traceEventsData.default[0])
    ) {
      const {nodes, maxLevel} = dataToNode(
        traceEventsData.default,
        config,
        eventToNode,
        recordNode,
      );
      const group = new Group(pid, nodes, config, startLevel, maxLevel);
      nodes.forEach((node) => {
        node.group = group;
      });
      startLevel += maxLevel;
      prev.push(group);
    } else {
      // for group header
      startLevel += 1;
      let maxLevelCount = 0;
      const nodesArr: TNode[] = [];
      const threadArr = Object.entries(traceEventsData)
        .map(([id, threadData]) => {
          const isAsyncEvent = isAsyncTraceEvent(threadData[0]);
          if (isAsyncEvent) {
            const {nodes, maxLevel} = asyncDataToNode(
              threadData,
              config,
              eventToNode,
              recordNode,
            );
            maxLevelCount += maxLevel;
            const track: Track = new AsyncTrack(
              `    ${id === 'default' ? '' : id}`,
              nodes,
              config,
              startLevel,
              maxLevel,
            );
            startLevel += maxLevel;
            nodesArr.push(...nodes);
            nodes.forEach((node) => {
              node.track = track;
            });
            return track;
          } else {
            const {nodes, maxLevel} = dataToNode(
              threadData,
              config,
              eventToNode,
              recordNode,
            );
            maxLevelCount += maxLevel;
            const track: Track = new Track(
              `    ${id === 'default' ? '' : id}`,
              nodes,
              config,
              startLevel,
              maxLevel,
            );
            startLevel += maxLevel;
            nodesArr.push(...nodes);
            nodes.forEach((node) => {
              node.track = track;
            });
            return track;
          }
        })
        .sort((a, b) => Number(b.isAsyncEvent) - Number(a.isAsyncEvent));

      const group = new GroupWithThread(
        pid,
        nodesArr,
        config,
        startLevel,
        maxLevelCount + 1,
        threadArr,
      );
      threadArr.forEach((t) => {
        t.group = group;
      });
      prev.push(group);
    }
    return prev;
  }, [] as Group[]);
  return groups;
}
