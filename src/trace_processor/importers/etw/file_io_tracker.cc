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

#include "src/trace_processor/importers/etw/file_io_tracker.h"

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/etw/etw.pbzero.h"
#include "protos/perfetto/trace/etw/etw_event.pbzero.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/tracks.h"

namespace perfetto::trace_processor {

namespace {

using protozero::ConstBytes;
using std::nullopt;
using std::optional;

// Argument field names used by file I/O events.
constexpr const char kCreateOptionsArg[] = "Create Options";
constexpr const char kDispositionArg[] = "Disposition";
constexpr const char kEnumerationPathArg[] = "Enumeration Path";
constexpr const char kExtraInfoArg[] = "Extra Info";
constexpr const char kFileAttributesArg[] = "File Attributes";
constexpr const char kFileIndexArg[] = "File Index";
constexpr const char kFileKeyArg[] = "File Key";
constexpr const char kFileObjectArg[] = "File Object";
constexpr const char kFileSizeArg[] = "File Size";
constexpr const char kInfoClassArg[] = "Info Class";
constexpr const char kIoFlagsArg[] = "I/O Flags";
constexpr const char kIrpArg[] = "I/O Request Packet";
constexpr const char kIoSizeArg[] = "I/O Size";
constexpr const char kNtStatusArg[] = "NT Status";
constexpr const char kOffsetArg[] = "Offset";
constexpr const char kOpenPathArg[] = "Open Path";
constexpr const char kShareAccessArg[] = "Share Access";
constexpr const char kThreadIdArg[] = "Thread ID";

// Opcodes for the file I/O event types. Source: `FileIo` class docs:
// https://learn.microsoft.com/en-us/windows/win32/etw/fileio
enum EventType {
  kFileCreate = 64,
  kDirectoryEnumeration = 72,
  kDirectoryNotification = 77,
  kSetInformation = 69,
  kDeleteFile = 70,
  kRenameFile = 71,
  kQueryFileInformation = 74,
  kFilesystemControlEvent = 75,
  kFileRead = 67,
  kFileWrite = 68,
  kCleanup = 65,
  kClose = 66,
  kFlush = 73
};

// Returns a readable description for a file I/O event type.
const char* GetEventTypeString(EventType event_type) {
  switch (event_type) {
    case kFileCreate:
      return "FileCreate";
    case kDirectoryEnumeration:
      return "DirectoryEnumeration";
    case kDirectoryNotification:
      return "DirectoryNotification";
    case kSetInformation:
      return "SetInformation";
    case kDeleteFile:
      return "DeleteFile";
    case kRenameFile:
      return "RenameFile";
    case kQueryFileInformation:
      return "QueryFileInformation";
    case kFilesystemControlEvent:
      return "FilesystemControlEvent";
    case kFileRead:
      return "FileRead";
    case kFileWrite:
      return "FileWrite";
    case kCleanup:
      return "Cleanup";
    case kClose:
      return "Close";
    case kFlush:
      return "Flush";
  }
  return nullptr;
}

// Values for the "File Info" argument. Source: `FILE_INFORMATION_CLASS` docs:
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_file_information_class
enum FileInfoClass {
  kFileDirectoryInformation = 1,
  kFileFullDirectoryInformation = 2,
  kFileBothDirectoryInformation = 3,
  kFileBasicInformation = 4,
  kFileStandardInformation = 5,
  kFileInternalInformation = 6,
  kFileEaInformation = 7,
  kFileAccessInformation = 8,
  kFileNameInformation = 9,
  kFileRenameInformation = 10,
  kFileLinkInformation = 11,
  kFileNamesInformation = 12,
  kFileDispositionInformation = 13,
  kFilePositionInformation = 14,
  kFileFullEaInformation = 15,
  kFileModeInformation = 16,
  kFileAlignmentInformation = 17,
  kFileAllInformation = 18,
  kFileAllocationInformation = 19,
  kFileEndOfFileInformation = 20,
  kFileAlternateNameInformation = 21,
  kFileStreamInformation = 22,
  kFilePipeInformation = 23,
  kFilePipeLocalInformation = 24,
  kFilePipeRemoteInformation = 25,
  kFileMailslotQueryInformation = 26,
  kFileMailslotSetInformation = 27,
  kFileCompressionInformation = 28,
  kFileObjectIdInformation = 29,
  kFileCompletionInformation = 30,
  kFileMoveClusterInformation = 31,
  kFileQuotaInformation = 32,
  kFileReparsePointInformation = 33,
  kFileNetworkOpenInformation = 34,
  kFileAttributeTagInformation = 35,
  kFileTrackingInformation = 36,
  kFileIdBothDirectoryInformation = 37,
  kFileIdFullDirectoryInformation = 38,
  kFileValidDataLengthInformation = 39,
  kFileShortNameInformation = 40,
  kFileIoCompletionNotificationInformation = 41,
  kFileIoStatusBlockRangeInformation = 42,
  kFileIoPriorityHintInformation = 43,
  kFileSfioReserveInformation = 44,
  kFileSfioVolumeInformation = 45,
  kFileHardLinkInformation = 46,
  kFileProcessIdsUsingFileInformation = 47,
  kFileNormalizedNameInformation = 48,
  kFileNetworkPhysicalNameInformation = 49,
  kFileIdGlobalTxDirectoryInformation = 50,
  kFileIsRemoteDeviceInformation = 51,
  kFileUnusedInformation = 52,
  kFileNumaNodeInformation = 53,
  kFileStandardLinkInformation = 54,
  kFileRemoteProtocolInformation = 55,
  kFileRenameInformationBypassAccessCheck = 56,
  kFileLinkInformationBypassAccessCheck = 57,
  kFileVolumeNameInformation = 58,
  kFileIdInformation = 59,
  kFileIdExtdDirectoryInformation = 60,
  kFileReplaceCompletionInformation = 61,
  kFileHardLinkFullIdInformation = 62,
  kFileIdExtdBothDirectoryInformation = 63,
  kFileDispositionInformationEx = 64,
  kFileRenameInformationEx = 65,
  kFileRenameInformationExBypassAccessCheck = 66,
  kFileDesiredStorageClassInformation = 67,
  kFileStatInformation = 68,
  kFileMemoryPartitionInformation = 69,
  kFileStatLxInformation = 70,
  kFileCaseSensitiveInformation = 71,
  kFileLinkInformationEx = 72,
  kFileLinkInformationExBypassAccessCheck = 73,
  kFileStorageReserveIdInformation = 74,
  kFileCaseSensitiveInformationForceAccessCheck = 75,
  kFileKnownFolderInformation = 76,
  kFileStatBasicInformation = 77,
  kFileId64ExtdDirectoryInformation = 78,
  kFileId64ExtdBothDirectoryInformation = 79,
  kFileIdAllExtdDirectoryInformation = 80,
  kFileIdAllExtdBothDirectoryInformation = 81
};

// Returns a readable description for a "File Info" argument value.
const char* GetFileInfoClassString(FileInfoClass file_info_class) {
  switch (file_info_class) {
    case kFileDirectoryInformation:
      return "FileDirectoryInformation";
    case kFileFullDirectoryInformation:
      return "FileFullDirectoryInformation";
    case kFileBothDirectoryInformation:
      return "FileBothDirectoryInformation";
    case kFileBasicInformation:
      return "FileBasicInformation";
    case kFileStandardInformation:
      return "FileStandardInformation";
    case kFileInternalInformation:
      return "FileInternalInformation";
    case kFileEaInformation:
      return "FileEaInformation";
    case kFileAccessInformation:
      return "FileAccessInformation";
    case kFileNameInformation:
      return "FileNameInformation";
    case kFileRenameInformation:
      return "FileRenameInformation";
    case kFileLinkInformation:
      return "FileLinkInformation";
    case kFileNamesInformation:
      return "FileNamesInformation";
    case kFileDispositionInformation:
      return "FileDispositionInformation";
    case kFilePositionInformation:
      return "FilePositionInformation";
    case kFileFullEaInformation:
      return "FileFullEaInformation";
    case kFileModeInformation:
      return "FileModeInformation";
    case kFileAlignmentInformation:
      return "FileAlignmentInformation";
    case kFileAllInformation:
      return "FileAllInformation";
    case kFileAllocationInformation:
      return "FileAllocationInformation";
    case kFileEndOfFileInformation:
      return "FileEndOfFileInformation";
    case kFileAlternateNameInformation:
      return "FileAlternateNameInformation";
    case kFileStreamInformation:
      return "FileStreamInformation";
    case kFilePipeInformation:
      return "FilePipeInformation";
    case kFilePipeLocalInformation:
      return "FilePipeLocalInformation";
    case kFilePipeRemoteInformation:
      return "FilePipeRemoteInformation";
    case kFileMailslotQueryInformation:
      return "FileMailslotQueryInformation";
    case kFileMailslotSetInformation:
      return "FileMailslotSetInformation";
    case kFileCompressionInformation:
      return "FileCompressionInformation";
    case kFileObjectIdInformation:
      return "FileObjectIdInformation";
    case kFileCompletionInformation:
      return "FileCompletionInformation";
    case kFileMoveClusterInformation:
      return "FileMoveClusterInformation";
    case kFileQuotaInformation:
      return "FileQuotaInformation";
    case kFileReparsePointInformation:
      return "FileReparsePointInformation";
    case kFileNetworkOpenInformation:
      return "FileNetworkOpenInformation";
    case kFileAttributeTagInformation:
      return "FileAttributeTagInformation";
    case kFileTrackingInformation:
      return "FileTrackingInformation";
    case kFileIdBothDirectoryInformation:
      return "FileIdBothDirectoryInformation";
    case kFileIdFullDirectoryInformation:
      return "FileIdFullDirectoryInformation";
    case kFileValidDataLengthInformation:
      return "FileValidDataLengthInformation";
    case kFileShortNameInformation:
      return "FileShortNameInformation";
    case kFileIoCompletionNotificationInformation:
      return "FileIoCompletionNotificationInformation";
    case kFileIoStatusBlockRangeInformation:
      return "FileIoStatusBlockRangeInformation";
    case kFileIoPriorityHintInformation:
      return "FileIoPriorityHintInformation";
    case kFileSfioReserveInformation:
      return "FileSfioReserveInformation";
    case kFileSfioVolumeInformation:
      return "FileSfioVolumeInformation";
    case kFileHardLinkInformation:
      return "FileHardLinkInformation";
    case kFileProcessIdsUsingFileInformation:
      return "FileProcessIdsUsingFileInformation";
    case kFileNormalizedNameInformation:
      return "FileNormalizedNameInformation";
    case kFileNetworkPhysicalNameInformation:
      return "FileNetworkPhysicalNameInformation";
    case kFileIdGlobalTxDirectoryInformation:
      return "FileIdGlobalTxDirectoryInformation";
    case kFileIsRemoteDeviceInformation:
      return "FileIsRemoteDeviceInformation";
    case kFileUnusedInformation:
      return "FileUnusedInformation";
    case kFileNumaNodeInformation:
      return "FileNumaNodeInformation";
    case kFileStandardLinkInformation:
      return "FileStandardLinkInformation";
    case kFileRemoteProtocolInformation:
      return "FileRemoteProtocolInformation";
    case kFileRenameInformationBypassAccessCheck:
      return "FileRenameInformationBypassAccessCheck";
    case kFileLinkInformationBypassAccessCheck:
      return "FileLinkInformationBypassAccessCheck";
    case kFileVolumeNameInformation:
      return "FileVolumeNameInformation";
    case kFileIdInformation:
      return "FileIdInformation";
    case kFileIdExtdDirectoryInformation:
      return "FileIdExtdDirectoryInformation";
    case kFileReplaceCompletionInformation:
      return "FileReplaceCompletionInformation";
    case kFileHardLinkFullIdInformation:
      return "FileHardLinkFullIdInformation";
    case kFileIdExtdBothDirectoryInformation:
      return "FileIdExtdBothDirectoryInformation";
    case kFileDispositionInformationEx:
      return "FileDispositionInformationEx";
    case kFileRenameInformationEx:
      return "FileRenameInformationEx";
    case kFileRenameInformationExBypassAccessCheck:
      return "FileRenameInformationExBypassAccessCheck";
    case kFileDesiredStorageClassInformation:
      return "FileDesiredStorageClassInformation";
    case kFileStatInformation:
      return "FileStatInformation";
    case kFileMemoryPartitionInformation:
      return "FileMemoryPartitionInformation";
    case kFileStatLxInformation:
      return "FileStatLxInformation";
    case kFileCaseSensitiveInformation:
      return "FileCaseSensitiveInformation";
    case kFileLinkInformationEx:
      return "FileLinkInformationEx";
    case kFileLinkInformationExBypassAccessCheck:
      return "FileLinkInformationExBypassAccessCheck";
    case kFileStorageReserveIdInformation:
      return "FileStorageReserveIdInformation";
    case kFileCaseSensitiveInformationForceAccessCheck:
      return "FileCaseSensitiveInformationForceAccessCheck";
    case kFileKnownFolderInformation:
      return "FileKnownFolderInformation";
    case kFileStatBasicInformation:
      return "FileStatBasicInformation";
    case kFileId64ExtdDirectoryInformation:
      return "FileId64ExtdDirectoryInformation";
    case kFileId64ExtdBothDirectoryInformation:
      return "FileId64ExtdBothDirectoryInformation";
    case kFileIdAllExtdDirectoryInformation:
      return "FileIdAllExtdDirectoryInformation";
    case kFileIdAllExtdBothDirectoryInformation:
      return "FileIdAllExtdBothDirectoryInformation";
  }
  return nullptr;
}

}  // namespace

FileIoTracker::FileIoTracker(TraceProcessorContext* context)
    : context_(context) {}

void FileIoTracker::ParseFileIoCreate(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoCreateEtwEvent::Decoder decoder(blob);
  if (!decoder.has_irp_ptr()) {
    return;
  }
  const auto irp = decoder.irp_ptr();
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto ttid = decoder.has_ttid() ? optional(decoder.ttid()) : nullopt;
  const auto create_options = decoder.has_create_options()
                                  ? optional(decoder.create_options())
                                  : nullopt;
  const auto file_attributes = decoder.has_file_attributes()
                                   ? optional(decoder.file_attributes())
                                   : nullopt;
  const auto share_access =
      decoder.has_share_access() ? optional(decoder.share_access()) : nullopt;
  const auto open_path =
      decoder.has_open_path()
          ? optional(context_->storage->InternString(decoder.open_path()))
          : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, irp, file_object, ttid, create_options, file_attributes,
       share_access, open_path](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(context_->storage->InternString(kIrpArg),
                         Variadic::Pointer(irp));
        if (file_object) {
          inserter->AddArg(context_->storage->InternString(kFileObjectArg),
                           Variadic::Pointer(*file_object));
        }
        if (ttid) {
          inserter->AddArg(context_->storage->InternString(kThreadIdArg),
                           Variadic::UnsignedInteger(*ttid));
        }
        if (create_options) {
          inserter->AddArg(context_->storage->InternString(kCreateOptionsArg),
                           Variadic::Pointer(*create_options));
        }
        if (file_attributes) {
          inserter->AddArg(context_->storage->InternString(kFileAttributesArg),
                           Variadic::Pointer(*file_attributes));
        }
        if (share_access) {
          inserter->AddArg(context_->storage->InternString(kShareAccessArg),
                           Variadic::Pointer(*share_access));
        }
        if (open_path) {
          inserter->AddArg(context_->storage->InternString(kOpenPathArg),
                           Variadic::String(*open_path));
        }
      };
  StartEvent(irp, {timestamp, kFileCreate, std::move(set_args)});
}

void FileIoTracker::ParseFileIoDirEnum(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoDirEnumEtwEvent::Decoder decoder(blob);
  if (!decoder.has_opcode() || !decoder.has_irp_ptr()) {
    return;
  }
  const auto irp = decoder.irp_ptr();
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto file_key =
      decoder.has_file_key() ? optional(decoder.file_key()) : nullopt;
  const auto ttid = decoder.has_ttid() ? optional(decoder.ttid()) : nullopt;
  const auto info_class =
      decoder.has_info_class() ? optional(decoder.info_class()) : nullopt;
  const auto file_index =
      decoder.has_file_index() ? optional(decoder.file_index()) : nullopt;
  const auto file_name =
      decoder.has_file_name()
          ? optional(context_->storage->InternString(decoder.file_name()))
          : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, irp, file_object, file_key, ttid, info_class, file_index,
       file_name](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(context_->storage->InternString(kIrpArg),
                         Variadic::Pointer(irp));
        if (file_object) {
          inserter->AddArg(context_->storage->InternString(kFileObjectArg),
                           Variadic::Pointer(*file_object));
        }
        if (file_key) {
          inserter->AddArg(context_->storage->InternString(kFileKeyArg),
                           Variadic::Pointer(*file_key));
        }
        if (ttid) {
          inserter->AddArg(context_->storage->InternString(kThreadIdArg),
                           Variadic::UnsignedInteger(*ttid));
        }
        if (info_class) {
          // If a string version of `info_class` is known, use that as the arg.
          // Otherwise, use its numerical value.
          const char* info_class_str =
              GetFileInfoClassString(static_cast<FileInfoClass>(*info_class));
          inserter->AddArg(
              context_->storage->InternString(kInfoClassArg),
              info_class_str ? Variadic::String(context_->storage->InternString(
                                   info_class_str))
                             : Variadic::UnsignedInteger(*info_class));
        }
        if (file_index) {
          inserter->AddArg(context_->storage->InternString(kFileIndexArg),
                           Variadic::UnsignedInteger(*file_index));
        }
        if (file_name) {
          inserter->AddArg(context_->storage->InternString(kEnumerationPathArg),
                           Variadic::String(*file_name));
        }
      };
  StartEvent(irp, {timestamp, decoder.opcode(), std::move(set_args)});
}

void FileIoTracker::ParseFileIoInfo(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoInfoEtwEvent::Decoder decoder(blob);
  if (!decoder.has_opcode() || !decoder.has_irp_ptr()) {
    return;
  }
  const auto irp = decoder.irp_ptr();
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto file_key =
      decoder.has_file_key() ? optional(decoder.file_key()) : nullopt;
  const auto ttid = decoder.has_ttid() ? optional(decoder.ttid()) : nullopt;
  const auto extra_info =
      decoder.has_extra_info() ? optional(decoder.extra_info()) : nullopt;
  const auto info_class =
      decoder.has_info_class() ? optional(decoder.info_class()) : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, irp, file_object, file_key, ttid, extra_info,
       info_class](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(context_->storage->InternString(kIrpArg),
                         Variadic::Pointer(irp));
        if (file_object) {
          inserter->AddArg(context_->storage->InternString(kFileObjectArg),
                           Variadic::Pointer(*file_object));
        }
        if (file_key) {
          inserter->AddArg(context_->storage->InternString(kFileKeyArg),
                           Variadic::Pointer(*file_key));
        }
        if (extra_info) {
          const char* extra_info_arg = kExtraInfoArg;
          if (info_class) {
            // Replace "Extra Info" with a more specific descriptor when the
            // type of information is known, per
            // https://learn.microsoft.com/en-us/windows/win32/etw/fileio-info.
            switch (*info_class) {
              case kFileDispositionInformation:
                extra_info_arg = kDispositionArg;
                break;
              case kFileEndOfFileInformation:
              case kFileAllocationInformation:
                extra_info_arg = kFileSizeArg;
                break;
            }
          }
          inserter->AddArg(context_->storage->InternString(extra_info_arg),
                           Variadic::UnsignedInteger(*extra_info));
        }
        if (ttid) {
          inserter->AddArg(context_->storage->InternString(kThreadIdArg),
                           Variadic::UnsignedInteger(*ttid));
        }
        if (info_class) {
          // If a string version of `info_class` is known, use that as the arg.
          // Otherwise, use its numerical value.
          const char* info_class_str =
              GetFileInfoClassString(static_cast<FileInfoClass>(*info_class));
          inserter->AddArg(
              context_->storage->InternString(kInfoClassArg),
              info_class_str ? Variadic::String(context_->storage->InternString(
                                   info_class_str))
                             : Variadic::UnsignedInteger(*info_class));
        }
      };
  StartEvent(irp, {timestamp, decoder.opcode(), std::move(set_args)});
}

void FileIoTracker::ParseFileIoReadWrite(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoReadWriteEtwEvent::Decoder decoder(blob);
  if (!decoder.has_opcode() || !decoder.has_irp_ptr()) {
    return;
  }
  const auto irp = decoder.irp_ptr();
  const auto offset =
      decoder.has_offset() ? optional(decoder.offset()) : nullopt;
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto file_key =
      decoder.has_file_key() ? optional(decoder.file_key()) : nullopt;
  const auto ttid = decoder.has_ttid() ? optional(decoder.ttid()) : nullopt;
  const auto io_size =
      decoder.has_io_size() ? optional(decoder.io_size()) : nullopt;
  const auto io_flags =
      decoder.has_io_flags() ? optional(decoder.io_flags()) : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, irp, offset, file_object, file_key, ttid, io_size,
       io_flags](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(context_->storage->InternString(kIrpArg),
                         Variadic::Pointer(irp));
        if (offset) {
          inserter->AddArg(context_->storage->InternString(kOffsetArg),
                           Variadic::UnsignedInteger(*offset));
        }
        if (file_object) {
          inserter->AddArg(context_->storage->InternString(kFileObjectArg),
                           Variadic::Pointer(*file_object));
        }
        if (file_key) {
          inserter->AddArg(context_->storage->InternString(kFileKeyArg),
                           Variadic::Pointer(*file_key));
        }
        if (ttid) {
          inserter->AddArg(context_->storage->InternString(kThreadIdArg),
                           Variadic::UnsignedInteger(*ttid));
        }
        if (io_size) {
          inserter->AddArg(context_->storage->InternString(kIoSizeArg),
                           Variadic::UnsignedInteger(*io_size));
        }
        if (io_flags) {
          inserter->AddArg(context_->storage->InternString(kIoFlagsArg),
                           Variadic::Pointer(*io_flags));
        }
      };
  StartEvent(irp, {timestamp, decoder.opcode(), std::move(set_args)});
}

void FileIoTracker::ParseFileIoSimpleOp(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoSimpleOpEtwEvent::Decoder decoder(blob);
  if (!decoder.has_opcode() || !decoder.has_irp_ptr()) {
    return;
  }
  const auto irp = decoder.irp_ptr();
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto file_key =
      decoder.has_file_key() ? optional(decoder.file_key()) : nullopt;
  const auto ttid = decoder.has_ttid() ? optional(decoder.ttid()) : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, irp, file_object, file_key,
       ttid](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(context_->storage->InternString(kIrpArg),
                         Variadic::Pointer(irp));
        if (file_object) {
          inserter->AddArg(context_->storage->InternString(kFileObjectArg),
                           Variadic::Pointer(*file_object));
        }
        if (file_key) {
          inserter->AddArg(context_->storage->InternString(kFileKeyArg),
                           Variadic::Pointer(*file_key));
        }
        if (ttid) {
          inserter->AddArg(context_->storage->InternString(kThreadIdArg),
                           Variadic::UnsignedInteger(*ttid));
        }
      };
  StartEvent(irp, {timestamp, decoder.opcode(), std::move(set_args)});
}

void FileIoTracker::ParseFileIoOpEnd(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::FileIoOpEndEtwEvent::Decoder decoder(blob);
  if (!decoder.has_irp_ptr()) {
    return;
  }
  const auto extra_info =
      decoder.has_extra_info() ? optional(decoder.extra_info()) : nullopt;
  const auto nt_status =
      decoder.has_nt_status() ? optional(decoder.nt_status()) : nullopt;
  SliceTracker::SetArgsCallback set_args =
      [this, extra_info, nt_status](ArgsTracker::BoundInserter* inserter) {
        if (extra_info) {
          inserter->AddArg(context_->storage->InternString(kExtraInfoArg),
                           Variadic::UnsignedInteger(*extra_info));
        }
        if (nt_status) {
          inserter->AddArg(context_->storage->InternString(kNtStatusArg),
                           Variadic::Pointer(*nt_status));
        }
      };
  EndEvent(timestamp, decoder.irp_ptr(), std::move(set_args));
}

void FileIoTracker::StartEvent(uint64_t irp, FileIoEvent event) {
  // Store started events to be added to the UI when a matching end event is
  // parsed. Only show events with a matching end event, because some ETW events
  // can be lost/dropped if the memory buffers are too full during tracing, and
  // a start event whose end event was dropped would appear to last until the
  // end of the trace (which is misleading).
  started_events_[irp] = event;
}

void FileIoTracker::EndEvent(int64_t end_timestamp,
                             uint64_t irp,
                             SliceTracker::SetArgsCallback set_args) {
  // Retrieve the started event that corresponds to this end event.
  auto started_event_it = started_events_.find(irp);
  if (started_event_it == started_events_.end()) {
    return;
  }
  const FileIoEvent event = started_event_it->second;
  started_events_.erase(started_event_it);

  // Use the event type (e.g., "RenameFile") as the name shown for the event.
  // Events are color-coded by name.
  const char* event_type =
      GetEventTypeString(static_cast<EventType>(event.opcode));
  if (!event_type) {
    return;
  }
  const StringId name = context_->storage->InternString(event_type);

  // The value of the "Category" field for file I/O events.
  static constexpr char kCategory[] = "ETW File I/O";
  const auto category = context_->storage->InternString(kCategory);

  // Display file I/O events in a single row titled `kCategory` under the IO
  // header (per the schema for type "etw_fileio" in `slice_tracks.ts`).
  static const auto kBlueprint = TrackCompressor::SliceBlueprint(
      "etw_fileio", tracks::DimensionBlueprints(),
      tracks::StaticNameBlueprint(kCategory));

  // `track_id` controls the row the events appear in. This must be created via
  // `TrackCompressor` because slices may be partially overlapping, which is not
  // supported in `SliceTracker`.
  const auto track_id = context_->track_compressor->InternScoped(
      kBlueprint, tracks::Dimensions(), event.timestamp,
      end_timestamp - event.timestamp);
  SliceTracker::SetArgsCallback set_all_args =
      [set_start_args = std::move(event.set_args),
       set_end_args =
           std::move(set_args)](ArgsTracker::BoundInserter* inserter) {
        set_start_args(inserter);
        set_end_args(inserter);
      };
  context_->slice_tracker->Scoped(event.timestamp, track_id, category, name,
                                  end_timestamp - event.timestamp,
                                  std::move(set_all_args));
}

}  // namespace perfetto::trace_processor
