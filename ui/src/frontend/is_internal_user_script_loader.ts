// Copyright (C) 2018 The Android Open Source Project
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

import {AppImpl} from '../core/app_impl';
import {raf} from '../core/raf_scheduler';
import {SqlPackage} from '../public/extra_sql_packages';
import {CommandInvocation} from '../core/command_manager';

// This controls how long we wait for the script to load before giving up and
// proceeding as if the user is not internal.
//const SCRIPT_LOAD_TIMEOUT_MS = 5000;
//const SCRIPT_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/internal-data-v1/amalgamated.js';

// This interface describes the required interface that the script expect to
// find on window.globals.
interface Globals {
  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet for
  // public consumption. The gated features themselves are not secret. If a user
  // has been detected as a Googler once, make that sticky in localStorage, so
  // that we keep treating them as such when they connect over public networks.
  // This is normally undefined is injected in via is_internal_user.js.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  isInternalUser: boolean;

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraSqlPackages: SqlPackage[];

  // JSON Amalgamator populates this with statsd atom descriptors
  // as a list of base64-encoded strings.
  extraParsingDescriptors: ReadonlyArray<string>;

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraMacros: Record<string, CommandInvocation[]>[];

  // TODO(stevegolton): Check if we actually need to use these.
  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown(): void;
}

/**
 * Sets up a proxy object on window.globals that forwards property accesses to
 * the given AppImpl instance.
 */
function setupGlobalsProxy(app: AppImpl) {
  // Patch the global window object with a few hooks that point into the app
  // object.
  (window as unknown as {globals?: Globals}).globals = {
    get isInternalUser() {
      return app.isInternalUser;
    },
    set isInternalUser(value: boolean) {
      app.isInternalUser = value;
    },
    get extraSqlPackages(): SqlPackage[] {
      return app.extraSqlPackages;
    },
    get extraParsingDescriptors(): ReadonlyArray<string> {
      return app.extraParsingDescriptors;
    },
    get extraMacros(): Record<string, CommandInvocation[]>[] {
      return app.extraMacros;
    },
    shutdown() {
      raf.shutdown();
    },
  };
}

/**
 * Loads a script that detects if the user is internal, allowing access to
 * non-public features and SQL packages.
 *
 * This function works by creating a temporary `window.globals` object that
 * acts as a proxy to the main `AppImpl` instance. An external script is then
 * loaded, which populates properties on `window.globals`. These properties are
 * transparently forwarded to the `AppImpl` instance.
 */
export async function tryLoadIsInternalUserScript(app: AppImpl): Promise<void> {
  // Set up the global object and attach it to `window` before loading the
  // script.
  setupGlobalsProxy(app);

  //const FDS_BASE_64 = 'CqACCg9iYXR0ZXJ5X2V4dGVuc2lvbi5wcm90bxIUY3VzdG9tLnN0YXRzZC5hdG9tcxolcHJvdG9zL3BlcmZldHRvL3RyYWNlL3N0YXRzZC9hdG9tLnByb3RvInEKHFJhd0JhdHRlcnlHYXVnZVN0YXRzUmVwb3J0ZWQSHwoXc3lzdGVtX2Nsb2NrX3RpbWVfbmFub3MYASADKAMSFQoNdm9sdGFnZV92b2x0cxgCIAMoAhIZChFjdXJyZW50X21pbGxpYW1wGAMgAygCOm8KIHJhd19iYXR0ZXJ5X2dhdWdlX3N0YXRzX3JlcG9ydGVkEhcuYW5kcm9pZC5vcy5zdGF0c2QuQXRvbRi9CCABKAsyMi5jdXN0b20uc3RhdHNkLmF0b21zLlJhd0JhdHRlcnlHYXVnZVN0YXRzUmVwb3J0ZWQ=';
  //const FDS_BASE_64 = 'CvYPCiVmcmFtZXdvcmtzL3Byb3RvX2xvZ2dpbmcvc3RhdHMvc3RhdHNkL3V0aWwucHJvdG8SF2FuZHJvaWQuc3RhdHNkLnV0aWwucHJvdG8iMwoLRmllbGRWYWx1ZRIMCgRmaWVsZBgBIAEoBRIWCg52YWx1ZV9zdHJpbmdfc2V0GAIgASgJKj4KCEF0b21UeXBlEgsKB1VOS05PV04QABIPCgtERVBSRUNBVEVEEAESFAoQUFVTSEVEX0FUT01fREFUQRAFKl8KDEV4cGVyaW1lbnRJZBIQCgxERVBGSUxFX1BSRUZTEAASGQoVR09PR0xFX1BSRUZTX0VYUEVSSU1FTlQQARIjCh9HT09HTEVfUFJFRVNfRVhQRVJJTUVOVF9IT0xEQkFDSxACKjcKC0Fubm90YXRpb25zEg4KCkFOTk9UX1VTRVIQABIUChBBTk5PVF9JTlZBTElEQVRFEAEqJAoJUHJvY2Vzc2VzEgsKB0FQUF9VSUQQABIKCgZTWVNURU0QASpLCg1FREtWYWx1ZXMSDQoJTVRES19BVURFRRABEg4KCk1US19WRURJTkcQAhIOCgpNREtfx4BWRURFTkMQBRIQCgxNREtfx4BWVFZPTkMQBCpeChdCYXR0ZXJ5UGx1Z2dlZFN0YXRlVGVtcBISCg5URUxQX1NUQVRFX1VOSxAAEhIKDlRFTFBfU1RBVEVfQUNEEAESEwoPVEVMUF9TVEFURV9VU0JFEAISFAoQVEVMUF9TVEFURV9XSUZJRRAFKnQKFkJhdHRlcnlQbHVnZ2VkU3RhdGVDdXJyEhEKDVNVUlJfU1RBVEVfVU5LEAASFAoQU1VSUl9TVEFURV9NTk9ORRAAEhMKD1NVUlJfU1RBVEVfTUFDQRAQEhUKEVNVUlJfU1RBVEVfTVVTRkIQIBIVChFTVVJSX1NUQVRFX01XSUZJECIqPgoLRGF0YUluZm9UYXUSEQoNREFUQV9TVEFURV9VTksQABINCglEQVRBX0RFQUxUEAESDQoJREFUQV9OT1JNQRACKjkKDkNvbm5lY3Rpdml0eVRhEhEKDUNPTk5fU1RBVEVfVU5LEAASFAoQQ09OTl9TVEFURV9DT05ORUQQASpEChJQbHVnZ2VkU3RhdGVEdXJhdGlvbhIUChBEVVJBX1NUQVRFX1BfVU5LEAASGAoURFVSQV9TVEFURV9QX0NIQVJHRUQQASpEChJQbHVnZ2VkU3RhdGVEdXJhdGlvbhIUChBEVVJBX1NUQVRFX1VfVU5LEAASGAoURFVSQV9TVEFURV9VX0NIQVJHRUQQASpkChJQbHVnZ2VkU3RhdGVEdXJhdGlvbhIXChNEVVJBX1NUQVRFX1BXX1VOS05PEAASHAoYRFVSQV9TVEFURV9QV19DT05ORUNURUQQARIbChdEVVJBX1NUQVRFX1BXX0NIQVJHSU5HEAIqQwoRUGx1Z2dlZFN0YXRlQ2hhcmdlEhMKD0NIQV9TVEFURV9QVU5LEAASGQoVQ0hBX1NUQVRFX1BDSEFSR0VfU1RBUlQQASpGChFQbHVnZ2VkU3RhdGVTaWduYWwSEgoOU0lHX1NUQVRFX1VOSxAAEh0KGVNJR19TVEFURV9CYXR0ZXJ5TGV2ZWxEb3duEAEqOwoMUmFkaW9TdGF0aW9uEhAKDFJBRElPX1VOS05PV04QABITCg9SQURJT19UUkFOU01JVFMQASpXChBSYWRpb0luZm9EZXBlbmRzEhIKDlJBRElPX1VOS05PV05BEAASGQoVUkFESU9fSE9QRV9GT1JfRk9VTE9VVBABEhQKEE1UTENEX1VOVEhJTktBQkxFEAIqNQoLRGF0YUNhbGxUeXBlEhEKDUNBTExfVFlQRV9VTksQABITCg9DQUxMX1RZUEVfUkFET1QQASqmAQoOU3dpdGNoU3RhdGVDb2RlEhEKDVNXSVRDSF9OT19DT0RFEAASEQoNU1dJVENIX05PX1RFTFAQARIUChBTV0lUQ0hfTk9fQ1VSUkVOVBACEhMKD1NXSVRDSF9OT19SQUxJThADEhMKD1NXSVRDSF9OT19TVEFURRAEEhMKD1NXSVRDSF9OT19WQUxVRRAFEhQKEFNXSVRDSF9OT19QTEFZRVIQBhISCg5TV0lUQ0hfTk9fU09VUhAHKjkKDlN3aXRjaFN0YXRlVGVtcBIRCg1URU1QX1NUQVRFX1VOSxAAEhQKEFRFTVBfU1RBVEVfQ0xPU0UQAQ==';
  //const FDS_BASE_64 = 'CtsCCiVmcmFtZXdvcmtzL3Byb3RvX2xvZ2dpbmcvc3RhdHMvc3RhdHNkL3V0aWwucHJvdG8SF2FuZHJvaWQuc3RhdHNkLnV0aWwucHJvdG8aJmZyYW1ld29ya3MvcHJvdG9fbG9nZ2luZy9zdGF0cy9hdG9tcy5wcm90bxoqcHJvdG9zL3BlcmZldHRvL3RyYWNlL3N0YXRzZC9hdG9tLnByb3RvInEKHFJhd0JhdHRlcnlHYXVnZVN0YXRzUmVwb3J0ZWQSHwoXc3lzdGVtX2Nsb2NrX3RpbWVfbmFub3MYASADKAMSFQoNdm9sdGFnZV92b2x0cxgCIAMoAhIZChFjdXJyZW50X21pbGxpYW1wGAMgAygCOm8KIHJhd19iYXR0ZXJ5X2dhdWdlX3N0YXRzX3JlcG9ydGVkEhcuYW5kcm9pZC5vcy5zdGF0c2QuQXRvbRi9CCABKAsyMi5jdXN0b20uc3RhdHNkLmF0b21zLlJhd0JhdHRlcnlHYXVnZVN0YXRzUmVwb3J0ZWQ=';
  // Prod JSON Amalgamator
  //const FDS_BASE_64 = 'CvADCmBsb2dzL3Byb3RvL3dpcmVsZXNzL2FuZHJvaWQvc3RhdHMvcGxhdGZvcm0vd2VzdHdvcmxkL2F0b21zL2JhdHRlcnkvYmF0dGVyeV9leHRlbnNpb25fYXRvbXMucHJvdG8SPGxvZ3MucHJvdG8ud2lyZWxlc3MuYW5kcm9pZC5zdGF0cy5wbGF0Zm9ybS53ZXN0d29ybGQuYmF0dGVyeSJxChxSYXdCYXR0ZXJ5R2F1Z2VTdGF0c1JlcG9ydGVkEh8KF3N5c3RlbV9jbG9ja190aW1lX25hbm9zGAEgAygDEhUKDXZvbHRhZ2Vfdm9sdHMYAiADKAISGQoRY3VycmVudF9taWxsaWFtcHMYAyADKAI6ngEKIHJhd19iYXR0ZXJ5X2dhdWdlX3N0YXRzX3JlcG9ydGVkEhcuYW5kcm9pZC5vcy5zdGF0c2QuQXRvbRi9CCABKAsyWi5sb2dzLnByb3RvLndpcmVsZXNzLmFuZHJvaWQuc3RhdHMucGxhdGZvcm0ud2VzdHdvcmxkLmJhdHRlcnkuUmF3QmF0dGVyeUdhdWdlU3RhdHNSZXBvcnRlZEItChZjb20uYW5kcm9pZC5vcy5iYXR0ZXJ5UAGSAwQQAiAD0u+AkAIGbGF0ZXN0YghlZGl0aW9uc3DoBwpVCiZzeW50aGV0aWMvYW5kcm9pZC9vcy9zdGF0c2QvYXRvbS5wcm90bxIRYW5kcm9pZC5vcy5zdGF0c2QiEAoEQXRvbSoICAEQgICAgAJiBnByb3RvMg==';
  //From old statsmodule test which kind of works
  const FDS_BASE_64 = 'CvADCmBsb2dzL3Byb3RvL3dpcmVsZXNzL2FuZHJvaWQvc3RhdHMvcGxhdGZvcm0vd2VzdHdvcmxkL2F0b21zL2JhdHRlcnkvYmF0dGVyeV9leHRlbnNpb25fYXRvbXMucHJvdG8SPGxvZ3MucHJvdG8ud2lyZWxlc3MuYW5kcm9pZC5zdGF0cy5wbGF0Zm9ybS53ZXN0d29ybGQuYmF0dGVyeSJxChxSYXdCYXR0ZXJ5R2F1Z2VTdGF0c1JlcG9ydGVkEh8KF3N5c3RlbV9jbG9ja190aW1lX25hbm9zGAEgAygDEhUKDXZvbHRhZ2Vfdm9sdHMYAiADKAISGQoRY3VycmVudF9taWxsaWFtcHMYAyADKAI6ngEKIHJhd19iYXR0ZXJ5X2dhdWdlX3N0YXRzX3JlcG9ydGVkEhcuYW5kcm9pZC5vcy5zdGF0c2QuQXRvbRi9CCABKAsyWi5sb2dzLnByb3RvLndpcmVsZXNzLmFuZHJvaWQuc3RhdHMucGxhdGZvcm0ud2VzdHdvcmxkLmJhdHRlcnkuUmF3QmF0dGVyeUdhdWdlU3RhdHNSZXBvcnRlZEItChZjb20uYW5kcm9pZC5vcy5iYXR0ZXJ5UAGSAwQgAxAC0u+AkAIGbGF0ZXN0YghlZGl0aW9uc3DoBwpVCiZzeW50aGV0aWMvYW5kcm9pZC9vcy9zdGF0c2QvYXRvbS5wcm90bxIRYW5kcm9pZC5vcy5zdGF0c2QiEAoEQXRvbSoICAEQgICAgAJiBnByb3RvMgozCgphdG9tLnByb3RvEhFhbmRyb2lkLm9zLnN0YXRzZCIKCgRBdG9tKgIIAWIGcHJvdG8y';
  (app as any).__appCtxForTrace.extraParsingDescriptors = [FDS_BASE_64];

  // await new Promise<void>((resolve) => {
  //   const script = document.createElement('script');
  //   script.src = SCRIPT_URL;
  //   script.async = true;
  //   script.onerror = () => resolve();
  //   script.onload = () => resolve();
  //   document.head.append(script);

  //   // Set a timeout to avoid blocking the UI for too long if the script is slow
  //   // to load.
  //   setTimeout(() => resolve(), SCRIPT_LOAD_TIMEOUT_MS);
  // });
}
