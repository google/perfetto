/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/util/zip_reader.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {
using base::gtest_matchers::IsError;

// This zip file contains the following:
// Zip file size: 386 bytes, number of entries: 2
// -rw-r--r--  3.0 unx        4 tx stor 22-Jul-25 16:43 stored_file
// -rw-r--r--  3.0 unx       89 tx defN 22-Jul-25 18:34 dir/deflated_file
// 2 files, 92 bytes uncompressed, 52 bytes compressed:  43.5%
//
// /stored_file      content: "foo"
// dir/deflated_file content: 2x "The quick brown fox jumps over the lazy dog\n"
const uint8_t kTestZip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x85,
    0xf9, 0x54, 0xa8, 0x65, 0x32, 0x7e, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x0b, 0x00, 0x1c, 0x00, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64,
    0x5f, 0x66, 0x69, 0x6c, 0x65, 0x55, 0x54, 0x09, 0x00, 0x03, 0x17, 0xba,
    0xde, 0x62, 0x44, 0xba, 0xde, 0x62, 0x75, 0x78, 0x0b, 0x00, 0x01, 0x04,
    0xce, 0x69, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x66, 0x6f, 0x6f,
    0x0a, 0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x47,
    0x94, 0xf9, 0x54, 0xf2, 0x03, 0x92, 0x3c, 0x34, 0x00, 0x00, 0x00, 0x59,
    0x00, 0x00, 0x00, 0x11, 0x00, 0x1c, 0x00, 0x64, 0x69, 0x72, 0x2f, 0x64,
    0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x5f, 0x66, 0x69, 0x6c, 0x65,
    0x55, 0x54, 0x09, 0x00, 0x03, 0x15, 0xd4, 0xde, 0x62, 0xf4, 0xba, 0xde,
    0x62, 0x75, 0x78, 0x0b, 0x00, 0x01, 0x04, 0xce, 0x69, 0x02, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c,
    0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50,
    0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28,
    0x01, 0x4a, 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0x73, 0x85,
    0x10, 0xa9, 0x36, 0xad, 0x08, 0xa8, 0x18, 0x00, 0x50, 0x4b, 0x01, 0x02,
    0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x85, 0xf9, 0x54,
    0xa8, 0x65, 0x32, 0x7e, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64,
    0x5f, 0x66, 0x69, 0x6c, 0x65, 0x55, 0x54, 0x05, 0x00, 0x03, 0x17, 0xba,
    0xde, 0x62, 0x75, 0x78, 0x0b, 0x00, 0x01, 0x04, 0xce, 0x69, 0x02, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x14,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x47, 0x94, 0xf9, 0x54, 0xf2, 0x03, 0x92,
    0x3c, 0x34, 0x00, 0x00, 0x00, 0x59, 0x00, 0x00, 0x00, 0x11, 0x00, 0x18,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x49,
    0x00, 0x00, 0x00, 0x64, 0x69, 0x72, 0x2f, 0x64, 0x65, 0x66, 0x6c, 0x61,
    0x74, 0x65, 0x64, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x55, 0x54, 0x05, 0x00,
    0x03, 0x15, 0xd4, 0xde, 0x62, 0x75, 0x78, 0x0b, 0x00, 0x01, 0x04, 0xce,
    0x69, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0xa8, 0x00, 0x00, 0x00,
    0xc8, 0x00, 0x00, 0x00, 0x00, 0x00};

// Same logical contents as kTestZip, but written as a zip64 archive: the local
// file headers use version 4.5 (45), set the 32-bit size fields to the
// 0xFFFFFFFF sentinel and carry the real sizes in a Zip64 Extended Information
// extra field (header id 0x0001).
// stored_file       content: "foo\n"
// dir/deflated_file content: 2x "The quick brown fox jumps over the lazy dog\n"
const uint8_t kTestZip64[] = {
    0x50, 0x4b, 0x03, 0x04, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x85,
    0xf9, 0x54, 0xa8, 0x65, 0x32, 0x7e, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x0b, 0x00, 0x14, 0x00, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64,
    0x5f, 0x66, 0x69, 0x6c, 0x65, 0x01, 0x00, 0x10, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x66, 0x6f, 0x6f, 0x0a, 0x50, 0x4b, 0x03, 0x04, 0x2d, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x47, 0x94, 0xf9, 0x54, 0xf2, 0x03, 0x92, 0x3c, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x11, 0x00, 0x14, 0x00, 0x64,
    0x69, 0x72, 0x2f, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x5f,
    0x66, 0x69, 0x6c, 0x65, 0x01, 0x00, 0x10, 0x00, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a,
    0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d,
    0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a, 0xe7, 0x24,
    0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0x73, 0x85, 0x10, 0xa9, 0x36, 0xad,
    0x08, 0xa8, 0x18, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x2d, 0x03, 0x2d, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6a, 0x85, 0xf9, 0x54, 0xa8, 0x65, 0x32, 0x7e,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x5f, 0x66, 0x69, 0x6c,
    0x65, 0x01, 0x00, 0x10, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x01,
    0x02, 0x2d, 0x03, 0x2d, 0x00, 0x00, 0x00, 0x08, 0x00, 0x47, 0x94, 0xf9,
    0x54, 0xf2, 0x03, 0x92, 0x3c, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x11, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x01, 0xff, 0xff, 0xff, 0xff, 0x64, 0x69, 0x72, 0x2f, 0x64,
    0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x5f, 0x66, 0x69, 0x6c, 0x65,
    0x01, 0x00, 0x18, 0x00, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x06, 0x06, 0x2c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x4b, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x60, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0xa8, 0x00, 0x00, 0x00,
    0xb8, 0x00, 0x00, 0x00, 0x00, 0x00};

// A zip64 archive written to a non-seekable stream: the single deflated entry
// uses general-purpose flag bit 3 (data descriptor), so the local header sizes
// are placeholders and the real 8-byte compressed/uncompressed sizes live in a
// trailing zip64 data descriptor.
// dir/deflated_file content: 2x "The quick brown fox jumps over the lazy dog\n"
const uint8_t kTestZip64DataDescriptor[] = {
    0x50, 0x4b, 0x03, 0x04, 0x2d, 0x00, 0x08, 0x00, 0x08, 0x00, 0x47, 0x94,
    0xf9, 0x54, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x11, 0x00, 0x14, 0x00, 0x64, 0x69, 0x72, 0x2f, 0x64, 0x65,
    0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x01,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0xc9, 0x48, 0x55, 0x28,
    0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48,
    0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b,
    0x2d, 0x52, 0x28, 0xc9, 0x48, 0x55, 0xc8, 0x49, 0xac, 0xaa, 0x54, 0x48,
    0xc9, 0x4f, 0xe7, 0x0a, 0x21, 0x52, 0x6d, 0x5a, 0x51, 0x7e, 0x3a, 0x17,
    0x00, 0x50, 0x4b, 0x07, 0x08, 0xf2, 0x03, 0x92, 0x3c, 0x36, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x50, 0x4b, 0x01, 0x02, 0x2d, 0x03, 0x2d, 0x00, 0x08, 0x00, 0x08,
    0x00, 0x47, 0x94, 0xf9, 0x54, 0xf2, 0x03, 0x92, 0x3c, 0x36, 0x00, 0x00,
    0x00, 0x59, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x64,
    0x69, 0x72, 0x2f, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x64, 0x5f,
    0x66, 0x69, 0x6c, 0x65, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x91, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

// A single local file header whose 32-bit sizes are the 0xFFFFFFFF sentinel,
// but whose Zip64 extra field (id 0x0001) declares a length of 4 bytes: too
// short to carry the 8-byte size it promises. A conforming reader must reject
// this rather than leave the sentinel in place as a real size.
const uint8_t kTestZip64TruncatedExtraField[] = {
    0x50, 0x4b, 0x03, 0x04,  // signature
    0x2d, 0x00,              // version needed 4.5
    0x00, 0x00,              // flags
    0x00, 0x00,              // compression: store
    0x6a, 0x85, 0xf9, 0x54,  // mod time + date
    0xa8, 0x65, 0x32, 0x7e,  // crc32
    0xff, 0xff, 0xff, 0xff,  // compressed size sentinel
    0xff, 0xff, 0xff, 0xff,  // uncompressed size sentinel
    0x0b, 0x00,              // fname_len = 11
    0x08, 0x00,              // extra_field_len = 8
    0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x5f, 0x66,  // "stored_f"
    0x69, 0x6c, 0x65,                                // "ile"
    0x01, 0x00,                                      // extra id 0x0001
    0x04, 0x00,                                      // extra size = 4 (< 8)
    0x00, 0x00, 0x00, 0x00,                          // 4 bytes of payload
};

std::string vec2str(const std::vector<uint8_t>& vec) {
  return {reinterpret_cast<const char*>(vec.data()), vec.size()};
}

void ValidateTestZip(ZipReader& zr) {
  ASSERT_EQ(zr.files().size(), 2u);

  std::vector<uint8_t> dec;
  ASSERT_EQ(zr.files()[0].name(), "stored_file");
  ASSERT_EQ(zr.files()[0].GetDatetimeStr(), "2022-07-25 16:43:20");

  ASSERT_EQ(zr.files()[1].name(), "dir/deflated_file");
  ASSERT_EQ(zr.files()[1].GetDatetimeStr(), "2022-07-25 18:34:14");

  // This file is STORE-d and doesn't require any decompression.
  auto res = zr.files()[0].Decompress(&dec);
  ASSERT_TRUE(res.ok()) << res.message();
  ASSERT_EQ(dec.size(), 4u);
  ASSERT_EQ(vec2str(dec), "foo\n");

  // This file is DEFLATE-d and requires zlib.
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
  res = zr.files()[1].Decompress(&dec);
  ASSERT_TRUE(res.ok()) << res.message();
  ASSERT_EQ(dec.size(), 89u);
  ASSERT_EQ(vec2str(dec),
            "The quick brown fox jumps over the lazy dog\n"
            "The quick brown fox jumps over the lazy frog\n");
#endif
}

TEST(ZipReaderTest, ValidZip_OneShotParse) {
  ZipReader zr;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(kTestZip, sizeof(kTestZip)))));
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, ValidZip_OneByteChunks) {
  ZipReader zr;
  for (auto i : kTestZip) {
    ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(&i, 1))));
  }
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, ValidZip64_OneShotParse) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(
      TraceBlobView(TraceBlob::CopyFrom(kTestZip64, sizeof(kTestZip64)))));
  ValidateTestZip(zr);
  ASSERT_EQ(zr.files()[0].uncompressed_size(), 4u);
  ASSERT_EQ(zr.files()[0].compressed_size(), 4u);
  ASSERT_EQ(zr.files()[1].uncompressed_size(), 89u);
  ASSERT_EQ(zr.files()[1].compressed_size(), 52u);
}

TEST(ZipReaderTest, ValidZip64_OneByteChunks) {
  ZipReader zr;
  for (auto i : kTestZip64) {
    ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(&i, 1))));
  }
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, MalformedZip_InvalidSignature) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));
  content[0] = 0xff;  // Invalid signature
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip)))),
      IsError());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip64_InvalidSignature) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(kTestZip64));
  content[0] = 0xff;  // Invalid signature
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip64)))),
      IsError());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip_VersionTooHigh) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));
  content[5] = 9;  // Version: 9.0
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip)))),
      IsError());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip64_VersionTooHigh) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(kTestZip64));
  content[5] = 9;  // Version: 9.0
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip64)))),
      IsError());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, TruncatedZip) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(kTestZip, 40))));
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, TruncatedZip64) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(kTestZip64, 40))));
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip64_TruncatedExtraField) {
  ZipReader zr;
  ASSERT_THAT(zr.Parse(TraceBlobView(
                  TraceBlob::CopyFrom(kTestZip64TruncatedExtraField,
                                      sizeof(kTestZip64TruncatedExtraField)))),
              IsError());
}

TEST(ZipReaderTest, MalformedZip64_MissingExtraField) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64TruncatedExtraField)];
  memcpy(content, kTestZip64TruncatedExtraField, sizeof(content));
  content[28] = 0x00;  // extra_field_len = 0, leaving 0xFFFFFFFF sentinels
  ASSERT_THAT(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, 41))),
              IsError());
}

TEST(ZipReaderTest, MalformedZip64_OversizedExtraFieldRecord) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64TruncatedExtraField)];
  memcpy(content, kTestZip64TruncatedExtraField, sizeof(content));
  content[43] = 0x10;  // extra record size = 16, but extra_field_len = 8
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))),
      IsError());
}

TEST(ZipReaderTest, MalformedZip64_DuplicateExtraField) {
  // Local header with two 0x0001 extra fields (each 16 bytes, total 40 bytes).
  uint8_t content[101]{};
  memcpy(content, kTestZip64, 61);  // Copy 1st file header + fname + 1st extra
  content[28] = 40;                 // extra_field_len = 40 (2 * 20 bytes)
  memcpy(content + 61, kTestZip64 + 41, 20);  // 2nd duplicate 0x0001 extra
  memcpy(content + 81, kTestZip64 + 61, 4);   // "foo\n"
  ASSERT_THAT(
      ZipReader().Parse(TraceBlobView(TraceBlob::CopyFrom(content, 85))),
      IsError());
}

TEST(ZipReaderTest, ValidZip64_EmptyArchive) {
  // Empty Zip64 archive starting directly with the 98-byte Zip64 EOCD record +
  // Zip64 EOCD locator + standard EOCD from kTestZip64.
  ZipReader zr;
  const uint8_t* eocd64 = kTestZip64 + sizeof(kTestZip64) - 98;
  ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(eocd64, 98))));
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, ValidZip_EmptyArchive) {
  // Standard 22-byte empty ZIP archive (smaller than the 30-byte local file
  // header).
  ZipReader zr;
  const uint8_t* eocd = kTestZip + sizeof(kTestZip) - 22;
  ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(eocd, 22))));
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip_StoredSizeMismatch) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(content));
  content[22] = 5;  // uncompressed_size = 5 != compressed_size = 4 for stored
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))),
      IsError());
}

TEST(ZipReaderTest, MalformedZip64_CompressedSizeOverflow) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  content[8] = 8;  // compression = deflate
  // Set 64-bit compressed_size in 0x0001 extra field (offset 53..60) to
  // UINT64_MAX so start_offset + compressed_size overflows size_t.
  memset(&content[53], 0xff, 8);
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))),
      IsError());
}

TEST(ZipReaderTest, Find) {
  ZipReader zr;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(kTestZip, sizeof(kTestZip)))));
  ASSERT_EQ(zr.Find("stored_file")->name(), "stored_file");
  ASSERT_EQ(zr.Find("dir/deflated_file")->name(), "dir/deflated_file");
  ASSERT_EQ(nullptr, zr.Find("stored_f"));
  ASSERT_EQ(nullptr, zr.Find("_file*"));
  ASSERT_EQ(nullptr, zr.Find("dirz/deflated_file"));
}

TEST(ZipReaderTest, Find64) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(
      TraceBlobView(TraceBlob::CopyFrom(kTestZip64, sizeof(kTestZip64)))));
  ASSERT_EQ(zr.Find("stored_file")->name(), "stored_file");
  ASSERT_EQ(zr.Find("dir/deflated_file")->name(), "dir/deflated_file");
  ASSERT_EQ(nullptr, zr.Find("stored_f"));
  ASSERT_EQ(nullptr, zr.Find("_file*"));
  ASSERT_EQ(nullptr, zr.Find("dirz/deflated_file"));
}

// All the tests below require zlib.
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

TEST(ZipReaderTest, ValidZip_DecompressLines) {
  ZipReader zr;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(kTestZip, sizeof(kTestZip)))));
  ValidateTestZip(zr);
  int num_callbacks = 0;
  zr.files()[1].DecompressLines(
      [&](const std::vector<base::StringView>& lines) {
        ASSERT_EQ(num_callbacks++, 0);
        ASSERT_TRUE(lines.size() == 2);
        ASSERT_EQ(lines[0].ToStdString(),
                  "The quick brown fox jumps over the lazy dog");
        ASSERT_EQ(lines[1].ToStdString(),
                  "The quick brown fox jumps over the lazy frog");
      });

  ASSERT_EQ(num_callbacks, 1);
}

TEST(ZipReaderTest, ValidZip64_DecompressLines) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(
      TraceBlobView(TraceBlob::CopyFrom(kTestZip64, sizeof(kTestZip64)))));
  ValidateTestZip(zr);
  int num_callbacks = 0;
  zr.files()[1].DecompressLines(
      [&](const std::vector<base::StringView>& lines) {
        ASSERT_EQ(num_callbacks++, 0);
        ASSERT_TRUE(lines.size() == 2);
        ASSERT_EQ(lines[0].ToStdString(),
                  "The quick brown fox jumps over the lazy dog");
        ASSERT_EQ(lines[1].ToStdString(),
                  "The quick brown fox jumps over the lazy frog");
      });

  ASSERT_EQ(num_callbacks, 1);
}

TEST(ZipReaderTest, ValidZip64DataDescriptor) {
  ZipReader zr;
  ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(
      kTestZip64DataDescriptor, sizeof(kTestZip64DataDescriptor)))));
  ASSERT_EQ(zr.files().size(), 1u);
  ASSERT_EQ(zr.files()[0].name(), "dir/deflated_file");

  std::vector<uint8_t> dec;
  auto res = zr.files()[0].Decompress(&dec);
  ASSERT_TRUE(res.ok()) << res.message();
  ASSERT_EQ(dec.size(), 89u);
  ASSERT_EQ(vec2str(dec),
            "The quick brown fox jumps over the lazy dog\n"
            "The quick brown fox jumps over the lazy frog\n");
}

TEST(ZipReaderTest, ValidZip64DataDescriptor_OneByteChunks) {
  ZipReader zr;
  for (auto i : kTestZip64DataDescriptor) {
    ASSERT_OK(zr.Parse(TraceBlobView(TraceBlob::CopyFrom(&i, 1))));
  }
  ASSERT_EQ(zr.files().size(), 1u);

  std::vector<uint8_t> dec;
  ASSERT_OK(zr.files()[0].Decompress(&dec));
  ASSERT_EQ(dec.size(), 89u);
}

TEST(ZipReaderTest, MalformedZip64DataDescriptor_SizeMismatch) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64DataDescriptor)];
  memcpy(content, kTestZip64DataDescriptor, sizeof(content));
  content[129] = 0x99;  // Corrupt compressed_size in the 64-bit data descriptor
  ASSERT_THAT(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))),
      IsError());
}

TEST(ZipReaderTest, ValidZip64_OnlyCompressedSizeSentinel) {
  // Per APPNOTE.TXT 4.5.3, a local header 0x0001 extra field carries both
  // 8-byte uncompressed and compressed sizes even if only compressed_size in
  // the local header is 0xFFFFFFFF.
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  // 2nd file header starts at offset 65; uncompressed_size is at 65 + 22 = 87.
  // Set 32-bit uncompressed_size to 89 (non-sentinel) while compressed_size
  // remains 0xFFFFFFFF.
  content[87] = 89;
  content[88] = 0;
  content[89] = 0;
  content[90] = 0;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))));
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, ValidZip64_OnlyUncompressedSizeSentinel) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  // 2nd file header starts at offset 65; compressed_size is at 65 + 18 = 83.
  // Set 32-bit compressed_size to 52 (non-sentinel) while uncompressed_size
  // remains 0xFFFFFFFF.
  content[83] = 52;
  content[84] = 0;
  content[85] = 0;
  content[86] = 0;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))));
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, ValidZip64DataDescriptor_EmptyExtraField) {
  // libarchive streaming Zip64 emits a 4-byte 0x0001 extra field with size == 0
  // in the local header to signal that a 24-byte 64-bit data descriptor
  // follows.
  std::vector<uint8_t> content(
      kTestZip64DataDescriptor,
      kTestZip64DataDescriptor + 47);  // Header (30) + fname (17)
  content[28] = 4;                     // extra_field_len = 4
  content[29] = 0;
  content.insert(content.end(), {0x01, 0x00, 0x00, 0x00});  // id=0x0001, size=0
  content.insert(content.end(), kTestZip64DataDescriptor + 67,
                 kTestZip64DataDescriptor + sizeof(kTestZip64DataDescriptor));

  ZipReader zr;
  ASSERT_OK(zr.Parse(
      TraceBlobView(TraceBlob::CopyFrom(content.data(), content.size()))));
  ASSERT_EQ(zr.files().size(), 1u);
  std::vector<uint8_t> dec;
  ASSERT_OK(zr.files()[0].Decompress(&dec));
  ASSERT_EQ(dec.size(), 89u);
}

TEST(ZipReaderTest, MalformedZip_DecomprError) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));

  // The 2nd file header starts at 103, the payload at 30 (header) + 17 (fname)
  // bytes later. We start clobbering at offset=150, so the header is intanct
  // but decompression fails.
  memset(&content[150], 0, 40);
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip)))));
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_OK(zr.files()[0].Decompress(&ignored));
  ASSERT_THAT(zr.files()[1].Decompress(&ignored), IsError());
}

TEST(ZipReaderTest, MalformedZip64_DecomprError) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(kTestZip64));

  memset(&content[140], 0, 30);
  ASSERT_OK(zr.Parse(
      TraceBlobView(TraceBlob::CopyFrom(content, sizeof(kTestZip64)))));
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_OK(zr.files()[0].Decompress(&ignored));
  ASSERT_THAT(zr.files()[1].Decompress(&ignored), IsError());
}

TEST(ZipReaderTest, MalformedZip64_UncompressedSizeMismatchSmaller) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  // 2nd file 0x0001 uncompressed_size is at offset 116 (actual = 89).
  content[116] = 10;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))));
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_THAT(zr.files()[1].Decompress(&ignored), IsError());
}

TEST(ZipReaderTest, MalformedZip64_UncompressedSizeMismatchLarger) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  // 2nd file 0x0001 uncompressed_size is at offset 116 (actual = 89).
  content[116] = 100;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))));
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_THAT(zr.files()[1].Decompress(&ignored), IsError());
}

TEST(ZipReaderTest, MalformedZip64_CrcChecksumFailure) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip64)];
  memcpy(content, kTestZip64, sizeof(content));
  // Corrupt 2nd file's CRC32 at offset 65 + 14 = 79.
  content[79] ^= 0xff;
  ASSERT_OK(
      zr.Parse(TraceBlobView(TraceBlob::CopyFrom(content, sizeof(content)))));
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_THAT(zr.files()[1].Decompress(&ignored), IsError());
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

}  // namespace
}  // namespace perfetto::trace_processor::util
