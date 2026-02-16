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

import {ProcessMemoryStats} from '../interfaces/recording_target';

/**
 * Parses the "Total PSS by process" section of `dumpsys meminfo` output.
 *
 * Example lines:
 *   "    267,177K: system (pid 1493)"
 *   "    178,498K: com.google.android.gms (pid 4915 / activities)"
 *   "     58,498K: .dataservices (pid 2588)"
 */
export function parseDumpsysMeminfo(output: string): ProcessMemoryStats[] {
  const results: ProcessMemoryStats[] = [];

  // Find the "Total PSS by process" section.
  const sectionStart = output.indexOf('Total PSS by process:');
  if (sectionStart === -1) return results;

  // Extract lines from this section until the next "Total" section or EOF.
  const sectionText = output.substring(
    sectionStart + 'Total PSS by process:'.length,
  );
  const lines = sectionText.split('\n');

  // Each process line looks like:
  //   "    123,456K: com.example.app (pid 1234 / activities)"
  //   "    123,456K: com.example.app (pid 1234)"
  const lineRegex = /^\s*([\d,]+)K:\s+(.+?)\s+\(pid\s+(\d+)/;

  for (const line of lines) {
    // Stop at the next section header.
    if (line.match(/^Total /)) break;

    const match = line.match(lineRegex);
    if (match) {
      const pssKb = parseInt(match[1].replace(/,/g, ''), 10);
      const processName = match[2];
      const pid = parseInt(match[3], 10);
      results.push({processName, pid, pssKb});
    }
  }

  return results;
}
