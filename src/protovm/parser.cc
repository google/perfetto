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

#include "src/protovm/parser.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "src/protovm/error_handling.h"
#include "src/protovm/program.h"

namespace perfetto {
namespace protovm {

Parser::Parser(Executor* executor) : executor_(executor) {}

StatusOr<void> Parser::Run(const Program& program,
                           RoCursor src,
                           RwProto::Cursor dst) {
  program_ = &program;
  cursors_.src = src;
  cursors_.dst = dst;
  cursors_.innermost_saved_dst = nullptr;
  return ParseInstructions(program.GetRootInstructions());
}

StatusOr<void> Parser::ParseInstructions(
    base::Span<const Instruction> instructions) {
  int instruction_index = 0;
  for (const Instruction& instruction : instructions) {
    auto status = ParseInstruction(instruction);

    if (status.IsAbort()) {
      PROTOVM_RETURN(status, "instruction[%d]", instruction_index);
    }

    if (status.IsError()) {
      auto abort_level = instruction.abort_level();
      if (abort_level == AbortLevel::kSkipCurrentInstructionAndBreakOuter) {
        return StatusOr<void>::Error();
      }
      if (abort_level == AbortLevel::kAbort) {
        PROTOVM_ABORT(
            "instruction[%d]: returned status = error and instruction's "
            "abort level = 'abort'",
            instruction_index);
      }
    }

    ++instruction_index;
  }

  return StatusOr<void>::Ok();
}

StatusOr<void> Parser::ParseInstruction(const Instruction& instruction) {
  switch (instruction.type()) {
    case InstructionType::kSelect: {
      auto status = ParseSelect(instruction.select);
      PROTOVM_RETURN(status, "select");
    }
    case InstructionType::kRegLoad: {
      auto status = ParseRegLoad(instruction.reg_load);
      PROTOVM_RETURN_IF_NOT_OK(status, "reg_load");
      break;
    }
    case InstructionType::kDel: {
      PROTOVM_ASSIGN_OR_RETURN(bool is_root, cursors_.dst.IsRoot());
      if (cursors_.innermost_saved_dst != nullptr &&
          cursors_.dst == *cursors_.innermost_saved_dst && !is_root) {
        // Illegal operation
        // For each "select" operation we effectively save a snapshot of the
        // cursors on the stack, so that they can be restored when a frame is
        // exited. This "del" operation is attempting to delete a "dst" cursor
        // that is currently aliased in the parent stack frame. This would
        // result in a dangling pointer when the current frame is exited and
        // the aliased "dst" is restored.
        PROTOVM_ABORT(
            "del would invalidate a cursor saved by an enclosing select");
      }
      auto status = executor_->Delete(&cursors_.dst);
      PROTOVM_RETURN_IF_NOT_OK(status, "del");
      break;
    }
    case InstructionType::kMerge: {
      auto status = ParseMerge(instruction.merge);
      PROTOVM_RETURN_IF_NOT_OK(status, "merge");
      break;
    }
    case InstructionType::kSet: {
      auto status = executor_->Set(&cursors_);
      PROTOVM_RETURN_IF_NOT_OK(status, "set");
      break;
    }
  }

  return ParseInstructions(instruction.GetNestedInstructions(*program_));
}

StatusOr<void> Parser::ParseMerge(const InstructionMerge& merge) {
  auto status = executor_->Merge(&cursors_, merge.merge_flags);
  PROTOVM_RETURN_IF_NOT_OK(status);
  return status;
}

StatusOr<void> Parser::ParseRegLoad(const InstructionRegLoad& reg_load) {
  auto saved_selected = cursors_.selected;
  cursors_.selected = static_cast<CursorEnum>(reg_load.cursor);
  auto status = executor_->WriteRegister(cursors_, reg_load.dst_register);
  cursors_.selected = saved_selected;
  PROTOVM_RETURN_IF_NOT_OK(status);
  return status;
}

StatusOr<void> Parser::ParseSelect(const InstructionSelect& select) {
  auto saved_cursors = cursors_;
  cursors_.innermost_saved_dst = &saved_cursors.dst;
  cursors_.selected = static_cast<CursorEnum>(select.cursor);
  cursors_.create_if_not_exist = select.create_if_not_exist;

  base::Span<const PathComponent> path_components =
      select.GetPathComponents(*program_);
  if (path_components.empty()) {
    auto status = ParseInstructions(select.GetNestedInstructions(*program_));
    cursors_ = saved_cursors;
    return status;
  }

  auto status = ParseSelectRec(select, path_components);
  cursors_ = saved_cursors;

  return status;
}

StatusOr<void> Parser::ParseSelectRec(
    const InstructionSelect& select,
    base::Span<const PathComponent> remaining_components) {
  const PathComponent& curr_component = remaining_components[0];
  base::Span<const PathComponent> next_components(remaining_components.b + 1,
                                                  remaining_components.e);

  switch (curr_component.type()) {
    case PathComponentType::kRepeatedForEach: {
      uint32_t field_id = curr_component.repeated_for_each.field_id;
      if (cursors_.selected == CursorEnum::VM_CURSOR_SRC) {
        auto status_or_it =
            executor_->IterateRepeatedField(&cursors_.src, field_id);
        PROTOVM_RETURN_IF_NOT_OK(status_or_it,
                                 "iterate repeated field (id = %d)",
                                 static_cast<int>(field_id));
        int index = 0;
        for (auto it = *status_or_it; it; ++it) {
          cursors_.src = *it;
          auto status =
              !next_components.empty()
                  ? ParseSelectRec(select, next_components)
                  : ParseInstructions(select.GetNestedInstructions(*program_));
          PROTOVM_RETURN_IF_NOT_OK(status,
                                   "repeated field (id = %d, index = %d)",
                                   static_cast<int>(field_id), index);
          ++index;
        }
      } else {
        auto status_or_it =
            executor_->IterateRepeatedField(&cursors_.dst, field_id);
        PROTOVM_RETURN_IF_NOT_OK(status_or_it,
                                 "iterate repeated field (id = %d)",
                                 static_cast<int>(field_id));
        int index = 0;
        for (auto it = *status_or_it; it; ++it) {
          cursors_.dst = it.GetCursor();
          auto status =
              !next_components.empty()
                  ? ParseSelectRec(select, next_components)
                  : ParseInstructions(select.GetNestedInstructions(*program_));
          PROTOVM_RETURN_IF_NOT_OK(status,
                                   "repeated field (id = %d, index = %d)",
                                   static_cast<int>(field_id), index);
          ++index;
        }
      }
      return StatusOr<void>::Ok();
    }

    case PathComponentType::kRepeatedByIndex: {
      const auto& component = curr_component.repeated_by_index;
      auto status_enter = executor_->EnterRepeatedFieldAt(
          &cursors_, component.field_id, component.array_index);
      PROTOVM_RETURN_IF_NOT_OK(status_enter, "enter indexed repeated field");
      auto status_select =
          !next_components.empty()
              ? ParseSelectRec(select, next_components)
              : ParseInstructions(select.GetNestedInstructions(*program_));
      PROTOVM_RETURN(status_select, "repeated field (id = %d, index = %d)",
                     static_cast<int>(component.field_id),
                     static_cast<int>(component.array_index));
    }

    case PathComponentType::kRepeatedByKey: {
      const auto& component = curr_component.repeated_by_key;
      auto key = executor_->ReadRegister(component.register_to_match);
      PROTOVM_RETURN_IF_NOT_OK(key, "enter mapped repeated field");
      auto status_enter = executor_->EnterRepeatedFieldByKey(
          &cursors_, component.field_id, component.map_key_field_id,
          static_cast<uint32_t>(*key));
      PROTOVM_RETURN_IF_NOT_OK(status_enter, "enter mapped repeated field");
      auto status_select =
          !next_components.empty()
              ? ParseSelectRec(select, next_components)
              : ParseInstructions(select.GetNestedInstructions(*program_));
      PROTOVM_RETURN(status_select, "mapped repeated field (id = %d, key = %d)",
                     static_cast<int>(component.field_id),
                     static_cast<int>(*key));
    }

    case PathComponentType::kFieldId: {
      uint32_t field_id = curr_component.field_id.field_id;
      auto status = executor_->EnterField(&cursors_, field_id);
      PROTOVM_RETURN_IF_NOT_OK(status, "enter field (id = %d)",
                               static_cast<int>(field_id));
      return !next_components.empty()
                 ? ParseSelectRec(select, next_components)
                 : ParseInstructions(select.GetNestedInstructions(*program_));
    }
  }

  PERFETTO_FATAL("For GCC");
}

}  // namespace protovm
}  // namespace perfetto
