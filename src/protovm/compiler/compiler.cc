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

#include "src/protovm/compiler/compiler.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/compiler/compile_config.descriptor.h"
#include "src/protovm/compiler/trace.descriptor.h"
#include "src/protovm/compiler/winscope.descriptor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"

namespace perfetto::protovm {

Compiler::Compiler() : emitter_(&pool_) {
  pool_.AddFromFileDescriptorSet(perfetto::kTraceDescriptor.data(),
                                 perfetto::kTraceDescriptor.size());
  pool_.AddFromFileDescriptorSet(perfetto::kWinscopeDescriptor.data(),
                                 perfetto::kWinscopeDescriptor.size());
  auto idx = pool_.FindDescriptorIdx(".perfetto.protos.TracePacket");
  PERFETTO_CHECK(idx);
  root_proto_ = &pool_.descriptors()[*idx];
}

base::StatusOr<std::string> Compiler::Compile(
    std::string_view config_textproto) {
  auto status_or_proto =
      protozero::TextToProto(perfetto::kCompileConfigDescriptor.data(),
                             perfetto::kCompileConfigDescriptor.size(),
                             ".perfetto.protos.CompileConfig",
                             "compile_config.textproto", config_textproto);
  if (!status_or_proto.ok()) {
    return base::ErrStatus("Failed to parse config: %s",
                           status_or_proto.status().c_message());
  }

  protos::pbzero::CompileConfig::Decoder config(status_or_proto->data(),
                                                status_or_proto->size());
  protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> program;
  auto root =
      InstructionEmitter::Scope{program.get(), root_proto_, root_proto_};
  RETURN_IF_ERROR(ParseCommands(root, config.commands()));
  return program.SerializeAsString();
}

base::Status Compiler::ParseCommands(
    const InstructionEmitter::Scope& scope,
    protozero::RepeatedFieldIterator<protozero::ConstBytes> commands) const {
  for (auto it = commands; it; ++it) {
    protos::pbzero::Command::Decoder command(*it);
    if (command.has_set()) {
      RETURN_IF_ERROR(ParseSet(scope, command));
    } else if (command.has_del()) {
      RETURN_IF_ERROR(ParseDel(scope, command));
    } else if (command.has_merge()) {
      RETURN_IF_ERROR(ParseMerge(scope, command));
    } else if (command.has_enter_scope()) {
      RETURN_IF_ERROR(ParseEnterScope(scope, command));
    } else {
      return base::ErrStatus("Unknown command type");
    }
  }
  return base::OkStatus();
}

base::Status Compiler::ParseSet(
    const InstructionEmitter::Scope& scope,
    const protos::pbzero::Command::Decoder& command) const {
  protos::pbzero::SetCommand::Decoder set(command.set());
  auto src_path = ParsePath(set.src());
  auto dst_path = ParsePath(set.dst());
  return emitter_.Set(scope, src_path, dst_path, GetAbortLevel(command));
}

base::Status Compiler::ParseDel(
    const InstructionEmitter::Scope& scope,
    const protos::pbzero::Command::Decoder& command) const {
  protos::pbzero::DelCommand::Decoder del(command.del());
  auto src_path = ParsePath(del.src());
  auto dst_path = ParsePath(del.dst());
  if (!(del.if_src_present() ^ del.has_dst_key_field())) {
    return base::ErrStatus(
        "del command must specify exactly one of if_src_present or "
        "dst_key_field");
  }
  if (del.if_src_present()) {
    return emitter_.DeleteIfPresent(scope, src_path, dst_path,
                                    GetAbortLevel(command));
  }
  if (del.has_dst_key_field()) {
    return emitter_.DeleteByKey(scope, src_path, dst_path,
                                del.dst_key_field().ToStdStringView(),
                                GetAbortLevel(command));
  }
  return base::ErrStatus("Unsupported Del command variant");
}

base::Status Compiler::ParseMerge(
    const InstructionEmitter::Scope& scope,
    const protos::pbzero::Command::Decoder& command) const {
  protos::pbzero::MergeCommand::Decoder merge(command.merge());
  auto src_path = ParsePath(merge.src());
  auto dst_path = ParsePath(merge.dst());
  auto abort_level = GetAbortLevel(command);
  if (merge.has_key_field()) {
    return emitter_.MergeByKey(scope, src_path, dst_path,
                               merge.key_field().ToStdStringView(),
                               merge.recursive(), abort_level);
  }
  return emitter_.Merge(scope, src_path, dst_path, merge.recursive(),
                        abort_level);
}

base::Status Compiler::ParseEnterScope(
    const InstructionEmitter::Scope& scope,
    const protos::pbzero::Command::Decoder& command) const {
  protos::pbzero::EnterScopeCommand::Decoder enter_scope(command.enter_scope());
  auto src_path = ParsePath(enter_scope.src());
  auto dst_path = ParsePath(enter_scope.dst());
  auto abort_level = GetAbortLevel(command);
  ASSIGN_OR_RETURN(auto src_scope,
                   emitter_.Select(scope, src_path, Cursor::VM_CURSOR_SRC,
                                   false, abort_level));
  ASSIGN_OR_RETURN(
      auto dst_scope,
      emitter_.Select(src_scope, dst_path, Cursor::VM_CURSOR_DST, true,
                      InstructionEmitter::kDefaultAbortLevel));
  return ParseCommands(dst_scope, enter_scope.commands());
}

Compiler::AbortLevel Compiler::GetAbortLevel(
    const protos::pbzero::Command::Decoder& command) const {
  if (!command.has_abort_level()) {
    return AbortLevel::SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER;
  }
  return static_cast<AbortLevel>(command.abort_level());
}

}  // namespace perfetto::protovm
