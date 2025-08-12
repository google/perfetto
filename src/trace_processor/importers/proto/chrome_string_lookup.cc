/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/chrome_string_lookup.h"

#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace/track_event/chrome_legacy_ipc.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/third_party/chromium/chrome_enums.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace chrome_enums = ::perfetto::protos::chrome_enums::pbzero;
using ::perfetto::protos::pbzero::ChromeThreadDescriptor;

namespace perfetto {
namespace trace_processor {

namespace {

// By design, these switches handle unrecognized enum values with a default
// fallback, so that new entries can be added to
// protos/third_party/chromium_chrome_enums.proto without immediately updating
// the list of names.
#if defined(__GNUC__)  // clang also supports GCC syntax.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcovered-switch-default"
#pragma GCC diagnostic ignored "-Wswitch-enum"
// -Wswitch includes all warnings from -Wswitch-enum except "enumeration value
// not explicitly handled in switch".
#pragma GCC diagnostic error "-Wswitch"
#endif  // __GNUC__

// Returns a name, which may be null, for `process_type`.
const char* GetProcessNameString(
    chrome_enums::ProcessType process_type,
    bool ignore_predefined_thread_types_for_testing) {
  if (!ignore_predefined_thread_types_for_testing) {
    switch (process_type) {
      case chrome_enums::PROCESS_UNSPECIFIED:
        return nullptr;
      case chrome_enums::PROCESS_BROWSER:
        return "Browser";
      case chrome_enums::PROCESS_RENDERER:
        return "Renderer";
      case chrome_enums::PROCESS_UTILITY:
        return "Utility";
      case chrome_enums::PROCESS_ZYGOTE:
        return "SandboxHelper";
      case chrome_enums::PROCESS_GPU:
        return "Gpu";
      case chrome_enums::PROCESS_PPAPI_PLUGIN:
        return "PpapiPlugin";
      case chrome_enums::PROCESS_PPAPI_BROKER:
        return "PpapiBroker";
      case chrome_enums::PROCESS_SERVICE_NETWORK:
        return "Service: network.mojom.NetworkService";
      case chrome_enums::PROCESS_SERVICE_TRACING:
        return "Service: tracing.mojom.TracingService";
      case chrome_enums::PROCESS_SERVICE_STORAGE:
        return "Service: storage.mojom.StorageService";
      case chrome_enums::PROCESS_SERVICE_AUDIO:
        return "Service: audio.mojom.AudioService";
      case chrome_enums::PROCESS_SERVICE_DATA_DECODER:
        return "Service: data_decoder.mojom.DataDecoderService";
      case chrome_enums::PROCESS_SERVICE_UTIL_WIN:
        return "Service: chrome.mojom.UtilWin";
      case chrome_enums::PROCESS_SERVICE_PROXY_RESOLVER:
        return "Service: proxy_resolver.mojom.ProxyResolverFactory";
      case chrome_enums::PROCESS_SERVICE_CDM:
        return "Service: media.mojom.CdmServiceBroker";
      case chrome_enums::PROCESS_SERVICE_MEDIA_FOUNDATION:
        return "Service: media.mojom.MediaFoundationServiceBroker";
      case chrome_enums::PROCESS_SERVICE_VIDEO_CAPTURE:
        return "Service: video_capture.mojom.VideoCaptureService";
      case chrome_enums::PROCESS_SERVICE_UNZIPPER:
        return "Service: unzip.mojom.Unzipper";
      case chrome_enums::PROCESS_SERVICE_MIRRORING:
        return "Service: mirroring.mojom.MirroringService";
      case chrome_enums::PROCESS_SERVICE_FILEPATCHER:
        return "Service: patch.mojom.FilePatcher";
      case chrome_enums::PROCESS_SERVICE_TTS:
        return "Service: chromeos.tts.mojom.TtsService";
      case chrome_enums::PROCESS_SERVICE_PRINTING:
        return "Service: printing.mojom.PrintingService";
      case chrome_enums::PROCESS_SERVICE_QUARANTINE:
        return "Service: quarantine.mojom.Quarantine";
      case chrome_enums::PROCESS_SERVICE_CROS_LOCALSEARCH:
        return "Service: "
               "chromeos.local_search_service.mojom.LocalSearchService";
      case chrome_enums::PROCESS_SERVICE_CROS_ASSISTANT_AUDIO_DECODER:
        return "Service: chromeos.assistant.mojom.AssistantAudioDecoderFactory";
      case chrome_enums::PROCESS_SERVICE_FILEUTIL:
        return "Service: chrome.mojom.FileUtilService";
      case chrome_enums::PROCESS_SERVICE_PRINTCOMPOSITOR:
        return "Service: printing.mojom.PrintCompositor";
      case chrome_enums::PROCESS_SERVICE_PAINTPREVIEW:
        return "Service: paint_preview.mojom.PaintPreviewCompositorCollection";
      case chrome_enums::PROCESS_SERVICE_SPEECHRECOGNITION:
        return "Service: media.mojom.SpeechRecognitionService";
      case chrome_enums::PROCESS_SERVICE_XRDEVICE:
        return "Service: device.mojom.XRDeviceService";
      case chrome_enums::PROCESS_SERVICE_READICON:
        return "Service: chrome.mojom.UtilReadIcon";
      case chrome_enums::PROCESS_SERVICE_LANGUAGEDETECTION:
        return "Service: language_detection.mojom.LanguageDetectionService";
      case chrome_enums::PROCESS_SERVICE_SHARING:
        return "Service: sharing.mojom.Sharing";
      case chrome_enums::PROCESS_SERVICE_MEDIAPARSER:
        return "Service: chrome.mojom.MediaParserFactory";
      case chrome_enums::PROCESS_SERVICE_QRCODEGENERATOR:
        return "Service: qrcode_generator.mojom.QRCodeService";
      case chrome_enums::PROCESS_SERVICE_PROFILEIMPORT:
        return "Service: chrome.mojom.ProfileImport";
      case chrome_enums::PROCESS_SERVICE_IME:
        return "Service: chromeos.ime.mojom.ImeService";
      case chrome_enums::PROCESS_SERVICE_RECORDING:
        return "Service: recording.mojom.RecordingService";
      case chrome_enums::PROCESS_SERVICE_SHAPEDETECTION:
        return "Service: shape_detection.mojom.ShapeDetectionService";
      case chrome_enums::PROCESS_RENDERER_EXTENSION:
        return "Extension Renderer";
      default:
        // Fall through to the generated name.
        break;
    }
  }
  return chrome_enums::ProcessType_Name(process_type);
}

// Returns a name, which may be null, for `thread_type`.
const char* GetThreadNameString(
    ChromeThreadDescriptor::ThreadType thread_type,
    bool ignore_predefined_thread_types_for_testing) {
  if (!ignore_predefined_thread_types_for_testing) {
    switch (thread_type) {
      case ChromeThreadDescriptor::THREAD_UNSPECIFIED:
        return nullptr;
      case ChromeThreadDescriptor::THREAD_MAIN:
        return "CrProcessMain";
      case ChromeThreadDescriptor::THREAD_IO:
        return "ChromeIOThread";
      case ChromeThreadDescriptor::THREAD_NETWORK_SERVICE:
        return "NetworkService";
      case ChromeThreadDescriptor::THREAD_POOL_BG_WORKER:
        return "ThreadPoolBackgroundWorker&";
      case ChromeThreadDescriptor::THREAD_POOL_FG_WORKER:
        return "ThreadPoolForegroundWorker&";
      case ChromeThreadDescriptor::THREAD_POOL_BG_BLOCKING:
        return "ThreadPoolSingleThreadBackgroundBlocking&";
      case ChromeThreadDescriptor::THREAD_POOL_FG_BLOCKING:
        return "ThreadPoolSingleThreadForegroundBlocking&";
      case ChromeThreadDescriptor::THREAD_POOL_SERVICE:
        return "ThreadPoolService";
      case ChromeThreadDescriptor::THREAD_COMPOSITOR:
        return "Compositor";
      case ChromeThreadDescriptor::THREAD_VIZ_COMPOSITOR:
        return "VizCompositorThread";
      case ChromeThreadDescriptor::THREAD_COMPOSITOR_WORKER:
        return "CompositorTileWorker&";
      case ChromeThreadDescriptor::THREAD_SERVICE_WORKER:
        return "ServiceWorkerThread&";
      case ChromeThreadDescriptor::THREAD_MEMORY_INFRA:
        return "MemoryInfra";
      case ChromeThreadDescriptor::THREAD_SAMPLING_PROFILER:
        return "StackSamplingProfiler";

      case ChromeThreadDescriptor::THREAD_BROWSER_MAIN:
        return "CrBrowserMain";
      case ChromeThreadDescriptor::THREAD_RENDERER_MAIN:
        return "CrRendererMain";
      case ChromeThreadDescriptor::THREAD_CHILD_IO:
        return "Chrome_ChildIOThread";
      case ChromeThreadDescriptor::THREAD_BROWSER_IO:
        return "Chrome_IOThread";
      case ChromeThreadDescriptor::THREAD_UTILITY_MAIN:
        return "CrUtilityMain";
      case ChromeThreadDescriptor::THREAD_GPU_MAIN:
        return "CrGpuMain";
      case ChromeThreadDescriptor::THREAD_CACHE_BLOCKFILE:
        return "CacheThread_BlockFile";
      case ChromeThreadDescriptor::ChromeThreadDescriptor::THREAD_MEDIA:
        return "Media";
      case ChromeThreadDescriptor::THREAD_AUDIO_OUTPUTDEVICE:
        return "AudioOutputDevice";
      case ChromeThreadDescriptor::THREAD_GPU_MEMORY:
        return "GpuMemoryThread";
      case ChromeThreadDescriptor::THREAD_GPU_VSYNC:
        return "GpuVSyncThread";
      case ChromeThreadDescriptor::THREAD_DXA_VIDEODECODER:
        return "DXVAVideoDecoderThread";
      case ChromeThreadDescriptor::THREAD_BROWSER_WATCHDOG:
        return "BrowserWatchdog";
      case ChromeThreadDescriptor::THREAD_WEBRTC_NETWORK:
        return "WebRTC_Network";
      case ChromeThreadDescriptor::THREAD_WINDOW_OWNER:
        return "Window owner thread";
      case ChromeThreadDescriptor::THREAD_WEBRTC_SIGNALING:
        return "WebRTC_Signaling";
      case ChromeThreadDescriptor::THREAD_PPAPI_MAIN:
        return "CrPPAPIMain";
      case ChromeThreadDescriptor::THREAD_GPU_WATCHDOG:
        return "GpuWatchdog";
      case ChromeThreadDescriptor::THREAD_SWAPPER:
        return "swapper";
      case ChromeThreadDescriptor::THREAD_GAMEPAD_POLLING:
        return "Gamepad polling thread";
      case ChromeThreadDescriptor::THREAD_AUDIO_INPUTDEVICE:
        return "AudioInputDevice";
      case ChromeThreadDescriptor::THREAD_WEBRTC_WORKER:
        return "WebRTC_Worker";
      case ChromeThreadDescriptor::THREAD_WEBCRYPTO:
        return "WebCrypto";
      case ChromeThreadDescriptor::THREAD_DATABASE:
        return "Database thread";
      case ChromeThreadDescriptor::THREAD_PROXYRESOLVER:
        return "Proxy Resolver";
      case ChromeThreadDescriptor::THREAD_DEVTOOLSADB:
        return "Chrome_DevToolsADBThread";
      case ChromeThreadDescriptor::THREAD_NETWORKCONFIGWATCHER:
        return "NetworkConfigWatcher";
      case ChromeThreadDescriptor::THREAD_WASAPI_RENDER:
        return "wasapi_render_thread";
      case ChromeThreadDescriptor::THREAD_LOADER_LOCK_SAMPLER:
        return "LoaderLockSampler";
      case ChromeThreadDescriptor::THREAD_COMPOSITOR_GPU:
        return "CompositorGpuThread";
      default:
        // Fall through to the generated name.
        break;
    }
  }
  return ChromeThreadDescriptor::ThreadType_Name(thread_type);
}

#if defined(__GNUC__)  // clang also supports GCC syntax.
#pragma GCC diagnostic pop
#endif  // __GNUC__

}  // namespace

ChromeStringLookup::ChromeStringLookup(
    TraceStorage* storage,
    bool ignore_predefined_names_for_testing) {
  for (int32_t i = chrome_enums::ProcessType_MIN;
       i <= chrome_enums::ProcessType_MAX; ++i) {
    const auto type = static_cast<chrome_enums::ProcessType>(i);
    const char* name =
        GetProcessNameString(type, ignore_predefined_names_for_testing);
    chrome_process_name_ids_[type] =
        name ? storage->InternString(name) : kNullStringId;
  }

  for (int32_t i =
           ::perfetto::protos::pbzero::ChromeThreadDescriptor_ThreadType_MIN;
       i <= ::perfetto::protos::pbzero::ChromeThreadDescriptor_ThreadType_MAX;
       ++i) {
    const auto type = static_cast<ChromeThreadDescriptor::ThreadType>(i);
    const char* name =
        GetThreadNameString(type, ignore_predefined_names_for_testing);
    chrome_thread_name_ids_[type] =
        name ? storage->InternString(name) : kNullStringId;
  }
}

StringId ChromeStringLookup::GetProcessName(int32_t process_type) const {
  auto process_name_it = chrome_process_name_ids_.find(process_type);
  if (process_name_it != chrome_process_name_ids_.end())
    return process_name_it->second;

  PERFETTO_DLOG("GetProcessName error: Unknown Chrome process type %u",
                process_type);
  return kNullStringId;
}

StringId ChromeStringLookup::GetThreadName(int32_t thread_type) const {
  auto thread_name_it = chrome_thread_name_ids_.find(thread_type);
  if (thread_name_it != chrome_thread_name_ids_.end())
    return thread_name_it->second;

  PERFETTO_DLOG("GetThreadName error: Unknown Chrome thread type %u",
                thread_type);
  return kNullStringId;
}

}  // namespace trace_processor
}  // namespace perfetto
