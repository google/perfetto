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

export type FrameCategory = 'app' | 'system' | 'runtime';

// Extracts a package or binary token from a process name for matching frames
// belonging to the target process (e.g. "com.example.app:remote" -> "com.example.app",
// "/usr/bin/my_app" -> "my_app").
export function extractProcessToken(procName?: string): string | undefined {
  if (procName === undefined) return undefined;
  const noSuffix = procName.split(':')[0] ?? '';
  const base = noSuffix.split('/').pop() ?? '';
  return base.length >= 3 ? base : undefined;
}

const APP_MAPPING_RE =
  /^\/(?:(?:(?:data|product|system|system_ext|vendor)\/(?:priv-)?(?:app|data)|data\/user|apex\/[^/]+\/(?:priv-)?app)\/|data\/dalvik-cache\/[^/]+\/(?:data|product|system|system_ext|vendor)@(?:priv-)?app@)/;

// True when a frame's mapping belongs to the app under test rather than a
// system/runtime library.
export function isAppMapping(mapping?: string, procToken?: string): boolean {
  if (mapping === undefined) return false;
  if (APP_MAPPING_RE.test(mapping)) return true;
  if (procToken !== undefined && mapping.includes(`/${procToken}`)) {
    return true;
  }
  return false;
}

const RUNTIME_MAPPING_RE =
  /\/(?:(?:libc|libc\+\+|libc\+\+_shared|libmalloc[^/]*|libheapprofd[^/]*|libscudo[^/]*|libjemalloc[^/]*|libartd?)\.so\b|app_process(?:32|64)?$)/;

// Allocators, JNI/ART glue, thread/Zygote/Looper/IPC/executor/coroutine
// scaffolding, and synthetic unwinder placeholders that obscure the real
// allocation site regardless of mapping.
const RUNTIME_FRAME_RE =
  /^(?:malloc|calloc|realloc|art_.*trampoline|libutil_thread_trampoline|INVALID_ELF|INVALID_MAP|UNWIND_INFO)\b|^operator new(?:\[\])?(?:\b|\(|$)|^(?:_?JNIEnv|_JNI|art::JNI|art::interpreter|android::AndroidRuntime)::|^(?:nterp_helper|unknown_frame|java\.lang\.Thread\.run)$|^android::(?:Thread::_threadLoop|PoolThread::threadLoop|A?Looper|IPCThreadState|BpBinder::transact|hardware::IPCThreadState)\b|\bstd::(?:__1::)?__thread_proxy\b|^JavaBBinderExt::onTransact\b|^com\.android\.internal\.os\.(?:ZygoteInit|RuntimeInit)\b|^android\.os\.(?:Looper\.loop(?:Once)?|Handler\.dispatchMessage|HandlerThread\.run|Binder\.execTransact(?:Internal)?)\b|^java\.util\.concurrent\.(?:ThreadPoolExecutor|Executors\$)|^kotlinx?\.coroutines\.|\[(?:DEDUPED|ambiguous)\]/;

// Framework/JDK package prefixes used to split JIT-cache frames between
// system and app code.
const SYSTEM_JIT_FRAME_RE =
  /^(?:android|com\.android\.(?:internal|server|wifi)|java|javax|dalvik|libcore|sun|jdk)\./;

export function categorizeFrame(
  name: string,
  mapping?: string,
  procToken?: string,
): FrameCategory {
  if (RUNTIME_FRAME_RE.test(name)) return 'runtime';
  if (mapping === undefined) return 'system';
  if (RUNTIME_MAPPING_RE.test(mapping)) return 'runtime';
  if (mapping.includes('jit-cache')) {
    if (name.startsWith('art_')) return 'runtime';
    return SYSTEM_JIT_FRAME_RE.test(name) ? 'system' : 'app';
  }
  if (isAppMapping(mapping, procToken)) return 'app';
  return 'system';
}
