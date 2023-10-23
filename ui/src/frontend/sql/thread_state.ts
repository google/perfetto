// Copyright (C) 2023 The Android Open Source Project
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

import {Duration, duration, TimeSpan} from '../../base/time';
import {LONG, NUM_NULL, STR, STR_NULL} from '../../common/query_result';
import {EngineProxy} from '../../public';
import {TreeNode} from '../../widgets/tree';
import {Utid} from '../sql_types';

// An individual node of the thread state breakdown tree.
class Node {
  parent?: Node;
  children: Map<string, Node>;
  dur: duration;
  startsCollapsed: boolean = true;

  constructor(parent?: Node) {
    this.parent = parent;
    this.children = new Map();
    this.dur = 0n;
  }

  getOrCreateChild(name: string) {
    let child = this.children.get(name);
    if (!child) {
      child = new Node(this);
      this.children.set(name, child);
    }
    return child;
  }

  addDuration(dur: duration) {
    let node: Node|undefined = this;
    while (node !== undefined) {
      node.dur += dur;
      node = node.parent;
    }
  }
}

// Thread state breakdown data (tree).
// Can be passed to ThreadStateBreakdownTreeNode to be rendered as a part of a
// tree.
export interface BreakdownByThreadState {
  root: Node;
}

// Compute a breakdown of thread states for a given thread for a given time
// interval.
export async function breakDownIntervalByThreadState(
    engine: EngineProxy, range: TimeSpan, utid: Utid):
    Promise<BreakdownByThreadState> {
  // TODO(altimin): this probably should share some code with pivot tables when
  // we actually get some pivot tables we like.
  const query = await engine.query(`
    INCLUDE PERFETTO MODULE common.thread_states;

    SELECT
      state,
      raw_state as rawState,
      cpu_type as cpuType,
      cpu,
      blocked_function as blockedFunction,
      dur
    FROM thread_state_summary_for_interval(${range.start}, ${range.duration}, ${
      utid});
  `);
  const it = query.iter({
    state: STR,
    rawState: STR,
    cpuType: STR_NULL,
    cpu: NUM_NULL,
    blockedFunction: STR_NULL,
    dur: LONG,
  });
  const root = new Node();
  for (; it.valid(); it.next()) {
    let currentNode = root;
    currentNode = currentNode.getOrCreateChild(it.state);
    // If the CPU time is not null, add it to the breakdown.
    if (it.cpuType !== null) {
      currentNode = currentNode.getOrCreateChild(it.cpuType);
    }
    if (it.cpu !== null) {
      currentNode = currentNode.getOrCreateChild(`CPU ${it.cpu}`);
    }
    if (it.blockedFunction !== null) {
      currentNode = currentNode.getOrCreateChild(`${it.blockedFunction}`);
    }
    currentNode.addDuration(it.dur);
  }
  return {
    root,
  };
}

function renderChildren(node: Node, totalDur: duration): m.Child[] {
  const res = Array.from(node.children.entries())
                  .map(([name, child]) => renderNode(child, name, totalDur));
  return res;
}

function renderNode(node: Node, name: string, totalDur: duration): m.Child {
  const durPercent = 100. * Number(node.dur) / Number(totalDur);
  return m(
      TreeNode,
      {
        left: name,
        right: `${Duration.humanise(node.dur)} (${durPercent.toFixed(2)}%)`,
        startsCollapsed: node.startsCollapsed,
      },
      renderChildren(node, totalDur));
}

interface BreakdownByThreadStateTreeNodeAttrs {
  dur: duration;
  data: BreakdownByThreadState;
}

// A tree node that displays a nested breakdown a time interval by thread state.
export class BreakdownByThreadStateTreeNode implements
    m.ClassComponent<BreakdownByThreadStateTreeNodeAttrs> {
  view({attrs}: m.Vnode<BreakdownByThreadStateTreeNodeAttrs>): m.Child[] {
    return renderChildren(attrs.data.root, attrs.dur);
  }
}
