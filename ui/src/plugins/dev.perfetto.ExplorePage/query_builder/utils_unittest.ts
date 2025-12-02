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

describe('utils', () => {
  describe('isNumericType', () => {
    it('should return true for INT type', () => {
      expect(isNumericType('INT')).toBe(true);
      expect(isNumericType('int')).toBe(true);
    });

    it('should return true for DOUBLE type', () => {
      expect(isNumericType('DOUBLE')).toBe(true);
      expect(isNumericType('double')).toBe(true);
    });

    it('should return true for DURATION type', () => {
      expect(isNumericType('DURATION')).toBe(true);
      expect(isNumericType('duration')).toBe(true);
    });

    it('should return true for TIMESTAMP type', () => {
      expect(isNumericType('TIMESTAMP')).toBe(true);
      expect(isNumericType('timestamp')).toBe(true);
    });

    it('should return true for BOOLEAN type', () => {
      expect(isNumericType('BOOLEAN')).toBe(true);
      expect(isNumericType('boolean')).toBe(true);
    });

    it('should return true for ID types', () => {
      expect(isNumericType('ID(slice)')).toBe(true);
      expect(isNumericType('ID(thread)')).toBe(true);
      expect(isNumericType('id(process)')).toBe(true);
    });

    it('should return true for JOINID types', () => {
      expect(isNumericType('JOINID(slice.id)')).toBe(true);
      expect(isNumericType('joinid(thread.id)')).toBe(true);
    });

    it('should return true for ARG_SET_ID type', () => {
      expect(isNumericType('ARG_SET_ID')).toBe(true);
      expect(isNumericType('arg_set_id')).toBe(true);
    });

    it('should return false for STRING type', () => {
      expect(isNumericType('STRING')).toBe(false);
      expect(isNumericType('string')).toBe(false);
    });

    it('should return false for unknown types', () => {
      expect(isNumericType('UNKNOWN')).toBe(false);
      expect(isNumericType('BLOB')).toBe(false);
    });
  });

  describe('isStringType', () => {
    it('should return true for STRING type', () => {
      expect(isStringType('STRING')).toBe(true);
      expect(isStringType('string')).toBe(true);
    });

    it('should return false for non-string types', () => {
      expect(isStringType('INT')).toBe(false);
      expect(isStringType('DOUBLE')).toBe(false);
      expect(isStringType('DURATION')).toBe(false);
    });
  });

  describe('isColumnValidForAggregation', () => {
    function createColumnInfo(name: string, type: string): ColumnInfo {
      return {
        name,
        type,
        checked: false,
        column: {
          name,
        },
      };
    }

    describe('MEAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'MEAN'),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'DOUBLE'),
            'MEAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('ts', 'TIMESTAMP'),
            'MEAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'MEAN',
          ),
        ).toBe(false);
      });
    });

    describe('MEDIAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'MEDIAN'),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'DOUBLE'),
            'MEDIAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'MEDIAN',
          ),
        ).toBe(false);
      });
    });

    describe('PERCENTILE operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', 'INT'),
            'PERCENTILE',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'DOUBLE'),
            'PERCENTILE',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('id', 'ID(slice)'),
            'PERCENTILE',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'PERCENTILE',
          ),
        ).toBe(false);
      });
    });

    describe('DURATION_WEIGHTED_MEAN operation', () => {
      it('should allow numeric columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'INT'),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', 'DURATION'),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(true);
      });

      it('should reject string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'DURATION_WEIGHTED_MEAN',
          ),
        ).toBe(false);
      });
    });

    describe('GLOB operation', () => {
      it('should allow string columns', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'GLOB',
          ),
        ).toBe(true);
      });

      it('should reject numeric columns', () => {
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'GLOB'),
        ).toBe(false);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'DOUBLE'),
            'GLOB',
          ),
        ).toBe(false);
      });
    });

    describe('COUNT operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'COUNT',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'COUNT'),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('value', 'DOUBLE'),
            'COUNT',
          ),
        ).toBe(true);
      });
    });

    describe('COUNT(*) operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'COUNT(*)',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', 'INT'),
            'COUNT(*)',
          ),
        ).toBe(true);
      });
    });

    describe('SUM operation', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'SUM',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'SUM'),
        ).toBe(true);
      });
    });

    describe('MIN/MAX operations', () => {
      it('should allow all column types', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'MIN',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'MIN'),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            'MAX',
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(createColumnInfo('dur', 'INT'), 'MAX'),
        ).toBe(true);
      });
    });

    describe('undefined operation', () => {
      it('should return true for undefined operation', () => {
        expect(
          isColumnValidForAggregation(
            createColumnInfo('name', 'STRING'),
            undefined,
          ),
        ).toBe(true);
        expect(
          isColumnValidForAggregation(
            createColumnInfo('dur', 'INT'),
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
