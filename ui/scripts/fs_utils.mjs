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

import fs from 'node:fs';
import path from 'node:path';

const pjoin = path.join;

export function ensureDir(dir) {
  fs.mkdirSync(dir, {recursive: true});
  return dir;
}

// Copies non-recursive: only files in |srcDir| matching |regex|.
export function copyByPattern(srcDir, dstDir, regex) {
  if (!fs.existsSync(srcDir)) return;
  ensureDir(dstDir);
  for (const name of fs.readdirSync(srcDir)) {
    if (!regex.test(name)) continue;
    const src = pjoin(srcDir, name);
    if (!fs.statSync(src).isFile()) continue;
    fs.copyFileSync(src, pjoin(dstDir, name));
  }
}

// Recursive copy of a directory tree, optionally filtering by |regex| on
// filenames (directories are always recursed into).
export function copyDir(srcDir, dstDir, regex) {
  if (!fs.existsSync(srcDir)) return;
  ensureDir(dstDir);
  for (const name of fs.readdirSync(srcDir)) {
    const src = pjoin(srcDir, name);
    const dst = pjoin(dstDir, name);
    const stat = fs.statSync(src);
    if (stat.isDirectory()) {
      copyDir(src, dst, regex);
    } else if (!regex || regex.test(name)) {
      fs.copyFileSync(src, dst);
    }
  }
}

// Returns all file paths under |dir| recursively.
export function listFilesRecursive(dir) {
  const out = [];
  for (const name of fs.readdirSync(dir)) {
    const p = pjoin(dir, name);
    if (fs.statSync(p).isDirectory()) {
      out.push(...listFilesRecursive(p));
    } else {
      out.push(p);
    }
  }
  return out;
}
