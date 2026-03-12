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
  isNumericType,
  isStringType,
  isColumnValidForAggregation,
  getAggregationTypeRequirements,
} from './utils';
import {ColumnInfo} from './column_info';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../trace_processor/perfetto_sql_type';

describe('utils', () => {
  describe('isNumericType', () => {
    it('should return true for INT type', () => {
      expect(isNumericType(PerfettoSqlTypes.INT)).toBe(true);
    });

    it('should return true for DOUBLE type', () => {
      expect(isNumericType(PerfettoSqlTypes.DOUBLE)).toBe(true);
    });

    it('should return true for DURATION type', () => {
      expect(isNumericType(PerfettoSqlTypes.DURATION)).toBe(true);
    });

    it('should return true for TIMESTAMP type', () => {
      expect(isNumericType(PerfettoSqlTypes.TIMESTAMP)).toBe(true);
    });

    it('should return true for BOOLEAN type', () => {
      expect(isNumericType(PerfettoSqlTypes.BOOLEAN)).toBe(true);
    });

    it('should return true for ID types', () => {
      expect(
        isNumericType({kind: 'id', source: {table: 'slice', column: 'id'}}),
      ).toBe(true);
      expect(
        isNumericType({kind: 'id', source: {table: 'thread', column: 'id'}}),
      ).toBe(true);
      expect(
        isNumericType({kind: 'id', source: {table: 'process', column: 'id'}}),
      ).toBe(true);
    });

    it('should return true for JOINID types', () => {
      expect(
        isNumericType({
          kind: 'joinid',
          source: {table: 'slice', column: 'id'},
        }),
      ).toBe(true);
      expect(
        isNumericType({
          kind: 'joinid',
          source: {table: 'thread', column: 'id'},
        }),
      ).toBe(true);
    });

    it('should return true for ARG_SET_ID type', () => {
      expect(isNumericType(PerfettoSqlTypes.ARG_SET_ID)).toBe(true);
    });

    it('should return false for STRING type', () => {
      expect(isNumericType(PerfettoSqlTypes.STRING)).toBe(false);
    });

    it('should return false for undefined type', () => {
      expect(isNumericType(undefined)).toBe(false);
    });
  });

  describe('isStringType', () => {
    it('should return true for STRING type', () => {
      expect(isStringType(PerfettoSqlTypes.STRING)).toBe(true);
    });

    it('should return false for non-string types', () => {
      expect(isStringType(PerfettoSqlTypes.INT)).toBe(false);
      expect(isStringType(PerfettoSqlTypes.DOUBLE)).toBe(false);
      expect(isStringType(PerfettoSqlTypes.DURATION)).toBe(false);
    });
  });

  describe('isColumnValidForAggregation', () => {
    function createColumnInfo(
      name: string,
      type?: PerfettoSqlType,
    ): ColumnInfo {
      return {
        name,
        checked: false,
        column: {
          name,
          type,
        },
      };
    }

    describe('MEAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'MEAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.DOUBLE),
            'MEAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('ts', PerfettoSqlTypes.TIMESTAMP),
            'MEAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'MEAN',
          ),
        ).toBe(false);
      });
    });

    describe('MEDIAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'MEDIAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.DOUBLE),
            'MEDIAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'MEDIAN',
          ),
        ).toBe(false);
      });
    });

    describe('PERCENTILE operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'PERCENTILE',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.DOUBLE),
            'PERCENTILE',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('id', {
              kind: 'id',
              source: {table: 'slice', column: 'id'},
            }),
            'PERCENTILE',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'PERCENTILE',
          ),
        ).toBe(false);
      });
    });

    describe('DURATION_WEIGHTED_MEAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.INT),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.DURATION),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(false);
      });
    });

    describe('GLOB operation', () => {
      it('should allow string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'GLOB',
          ),
        ).toBe(true);
      });

      it('should reject numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'GLOB',
          ),
        ).toBe(false);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.DOUBLE),
            'GLOB',
          ),
        ).toBe(false);
      });
    });

    describe('COUNT operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'COUNT',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'COUNT',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', PerfettoSqlTypes.DOUBLE),
            'COUNT',
          ),
        ).toBe(true);
      });
    });

    describe('COUNT(*) operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'COUNT(*)',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'COUNT(*)',
          ),
        ).toBe(true);
      });
    });

    describe('SUM operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'SUM',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'SUM',
          ),
        ).toBe(true);
      });
    });

    describe('MIN/MAX operations', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'MIN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'MIN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            'MAX',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            'MAX',
          ),
        ).toBe(true);
      });
    });

    describe('undefined operation', () => {
      it('should return true for undefined operation', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', PerfettoSqlTypes.STRING),
            undefined,
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', PerfettoSqlTypes.INT),
            undefined,
          ),
        ).toBe(true);
      });
    });
  });

  describe('getAggregationTypeRequirements', () => {
    it('should return correct requirements for numeric-only operations', () => {
      expect(getAggregationTypeRequirements('MEAN')).toBe(
        'Requires numeric column',
      );
      expect(getAggregationTypeRequirements('MEDIAN')).toBe(
        'Requires numeric column',
      );
      expect(getAggregationTypeRequirements('PERCENTILE')).toBe(
        'Requires numeric column',
      );
      expect(getAggregationTypeRequirements('DURATION_WEIGHTED_MEAN')).toBe(
        'Requires numeric column',
      );
    });

    it('should return correct requirements for string-only operations', () => {
      expect(getAggregationTypeRequirements('GLOB')).toBe(
        'Requires string column',
      );
    });

    it('should return correct requirements for COUNT(*)', () => {
      expect(getAggregationTypeRequirements('COUNT(*)')).toBe(
        'No column required',
      );
    });

    it('should return correct requirements for universal operations', () => {
      expect(getAggregationTypeRequirements('COUNT')).toBe(
        'Works with any column type',
      );
      expect(getAggregationTypeRequirements('SUM')).toBe(
        'Works with any column type',
      );
      expect(getAggregationTypeRequirements('MIN')).toBe(
        'Works with any column type',
      );
      expect(getAggregationTypeRequirements('MAX')).toBe(
        'Works with any column type',
      );
    });

    it('should return unknown for unrecognized operations', () => {
      expect(getAggregationTypeRequirements('UNKNOWN_OP')).toBe(
        'Unknown operation',
      );
    });
  });
});
