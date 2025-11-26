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

import {ModifyColumnsNode} from './modify_columns_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';

describe('ModifyColumnsNode', () => {
  function createMockPrevNode(): QueryNode {
    return {
      nodeId: 'mock',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [
        {
          name: 'id',
          type: 'INT',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'status',
          type: 'STRING',
          checked: true,
          column: {name: 'status'},
        },
        {
          name: 'value',
          type: 'INT',
          checked: true,
          column: {name: 'value'},
        },
      ],
      state: {},
      validate: () => true,
      getTitle: () => 'Mock',
      nodeSpecificModify: () => null,
      nodeInfo: () => null,
      clone: () => createMockPrevNode(),
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createColumnInfo(name: string, type: string): ColumnInfo {
    return {
      name,
      type,
      checked: true,
      column: {name},
    };
  }

  describe('SWITCH column generation', () => {
    it('should generate correct CASE statement with single case', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_numeric',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
      );
    });

    it('should generate correct CASE statement with multiple cases', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [
              {when: "'active'", then: '1'},
              {when: "'pending'", then: '2'},
              {when: "'done'", then: '3'},
            ],
            defaultValue: '0',
            expression:
              "CASE WHEN status = 'active' THEN 1 WHEN status = 'pending' THEN 2 WHEN status = 'done' THEN 3 ELSE 0 END",
            name: 'status_code',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status = 'active' THEN 1 WHEN status = 'pending' THEN 2 WHEN status = 'done' THEN 3 ELSE 0 END",
      );
    });

    it('should handle CASE statement without default value', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '',
            expression: "CASE WHEN status = 'active' THEN 1 END",
            name: 'status_flag',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe("CASE WHEN status = 'active' THEN 1 END");
    });

    it('should filter out empty cases', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [
              {when: "'active'", then: '1'},
              {when: '', then: ''},
              {when: "'pending'", then: '2'},
            ],
            defaultValue: '0',
            expression:
              "CASE WHEN status = 'active' THEN 1 WHEN status = 'pending' THEN 2 ELSE 0 END",
            name: 'status_code',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status = 'active' THEN 1 WHEN status = 'pending' THEN 2 ELSE 0 END",
      );
    });

    it('should handle empty expression when no valid cases', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [],
            defaultValue: '',
            expression: '',
            name: 'empty_switch',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe('');
    });

    it('should handle CASE with only default value', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [],
            defaultValue: '0',
            expression: 'CASE  ELSE 0 END',
            name: 'default_only',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe('CASE  ELSE 0 END');
    });

    it('should handle numeric comparisons', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'value',
            cases: [
              {when: '0', then: "'zero'"},
              {when: '1', then: "'one'"},
              {when: '2', then: "'two'"},
            ],
            defaultValue: "'many'",
            expression:
              "CASE WHEN value = 0 THEN 'zero' WHEN value = 1 THEN 'one' WHEN value = 2 THEN 'two' ELSE 'many' END",
            name: 'value_name',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN value = 0 THEN 'zero' WHEN value = 1 THEN 'one' WHEN value = 2 THEN 'two' ELSE 'many' END",
      );
    });

    it('should handle cases with whitespace trimming', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [
              {when: "  'active'  ", then: '  1  '},
              {when: '  ', then: '  '},
            ],
            defaultValue: '  0  ',
            expression:
              "CASE WHEN status =   'active'   THEN   1   ELSE   0   END",
            name: 'trimmed',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      // The second case should be filtered out as both when and then are whitespace
      expect(newCol.expression).toBe(
        "CASE WHEN status =   'active'   THEN   1   ELSE   0   END",
      );
    });

    it('should generate CASE statement with GLOB operator when useGlob is true', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [
              {when: "'act*'", then: '1'},
              {when: "'pend*'", then: '2'},
            ],
            defaultValue: '0',
            useGlob: true,
            expression:
              "CASE WHEN status GLOB 'act*' THEN 1 WHEN status GLOB 'pend*' THEN 2 ELSE 0 END",
            name: 'status_pattern',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status GLOB 'act*' THEN 1 WHEN status GLOB 'pend*' THEN 2 ELSE 0 END",
      );
    });

    it('should generate CASE statement with = operator when useGlob is false', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            useGlob: false,
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_exact',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
      );
    });

    it('should generate CASE statement with = operator when useGlob is undefined', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_default',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
      );
    });

    it('should handle glob patterns with complex wildcards', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [
              {when: "'[aA]ctive'", then: '1'},
              {when: "'*ing'", then: '2'},
              {when: "'done*'", then: '3'},
            ],
            defaultValue: '0',
            useGlob: true,
            expression:
              "CASE WHEN status GLOB '[aA]ctive' THEN 1 WHEN status GLOB '*ing' THEN 2 WHEN status GLOB 'done*' THEN 3 ELSE 0 END",
            name: 'status_glob',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN status GLOB '[aA]ctive' THEN 1 WHEN status GLOB '*ing' THEN 2 WHEN status GLOB 'done*' THEN 3 ELSE 0 END",
      );
    });
  });

  describe('IF column generation', () => {
    it('should generate correct CASE statement for IF', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'if',
            clauses: [{if: 'value > 10', then: "'high'"}],
            elseValue: "'low'",
            expression: "CASE WHEN value > 10 THEN 'high' ELSE 'low' END",
            name: 'value_category',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN value > 10 THEN 'high' ELSE 'low' END",
      );
    });

    it('should generate correct CASE statement with multiple IF clauses', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'if',
            clauses: [
              {if: 'value > 100', then: "'very high'"},
              {if: 'value > 50', then: "'high'"},
              {if: 'value > 10', then: "'medium'"},
            ],
            elseValue: "'low'",
            expression:
              "CASE WHEN value > 100 THEN 'very high' WHEN value > 50 THEN 'high' WHEN value > 10 THEN 'medium' ELSE 'low' END",
            name: 'value_tier',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN value > 100 THEN 'very high' WHEN value > 50 THEN 'high' WHEN value > 10 THEN 'medium' ELSE 'low' END",
      );
    });

    it('should handle IF without ELSE', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'if',
            clauses: [{if: 'value > 10', then: "'high'"}],
            elseValue: undefined,
            expression: "CASE WHEN value > 10 THEN 'high' END",
            name: 'optional_flag',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe("CASE WHEN value > 10 THEN 'high' END");
    });

    it('should filter out empty IF clauses', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'if',
            clauses: [
              {if: 'value > 10', then: "'high'"},
              {if: '', then: ''},
              {if: 'value > 5', then: "'medium'"},
            ],
            elseValue: "'low'",
            expression:
              "CASE WHEN value > 10 THEN 'high' WHEN value > 5 THEN 'medium' ELSE 'low' END",
            name: 'filtered_if',
          },
        ],
        selectedColumns: [],
      });

      const newCol = node.state.newColumns[0];
      expect(newCol.expression).toBe(
        "CASE WHEN value > 10 THEN 'high' WHEN value > 5 THEN 'medium' ELSE 'low' END",
      );
    });
  });

  describe('validation', () => {
    it('should validate when at least one column is selected', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [],
        selectedColumns: [createColumnInfo('id', 'INT')],
      });

      expect(node.validate()).toBe(true);
    });

    it('should validate when new column has expression and name', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_code',
          },
        ],
        selectedColumns: [],
      });

      expect(node.validate()).toBe(true);
    });

    it('should fail validation when no columns selected and no valid new columns', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [],
        selectedColumns: [],
      });

      // Uncheck all auto-populated columns
      node.state.selectedColumns.forEach((col) => {
        col.checked = false;
      });

      expect(node.validate()).toBe(false);
    });

    it('should fail validation for new column without name', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            expression: "CASE WHEN status = 'active' THEN 1 END",
            name: '',
          },
        ],
        selectedColumns: [],
      });

      expect(node.validate()).toBe(false);
    });

    it('should fail validation for duplicate column names', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            expression: "CASE WHEN status = 'active' THEN 1 END",
            name: 'code',
          },
          {
            expression: "CASE WHEN value > 10 THEN 'high' END",
            name: 'code',
          },
        ],
        selectedColumns: [],
      });

      expect(node.validate()).toBe(false);
    });
  });

  describe('serialization', () => {
    it('should serialize SWITCH column correctly', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_code',
          },
        ],
        selectedColumns: [],
      });

      const serialized = node.serializeState();

      expect(serialized.newColumns).toBeDefined();
      expect(serialized.newColumns.length).toBe(1);
      expect(serialized.newColumns[0].type).toBe('switch');
      expect(serialized.newColumns[0].switchOn).toBe('status');
      expect(serialized.newColumns[0].cases?.length).toBe(1);
      expect(serialized.newColumns[0].defaultValue).toBe('0');
    });

    it('should serialize IF column correctly', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'if',
            clauses: [{if: 'value > 10', then: "'high'"}],
            elseValue: "'low'",
            expression: "CASE WHEN value > 10 THEN 'high' ELSE 'low' END",
            name: 'category',
          },
        ],
        selectedColumns: [],
      });

      const serialized = node.serializeState();

      expect(serialized.newColumns).toBeDefined();
      expect(serialized.newColumns.length).toBe(1);
      expect(serialized.newColumns[0].type).toBe('if');
      expect(serialized.newColumns[0].clauses?.length).toBe(1);
      expect(serialized.newColumns[0].elseValue).toBe("'low'");
    });

    it('should serialize SWITCH column with useGlob correctly', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'act*'", then: '1'}],
            defaultValue: '0',
            useGlob: true,
            expression: "CASE WHEN status GLOB 'act*' THEN 1 ELSE 0 END",
            name: 'status_pattern',
          },
        ],
        selectedColumns: [],
      });

      const serialized = node.serializeState();

      expect(serialized.newColumns).toBeDefined();
      expect(serialized.newColumns.length).toBe(1);
      expect(serialized.newColumns[0].type).toBe('switch');
      expect(serialized.newColumns[0].switchOn).toBe('status');
      expect(serialized.newColumns[0].useGlob).toBe(true);
      expect(serialized.newColumns[0].cases?.length).toBe(1);
      expect(serialized.newColumns[0].defaultValue).toBe('0');
    });

    it('should serialize SWITCH column without useGlob when false', () => {
      const node = new ModifyColumnsNode({
        prevNode: createMockPrevNode(),
        newColumns: [
          {
            type: 'switch',
            switchOn: 'status',
            cases: [{when: "'active'", then: '1'}],
            defaultValue: '0',
            useGlob: false,
            expression: "CASE WHEN status = 'active' THEN 1 ELSE 0 END",
            name: 'status_code',
          },
        ],
        selectedColumns: [],
      });

      const serialized = node.serializeState();

      expect(serialized.newColumns).toBeDefined();
      expect(serialized.newColumns.length).toBe(1);
      expect(serialized.newColumns[0].useGlob).toBe(false);
    });
  });
});
