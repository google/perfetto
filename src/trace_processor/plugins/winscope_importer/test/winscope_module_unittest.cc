/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/winscope_importer/winscope_module.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_interned_data.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/winscope/frameworks_base_winscope.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/frameworks_native_interned_data.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/android_input_event.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/frameworks_native_winscope.pbzero.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/selective_trace_packet_decoder.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using com::android::internal::pbzero::AndroidInputEvent;
using com::android::internal::pbzero::FrameworksBaseInternedData;
using com::android::internal::pbzero::FrameworksBaseWinscopeExtensions;
using com::android::internal::pbzero::FrameworksBaseWinscopeTracePacket;
using com::android::internal::pbzero::FrameworksNativeInternedData;
using com::android::internal::pbzero::FrameworksNativeWinscopeExtensions;
using com::android::internal::pbzero::FrameworksNativeWinscopeTracePacket;
using perfetto::protos::pbzero::TracePacket;

class WinscopeModuleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_ = std::make_unique<TraceProcessorContext>();
    context_->storage = std::make_unique<TraceStorage>();
    context_->descriptor_pool_ = std::make_unique<DescriptorPool>();
    context_->global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_->storage.get());
    context_->machine_tracker =
        std::make_unique<MachineTracker>(context_.get(), kDefaultMachineId);
    context_->trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_->stats_tracker = std::make_unique<StatsTracker>(context_.get());
    module_ =
        std::make_unique<WinscopeModule>(&module_context_, context_.get());
  }

  int64_t FeedRawPacket(const std::vector<uint8_t>& bytes) {
    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    SelectiveTracePacketDecoder decoder(tbv.data(), tbv.length());
    module_->OnRawPacket(decoder, tbv);
    return static_cast<int64_t>(tbv.length());
  }

  int64_t GetStat(size_t stat_key) const {
    return context_->stats_tracker->GetStats(stat_key);
  }

  ProtoImporterModuleContext module_context_;
  std::unique_ptr<TraceProcessorContext> context_;
  std::unique_ptr<WinscopeModule> module_;
};

TEST_F(WinscopeModuleTest, RegistersAsRawPacketObserver) {
  ASSERT_EQ(module_context_.raw_packet_observers.size(), 1u);
  EXPECT_EQ(module_context_.raw_packet_observers[0], module_.get());
}

TEST_F(WinscopeModuleTest, RecordsPreProtoVmPacketAndByteStats) {
  // SurfaceFlinger layers snapshot + layers patch.
  protozero::HeapBuffered<protozero::Message> sf_snap;
  sf_snap->AppendString(FrameworksNativeWinscopeTracePacket::
                            kSurfaceflingerLayersSnapshotFieldNumber,
                        "snap");
  int64_t sf_snap_len = FeedRawPacket(sf_snap.SerializeAsArray());

  protozero::HeapBuffered<protozero::Message> sf_patch;
  sf_patch
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(FrameworksNativeWinscopeExtensions::
                         kSurfaceflingerLayersPatchFieldNumber,
                     "patch");
  int64_t sf_patch_len = FeedRawPacket(sf_patch.SerializeAsArray());

  EXPECT_EQ(GetStat(stats::winscope_sf_layers_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_sf_layers_bytes),
            sf_snap_len + sf_patch_len);

  // SurfaceFlinger transactions.
  protozero::HeapBuffered<protozero::Message> sf_tx;
  sf_tx->AppendString(FrameworksNativeWinscopeTracePacket::
                          kSurfaceflingerTransactionsFieldNumber,
                      "tx");
  int64_t sf_tx_len = FeedRawPacket(sf_tx.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_sf_transactions_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_sf_transactions_bytes), sf_tx_len);

  // WindowManager entry + patch.
  protozero::HeapBuffered<protozero::Message> wm_entry;
  wm_entry
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(
          FrameworksBaseWinscopeExtensions::kWindowmanagerFieldNumber, "wm");
  int64_t wm_entry_len = FeedRawPacket(wm_entry.SerializeAsArray());

  protozero::HeapBuffered<protozero::Message> wm_patch;
  wm_patch
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(
          FrameworksBaseWinscopeExtensions::kWindowmanagerPatchFieldNumber,
          "wmp");
  int64_t wm_patch_len = FeedRawPacket(wm_patch.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_windowmanager_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_windowmanager_bytes),
            wm_entry_len + wm_patch_len);

  // ViewCapture.
  protozero::HeapBuffered<protozero::Message> vc;
  vc->BeginNestedMessage<protozero::Message>(
        FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber,
                     "vc");
  int64_t vc_len = FeedRawPacket(vc.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_bytes), vc_len);

  // IME clients, service, manager_service.
  protozero::HeapBuffered<protozero::Message> ime_clients;
  ime_clients
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(
          FrameworksBaseWinscopeExtensions::kInputmethodClientsFieldNumber,
          "ic");
  int64_t ime_clients_len = FeedRawPacket(ime_clients.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_clients_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_clients_bytes),
            ime_clients_len);

  protozero::HeapBuffered<protozero::Message> ime_service;
  ime_service
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(
          FrameworksBaseWinscopeExtensions::kInputmethodServiceFieldNumber,
          "is");
  int64_t ime_service_len = FeedRawPacket(ime_service.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_service_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_service_bytes),
            ime_service_len);

  protozero::HeapBuffered<protozero::Message> ime_mgr;
  ime_mgr
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(FrameworksBaseWinscopeExtensions::
                         kInputmethodManagerServiceFieldNumber,
                     "im");
  int64_t ime_mgr_len = FeedRawPacket(ime_mgr.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_manager_service_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_inputmethod_manager_service_bytes),
            ime_mgr_len);

  // Android input events: motion, key, window_dispatch.
  auto make_input = [](uint32_t sub_field_id, const char* payload) {
    protozero::HeapBuffered<protozero::Message> msg;
    msg->BeginNestedMessage<protozero::Message>(
           FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
        ->BeginNestedMessage<protozero::Message>(
            FrameworksNativeWinscopeExtensions::kAndroidInputEventFieldNumber)
        ->AppendString(sub_field_id, payload);
    return msg.SerializeAsArray();
  };
  int64_t motion_len = FeedRawPacket(
      make_input(AndroidInputEvent::kDispatcherMotionEventFieldNumber, "m"));
  int64_t motion_red_len = FeedRawPacket(make_input(
      AndroidInputEvent::kDispatcherMotionEventRedactedFieldNumber, "mr"));
  EXPECT_EQ(GetStat(stats::winscope_input_motion_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_input_motion_bytes),
            motion_len + motion_red_len);

  int64_t key_len = FeedRawPacket(
      make_input(AndroidInputEvent::kDispatcherKeyEventFieldNumber, "k"));
  EXPECT_EQ(GetStat(stats::winscope_input_key_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_input_key_bytes), key_len);

  int64_t wd_len = FeedRawPacket(make_input(
      AndroidInputEvent::kDispatcherWindowDispatchEventRedactedFieldNumber,
      "wd"));
  EXPECT_EQ(GetStat(stats::winscope_input_window_dispatch_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_input_window_dispatch_bytes), wd_len);

  // Shell transitions + handler mappings.
  protozero::HeapBuffered<protozero::Message> shell_tr;
  shell_tr->AppendString(
      FrameworksBaseWinscopeTracePacket::kShellTransitionFieldNumber, "st");
  int64_t shell_tr_len = FeedRawPacket(shell_tr.SerializeAsArray());

  protozero::HeapBuffered<protozero::Message> shell_hm;
  shell_hm->AppendString(
      FrameworksBaseWinscopeTracePacket::kShellHandlerMappingsFieldNumber,
      "hm");
  int64_t shell_hm_len = FeedRawPacket(shell_hm.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_shell_transitions_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_shell_transitions_bytes),
            shell_tr_len + shell_hm_len);

  // ProtoLog message + viewer config.
  protozero::HeapBuffered<protozero::Message> pl_msg;
  pl_msg->AppendString(
      FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber, "pl");
  int64_t pl_msg_len = FeedRawPacket(pl_msg.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_protolog_bytes), pl_msg_len);

  protozero::HeapBuffered<protozero::Message> pl_cfg;
  pl_cfg->AppendString(
      FrameworksNativeWinscopeTracePacket::kProtologViewerConfigFieldNumber,
      "cfg");
  int64_t pl_cfg_len = FeedRawPacket(pl_cfg.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_protolog_view_config_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_protolog_view_config_bytes), pl_cfg_len);
}

TEST_F(WinscopeModuleTest, RecordsStandaloneInternedDataStats) {
  // 1. ProtoLog-only standalone interned_data.
  protozero::HeapBuffered<protozero::Message> pl_interned;
  pl_interned
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::kInternedDataFieldNumber)
      ->AppendString(
          FrameworksNativeInternedData::kProtologStringArgsFieldNumber, "arg1");
  int64_t pl_len = FeedRawPacket(pl_interned.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_protolog_bytes), pl_len);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 0);

  // 2. ViewCapture-only standalone interned_data.
  protozero::HeapBuffered<protozero::Message> vc_interned;
  vc_interned
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::kInternedDataFieldNumber)
      ->AppendString(
          FrameworksBaseInternedData::kViewcapturePackageNameFieldNumber,
          "com.example");
  int64_t vc_len = FeedRawPacket(vc_interned.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_bytes), vc_len);

  // 3. Mixed ProtoLog + ViewCapture standalone interned_data: must NOT
  // double-count raw_packet_bytes; each gets only its inner payload bytes and
  // the single packet count goes to the larger payload (ViewCapture here).
  const std::string pl_payload = "ab";
  const std::string vc_payload = "0123456789";
  protozero::HeapBuffered<protozero::Message> mixed;
  auto* id = mixed->BeginNestedMessage<protozero::Message>(
      TracePacket::kInternedDataFieldNumber);
  id->AppendString(FrameworksNativeInternedData::kProtologStacktraceFieldNumber,
                   pl_payload);
  id->AppendString(FrameworksBaseInternedData::kViewcaptureTextFieldNumber,
                   vc_payload);
  int64_t mixed_len = FeedRawPacket(mixed.SerializeAsArray());
  EXPECT_LT(static_cast<int64_t>(pl_payload.size() + vc_payload.size()),
            mixed_len);
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 1);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_protolog_bytes),
            pl_len + static_cast<int64_t>(pl_payload.size()));
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_bytes),
            vc_len + static_cast<int64_t>(vc_payload.size()));

  // 4. TrackEvent packet carrying interned_data must be skipped (fast path).
  protozero::HeapBuffered<protozero::Message> te;
  te->AppendString(TracePacket::kTrackEventFieldNumber, "te");
  te->BeginNestedMessage<protozero::Message>(
        TracePacket::kInternedDataFieldNumber)
      ->AppendString(
          FrameworksNativeInternedData::kProtologStringArgsFieldNumber, "skip");
  FeedRawPacket(te.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 1);

  // 5. ProtoLog message + interned_data in the same packet is counted once.
  protozero::HeapBuffered<protozero::Message> pl_both;
  pl_both->AppendString(
      FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber, "msg");
  pl_both
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::kInternedDataFieldNumber)
      ->AppendString(
          FrameworksNativeInternedData::kProtologStringArgsFieldNumber, "arg");
  int64_t pl_both_len = FeedRawPacket(pl_both.SerializeAsArray());
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 2);
  EXPECT_EQ(GetStat(stats::winscope_protolog_bytes),
            pl_len + static_cast<int64_t>(pl_payload.size()) + pl_both_len);
}

TEST_F(WinscopeModuleTest, RecordsProtoVmStateBytes) {
  // Build serialized TracePacket states for SF layers, WM, and VC.
  protozero::HeapBuffered<protozero::Message> sf_state_msg;
  sf_state_msg->AppendString(FrameworksNativeWinscopeTracePacket::
                                 kSurfaceflingerLayersSnapshotFieldNumber,
                             "sf_base_state");
  std::vector<uint8_t> sf_state = sf_state_msg.SerializeAsArray();

  protozero::HeapBuffered<protozero::Message> wm_state_msg;
  wm_state_msg
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(
          FrameworksBaseWinscopeExtensions::kWindowmanagerFieldNumber,
          "wm_base_state");
  std::vector<uint8_t> wm_state = wm_state_msg.SerializeAsArray();

  protozero::HeapBuffered<protozero::Message> vc_state_msg;
  vc_state_msg
      ->BeginNestedMessage<protozero::Message>(
          FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber)
      ->AppendString(FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber,
                     "vc_base_state");
  std::vector<uint8_t> vc_state = vc_state_msg.SerializeAsArray();

  protozero::HeapBuffered<protozero::Message> protovms_pkt;
  auto* protovms = protovms_pkt->BeginNestedMessage<protozero::Message>(
      TracePacket::kProtovmsFieldNumber);
  protovms
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::ProtoVms::kInstanceFieldNumber)
      ->AppendBytes(TracePacket::ProtoVms::Instance::kStateFieldNumber,
                    sf_state.data(), sf_state.size());
  protovms
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::ProtoVms::kInstanceFieldNumber)
      ->AppendBytes(TracePacket::ProtoVms::Instance::kStateFieldNumber,
                    wm_state.data(), wm_state.size());
  protovms
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::ProtoVms::kInstanceFieldNumber)
      ->AppendBytes(TracePacket::ProtoVms::Instance::kStateFieldNumber,
                    vc_state.data(), vc_state.size());

  FeedRawPacket(protovms_pkt.SerializeAsArray());

  EXPECT_EQ(GetStat(stats::winscope_sf_layers_protovm_state_bytes),
            static_cast<int64_t>(sf_state.size()));
  EXPECT_EQ(GetStat(stats::winscope_windowmanager_protovm_state_bytes),
            static_cast<int64_t>(wm_state.size()));
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_protovm_state_bytes),
            static_cast<int64_t>(vc_state.size()));
  EXPECT_EQ(GetStat(stats::winscope_sf_layers_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_windowmanager_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 0);
}

TEST_F(WinscopeModuleTest, IgnoresMalformedWireTypesWithoutCrashing) {
  // Non-length-delimited outer Winscope fields.
  protozero::HeapBuffered<protozero::Message> bad_outer;
  bad_outer->AppendVarInt(FrameworksNativeWinscopeTracePacket::
                              kSurfaceflingerLayersSnapshotFieldNumber,
                          123);
  bad_outer->AppendVarInt(
      FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber, 456);
  FeedRawPacket(bad_outer.SerializeAsArray());

  // Non-length-delimited nested WinscopeExtensions and AndroidInputEvent.
  protozero::HeapBuffered<protozero::Message> bad_nested;
  auto* ext = bad_nested->BeginNestedMessage<protozero::Message>(
      FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber);
  ext->AppendVarInt(FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber,
                    1);
  ext->BeginNestedMessage<protozero::Message>(
         FrameworksNativeWinscopeExtensions::kAndroidInputEventFieldNumber)
      ->AppendVarInt(AndroidInputEvent::kDispatcherMotionEventFieldNumber, 2);
  FeedRawPacket(bad_nested.SerializeAsArray());

  // Non-length-delimited interned_data outer field and inner subfields.
  protozero::HeapBuffered<protozero::Message> bad_interned_outer;
  bad_interned_outer->AppendVarInt(TracePacket::kInternedDataFieldNumber, 7);
  FeedRawPacket(bad_interned_outer.SerializeAsArray());

  protozero::HeapBuffered<protozero::Message> bad_interned_inner;
  bad_interned_inner
      ->BeginNestedMessage<protozero::Message>(
          TracePacket::kInternedDataFieldNumber)
      ->AppendVarInt(
          FrameworksNativeInternedData::kProtologStringArgsFieldNumber, 8);
  FeedRawPacket(bad_interned_inner.SerializeAsArray());

  // Non-length-delimited protovms outer field, instance, and state.
  protozero::HeapBuffered<protozero::Message> bad_protovms_outer;
  bad_protovms_outer->AppendVarInt(TracePacket::kProtovmsFieldNumber, 9);
  FeedRawPacket(bad_protovms_outer.SerializeAsArray());

  protozero::HeapBuffered<protozero::Message> bad_protovms_inner;
  auto* pvm = bad_protovms_inner->BeginNestedMessage<protozero::Message>(
      TracePacket::kProtovmsFieldNumber);
  pvm->AppendVarInt(TracePacket::ProtoVms::kInstanceFieldNumber, 10);
  pvm->BeginNestedMessage<protozero::Message>(
         TracePacket::ProtoVms::kInstanceFieldNumber)
      ->AppendVarInt(TracePacket::ProtoVms::Instance::kStateFieldNumber, 11);
  FeedRawPacket(bad_protovms_inner.SerializeAsArray());

  EXPECT_EQ(GetStat(stats::winscope_sf_layers_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_input_motion_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_protolog_packets), 0);
  EXPECT_EQ(GetStat(stats::winscope_sf_layers_protovm_state_bytes), 0);
  EXPECT_EQ(GetStat(stats::winscope_windowmanager_protovm_state_bytes), 0);
  EXPECT_EQ(GetStat(stats::winscope_viewcapture_protovm_state_bytes), 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
