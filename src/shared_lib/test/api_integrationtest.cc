/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <condition_variable>
#include <mutex>
#include <thread>

#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/pb_decoder_abi.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"

#include "test/gtest_and_gmock.h"

#include "src/shared_lib/reset_for_testing.h"
#include "src/shared_lib/test/utils.h"

// Tests for the perfetto shared library.

namespace {

using ::perfetto::shlib::test_utils::FieldView;
using ::perfetto::shlib::test_utils::IdFieldView;
using ::perfetto::shlib::test_utils::MsgField;
using ::perfetto::shlib::test_utils::PbField;
using ::perfetto::shlib::test_utils::StringField;
using ::perfetto::shlib::test_utils::TracingSession;
using ::testing::_;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SaveArg;

constexpr char kDataSourceName1[] = "dev.perfetto.example_data_source";
struct PerfettoDs data_source_1 = PERFETTO_DS_INIT();

constexpr char kDataSourceName2[] = "dev.perfetto.example_data_source2";
struct PerfettoDs data_source_2 = PERFETTO_DS_INIT();
void* const kDataSource2UserArg = reinterpret_cast<void*>(0x555);

class MockDs2Callbacks : testing::Mock {
 public:
  MOCK_METHOD(void*,
              OnSetup,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* ds_config,
               size_t ds_config_size,
               void* user_arg));
  MOCK_METHOD(void,
              OnStart,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* user_arg,
               void* inst_ctx));
  MOCK_METHOD(void,
              OnStop,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* user_arg,
               void* inst_ctx,
               struct PerfettoDsOnStopArgs* args));
  MOCK_METHOD(void*,
              OnCreateTls,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               struct PerfettoDsTracerImpl* tracer,
               void* user_arg));
  MOCK_METHOD(void, OnDeleteTls, (void*));
  MOCK_METHOD(void*,
              OnCreateIncr,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               struct PerfettoDsTracerImpl* tracer,
               void* user_arg));
  MOCK_METHOD(void, OnDeleteIncr, (void*));
};

class Notification {
 public:
  Notification() = default;
  void Notify() {
    std::unique_lock<std::mutex> lock(m_);
    notified_ = true;
    cv_.notify_one();
  }
  bool WaitForNotification() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [this] { return notified_; });
    return notified_;
  }
  bool Notified() {
    std::unique_lock<std::mutex> lock(m_);
    return notified_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool notified_ = false;
};

class SharedLibDataSourceTest : public testing::Test {
 protected:
  void SetUp() override {
    struct PerfettoProducerInitArgs args = {0};
    args.backends = PERFETTO_BACKEND_IN_PROCESS;
    PerfettoProducerInit(args);
    PerfettoDsRegister(&data_source_1, kDataSourceName1,
                       PerfettoDsNoCallbacks());
    RegisterDataSource2();
  }

  void TearDown() override {
    perfetto::shlib::ResetForTesting();
    data_source_1.enabled = &perfetto_atomic_false;
    perfetto::shlib::DsImplDestroy(data_source_1.impl);
    data_source_1.impl = nullptr;
    data_source_2.enabled = &perfetto_atomic_false;
    perfetto::shlib::DsImplDestroy(data_source_2.impl);
    data_source_2.impl = nullptr;
  }

  struct Ds2CustomState {
    void* actual;
    SharedLibDataSourceTest* thiz;
  };

  void RegisterDataSource2() {
    static struct PerfettoDsCallbacks callbacks = {};
    callbacks.on_setup_cb = [](struct PerfettoDsImpl* ds_impl,
                               PerfettoDsInstanceIndex inst_id, void* ds_config,
                               size_t ds_config_size, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      return thiz->ds2_callbacks_.OnSetup(ds_impl, inst_id, ds_config,
                                          ds_config_size, thiz->ds2_user_arg_);
    };
    callbacks.on_start_cb = [](struct PerfettoDsImpl* ds_impl,
                               PerfettoDsInstanceIndex inst_id, void* user_arg,
                               void* inst_ctx) -> void {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      return thiz->ds2_callbacks_.OnStart(ds_impl, inst_id, thiz->ds2_user_arg_,
                                          inst_ctx);
    };
    callbacks.on_stop_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           void* user_arg, void* inst_ctx, struct PerfettoDsOnStopArgs* args) {
          auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
          return thiz->ds2_callbacks_.OnStop(
              ds_impl, inst_id, thiz->ds2_user_arg_, inst_ctx, args);
        };
    callbacks.on_create_tls_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           struct PerfettoDsTracerImpl* tracer, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      auto* state = new Ds2CustomState();
      state->thiz = thiz;
      state->actual = thiz->ds2_callbacks_.OnCreateTls(ds_impl, inst_id, tracer,
                                                       thiz->ds2_user_arg_);
      return state;
    };
    callbacks.on_delete_tls_cb = [](void* ptr) {
      auto* state = static_cast<Ds2CustomState*>(ptr);
      state->thiz->ds2_callbacks_.OnDeleteTls(state->actual);
      delete state;
    };
    callbacks.on_create_incr_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           struct PerfettoDsTracerImpl* tracer, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      auto* state = new Ds2CustomState();
      state->thiz = thiz;
      state->actual = thiz->ds2_callbacks_.OnCreateIncr(
          ds_impl, inst_id, tracer, thiz->ds2_user_arg_);
      return state;
    };
    callbacks.on_delete_incr_cb = [](void* ptr) {
      auto* state = static_cast<Ds2CustomState*>(ptr);
      state->thiz->ds2_callbacks_.OnDeleteIncr(state->actual);
      delete state;
    };
    callbacks.user_arg = this;
    PerfettoDsRegister(&data_source_2, kDataSourceName2, callbacks);
  }

  void* Ds2ActualCustomState(void* ptr) {
    auto* state = static_cast<Ds2CustomState*>(ptr);
    return state->actual;
  }

  NiceMock<MockDs2Callbacks> ds2_callbacks_;
  void* ds2_user_arg_ = kDataSource2UserArg;
};

TEST_F(SharedLibDataSourceTest, DisabledNotExecuted) {
  bool executed = false;

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed = true;
  }

  EXPECT_FALSE(executed);
}

TEST_F(SharedLibDataSourceTest, EnabledOnce) {
  size_t executed = 0;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed++;
  }

  EXPECT_EQ(executed, 1u);
}

TEST_F(SharedLibDataSourceTest, EnabledTwice) {
  size_t executed = 0;
  TracingSession tracing_session1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  TracingSession tracing_session2 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed++;
  }

  EXPECT_EQ(executed, 2u);
}

TEST_F(SharedLibDataSourceTest, Serialization) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);

    {
      struct perfetto_protos_TestEvent for_testing;
      perfetto_protos_TracePacket_begin_for_testing(&trace_packet.msg,
                                                    &for_testing);
      {
        struct perfetto_protos_TestEvent_TestPayload payload;
        perfetto_protos_TestEvent_begin_payload(&for_testing, &payload);
        perfetto_protos_TestEvent_TestPayload_set_cstr_str(&payload,
                                                           "ABCDEFGH");
        perfetto_protos_TestEvent_end_payload(&for_testing, &payload);
      }
      perfetto_protos_TracePacket_end_for_testing(&trace_packet.msg,
                                                  &for_testing);
    }
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }
  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found_for_testing = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() == 0) {
      continue;
    }
    found_for_testing = true;
    ASSERT_EQ(for_testing.size(), 1u);
    ASSERT_THAT(FieldView(for_testing.front()),
                ElementsAre(PbField(
                    perfetto_protos_TestEvent_payload_field_number,
                    MsgField(ElementsAre(PbField(
                        perfetto_protos_TestEvent_TestPayload_str_field_number,
                        StringField("ABCDEFGH")))))));
  }
  EXPECT_TRUE(found_for_testing);
}

TEST_F(SharedLibDataSourceTest, Break) {
  TracingSession tracing_session1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  TracingSession tracing_session2 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);

    {
      struct perfetto_protos_TestEvent for_testing;
      perfetto_protos_TracePacket_begin_for_testing(&trace_packet.msg,
                                                    &for_testing);
      perfetto_protos_TracePacket_end_for_testing(&trace_packet.msg,
                                                  &for_testing);
    }
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
    // Break: the packet will be emitted only on the first data source instance
    // and therefore will not show up on `tracing_session2`.
    PERFETTO_DS_TRACE_BREAK(data_source_1, ctx);
  }
  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }

  tracing_session1.StopBlocking();
  std::vector<uint8_t> data1 = tracing_session1.ReadBlocking();
  EXPECT_THAT(
      FieldView(data1),
      Contains(PbField(perfetto_protos_Trace_packet_field_number,
                       MsgField(Contains(PbField(
                           perfetto_protos_TracePacket_for_testing_field_number,
                           MsgField(_)))))));
  tracing_session2.StopBlocking();
  std::vector<uint8_t> data2 = tracing_session2.ReadBlocking();
  EXPECT_THAT(
      FieldView(data2),
      Each(PbField(
          perfetto_protos_Trace_packet_field_number,
          MsgField(Not(Contains(PbField(
              perfetto_protos_TracePacket_for_testing_field_number, _)))))));
}

TEST_F(SharedLibDataSourceTest, FlushCb) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  Notification notification;

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    PerfettoDsTracerFlush(
        &ctx,
        [](void* p_notification) {
          static_cast<Notification*>(p_notification)->Notify();
        },
        &notification);
  }

  notification.WaitForNotification();
  EXPECT_TRUE(notification.Notified());
}

TEST_F(SharedLibDataSourceTest, LifetimeCallbacks) {
  void* const kInstancePtr = reinterpret_cast<void*>(0x44);
  testing::InSequence seq;
  PerfettoDsInstanceIndex setup_inst, start_inst, stop_inst;
  EXPECT_CALL(ds2_callbacks_, OnSetup(_, _, _, _, kDataSource2UserArg))
      .WillOnce(DoAll(SaveArg<1>(&setup_inst), Return(kInstancePtr)));
  EXPECT_CALL(ds2_callbacks_, OnStart(_, _, kDataSource2UserArg, kInstancePtr))
      .WillOnce(SaveArg<1>(&start_inst));

  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_, OnStop(_, _, kDataSource2UserArg, kInstancePtr, _))
      .WillOnce(SaveArg<1>(&stop_inst));

  tracing_session.StopBlocking();

  EXPECT_EQ(setup_inst, start_inst);
  EXPECT_EQ(setup_inst, stop_inst);
}

TEST_F(SharedLibDataSourceTest, StopDone) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  Notification stop_called;
  struct PerfettoDsAsyncStopper* stopper;

  EXPECT_CALL(ds2_callbacks_, OnStop(_, _, kDataSource2UserArg, _, _))
      .WillOnce([&](struct PerfettoDsImpl*, PerfettoDsInstanceIndex, void*, void*,
                    struct PerfettoDsOnStopArgs* args) {
        stopper = PerfettoDsOnStopArgsPostpone(args);
        stop_called.Notify();
      });

  std::thread t([&]() { tracing_session.StopBlocking(); });

  stop_called.WaitForNotification();
  PerfettoDsStopDone(stopper);

  t.join();
}

TEST_F(SharedLibDataSourceTest, ThreadLocalState) {
  bool ignored = false;
  void* const kTlsPtr = &ignored;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_, OnCreateTls).WillOnce(Return(kTlsPtr));

  void* tls_state = nullptr;
  PERFETTO_DS_TRACE(data_source_2, ctx) {
    tls_state = PerfettoDsGetCustomTls(&data_source_2, &ctx);
  }
  EXPECT_EQ(Ds2ActualCustomState(tls_state), kTlsPtr);

  tracing_session.StopBlocking();

  EXPECT_CALL(ds2_callbacks_, OnDeleteTls(kTlsPtr));

  // The OnDelete callback will be called by
  // DestroyStoppedTraceWritersForCurrentThread(). One way to trigger that is to
  // trace with another data source.
  TracingSession tracing_session_1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  PERFETTO_DS_TRACE(data_source_1, ctx) {}
}

TEST_F(SharedLibDataSourceTest, IncrementalState) {
  bool ignored = false;
  void* const kIncrPtr = &ignored;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_, OnCreateIncr).WillOnce(Return(kIncrPtr));

  void* tls_state = nullptr;
  PERFETTO_DS_TRACE(data_source_2, ctx) {
    tls_state = PerfettoDsGetIncrementalState(&data_source_2, &ctx);
  }
  EXPECT_EQ(Ds2ActualCustomState(tls_state), kIncrPtr);

  tracing_session.StopBlocking();

  EXPECT_CALL(ds2_callbacks_, OnDeleteIncr(kIncrPtr));

  // The OnDelete callback will be called by
  // DestroyStoppedTraceWritersForCurrentThread(). One way to trigger that is to
  // trace with another data source.
  TracingSession tracing_session_1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  PERFETTO_DS_TRACE(data_source_1, ctx) {}
}

}  // namespace
