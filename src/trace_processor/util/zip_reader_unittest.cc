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

#include <time.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

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

std::string vec2str(const std::vector<uint8_t>& vec) {
  return std::string(reinterpret_cast<const char*>(vec.data()), vec.size());
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
  base::Status res = zr.Parse(kTestZip, sizeof(kTestZip));
  ASSERT_TRUE(res.ok()) << res.message();
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, ValidZip_OneByteChunks) {
  ZipReader zr;
  for (size_t i = 0; i < sizeof(kTestZip); i++) {
    base::Status res = zr.Parse(&kTestZip[i], 1);
    ASSERT_TRUE(res.ok()) << res.message();
  }
  ValidateTestZip(zr);
}

TEST(ZipReaderTest, MalformedZip_InvalidSignature) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));
  content[0] = 0xff;  // Invalid signature
  base::Status res = zr.Parse(content, sizeof(kTestZip));
  ASSERT_FALSE(res.ok());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, MalformedZip_VersionTooHigh) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));
  content[5] = 9;  // Version: 9.0
  base::Status res = zr.Parse(content, sizeof(kTestZip));
  ASSERT_FALSE(res.ok());
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, TruncatedZip) {
  ZipReader zr;
  base::Status res = zr.Parse(kTestZip, 40);
  ASSERT_EQ(zr.files().size(), 0u);
}

TEST(ZipReaderTest, Find) {
  ZipReader zr;
  base::Status res = zr.Parse(kTestZip, sizeof(kTestZip));
  ASSERT_TRUE(res.ok()) << res.message();
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
  base::Status res = zr.Parse(kTestZip, sizeof(kTestZip));
  ASSERT_TRUE(res.ok()) << res.message();
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

TEST(ZipReaderTest, MalformedZip_DecomprError) {
  ZipReader zr;
  uint8_t content[sizeof(kTestZip)];
  memcpy(content, kTestZip, sizeof(kTestZip));

  // The 2nd file header starts at 103, the payload at 30 (header) + 17 (fname)
  // bytes later. We start clobbering at offset=150, so the header is intanct
  // but decompression fails.
  memset(&content[150], 0, 40);
  base::Status res = zr.Parse(content, sizeof(kTestZip));
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(zr.files().size(), 2u);
  std::vector<uint8_t> ignored;
  ASSERT_TRUE(zr.files()[0].Decompress(&ignored).ok());
  ASSERT_FALSE(zr.files()[1].Decompress(&ignored).ok());
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
