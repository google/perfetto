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

import {isDebuggableAndroidBuild, parseAndroidBuildVariant} from './utils';

describe('parseAndroidBuildVariant', () => {
  it('identifies userdebug build from standard fingerprint', () => {
    const fp =
      'google/coral/coral:12/SP1A.210812.015/7679548:userdebug/dev-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('userdebug');
  });

  it('identifies eng build from standard fingerprint', () => {
    const fp =
      'google/tangorpro/tangorpro:VanillaIceCream/MAIN/eng.prabir.20240606.172326:eng/dev-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('eng');
  });

  it('identifies user build from standard fingerprint', () => {
    const fp =
      'google/oriole/oriole:14/AP1A.240405.002/11487190:user/release-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('user');
  });

  it('handles AOSP generic fingerprints', () => {
    const fp = 'generic/aosp_arm64/arm64:14/UDC/1234:eng/test-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('eng');
  });

  it('handles fingerprint with build ID containing words like user or eng', () => {
    const fp =
      'Android/aosp_raven/raven:VanillaIceCream/MAIN/eng.user.20240101.000000:userdebug/test-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('userdebug');
  });

  it('handles uppercase and mixed case build variants', () => {
    const fp = 'google/coral/coral:12/SP1A/123:USERDEBUG/dev-keys';
    expect(parseAndroidBuildVariant(fp)).toBe('userdebug');
  });

  it('handles fingerprint without trailing tags', () => {
    const fp = 'google/coral/coral:12/SP1A/123:userdebug';
    expect(parseAndroidBuildVariant(fp)).toBe('userdebug');
  });

  it('falls back to substring matching when colons are absent', () => {
    expect(parseAndroidBuildVariant('aosp_cf_x86_phone-userdebug')).toBe(
      'userdebug',
    );
    expect(parseAndroidBuildVariant('linux-eng-build')).toBe('eng');
  });

  it('returns unknown for undefined, null, or empty string', () => {
    expect(parseAndroidBuildVariant(undefined)).toBe('unknown');
    expect(parseAndroidBuildVariant('')).toBe('unknown');
  });

  it('returns unknown for unrecognizable fingerprints', () => {
    expect(
      parseAndroidBuildVariant('custom/device/model:1.0/foo/bar:custom/tag'),
    ).toBe('unknown');
  });
});

describe('isDebuggableAndroidBuild', () => {
  it('returns true for userdebug fingerprint', () => {
    const fp =
      'google/coral/coral:12/SP1A.210812.015/7679548:userdebug/dev-keys';
    expect(isDebuggableAndroidBuild(fp)).toBe(true);
  });

  it('returns true for eng fingerprint', () => {
    const fp =
      'google/tangorpro/tangorpro:VanillaIceCream/MAIN/eng.prabir.20240606.172326:eng/dev-keys';
    expect(isDebuggableAndroidBuild(fp)).toBe(true);
  });

  it('returns false for user fingerprint', () => {
    const fp =
      'google/oriole/oriole:14/AP1A.240405.002/11487190:user/release-keys';
    expect(isDebuggableAndroidBuild(fp)).toBe(false);
  });

  it('returns false for undefined or empty string', () => {
    expect(isDebuggableAndroidBuild(undefined)).toBe(false);
    expect(isDebuggableAndroidBuild('')).toBe(false);
  });
});
