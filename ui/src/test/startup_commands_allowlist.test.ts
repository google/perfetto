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

import {test, expect} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
import {
  STARTUP_COMMAND_ALLOWLIST,
  isStartupCommandAllowed,
} from '../core/startup_command_allowlist';

interface CommandTestCase {
  id: string;
  args: unknown[];
  traceFile?: string;
  before?: () => Promise<void>;
  after?: () => Promise<void>;
  maskQueryDetails?: boolean;
  testName?: string; // Optional custom test name for unique identification
}

// Test cases for each startup command
const COMMAND_TEST_CASES: CommandTestCase[] = [
  // Track manipulation commands
  {
    id: 'dev.perfetto.PinTracksByRegex',
    args: ['.*surfaceflinger.*'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with surfaceflinger tracks
  },
  {
    id: 'dev.perfetto.PinTracksByRegex',
    args: ['.*surfaceflinger.*', 'name'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with surfaceflinger tracks - explicit name filtering
    testName: 'dev.perfetto.PinTracksByRegex with explicit name filtering',
  },
  {
    id: 'dev.perfetto.PinTracksByRegex',
    args: ['.*surfaceflinger.*', 'path'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with surfaceflinger tracks - path filtering
    testName: 'dev.perfetto.PinTracksByRegex with path filtering',
  },
  {
    id: 'dev.perfetto.ExpandTracksByRegex',
    args: ['.*system_server.*'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks
  },
  {
    id: 'dev.perfetto.ExpandTracksByRegex',
    args: ['.*system_server.*', 'name'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks - explicit name filtering
    testName: 'dev.perfetto.ExpandTracksByRegex with explicit name filtering',
  },
  {
    id: 'dev.perfetto.ExpandTracksByRegex',
    args: ['.*system_server.*', 'path'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks - path filtering
    testName: 'dev.perfetto.ExpandTracksByRegex with path filtering',
  },
  {
    id: 'dev.perfetto.CollapseTracksByRegex',
    args: ['CPU Scheduling'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks
  },
  {
    id: 'dev.perfetto.CollapseTracksByRegex',
    args: ['CPU Scheduling', 'name'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks - explicit name filtering
    testName: 'dev.perfetto.CollapseTracksByRegex with explicit name filtering',
  },
  {
    id: 'dev.perfetto.CollapseTracksByRegex',
    args: ['.*CPU.*', 'path'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks - path filtering
    testName: 'dev.perfetto.CollapseTracksByRegex with path filtering',
  },

  // Debug track commands
  {
    id: 'dev.perfetto.AddDebugSliceTrack',
    args: [
      'select ts, dur, name from slice order by dur desc limit 100',
      'Test Debug Slice Track',
    ],
    traceFile: 'api34_startup_cold.perfetto-trace', // Chrome trace with rich slice data
    maskQueryDetails: true,
  },
  {
    id: 'dev.perfetto.AddDebugSliceTrackWithPivot',
    args: [
      `select ts, dur, name, ifnull(category, '[NULL]') as category from slice order by dur desc limit 100`,
      'category',
      'Test Debug Slice Track with Pivot',
    ],
    traceFile: 'api34_startup_cold.perfetto-trace', // Chrome trace with categorized slices
    maskQueryDetails: true,
  },
  {
    id: 'dev.perfetto.AddDebugCounterTrack',
    args: [
      'select ts, value from counter limit 100',
      'Test Debug Counter Track',
    ],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with counter data
    maskQueryDetails: true,
  },
  {
    id: 'dev.perfetto.AddDebugCounterTrackWithPivot',
    args: [
      'select ts, value, name from counter join counter_track on counter.track_id = counter_track.id limit 100',
      'name',
      'Test Debug Counter Track with Pivot',
    ],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with named counter tracks
    maskQueryDetails: true,
  },

  // Workspace commands
  {
    id: 'dev.perfetto.CreateWorkspace',
    args: ['Test Workspace'],
    traceFile: 'missing_track_names.pb', // Simple trace for workspace operations
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      const workspace = trace.workspaces.all.find(
        (w) => w.title === 'Test Workspace',
      );
      if (!workspace) throw new Error('Test Workspace not found');
      trace.workspaces.switchWorkspace(workspace);
    },
  },
  {
    id: 'dev.perfetto.SwitchWorkspace',
    args: ['Test Workspace'],
    traceFile: 'missing_track_names.pb', // Simple trace for workspace operations
    before: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      trace.workspaces.createEmptyWorkspace('Test Workspace');
    },
  },
  {
    id: 'dev.perfetto.CopyTracksToWorkspaceByRegex',
    args: ['(Expected|Actual) Timeline', 'Test Workspace'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with surfaceflinger tracks
    before: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      trace.workspaces.createEmptyWorkspace('Test Workspace');
    },
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      const workspace = trace.workspaces.all.find(
        (w) => w.title === 'Test Workspace',
      );
      if (!workspace) throw new Error('Test Workspace not found');
      trace.workspaces.switchWorkspace(workspace);
    },
  },
  {
    id: 'dev.perfetto.CopyTracksToWorkspaceByRegex',
    args: ['.*surfaceflinger.*', 'Test Workspace Path', 'path'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with surfaceflinger tracks - path filtering
    testName: 'dev.perfetto.CopyTracksToWorkspaceByRegex with path filtering',
    before: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      trace.workspaces.createEmptyWorkspace('Test Workspace Path');
    },
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      const workspace = trace.workspaces.all.find(
        (w) => w.title === 'Test Workspace Path',
      );
      if (!workspace) throw new Error('Test Workspace Path not found');
      trace.workspaces.switchWorkspace(workspace);
    },
  },
  {
    id: 'dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors',
    args: ['(Expected|Actual) Timeline', 'Test Workspace'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks
    before: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      trace.workspaces.createEmptyWorkspace('Test Workspace');
    },
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      const workspace = trace.workspaces.all.find(
        (w) => w.title === 'Test Workspace',
      );
      if (!workspace) throw new Error('Test Workspace not found');
      trace.workspaces.switchWorkspace(workspace);
    },
  },
  {
    id: 'dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors',
    args: ['.*surfaceflinger.*', 'Test Workspace Ancestors Path', 'path'],
    traceFile: 'api34_startup_cold.perfetto-trace', // Android trace with system_server tracks - path filtering
    testName:
      'dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors with path filtering',
    before: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      trace.workspaces.createEmptyWorkspace('Test Workspace Ancestors Path');
    },
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');
      const workspace = trace.workspaces.all.find(
        (w) => w.title === 'Test Workspace Ancestors Path',
      );
      if (!workspace) {
        throw new Error('Test Workspace Ancestors Path not found');
      }
      trace.workspaces.switchWorkspace(workspace);
    },
  },

  // Query commands
  {
    id: 'dev.perfetto.RunQuery',
    args: ['CREATE TABLE test_command_execution AS SELECT 42 as test_value'],
    traceFile: 'chrome_rendering_desktop.pftrace', // Chrome trace for creating test table
    maskQueryDetails: true,
    after: async () => {
      const trace = self.app.trace;
      if (!trace) throw new Error('No trace loaded');

      // Verify the table was created by querying it
      const result = await trace.engine.query(
        'SELECT test_value FROM test_command_execution',
      );
      if (result.error() !== undefined) {
        throw new Error(`Failed to query test table: ${result.error()}`);
      }

      // Verify we got the expected constant value (cannot use NUM here as
      // we are inside the puppeteer context).
      const row = result.firstRow({test_value: Number()});
      if (row.test_value !== 42) {
        throw new Error(`Expected test_value=42, got: ${row.test_value}`);
      }
    },
  },
  {
    id: 'dev.perfetto.RunQueryAndShowTab',
    args: ['select ts, dur, name from slice limit 50'],
    traceFile: 'chrome_rendering_desktop.pftrace', // Chrome trace with rich slice data for queries
    maskQueryDetails: true,
  },
];

test('macro commands are allowed correctly', async () => {
  // Test isStartupCommandAllowed function with macro commands
  expect(isStartupCommandAllowed('dev.perfetto.UserMacro.TestMacro')).toBe(
    true,
  );
  expect(isStartupCommandAllowed('dev.perfetto.UserMacro.AnotherMacro')).toBe(
    true,
  );
  expect(isStartupCommandAllowed('dev.perfetto.UserMacro.')).toBe(true); // Edge case: empty name
  expect(isStartupCommandAllowed('dev.perfetto.UserMacro')).toBe(false); // Missing dot
  expect(isStartupCommandAllowed('dev.perfetto.NotUserMacro.Test')).toBe(false); // Different prefix
});

test('all allowlisted commands have corresponding test cases', async () => {
  // Extract command IDs from allowlist and test cases
  const allowlistedIds = new Set(STARTUP_COMMAND_ALLOWLIST);
  const testCaseIds = new Set(
    COMMAND_TEST_CASES.map((testCase) => testCase.id),
  );

  // Find commands that are allowlisted but don't have test cases
  const missingTestCases = [...allowlistedIds].filter(
    (id) => !testCaseIds.has(id),
  );

  // Find test cases that don't correspond to allowlisted commands
  const extraTestCases = [...testCaseIds].filter(
    (id) => !allowlistedIds.has(id),
  );

  // Report any mismatches
  if (missingTestCases.length > 0) {
    console.error(
      'Commands in allowlist without test cases:',
      missingTestCases,
    );
  }
  if (extraTestCases.length > 0) {
    console.error(
      'Test cases without corresponding allowlisted commands:',
      extraTestCases,
    );
  }

  // Assert that all allowlisted commands have test cases and vice versa
  expect(missingTestCases).toEqual([]);
  expect(extraTestCases).toEqual([]);
});

// Generate screenshot tests for each command
for (const testCase of COMMAND_TEST_CASES) {
  const testTitle = testCase.testName || `${testCase.id} command test`;
  test(testTitle, async ({browser}) => {
    const page = await browser.newPage();
    const pth = new PerfettoTestHelper(page);

    // Load the appropriate trace for this test case
    if (testCase.traceFile) {
      await pth.openTraceFile(testCase.traceFile);
    }

    // Disable omnibox prompts to evaluate similar to a startup command
    // environment.
    pth.disableOmniboxPrompt();

    // Run before if provided
    if (testCase.before) {
      await page.evaluate(testCase.before);
      await pth.waitForPerfettoIdle();
    }

    // Execute the command
    await pth.runCommand(testCase.id, ...testCase.args);
    await pth.waitForPerfettoIdle();

    // Run after if provided
    if (testCase.after) {
      await page.evaluate(testCase.after);
      await pth.waitForPerfettoIdle();
    }

    // Do a full redraw in case `after` made some changes which didn't trigger
    // a redraw.
    await pth.scheduleFullRedraw();

    // Take after screenshot with masking if specified
    const screenshotOptions = testCase.maskQueryDetails
      ? {mask: [page.locator('.pf-query-table .pf-header-bar')]}
      : {};
    await pth.waitForIdleAndScreenshot(`after.png`, screenshotOptions);
  });
}
