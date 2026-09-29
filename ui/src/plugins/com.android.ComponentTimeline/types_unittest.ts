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

import {describe, expect, test} from 'vitest';
import {
  classifyProcStateFamily,
  formatShortProcState,
  formatShortTarget,
  PROC_STATE_FAMILY_COLORS,
  STATE_COLORS,
} from './types';

describe('classifyProcStateFamily', () => {
  test('classifies killed bucket state as KILLED regardless of proc_state', () => {
    expect(classifyProcStateFamily('TOP', 'K')).toBe('KILLED');
    expect(classifyProcStateFamily(null, 'K')).toBe('KILLED');
  });

  test('classifies top/foreground/persistent states as TOP', () => {
    expect(classifyProcStateFamily('TOP', 'C')).toBe('TOP');
    expect(classifyProcStateFamily('BOUND_TOP', 'R')).toBe('TOP');
    expect(classifyProcStateFamily('PERSISTENT', 'I')).toBe('TOP');
    expect(classifyProcStateFamily('PERSISTENT_UI', 'C')).toBe('TOP');
    expect(classifyProcStateFamily('IMPORTANT_FOREGROUND', 'C')).toBe('TOP');
    expect(classifyProcStateFamily('top_app', 'C')).toBe('TOP');
  });

  test('classifies foreground service states as FG_SVC', () => {
    expect(classifyProcStateFamily('FOREGROUND_SERVICE', 'R')).toBe('FG_SVC');
    expect(classifyProcStateFamily('BOUND_FOREGROUND_SERVICE', 'C')).toBe(
      'FG_SVC',
    );
    expect(classifyProcStateFamily('SHORT_SERVICE', 'S')).toBe('FG_SVC');
    expect(classifyProcStateFamily('fg_service', 'C')).toBe('FG_SVC');
  });

  test('classifies receiver and backup states as RECEIVER', () => {
    expect(classifyProcStateFamily('RECEIVER', 'R')).toBe('RECEIVER');
    expect(classifyProcStateFamily('BACKUP', 'C')).toBe('RECEIVER');
  });

  test('classifies background service and perceptible states as SERVICE', () => {
    expect(classifyProcStateFamily('SERVICE', 'R')).toBe('SERVICE');
    expect(classifyProcStateFamily('IMPORTANT_BACKGROUND', 'I')).toBe(
      'SERVICE',
    );
    expect(classifyProcStateFamily('TRANSIENT_BACKGROUND', 'C')).toBe(
      'SERVICE',
    );
    expect(classifyProcStateFamily('perceptible_low', 'I')).toBe('SERVICE');
  });

  test('classifies cached and null states as CACHED', () => {
    expect(classifyProcStateFamily('CACHED_EMPTY', 'I')).toBe('CACHED');
    expect(classifyProcStateFamily('CACHED_ACTIVITY', 'I')).toBe('CACHED');
    expect(classifyProcStateFamily('HOME', 'I')).toBe('CACHED');
    expect(classifyProcStateFamily(null, 'I')).toBe('CACHED');
  });
});

describe('formatShortProcState', () => {
  test('abbreviates verbose framework process state names', () => {
    expect(formatShortProcState('FOREGROUND_SERVICE', 50)).toBe('FGS');
    expect(formatShortProcState('BOUND_FOREGROUND_SERVICE', 50)).toBe('BFGS');
    expect(formatShortProcState('RECEIVER', 0)).toBe('RCVR');
    expect(formatShortProcState('CACHED_EMPTY', 900)).toBe('CACHED');
    expect(formatShortProcState('TOP', 0)).toBe('TOP');
    expect(formatShortProcState('fg_svc', 50)).toBe('FGS');
    expect(formatShortProcState('cached_act', 900)).toBe('CACHED_ACT');
    expect(formatShortProcState('imp_fg', 100)).toBe('IMP_FG');
  });

  test('falls back to oom score when proc_state is null', () => {
    expect(formatShortProcState(null, 905)).toBe('oom 905');
    expect(formatShortProcState(null, 0)).toBe('oom 0');
    expect(formatShortProcState(null, null)).toBeNull();
  });
});

describe('palettes', () => {
  test('defines colors for all active bucket states and proc_state families', () => {
    expect(STATE_COLORS.S).toBe('#e9a23b');
    expect(STATE_COLORS.R).toBe('#3b7ddd');
    expect(STATE_COLORS.C).toBe('#2fa36b');
    expect(STATE_COLORS.I).toBe('#a4acb7');
    expect(STATE_COLORS.K).toBe('#e04848');
    expect(PROC_STATE_FAMILY_COLORS.TOP).toBeDefined();
    expect(PROC_STATE_FAMILY_COLORS.KILLED).toBe('#e04848');
  });
});

describe('formatShortTarget', () => {
  test('abbreviates broadcast action names, services, jobs, providers, and activities', () => {
    expect(
      formatShortTarget('broadcast', 'android.intent.action.BOOT_COMPLETED'),
    ).toBe('BOOT_COMPLETED');
    expect(
      formatShortTarget('service', 'com.google.android.gms/.gcm.GcmService'),
    ).toBe('GcmService');
    expect(
      formatShortTarget(
        'job',
        'com.google.android.gms/com.google.android.gms.gcm.nts.SchedulerJobService',
      ),
    ).toBe('SchedulerJobService');
    expect(
      formatShortTarget(
        'provider',
        'media;com.android.providers.media.MediaProvider',
      ),
    ).toBe('media');
    expect(
      formatShortTarget(
        'activity',
        'com.google.android.apps.nexuslauncher.NexusLauncherActivity',
      ),
    ).toBe('NexusLauncherActivity');
  });
});
