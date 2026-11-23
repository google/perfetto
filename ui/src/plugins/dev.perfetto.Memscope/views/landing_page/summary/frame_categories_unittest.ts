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

import {
  categorizeFrame,
  extractProcessToken,
  isAppMapping,
} from './frame_categories';

describe('extractProcessToken', () => {
  it('returns package name as-is for standard Android process names', () => {
    expect(extractProcessToken('com.example.app')).toBe('com.example.app');
  });

  it('strips colon-separated process suffixes', () => {
    expect(extractProcessToken('com.example.app:remote')).toBe(
      'com.example.app',
    );
  });

  it('extracts basename from path-based binary names', () => {
    expect(extractProcessToken('/usr/bin/my_app')).toBe('my_app');
    expect(extractProcessToken('/vendor/bin/hw/camera_service:0')).toBe(
      'camera_service',
    );
  });

  it('returns undefined for undefined, empty, or very short names', () => {
    expect(extractProcessToken(undefined)).toBeUndefined();
    expect(extractProcessToken('')).toBeUndefined();
    expect(extractProcessToken('ab')).toBeUndefined();
    expect(extractProcessToken('/bin/sh')).toBeUndefined();
  });
});

describe('isAppMapping', () => {
  it('matches standard Android app and data paths', () => {
    expect(
      isAppMapping(
        '/data/app/~~abc==/com.example.app-xyz==/lib/arm64/libfoo.so',
      ),
    ).toBe(true);
    expect(isAppMapping('/data/data/com.example.app/files/libbar.so')).toBe(
      true,
    );
    expect(isAppMapping('/data/user/0/com.example.app/code_cache/a.so')).toBe(
      true,
    );
    expect(isAppMapping('/product/priv-app/MyApp/MyApp.apk')).toBe(true);
    expect(isAppMapping('/system/priv-app/SystemUI/SystemUI.apk')).toBe(true);
    expect(isAppMapping('/system_ext/app/Foo/Foo.apk')).toBe(true);
    expect(isAppMapping('/vendor/app/Bar/Bar.apk')).toBe(true);
    expect(isAppMapping('/apex/com.android.foo/priv-app/Baz/Baz.apk')).toBe(
      true,
    );
  });

  it('matches dalvik-cache entries for app packages', () => {
    expect(
      isAppMapping(
        '/data/dalvik-cache/arm64/product@priv-app@MyApp@MyApp.apk@classes.dex',
      ),
    ).toBe(true);
    expect(
      isAppMapping(
        '/data/dalvik-cache/arm64/data@app@com.example.app@base.apk@classes.dex',
      ),
    ).toBe(true);
  });

  it('matches mappings containing the process token', () => {
    expect(isAppMapping('/opt/custom/my_app/libcore.so', 'my_app')).toBe(true);
    expect(isAppMapping('/opt/custom/other/libcore.so', 'my_app')).toBe(false);
  });

  it('returns false for undefined or system library mappings', () => {
    expect(isAppMapping(undefined)).toBe(false);
    expect(isAppMapping('/system/lib64/libhwui.so')).toBe(false);
    expect(isAppMapping('/system/framework/framework.jar')).toBe(false);
    expect(
      isAppMapping('/data/dalvik-cache/arm64/system@framework@boot.oat'),
    ).toBe(false);
  });
});

describe('categorizeFrame', () => {
  it('classifies allocator, glue, scaffolding, and unwinder frames as runtime', () => {
    const appMapping = '/data/app/com.example.app/lib/arm64/libapp.so';
    const runtimeNames = [
      'malloc',
      'calloc',
      'realloc',
      'operator new(unsigned long)',
      'operator new[](unsigned long)',
      'art_quick_generic_jni_trampoline',
      'JNIEnv::NewObject',
      'art::JNI::CallVoidMethod',
      'nterp_helper',
      'java.lang.Thread.run',
      'android.os.Looper.loopOnce',
      'android.os.Handler.dispatchMessage',
      'com.android.internal.os.ZygoteInit.main',
      'java.util.concurrent.ThreadPoolExecutor.runWorker',
      'kotlinx.coroutines.DispatchedTask.run',
      'android::IPCThreadState::executeCommand',
      'android::PoolThread::threadLoop()',
      'void* std::__1::__thread_proxy[abi:nqn230000]<std::__1::tuple<void*>>(void*)',
      'INVALID_ELF',
      'unknown_frame',
      '[DEDUPED]',
      '[ambiguous]',
    ];
    for (const name of runtimeNames) {
      expect(categorizeFrame(name, appMapping)).toBe('runtime');
    }
  });

  it('classifies runtime library mappings as runtime', () => {
    const runtimeMappings = [
      '/apex/com.android.runtime/lib64/bionic/libc.so',
      '/system/lib64/libc++.so',
      '/apex/com.android.art/lib64/libart.so',
      '/system/lib64/libscudo.so',
      '/system/lib64/libheapprofd_client.so',
      '/system/bin/app_process64',
    ];
    for (const mapping of runtimeMappings) {
      expect(categorizeFrame('SomeInternalHelper', mapping)).toBe('runtime');
    }
  });

  it('splits jit-cache frames between runtime, system, and app', () => {
    const jitMapping = '/memfd:jit-cache (deleted)';
    expect(categorizeFrame('art_quick_alloc_object', jitMapping)).toBe(
      'runtime',
    );
    expect(categorizeFrame('android.view.View.draw', jitMapping)).toBe(
      'system',
    );
    expect(categorizeFrame('java.util.HashMap.put', jitMapping)).toBe('system');
    expect(
      categorizeFrame(
        'com.android.server.am.ActivityManagerService.start',
        jitMapping,
      ),
    ).toBe('system');
    expect(
      categorizeFrame(
        'com.android.wifi.x.com.android.internal.util.StateMachine.processMsg',
        jitMapping,
      ),
    ).toBe('system');
    expect(categorizeFrame('com.example.app.Renderer.render', jitMapping)).toBe(
      'app',
    );
  });

  it('classifies app mappings and process-token matches as app', () => {
    expect(
      categorizeFrame(
        'MyRenderer::DrawFrame()',
        '/data/app/com.example.app/lib/arm64/librenderer.so',
      ),
    ).toBe('app');
    expect(
      categorizeFrame('doWork()', '/usr/local/bin/my_binary', 'my_binary'),
    ).toBe('app');
  });

  it('classifies missing mappings and framework/OS libraries as system', () => {
    expect(categorizeFrame('SomeFrame', undefined)).toBe('system');
    expect(
      categorizeFrame(
        'android::uirenderer::RenderThread::threadLoop',
        '/system/lib64/libhwui.so',
      ),
    ).toBe('system');
    expect(
      categorizeFrame(
        'android.graphics.Bitmap.nativeCreate',
        '/system/framework/arm64/boot-framework.oat',
      ),
    ).toBe('system');
  });
});
