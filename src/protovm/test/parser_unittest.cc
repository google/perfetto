/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/protovm/executor.h"
#include "src/protovm/test/protos/incremental_trace.pb.h"
#include "src/protovm/test/sample_packets.h"
#include "test/gtest_and_gmock.h"

#include "src/protovm/parser.h"
#include "src/protovm/program_decoder.h"
#include "src/protovm/ro_cursor.h"
#include "src/protovm/rw_proto.h"
#include "src/protovm/test/mock_executor.h"
#include "src/protovm/test/sample_programs.h"
#include "src/protovm/test/utils.h"

namespace perfetto {
namespace protovm {
namespace test {

class ParserTest : public ::testing::Test {
 protected:
  StatusOr<void> RunProgram(const perfetto::protos::VmProgram& proto_program) {
    auto serialized = proto_program.SerializeAsString();
    auto program = ProgramDecoder::Decode(AsConstBytes(serialized));
    PERFETTO_CHECK(program.IsOk());
    Parser parser(&*program, &executor_);
    return parser.Run(RoCursor{}, rw_proto_.GetRoot());
  }

  MockExecutor executor_;
  Allocator allocator_{10 * 1024 * 1024};
  RwProto rw_proto_{&allocator_};
};

TEST_F(ParserTest, NoInstructions) {
  EXPECT_CALL(
      executor_,
      EnterField(testing::Field(&Cursors::selected, Cursor::kSrc), testing::_))
      .Times(0);

  EXPECT_CALL(executor_,
              EnterRepeatedFieldAt(testing::_, testing::_, testing::_))
      .Times(0);

  EXPECT_CALL(executor_,
              IterateRepeatedField(testing::An<RoCursor*>(), testing::_))
      .Times(0);

  EXPECT_CALL(executor_, ReadRegister(testing::_)).Times(0);

  ASSERT_TRUE(RunProgram(SamplePrograms::NoInstructions()).IsOk());
}

TEST_F(ParserTest, RegLoad) {
  EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_))
      .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

  ASSERT_TRUE(RunProgram(SamplePrograms::RegLoad()).IsOk());
}

TEST_F(ParserTest, Del) {
  EXPECT_CALL(executor_, Delete(testing::_))
      .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

  ASSERT_TRUE(RunProgram(SamplePrograms::Delete()).IsOk());
}

TEST_F(ParserTest, Merge) {
  EXPECT_CALL(executor_, Merge(testing::_, Executor::MergeFlags::kNone))
      .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

  ASSERT_TRUE(RunProgram(SamplePrograms::Merge()).IsOk());
}

TEST_F(ParserTest, Merge_DelIfSrcEmpty) {
  EXPECT_CALL(executor_,
              Merge(testing::_, Executor::MergeFlags::kDelIfSrcEmpty))
      .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

  ASSERT_TRUE(RunProgram(SamplePrograms::Merge_DelIfSrcEmpty()).IsOk());
}

TEST_F(ParserTest, Set) {
  EXPECT_CALL(executor_, Set(testing::_))
      .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

  ASSERT_TRUE(RunProgram(SamplePrograms::Set()).IsOk());
}

TEST_F(ParserTest, Select_AllCursorTypes) {
  {
    testing::InSequence seq;

    // Default (SRC)
    EXPECT_CALL(executor_,
                EnterField(testing::Field(&Cursors::selected, Cursor::kSrc), 1))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // SRC
    EXPECT_CALL(executor_,
                EnterField(testing::Field(&Cursors::selected, Cursor::kSrc), 2))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // DST
    EXPECT_CALL(executor_,
                EnterField(testing::Field(&Cursors::selected, Cursor::kDst), 3))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::Select_AllCursorTypes()).IsOk());
}

TEST_F(ParserTest, Select_InlinedPathComponent) {
  {
    testing::InSequence seq;

    EXPECT_CALL(
        executor_,
        EnterField(testing::Field(&Cursors::selected, Cursor::kSrc), 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    EXPECT_CALL(executor_,
                IterateRepeatedField(testing::An<RwProto::Cursor*>(), 11))
        .WillOnce(testing::Return(
            testing::ByMove(StatusOr<RwProto::Cursor::RepeatedFieldIterator>(
                RwProto::Cursor::RepeatedFieldIterator{}))));
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::Select_InlinedPathComponent()).IsOk());
}

TEST_F(ParserTest, Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max) {
  {
    testing::InSequence seq;

    EXPECT_CALL(
        executor_,
        EnterField(testing::Field(&Cursors::selected, Cursor::kSrc), 65536))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    EXPECT_CALL(executor_,
                IterateRepeatedField(testing::An<RoCursor*>(), 65537))
        .WillOnce(testing::Return(
            testing::ByMove(StatusOr<RoCursor::RepeatedFieldIterator>(
                RoCursor::RepeatedFieldIterator{}))));
  }

  ASSERT_TRUE(
      RunProgram(
          SamplePrograms::
              Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max())
          .IsOk());
}

TEST_F(ParserTest, Select_FusedPathComponents) {
  {
    testing::InSequence seq;

    EXPECT_CALL(executor_,
                EnterRepeatedFieldAt(
                    testing::Field(&Cursors::selected, Cursor::kSrc), 1, 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    EXPECT_CALL(executor_, ReadRegister(4))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<uint64_t>(42ul))));

    EXPECT_CALL(executor_,
                EnterRepeatedFieldByKey(
                    testing::Field(&Cursors::selected, Cursor::kSrc), 2, 3, 42))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::Select_FusedPathComponents()).IsOk());
}

TEST_F(ParserTest, Select_AccessAllFieldTypes) {
  auto proto_entry_with_two_elements =
      SamplePackets::TraceEntryWithTwoElements().SerializeAsString();

  {
    testing::InSequence seq;

    // enter field
    EXPECT_CALL(executor_, EnterField(testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // enter repeated field
    EXPECT_CALL(executor_,
                EnterRepeatedFieldAt(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // enter mapped repeated field
    EXPECT_CALL(executor_, ReadRegister(testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<uint64_t>(0ul))));

    EXPECT_CALL(executor_, EnterRepeatedFieldByKey(testing::_, testing::_,
                                                   testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // iterate repeated fields
    auto it_two_elements =
        RoCursor{AsConstBytes(proto_entry_with_two_elements)}
            .IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
    ASSERT_TRUE(it_two_elements.IsOk());
    EXPECT_CALL(executor_,
                IterateRepeatedField(testing::An<RoCursor*>(), testing::_))
        .WillOnce(testing::Return(testing::ByMove(std::move(it_two_elements))));

    // execute nested instruction (reg_load)
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_))
        .Times(2)
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::Select_AllFieldTypes()).IsOk());
}

TEST_F(ParserTest, Select_ExecutesNestedInstructions) {
  auto proto_entry_with_two_elementes =
      SamplePackets::TraceEntryWithTwoElements().SerializeAsString();

  {
    testing::InSequence seq;

    // iterate repeated fields
    auto status_or_it =
        RoCursor{AsConstBytes(proto_entry_with_two_elementes)}
            .IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
    ASSERT_TRUE(status_or_it.IsOk());
    EXPECT_CALL(executor_,
                IterateRepeatedField(testing::An<RoCursor*>(), testing::_))
        .WillOnce(testing::Return(testing::ByMove(std::move(status_or_it))));

    // repeated field #1
    {
      // nested instruction #1
      EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
          .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

      // nested instruction #2
      EXPECT_CALL(executor_, WriteRegister(testing::_, 11))
          .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
    }

    // repeated field #2
    {
      // nested instruction #1
      EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
          .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

      // nested instruction #2
      EXPECT_CALL(executor_, WriteRegister(testing::_, 11))
          .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
    }
  }

  ASSERT_TRUE(
      RunProgram(SamplePrograms::Select_ExecutesNestedInstructions()).IsOk());
}

TEST_F(ParserTest, Select_CanBreakOuterNestedInstructions) {
  {
    testing::InSequence seq;

    // root instruction
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // nested instruction #1: ok
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));

    // nested instruction #2: failing select
    EXPECT_CALL(executor_, EnterField(testing::_, testing::_))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Error())));

    // nested select #3: skipped because select above failed
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_)).Times(0);
  }

  ASSERT_TRUE(
      RunProgram(SamplePrograms::Select_CanBreakOuterNestedInstructions())
          .IsError());
}

TEST_F(ParserTest,
       AbortLevel_default_is_SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER) {
  {
    testing::InSequence seq;

    EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Error())));

    // dont' execute following instruction (break outer)
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_)).Times(0);
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::AbortLevel_default()).IsError());
}

TEST_F(ParserTest, AbortLevel_SKIP_CURRENT_INSTRUCTION) {
  {
    testing::InSequence seq;

    EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Error())));

    // execute following instruction in spite of failure
    EXPECT_CALL(executor_, WriteRegister(testing::_, 11))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Ok())));
  }

  ASSERT_TRUE(
      RunProgram(SamplePrograms::AbortLevel_SKIP_CURRENT_INSTRUCTION()).IsOk());
}

TEST_F(ParserTest, AbortLevel_SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER) {
  {
    testing::InSequence seq;

    EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Error())));

    // don't execute following instruction (break outer)
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_)).Times(0);
  }

  ASSERT_TRUE(
      RunProgram(
          SamplePrograms::AbortLevel_SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER())
          .IsError());
}

TEST_F(ParserTest, AbortLevel_ABORT) {
  {
    testing::InSequence seq;

    EXPECT_CALL(executor_, WriteRegister(testing::_, 10))
        .WillOnce(testing::Return(testing::ByMove(StatusOr<void>::Error())));

    // don't execute following instructions
    EXPECT_CALL(executor_, WriteRegister(testing::_, testing::_)).Times(0);
  }

  ASSERT_TRUE(RunProgram(SamplePrograms::AbortLevel_ABORT()).IsAbort());
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
