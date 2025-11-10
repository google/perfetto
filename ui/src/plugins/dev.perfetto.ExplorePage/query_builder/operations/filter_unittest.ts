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

import {
  UIFilter,
  FilterGroup,
  findFilterGroup,
  removeFilterFromGroup,
  removeFilterFromGroupsOrFilters,
  extractFilterFromLocation,
  addFilterToGroup,
  createFilterGroup,
  createFiltersProto,
  createExperimentalFiltersProto,
} from './filter';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

describe('filter operations', () => {
  // Helper to create test filters
  const createFilter = (
    column: string,
    value: string | number,
    enabled = true,
  ): UIFilter => ({
    column,
    op: '=',
    value,
    enabled,
  });

  const filter1 = createFilter('id', '1');
  const filter2 = createFilter('name', 'test');
  const filter3 = createFilter('age', '25');
  const filter4 = createFilter('status', 'active');

  describe('filter group utilities', () => {
    describe('findFilterGroup', () => {
      it('should find the group containing a filter', () => {
        const group1: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const group2: FilterGroup = {
          id: 'group2',
          filters: [filter3, filter4],
          enabled: true,
        };
        const groups = [group1, group2];

        const result = findFilterGroup(filter2, groups);

        expect(result).toBe(group1);
      });

      it('should return undefined if filter is not in any group', () => {
        const group1: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const groups = [group1];

        const result = findFilterGroup(filter3, groups);

        expect(result).toBeUndefined();
      });

      it('should return undefined for empty groups list', () => {
        const result = findFilterGroup(filter1, []);

        expect(result).toBeUndefined();
      });
    });

    describe('removeFilterFromGroup', () => {
      it('should remove filter from group with more than 2 filters', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2, filter3],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group];

        const result = removeFilterFromGroup(filter2, group, filters, groups);

        expect(result.filters).toEqual([]);
        expect(result.groups).toHaveLength(1);
        expect(result.groups[0].filters).toEqual([filter1, filter3]);
        expect(result.groups[0].id).toBe('group1');
      });

      it('should dissolve group when removing second-to-last filter', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group];

        const result = removeFilterFromGroup(filter1, group, filters, groups);

        expect(result.filters).toEqual([filter2]);
        expect(result.groups).toEqual([]);
      });

      it('should dissolve group when removing last filter', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group];

        const result = removeFilterFromGroup(filter1, group, filters, groups);

        expect(result.filters).toEqual([]);
        expect(result.groups).toEqual([]);
      });

      it('should preserve existing main filters when dissolving group', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const filters: UIFilter[] = [filter3, filter4];
        const groups = [group];

        const result = removeFilterFromGroup(filter1, group, filters, groups);

        expect(result.filters).toEqual([filter3, filter4, filter2]);
        expect(result.groups).toEqual([]);
      });

      it('should not affect other groups', () => {
        const group1: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const group2: FilterGroup = {
          id: 'group2',
          filters: [filter3, filter4],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group1, group2];

        const result = removeFilterFromGroup(filter1, group1, filters, groups);

        expect(result.filters).toEqual([filter2]);
        expect(result.groups).toEqual([group2]);
      });
    });

    describe('removeFilterFromGroupsOrFilters', () => {
      it('should remove filter from a group', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2, filter3],
          enabled: true,
        };
        const filters: UIFilter[] = [filter4];
        const groups = [group];

        const result = removeFilterFromGroupsOrFilters(
          filter2,
          filters,
          groups,
        );

        expect(result.filters).toEqual([filter4]);
        expect(result.groups[0].filters).toEqual([filter1, filter3]);
      });

      it('should remove filter from main filters list', () => {
        const filters: UIFilter[] = [filter1, filter2, filter3];
        const groups: FilterGroup[] = [];

        const result = removeFilterFromGroupsOrFilters(
          filter2,
          filters,
          groups,
        );

        expect(result.filters).toEqual([filter1, filter3]);
        expect(result.groups).toEqual([]);
      });

      it('should dissolve group when removing causes it to have < 2 filters', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const filters: UIFilter[] = [filter3];
        const groups = [group];

        const result = removeFilterFromGroupsOrFilters(
          filter1,
          filters,
          groups,
        );

        expect(result.filters).toEqual([filter3, filter2]);
        expect(result.groups).toEqual([]);
      });
    });

    describe('extractFilterFromLocation', () => {
      it('should extract filter from group and dissolve if needed', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const filters: UIFilter[] = [filter3];
        const groups = [group];

        const result = extractFilterFromLocation(filter1, filters, groups);

        expect(result.filters).toEqual([filter3, filter2]);
        expect(result.groups).toEqual([]);
      });

      it('should extract filter from group without dissolving', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2, filter3],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group];

        const result = extractFilterFromLocation(filter2, filters, groups);

        expect(result.filters).toEqual([]);
        expect(result.groups[0].filters).toEqual([filter1, filter3]);
      });

      it('should extract filter from main filters list', () => {
        const filters: UIFilter[] = [filter1, filter2, filter3];
        const groups: FilterGroup[] = [];

        const result = extractFilterFromLocation(filter2, filters, groups);

        expect(result.filters).toEqual([filter1, filter3]);
        expect(result.groups).toEqual([]);
      });
    });

    describe('addFilterToGroup', () => {
      it('should add filter to existing group', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const groups = [group];

        const result = addFilterToGroup(filter3, group, groups);

        expect(result).toHaveLength(1);
        expect(result[0].filters).toEqual([filter1, filter2, filter3]);
        expect(result[0].id).toBe('group1');
      });

      it('should not modify other groups', () => {
        const group1: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const group2: FilterGroup = {
          id: 'group2',
          filters: [filter3],
          enabled: true,
        };
        const groups = [group1, group2];

        const result = addFilterToGroup(filter4, group1, groups);

        expect(result).toHaveLength(2);
        expect(result[0].filters).toEqual([filter1, filter2, filter4]);
        expect(result[1].filters).toEqual([filter3]);
      });

      it('should preserve group enabled state', () => {
        const group: FilterGroup = {
          id: 'group1',
          filters: [filter1],
          enabled: false,
        };
        const groups = [group];

        const result = addFilterToGroup(filter2, group, groups);

        expect(result[0].enabled).toBe(false);
      });
    });

    describe('createFilterGroup', () => {
      it('should create a new group with given filters', () => {
        const filters = [filter1, filter2, filter3];

        const result = createFilterGroup(filters);

        expect(result.filters).toEqual(filters);
        expect(result.enabled).toBe(true);
        expect(result.id).toMatch(/^group_\d+$/);
      });

      it('should create group with unique IDs', () => {
        const group1 = createFilterGroup([filter1]);
        const group2 = createFilterGroup([filter2]);

        // IDs should follow the pattern group_<counter>
        expect(group1.id).toMatch(/^group_\d+$/);
        expect(group2.id).toMatch(/^group_\d+$/);

        // IDs should be unique
        expect(group1.id).not.toBe(group2.id);
      });

      it('should create enabled group by default', () => {
        const result = createFilterGroup([filter1, filter2]);

        expect(result.enabled).toBe(true);
      });
    });

    describe('integration scenarios', () => {
      it('should handle complex drag-drop scenario: extract from group A and add to group B', () => {
        const groupA: FilterGroup = {
          id: 'groupA',
          filters: [filter1, filter2, filter3],
          enabled: true,
        };
        const groupB: FilterGroup = {
          id: 'groupB',
          filters: [filter4],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [groupA, groupB];

        // Extract filter2 from groupA
        const extractResult = extractFilterFromLocation(
          filter2,
          filters,
          groups,
        );

        // Add filter2 to groupB
        const finalGroups = addFilterToGroup(
          filter2,
          extractResult.groups.find((g) => g.id === 'groupB')!,
          extractResult.groups,
        );

        const groupAFinal = finalGroups.find((g) => g.id === 'groupA');
        const groupBFinal = finalGroups.find((g) => g.id === 'groupB');

        expect(groupAFinal?.filters).toEqual([filter1, filter3]);
        expect(groupBFinal?.filters).toEqual([filter4, filter2]);
      });

      it('should handle creating new group by dragging two main filters together', () => {
        const filters: UIFilter[] = [filter1, filter2, filter3, filter4];
        const groups: FilterGroup[] = [];

        // Extract filter1 from main list
        const extractResult = extractFilterFromLocation(
          filter1,
          filters,
          groups,
        );

        // Extract filter2 from main list
        const extractResult2 = extractFilterFromLocation(
          filter2,
          extractResult.filters,
          extractResult.groups,
        );

        // Create new group with both
        const newGroup = createFilterGroup([filter1, filter2]);
        const finalGroups = [...extractResult2.groups, newGroup];

        expect(extractResult2.filters).toEqual([filter3, filter4]);
        expect(finalGroups).toHaveLength(1);
        expect(finalGroups[0].filters).toEqual([filter1, filter2]);
      });

      it('should handle multiple groups with dissolution', () => {
        const group1: FilterGroup = {
          id: 'group1',
          filters: [filter1, filter2],
          enabled: true,
        };
        const group2: FilterGroup = {
          id: 'group2',
          filters: [filter3, filter4],
          enabled: true,
        };
        const filters: UIFilter[] = [];
        const groups = [group1, group2];

        // Remove filter1 from group1 (should dissolve)
        const result1 = removeFilterFromGroupsOrFilters(
          filter1,
          filters,
          groups,
        );

        // Remove filter3 from group2 (should dissolve)
        const result2 = removeFilterFromGroupsOrFilters(
          filter3,
          result1.filters,
          result1.groups,
        );

        expect(result2.filters).toEqual([filter2, filter4]);
        expect(result2.groups).toEqual([]);
      });
    });
  });

  describe('proto generation', () => {
    const sourceCols: ColumnInfo[] = [
      {
        name: 'id',
        type: 'int',
        checked: false,
        column: {name: 'id', type: {kind: 'int'}},
      },
      {
        name: 'name',
        type: 'string',
        checked: false,
        column: {name: 'name', type: {kind: 'string'}},
      },
      {
        name: 'age',
        type: 'int',
        checked: false,
        column: {name: 'age', type: {kind: 'int'}},
      },
      {
        name: 'status',
        type: 'string',
        checked: false,
        column: {name: 'status', type: {kind: 'string'}},
      },
    ];

    describe('createFiltersProto', () => {
      it('should return undefined for empty filters', () => {
        const result = createFiltersProto([], sourceCols);
        expect(result).toBeUndefined();
      });

      it('should return undefined for undefined filters', () => {
        const result = createFiltersProto(undefined, sourceCols);
        expect(result).toBeUndefined();
      });

      it('should filter out disabled filters', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1, true),
          createFilter('name', 'test', false),
          createFilter('age', 25, true),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(2);
        expect(result![0].columnName).toBe('id');
        expect(result![1].columnName).toBe('age');
      });

      it('should return undefined if all filters are disabled', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1, false),
          createFilter('name', 'test', false),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toBeUndefined();
      });

      it('should create proto for string filter', () => {
        const filters: UIFilter[] = [createFilter('name', 'test')];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(1);
        expect(result![0].columnName).toBe('name');
        expect(result![0].stringRhs).toEqual(['test']);
      });

      it('should create proto for number filter', () => {
        const filters: UIFilter[] = [createFilter('id', 42)];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(1);
        expect(result![0].columnName).toBe('id');
        expect(result![0].int64Rhs).toEqual([42]);
      });

      it('should handle multiple filters', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1),
          createFilter('name', 'alice'),
          createFilter('age', 30),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(3);
        expect(result![0].columnName).toBe('id');
        expect(result![1].columnName).toBe('name');
        expect(result![2].columnName).toBe('age');
      });
    });

    describe('createExperimentalFiltersProto', () => {
      describe('without groups', () => {
        it('should return undefined for empty filters and no groups', () => {
          const result = createExperimentalFiltersProto(
            [],
            sourceCols,
            'AND',
            [],
          );

          expect(result).toBeUndefined();
        });

        it('should create AND group for multiple filters', () => {
          const filters: UIFilter[] = [
            createFilter('id', 1),
            createFilter('name', 'test'),
          ];

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'AND',
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
          expect(result!.filters).toHaveLength(2);
        });

        it('should create OR group for multiple filters', () => {
          const filters: UIFilter[] = [
            createFilter('id', 1),
            createFilter('name', 'test'),
          ];

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'OR',
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .OR,
          );
          expect(result!.filters).toHaveLength(2);
        });

        it('should default to AND when operator not specified', () => {
          const filters: UIFilter[] = [createFilter('id', 1)];

          const result = createExperimentalFiltersProto(filters, sourceCols);

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
        });
      });

      describe('with groups', () => {
        it('should create nested structure with OR groups ANDed at root', () => {
          const filters: UIFilter[] = [createFilter('status', 'active')];

          const group1: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1), createFilter('id', 2)],
            enabled: true,
          };

          const group2: FilterGroup = {
            id: 'group2',
            filters: [
              createFilter('name', 'alice'),
              createFilter('name', 'bob'),
            ],
            enabled: true,
          };

          const groups = [group1, group2];

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'AND',
            groups,
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
          expect(result!.filters).toHaveLength(1);
          expect(result!.filters![0].columnName).toBe('status');
          expect(result!.groups).toHaveLength(2);

          // Check first OR group
          expect(result!.groups![0].op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .OR,
          );
          expect(result!.groups![0].filters).toHaveLength(2);
          expect(result!.groups![0].filters![0].columnName).toBe('id');
          expect(result!.groups![0].filters![1].columnName).toBe('id');

          // Check second OR group
          expect(result!.groups![1].op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .OR,
          );
          expect(result!.groups![1].filters).toHaveLength(2);
          expect(result!.groups![1].filters![0].columnName).toBe('name');
          expect(result!.groups![1].filters![1].columnName).toBe('name');
        });

        it('should handle groups without main filters', () => {
          const group: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1), createFilter('id', 2)],
            enabled: true,
          };

          const result = createExperimentalFiltersProto([], sourceCols, 'AND', [
            group,
          ]);

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
          // When main filters are empty, filters should be undefined or an empty array
          expect(
            result!.filters === undefined || result!.filters.length === 0,
          ).toBe(true);
          expect(result!.groups).toHaveLength(1);
          expect(result!.groups![0].op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .OR,
          );
        });

        it('should filter out disabled groups', () => {
          const group1: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1), createFilter('id', 2)],
            enabled: true,
          };

          const group2: FilterGroup = {
            id: 'group2',
            filters: [createFilter('name', 'alice')],
            enabled: false,
          };

          const result = createExperimentalFiltersProto([], sourceCols, 'AND', [
            group1,
            group2,
          ]);

          expect(result).toBeDefined();
          expect(result!.groups).toHaveLength(1);
          expect(result!.groups![0].filters).toHaveLength(2);
        });

        it('should filter out disabled filters within groups', () => {
          const group: FilterGroup = {
            id: 'group1',
            filters: [
              createFilter('id', 1, true),
              createFilter('id', 2, false),
              createFilter('id', 3, true),
            ],
            enabled: true,
          };

          const result = createExperimentalFiltersProto([], sourceCols, 'AND', [
            group,
          ]);

          expect(result).toBeDefined();
          expect(result!.groups).toHaveLength(1);
          expect(result!.groups![0].filters).toHaveLength(2);
          expect(result!.groups![0].filters![0].int64Rhs).toEqual([1]);
          expect(result!.groups![0].filters![1].int64Rhs).toEqual([3]);
        });

        it('should return undefined if groups have no enabled filters', () => {
          const group: FilterGroup = {
            id: 'group1',
            filters: [
              createFilter('id', 1, false),
              createFilter('id', 2, false),
            ],
            enabled: true,
          };

          const result = createExperimentalFiltersProto([], sourceCols, 'AND', [
            group,
          ]);

          expect(result).toBeUndefined();
        });

        it('should skip groups that become empty after filtering disabled filters', () => {
          const group1: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1, false)],
            enabled: true,
          };

          const group2: FilterGroup = {
            id: 'group2',
            filters: [createFilter('name', 'alice', true)],
            enabled: true,
          };

          const result = createExperimentalFiltersProto([], sourceCols, 'AND', [
            group1,
            group2,
          ]);

          expect(result).toBeDefined();
          expect(result!.groups).toHaveLength(1);
          expect(result!.groups![0].filters![0].columnName).toBe('name');
        });

        it('should handle complex nested scenario', () => {
          // Main filters: status = 'active'
          // Group 1 (OR): id = 1 OR id = 2
          // Group 2 (OR): name = 'alice' OR name = 'bob' OR name = 'charlie'
          // Result: status = 'active' AND (id = 1 OR id = 2) AND (name = 'alice' OR name = 'bob' OR name = 'charlie')

          const filters: UIFilter[] = [createFilter('status', 'active')];

          const group1: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1), createFilter('id', 2)],
            enabled: true,
          };

          const group2: FilterGroup = {
            id: 'group2',
            filters: [
              createFilter('name', 'alice'),
              createFilter('name', 'bob'),
              createFilter('name', 'charlie'),
            ],
            enabled: true,
          };

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'AND',
            [group1, group2],
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );

          // Main filter
          expect(result!.filters).toHaveLength(1);
          expect(result!.filters![0].columnName).toBe('status');

          // Two OR groups
          expect(result!.groups).toHaveLength(2);

          // First OR group
          expect(result!.groups![0].filters).toHaveLength(2);
          expect(result!.groups![0].filters![0].int64Rhs).toEqual([1]);
          expect(result!.groups![0].filters![1].int64Rhs).toEqual([2]);

          // Second OR group
          expect(result!.groups![1].filters).toHaveLength(3);
          expect(result!.groups![1].filters![0].stringRhs).toEqual(['alice']);
          expect(result!.groups![1].filters![1].stringRhs).toEqual(['bob']);
          expect(result!.groups![1].filters![2].stringRhs).toEqual(['charlie']);
        });

        it('should handle mix of enabled and disabled main filters with groups', () => {
          const filters: UIFilter[] = [
            createFilter('status', 'active', true),
            createFilter('deleted', 0, false),
          ];

          const group: FilterGroup = {
            id: 'group1',
            filters: [createFilter('id', 1), createFilter('id', 2)],
            enabled: true,
          };

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'AND',
            [group],
          );

          expect(result).toBeDefined();
          expect(result!.filters).toHaveLength(1);
          expect(result!.filters![0].columnName).toBe('status');
        });
      });
    });
  });
});
