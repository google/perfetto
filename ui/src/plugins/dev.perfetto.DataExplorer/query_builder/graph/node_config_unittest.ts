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

import {buildReadOnlyNodeConfig} from './node_config';
import {NodeType} from '../../query_node';
import {
  createMockNode,
  connectNodes,
  connectSecondary,
  createUnboundedSecondaryInputs,
} from '../testing/test_utils';

describe('buildReadOnlyNodeConfig', () => {
  it('should dock a single-operation child into its parent via next', () => {
    // A (source) → B (filter, single-operation) — B should be docked.
    const a = createMockNode({nodeId: 'a', type: NodeType.kTable});
    const b = createMockNode({nodeId: 'b', type: NodeType.kFilter});
    connectNodes(a, b);

    const innerSet = new Set(['a', 'b']);
    const config = buildReadOnlyNodeConfig(a, innerSet);

    expect(config.next).toBeDefined();
    expect(config.next?.id).toBe('b');
  });

  it('should dock children even when they have secondary inputs', () => {
    // Scenario: Slices → AddColumns, TableThread → AddColumns (secondary)
    // The nodegraph widget can route connections to docked children,
    // so AddColumns should remain docked to Slices.
    const slices = createMockNode({
      nodeId: 'slices',
      type: NodeType.kSimpleSlices,
    });
    const addCols = createMockNode({
      nodeId: 'addCols',
      type: NodeType.kAddColumns,
      secondaryInputs: createUnboundedSecondaryInputs(),
    });
    const tableThread = createMockNode({
      nodeId: 'tableThread',
      type: NodeType.kTable,
    });

    connectNodes(slices, addCols);
    connectSecondary(tableThread, addCols, 0);

    const innerSet = new Set(['slices', 'addCols', 'tableThread']);
    const config = buildReadOnlyNodeConfig(slices, innerSet);

    // addCols should be docked — the widget handles connection routing.
    expect(config.next).toBeDefined();
    expect(config.next?.id).toBe('addCols');
  });

  it('should preserve port indices for docked children with secondary inputs', () => {
    // When a docked child has secondary inputs, port indices must be
    // stable so connections target the correct port.
    const slices = createMockNode({
      nodeId: 'slices',
      type: NodeType.kSimpleSlices,
    });
    const addCols = createMockNode({
      nodeId: 'addCols',
      type: NodeType.kAddColumns,
      secondaryInputs: createUnboundedSecondaryInputs(),
    });
    const tableThread = createMockNode({
      nodeId: 'tableThread',
      type: NodeType.kTable,
    });

    connectNodes(slices, addCols);
    connectSecondary(tableThread, addCols, 0);

    const innerSet = new Set(['slices', 'addCols', 'tableThread']);

    // Mark the docked child's primary input port as connected (as
    // buildInnerGraphPreview does) so it isn't filtered out.
    const connectedInputs = new Set(['addCols:0', 'addCols:1']);
    const connectedOutputs = new Set(['slices:0', 'tableThread:0']);

    const config = buildReadOnlyNodeConfig(
      slices,
      innerSet,
      connectedInputs,
      connectedOutputs,
    );

    // The docked addCols should have 2 input ports:
    // port 0 (top, primary) and port 1 (left, secondary).
    const dockedChild = config.next;
    expect(dockedChild).toBeDefined();
    expect(dockedChild?.inputs).toHaveLength(2);
    expect(dockedChild?.inputs?.[0]?.direction).toBe('top');
    expect(dockedChild?.inputs?.[1]?.direction).toBe('left');
  });

  it('should still dock children without secondary inputs', () => {
    // Slices → Filter (no secondary inputs) — Filter should be docked.
    const slices = createMockNode({
      nodeId: 'slices',
      type: NodeType.kSimpleSlices,
    });
    const filter = createMockNode({
      nodeId: 'filter',
      type: NodeType.kFilter,
    });
    connectNodes(slices, filter);

    const innerSet = new Set(['slices', 'filter']);
    const config = buildReadOnlyNodeConfig(slices, innerSet);

    expect(config.next).toBeDefined();
    expect(config.next?.id).toBe('filter');
  });
});
