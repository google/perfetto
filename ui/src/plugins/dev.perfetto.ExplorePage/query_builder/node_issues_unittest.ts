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

import {NodeIssues} from './node_issues';

describe('NodeIssues', () => {
  describe('hasIssues', () => {
    it('should return false when no issues exist', () => {
      const issues = new NodeIssues();

      expect(issues.hasIssues()).toBe(false);
    });

    it('should return true when queryError exists', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query error');

      expect(issues.hasIssues()).toBe(true);
    });

    it('should return true when responseError exists', () => {
      const issues = new NodeIssues();
      issues.responseError = new Error('Response error');

      expect(issues.hasIssues()).toBe(true);
    });

    it('should return true when dataError exists', () => {
      const issues = new NodeIssues();
      issues.dataError = new Error('Data error');

      expect(issues.hasIssues()).toBe(true);
    });

    it('should return true when warnings exist', () => {
      const issues = new NodeIssues();
      issues.warnings = [new Error('Warning 1')];

      expect(issues.hasIssues()).toBe(true);
    });

    it('should return true when executionError exists', () => {
      const issues = new NodeIssues();
      issues.executionError = new Error('Execution error');

      expect(issues.hasIssues()).toBe(true);
    });

    it('should return true when multiple issues exist', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query error');
      issues.warnings = [new Error('Warning 1'), new Error('Warning 2')];

      expect(issues.hasIssues()).toBe(true);
    });
  });

  describe('getTitle', () => {
    it('should return empty string when no issues exist', () => {
      const issues = new NodeIssues();

      const result = issues.getTitle();

      expect(result).toBe('');
    });

    it('should format queryError correctly', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Invalid SQL syntax');

      const result = issues.getTitle();

      expect(result).toBe('Query Error: Invalid SQL syntax\n');
    });

    it('should format responseError correctly', () => {
      const issues = new NodeIssues();
      issues.responseError = new Error('Connection timeout');

      const result = issues.getTitle();

      expect(result).toBe('Response Error: Connection timeout\n');
    });

    it('should format dataError correctly', () => {
      const issues = new NodeIssues();
      issues.dataError = new Error('Invalid data format');

      const result = issues.getTitle();

      expect(result).toBe('Data Error: Invalid data format\n');
    });

    it('should format executionError correctly', () => {
      const issues = new NodeIssues();
      issues.executionError = new Error('Materialization failed');

      const result = issues.getTitle();

      expect(result).toBe('Execution Error: Materialization failed\n');
    });

    it('should format warnings correctly', () => {
      const issues = new NodeIssues();
      issues.warnings = [
        new Error('Warning 1'),
        new Error('Warning 2'),
        new Error('Warning 3'),
      ];

      const result = issues.getTitle();

      expect(result).toContain('Warnings:');
      expect(result).toContain('Warning 1');
      expect(result).toContain('Warning 2');
      expect(result).toContain('Warning 3');
    });

    it('should format multiple issue types together', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query failed');
      issues.responseError = new Error('Response failed');
      issues.dataError = new Error('Data failed');
      issues.warnings = [new Error('Warning 1'), new Error('Warning 2')];

      const result = issues.getTitle();

      expect(result).toContain('Query Error: Query failed');
      expect(result).toContain('Response Error: Response failed');
      expect(result).toContain('Data Error: Data failed');
      expect(result).toContain('Warnings:');
      expect(result).toContain('Warning 1');
      expect(result).toContain('Warning 2');
    });

    it('should handle empty warnings array', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query failed');
      issues.warnings = [];

      const result = issues.getTitle();

      expect(result).toBe('Query Error: Query failed\n');
      expect(result).not.toContain('Warnings:');
    });
  });

  describe('clear', () => {
    it('should clear validation errors and warnings but not executionError', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query error');
      issues.responseError = new Error('Response error');
      issues.dataError = new Error('Data error');
      issues.executionError = new Error('Execution error');
      issues.warnings = [new Error('Warning 1'), new Error('Warning 2')];

      issues.clear();

      expect(issues.queryError).toBeUndefined();
      expect(issues.responseError).toBeUndefined();
      expect(issues.dataError).toBeUndefined();
      expect(issues.warnings).toEqual([]);
      // executionError should NOT be cleared by clear()
      expect(issues.executionError).toBeDefined();
      expect(issues.executionError?.message).toBe('Execution error');
      expect(issues.hasIssues()).toBe(true);
    });

    it('should work when called multiple times', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query error');

      issues.clear();
      issues.clear();

      expect(issues.queryError).toBeUndefined();
    });

    it('should allow adding new issues after clearing', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('First error');

      issues.clear();

      issues.responseError = new Error('Second error');

      expect(issues.queryError).toBeUndefined();
      expect(issues.responseError).toBeDefined();
      expect(issues.hasIssues()).toBe(true);
    });

    it('should work on empty NodeIssues', () => {
      const issues = new NodeIssues();

      expect(() => issues.clear()).not.toThrow();
      expect(issues.hasIssues()).toBe(false);
    });
  });

  describe('clearExecutionError', () => {
    it('should clear only executionError', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error('Query error');
      issues.executionError = new Error('Execution error');

      issues.clearExecutionError();

      expect(issues.executionError).toBeUndefined();
      expect(issues.queryError).toBeDefined();
      expect(issues.hasIssues()).toBe(true);
    });

    it('should work when executionError is undefined', () => {
      const issues = new NodeIssues();

      expect(() => issues.clearExecutionError()).not.toThrow();
    });
  });

  describe('integration tests', () => {
    it('should maintain consistent state through lifecycle', () => {
      const issues = new NodeIssues();

      // Initially no issues
      expect(issues.hasIssues()).toBe(false);
      expect(issues.getTitle()).toBe('');

      // Add query error
      issues.queryError = new Error('Query failed');
      expect(issues.hasIssues()).toBe(true);
      expect(issues.getTitle()).toContain('Query Error: Query failed');

      // Add warnings
      issues.warnings.push(new Error('Warning 1'));
      expect(issues.hasIssues()).toBe(true);
      expect(issues.getTitle()).toContain('Warning 1');

      // Clear all
      issues.clear();
      expect(issues.hasIssues()).toBe(false);
      expect(issues.getTitle()).toBe('');
    });

    it('should handle complex error messages', () => {
      const issues = new NodeIssues();
      issues.queryError = new Error(
        'SQL syntax error at line 5: unexpected token "FROM"',
      );
      issues.warnings = [
        new Error('Performance: Query may be slow'),
        new Error('Deprecated: Using old API version'),
      ];

      const title = issues.getTitle();

      expect(title).toContain(
        'SQL syntax error at line 5: unexpected token "FROM"',
      );
      expect(title).toContain('Performance: Query may be slow');
      expect(title).toContain('Deprecated: Using old API version');
    });
  });
});
