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

#include <type_traits>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto {
namespace protovm {

Parser::Parser(const Program* program, Executor* executor)
    : program_(program), executor_(executor) {}

StatusOr<void> Parser::Run(RoCursor src, RwProto::Cursor dst) {
  cursors_.src = src;
  cursors_.dst = dst;
  cursors_.innermost_saved_dst = nullptr;
  return ParseInstructions(program_->GetRootInstructions());
}

StatusOr<void> Parser::ParseInstructions(
    base::Span<const Instruction> instructions) {
  int instruction_idx = 0;
  for (const Instruction& instruction : instructions) {
    auto status = ParseInstruction(instruction);

    if (status.IsAbort()) {
      PROTOVM_RETURN(status, "instruction[%d]", instruction_idx);
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
            instruction_idx);
      }
    }

    ++instruction_idx;
  }

  return StatusOr<void>::Ok();
}

StatusOr<void> Parser::ParseInstruction(const Instruction& instruction) {
  switch (instruction.type()) {
    case InstructionType::kSelect:
    case InstructionType::kSelectInlinedFieldId:
    case InstructionType::kSelectInlinedRepeatedForEach: {
      auto status = ParseSelect(instruction.select);
      PROTOVM_RETURN(status, "select");
    }
    case InstructionType::kRegLoad: {
      auto status = ParseRegLoad(instruction.reg_load);
      PROTOVM_RETURN_IF_NOT_OK(status, "reg_load");
      break;
    }
    case InstructionType::kDel: {
      auto status = ParseDel();
      PROTOVM_RETURN_IF_NOT_OK(status, "del");
      break;
    }
    case InstructionType::kMerge: {
      auto status = executor_->Merge(&cursors_, instruction.merge.merge_flags);
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

StatusOr<void> Parser::ParseRegLoad(const InstructionRegLoad& reg_load) {
  auto saved_selected = cursors_.selected;
  cursors_.selected = reg_load.cursor;
  auto status = executor_->WriteRegister(cursors_, reg_load.dst_register);
  cursors_.selected = saved_selected;
  return status;
}

StatusOr<void> Parser::ParseDel() {
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
    PROTOVM_ABORT("del would invalidate a cursor saved by an enclosing select");
  }
  return executor_->Delete(&cursors_.dst);
}

StatusOr<void> Parser::ParseSelect(const InstructionSelect& select) {
  const Cursors saved_cursors = cursors_;
  auto restore_cursors = base::OnScopeExit([&] { cursors_ = saved_cursors; });
  cursors_.innermost_saved_dst = &saved_cursors.dst;
  cursors_.selected = select.cursor;
  cursors_.create_if_not_exist = select.create_if_not_exist;

  if (select.type == InstructionType::kSelectInlinedFieldId) {
    uint32_t field_id = select.inlined_field_id;
    auto status = executor_->EnterField(&cursors_, field_id);
    PROTOVM_RETURN_IF_NOT_OK(status, "enter field (id = %d)",
                             static_cast<int>(field_id));
    return ParseInstructions(select.GetNestedInstructions(*program_));
  }
  if (select.type == InstructionType::kSelectInlinedRepeatedForEach) {
    return IterateRepeatedField(select, select.inlined_field_id, {});
  }
  return ParseSelectRec(select, select.GetPathComponents(*program_));
}

StatusOr<void> Parser::ParseSelectRec(
    const InstructionSelect& select,
    base::Span<const PathComponent> remaining_components) {
  for (const PathComponent* it = remaining_components.begin();
       it != remaining_components.end(); ++it) {
    const PathComponent& curr_component = *it;
    switch (curr_component.type()) {
      case PathComponentType::kFieldId: {
        uint32_t field_id = curr_component.field_id.field_id;
        auto status = executor_->EnterField(&cursors_, field_id);
        PROTOVM_RETURN_IF_NOT_OK(status, "enter field (id = %d)",
                                 static_cast<int>(field_id));
        break;
      }

      case PathComponentType::kRepeatedByIndex: {
        const auto& component = curr_component.repeated_by_index;
        auto status_enter = executor_->EnterRepeatedFieldAt(
            &cursors_, component.field_id, component.array_index);
        PROTOVM_RETURN_IF_NOT_OK(
            status_enter, "enter indexed repeated field (id = %d, index = %d)",
            static_cast<int>(component.field_id),
            static_cast<int>(component.array_index));
        break;
      }

      case PathComponentType::kRepeatedByKey: {
        const auto& component = curr_component.repeated_by_key;
        auto key = executor_->ReadRegister(component.register_to_match);
        PROTOVM_RETURN_IF_NOT_OK(key, "enter mapped repeated field");
        auto status_enter = executor_->EnterRepeatedFieldByKey(
            &cursors_, component.field_id, component.map_key_field_id, *key);
        PROTOVM_RETURN_IF_NOT_OK(
            status_enter,
            "enter mapped repeated field (id = %d, key = %" PRIu64 ")",
            static_cast<int>(component.field_id), *key);
        break;
      }

      case PathComponentType::kRepeatedForEach: {
        uint32_t field_id = curr_component.repeated_for_each.field_id;
        base::Span<const PathComponent> next_components(
            it + 1, remaining_components.end());
        return IterateRepeatedField(select, field_id, next_components);
      }
    }
  }

  return ParseInstructions(select.GetNestedInstructions(*program_));
}

StatusOr<void> Parser::IterateRepeatedField(
    const InstructionSelect& select,
    uint32_t field_id,
    base::Span<const PathComponent> next_components) {
  if (cursors_.selected == CursorSelection::kSrc) {
    return IterateRepeatedFieldImpl<RoCursor>(select, field_id,
                                              next_components);
  }
  PERFETTO_DCHECK(cursors_.selected == CursorSelection::kDst);
  return IterateRepeatedFieldImpl<RwProto::Cursor>(select, field_id,
                                                   next_components);
}

template <class CursorType>
StatusOr<void> Parser::IterateRepeatedFieldImpl(
    const InstructionSelect& select,
    uint32_t field_id,
    base::Span<const PathComponent> next_components) {
  CursorType* cursor;
  if constexpr (std::is_same_v<CursorType, RoCursor>) {
    cursor = &cursors_.src;
  } else {
    cursor = &cursors_.dst;
  }
  auto status_or_it = executor_->IterateRepeatedField(cursor, field_id);
  PROTOVM_RETURN_IF_NOT_OK(status_or_it, "iterate repeated field (id = %d)",
                           static_cast<int>(field_id));
  base::Span<const Instruction> nested_instructions =
      select.GetNestedInstructions(*program_);
  int index = 0;
  for (auto elem_it = *status_or_it; elem_it; ++elem_it) {
    if constexpr (std::is_same_v<CursorType, RoCursor>) {
      *cursor = *elem_it;
    } else {
      *cursor = elem_it.GetCursor();
    }
    auto status = !next_components.empty()
                      ? ParseSelectRec(select, next_components)
                      : ParseInstructions(nested_instructions);
    PROTOVM_RETURN_IF_NOT_OK(status, "repeated field (id = %d, index = %d)",
                             static_cast<int>(field_id), index);
    ++index;
  }
  return StatusOr<void>::Ok();
}

}  // namespace protovm
}  // namespace perfetto
