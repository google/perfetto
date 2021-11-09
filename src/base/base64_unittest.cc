/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/base64.h"

#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

struct TestPattern {
  size_t decoded_len;
  const char* decoded;
  const char* encoded;
};

TestPattern kPatterns[] = {

    // Basic bit patterns;
    // values obtained with "echo -n '...' | uuencode -m test"

    {1, "\000", "AA=="},
    {1, "\001", "AQ=="},
    {1, "\002", "Ag=="},
    {1, "\004", "BA=="},
    {1, "\010", "CA=="},
    {1, "\020", "EA=="},
    {1, "\040", "IA=="},
    {1, "\100", "QA=="},
    {1, "\200", "gA=="},

    {1, "\377", "/w=="},
    {1, "\376", "/g=="},
    {1, "\375", "/Q=="},
    {1, "\373", "+w=="},
    {1, "\367", "9w=="},
    {1, "\357", "7w=="},
    {1, "\337", "3w=="},
    {1, "\277", "vw=="},
    {1, "\177", "fw=="},
    {2, "\000\000", "AAA="},
    {2, "\000\001", "AAE="},
    {2, "\000\002", "AAI="},
    {2, "\000\004", "AAQ="},
    {2, "\000\010", "AAg="},
    {2, "\000\020", "ABA="},
    {2, "\000\040", "ACA="},
    {2, "\000\100", "AEA="},
    {2, "\000\200", "AIA="},
    {2, "\001\000", "AQA="},
    {2, "\002\000", "AgA="},
    {2, "\004\000", "BAA="},
    {2, "\010\000", "CAA="},
    {2, "\020\000", "EAA="},
    {2, "\040\000", "IAA="},
    {2, "\100\000", "QAA="},
    {2, "\200\000", "gAA="},

    {2, "\377\377", "//8="},
    {2, "\377\376", "//4="},
    {2, "\377\375", "//0="},
    {2, "\377\373", "//s="},
    {2, "\377\367", "//c="},
    {2, "\377\357", "/+8="},
    {2, "\377\337", "/98="},
    {2, "\377\277", "/78="},
    {2, "\377\177", "/38="},
    {2, "\376\377", "/v8="},
    {2, "\375\377", "/f8="},
    {2, "\373\377", "+/8="},
    {2, "\367\377", "9/8="},
    {2, "\357\377", "7/8="},
    {2, "\337\377", "3/8="},
    {2, "\277\377", "v/8="},
    {2, "\177\377", "f/8="},

    {3, "\000\000\000", "AAAA"},
    {3, "\000\000\001", "AAAB"},
    {3, "\000\000\002", "AAAC"},
    {3, "\000\000\004", "AAAE"},
    {3, "\000\000\010", "AAAI"},
    {3, "\000\000\020", "AAAQ"},
    {3, "\000\000\040", "AAAg"},
    {3, "\000\000\100", "AABA"},
    {3, "\000\000\200", "AACA"},
    {3, "\000\001\000", "AAEA"},
    {3, "\000\002\000", "AAIA"},
    {3, "\000\004\000", "AAQA"},
    {3, "\000\010\000", "AAgA"},
    {3, "\000\020\000", "ABAA"},
    {3, "\000\040\000", "ACAA"},
    {3, "\000\100\000", "AEAA"},
    {3, "\000\200\000", "AIAA"},
    {3, "\001\000\000", "AQAA"},
    {3, "\002\000\000", "AgAA"},
    {3, "\004\000\000", "BAAA"},
    {3, "\010\000\000", "CAAA"},
    {3, "\020\000\000", "EAAA"},
    {3, "\040\000\000", "IAAA"},
    {3, "\100\000\000", "QAAA"},
    {3, "\200\000\000", "gAAA"},

    {3, "\377\377\377", "////"},
    {3, "\377\377\376", "///+"},
    {3, "\377\377\375", "///9"},
    {3, "\377\377\373", "///7"},
    {3, "\377\377\367", "///3"},
    {3, "\377\377\357", "///v"},
    {3, "\377\377\337", "///f"},
    {3, "\377\377\277", "//+/"},
    {3, "\377\377\177", "//9/"},
    {3, "\377\376\377", "//7/"},
    {3, "\377\375\377", "//3/"},
    {3, "\377\373\377", "//v/"},
    {3, "\377\367\377", "//f/"},
    {3, "\377\357\377", "/+//"},
    {3, "\377\337\377", "/9//"},
    {3, "\377\277\377", "/7//"},
    {3, "\377\177\377", "/3//"},
    {3, "\376\377\377", "/v//"},
    {3, "\375\377\377", "/f//"},
    {3, "\373\377\377", "+///"},
    {3, "\367\377\377", "9///"},
    {3, "\357\377\377", "7///"},
    {3, "\337\377\377", "3///"},
    {3, "\277\377\377", "v///"},
    {3, "\177\377\377", "f///"},

    // Random numbers: values obtained with
    //
    //  #! /bin/bash
    //  dd bs=$1 count=1 if=/dev/random of=/tmp/bar.random
    //  od -N $1 -t o1 /tmp/bar.random
    //  uuencode -m test < /tmp/bar.random
    //
    // where $1 is the number of bytes (2, 3)

    {2, "\243\361", "o/E="},
    {2, "\024\167", "FHc="},
    {2, "\313\252", "y6o="},
    {2, "\046\041", "JiE="},
    {2, "\145\236", "ZZ4="},
    {2, "\254\325", "rNU="},
    {2, "\061\330", "Mdg="},
    {2, "\245\032", "pRo="},
    {2, "\006\000", "BgA="},
    {2, "\375\131", "/Vk="},
    {2, "\303\210", "w4g="},
    {2, "\040\037", "IB8="},
    {2, "\261\372", "sfo="},
    {2, "\335\014", "3Qw="},
    {2, "\233\217", "m48="},
    {2, "\373\056", "+y4="},
    {2, "\247\232", "p5o="},
    {2, "\107\053", "Rys="},
    {2, "\204\077", "hD8="},
    {2, "\276\211", "vok="},
    {2, "\313\110", "y0g="},
    {2, "\363\376", "8/4="},
    {2, "\251\234", "qZw="},
    {2, "\103\262", "Q7I="},
    {2, "\142\312", "Yso="},
    {2, "\067\211", "N4k="},
    {2, "\220\001", "kAE="},
    {2, "\152\240", "aqA="},
    {2, "\367\061", "9zE="},
    {2, "\133\255", "W60="},
    {2, "\176\035", "fh0="},
    {2, "\032\231", "Gpk="},

    {3, "\013\007\144", "Cwdk"},
    {3, "\030\112\106", "GEpG"},
    {3, "\047\325\046", "J9Um"},
    {3, "\310\160\022", "yHAS"},
    {3, "\131\100\237", "WUCf"},
    {3, "\064\342\134", "NOJc"},
    {3, "\010\177\004", "CH8E"},
    {3, "\345\147\205", "5WeF"},
    {3, "\300\343\360", "wOPw"},
    {3, "\061\240\201", "MaCB"},
    {3, "\225\333\044", "ldsk"},
    {3, "\215\137\352", "jV/q"},
    {3, "\371\147\160", "+Wdw"},
    {3, "\030\320\051", "GNAp"},
    {3, "\044\174\241", "JHyh"},
    {3, "\260\127\037", "sFcf"},
    {3, "\111\045\033", "SSUb"},
    {3, "\202\114\107", "gkxH"},
    {3, "\057\371\042", "L/ki"},
    {3, "\223\247\244", "k6ek"},
    {3, "\047\216\144", "J45k"},
    {3, "\203\070\327", "gzjX"},
    {3, "\247\140\072", "p2A6"},
    {3, "\124\115\116", "VE1O"},
    {3, "\157\162\050", "b3Io"},
    {3, "\357\223\004", "75ME"},
    {3, "\052\117\156", "Kk9u"},
    {3, "\347\154\000", "52wA"},
    {3, "\303\012\142", "wwpi"},
    {3, "\060\035\362", "MB3y"},
    {3, "\130\226\361", "WJbx"},
    {3, "\173\013\071", "ews5"},
    {3, "\336\004\027", "3gQX"},
    {3, "\357\366\234", "7/ac"},
    {3, "\353\304\111", "68RJ"},
    {3, "\024\264\131", "FLRZ"},
    {3, "\075\114\251", "PUyp"},
    {3, "\315\031\225", "zRmV"},
    {3, "\154\201\276", "bIG+"},
    {3, "\200\066\072", "gDY6"},
    {3, "\142\350\267", "Yui3"},
    {3, "\033\000\166", "GwB2"},
    {3, "\210\055\077", "iC0/"},
    {3, "\341\037\124", "4R9U"},
    {3, "\161\103\152", "cUNq"},
    {3, "\270\142\131", "uGJZ"},
    {3, "\337\076\074", "3z48"},
    {3, "\375\106\362", "/Uby"},
    {3, "\227\301\127", "l8FX"},
    {3, "\340\002\234", "4AKc"},
    {3, "\121\064\033", "UTQb"},
    {3, "\157\134\143", "b1xj"},
    {3, "\247\055\327", "py3X"},
    {3, "\340\142\005", "4GIF"},
    {3, "\060\260\143", "MLBj"},
    {3, "\075\203\170", "PYN4"},
    {3, "\143\160\016", "Y3AO"},
    {3, "\313\013\063", "ywsz"},
    {3, "\174\236\135", "fJ5d"},
    {3, "\103\047\026", "QycW"},
    {3, "\365\005\343", "9QXj"},
    {3, "\271\160\223", "uXCT"},
    {3, "\362\255\172", "8q16"},
    {3, "\113\012\015", "SwoN"},

    // various lengths, generated by this python script:
    //
    // from string import lowercase as lc
    // for i in range(27):
    //   print '{ %2d, "%s",%s "%s" },' % (i, lc[:i], ' ' * (26-i),
    //                                     lc[:i].encode('base64').strip())

    {0, "abcdefghijklmnopqrstuvwxyz", ""},
    {1, "abcdefghijklmnopqrstuvwxyz", "YQ=="},
    {2, "abcdefghijklmnopqrstuvwxyz", "YWI="},
    {3, "abcdefghijklmnopqrstuvwxyz", "YWJj"},
    {4, "abcdefghijklmnopqrstuvwxyz", "YWJjZA=="},
    {5, "abcdefghijklmnopqrstuvwxyz", "YWJjZGU="},
    {6, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVm"},
    {7, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZw=="},
    {8, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2g="},
    {9, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hp"},
    {10, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpag=="},
    {11, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpams="},
    {12, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamts"},
    {13, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbQ=="},
    {14, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW4="},
    {15, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5v"},
    {16, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcA=="},
    {17, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHE="},
    {18, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFy"},
    {19, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFycw=="},
    {20, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3Q="},
    {21, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1"},
    {22, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1dg=="},
    {23, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnc="},
    {24, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4"},
    {25, "abcdefghijklmnopqrstuvwxy", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eQ=="},
    {26, "abcdefghijklmnopqrstuvwxyz", "YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXo="},
};

TEST(Base64Test, Encode) {
  EXPECT_EQ(Base64Encode(""), "");
  EXPECT_EQ(Base64Encode("f"), "Zg==");
  EXPECT_EQ(Base64Encode("fo"), "Zm8=");
  EXPECT_EQ(Base64Encode("foo"), "Zm9v");
  EXPECT_EQ(Base64Encode("foob"), "Zm9vYg==");
  EXPECT_EQ(Base64Encode("fooba"), "Zm9vYmE=");
  EXPECT_EQ(Base64Encode("foobar"), "Zm9vYmFy");
  EXPECT_EQ(Base64Encode("\xff"), "/w==");
  EXPECT_EQ(Base64Encode("\xff\xfe"), "//4=");
  EXPECT_EQ(Base64Encode("\xff\xfe\xfd"), "//79");
  EXPECT_EQ(Base64Encode("\xff\xfe\xfd\xfc"), "//79/A==");

  for (size_t i = 0; i < ArraySize(kPatterns); ++i) {
    const auto& p = kPatterns[i];
    std::string res = Base64Encode(StringView(p.decoded, p.decoded_len));
    EXPECT_EQ(p.encoded, res);
  }

  // Error cases
  char buf[4];
  EXPECT_EQ(0, Base64Encode("", 0, buf, 0));
  EXPECT_EQ(0, Base64Encode("", 0, buf, 1));
  EXPECT_EQ(-1, Base64Encode("a", 1, buf, 0));
  EXPECT_EQ(-1, Base64Encode("abc", 3, buf, 0));
  EXPECT_EQ(-1, Base64Encode("abc", 3, buf, 1));
  EXPECT_EQ(-1, Base64Encode("abc", 3, buf, 3));
  EXPECT_EQ(4, Base64Encode("abc", 3, buf, 4));
}

TEST(Base64Test, Decode) {
  EXPECT_EQ(Base64Decode(""), "");
  EXPECT_EQ(Base64Decode("Zg=="), "f");
  EXPECT_EQ(Base64Decode("Zg="), "f");
  EXPECT_EQ(Base64Decode("Zg"), "f");
  EXPECT_EQ(Base64Decode("Zm8="), "fo");
  EXPECT_EQ(Base64Decode("Zm8"), "fo");
  EXPECT_EQ(Base64Decode("Zm9v"), "foo");
  EXPECT_EQ(Base64Decode("Zm9vYg=="), "foob");
  EXPECT_EQ(Base64Decode("Zm9vYg="), "foob");
  EXPECT_EQ(Base64Decode("Zm9vYg"), "foob");
  EXPECT_EQ(Base64Decode("Zm9vYmE="), "fooba");
  EXPECT_EQ(Base64Decode("Zm9vYmE"), "fooba");
  EXPECT_EQ(Base64Decode("Zm9vYmFy"), "foobar");
  EXPECT_EQ(Base64Decode("/w=="), "\xff");
  EXPECT_EQ(Base64Decode("/w="), "\xff");
  EXPECT_EQ(Base64Decode("/w"), "\xff");
  EXPECT_EQ(Base64Decode("//4="), "\xff\xfe");
  EXPECT_EQ(Base64Decode("//4"), "\xff\xfe");
  EXPECT_EQ(Base64Decode("//79"), "\xff\xfe\xfd");
  EXPECT_EQ(Base64Decode("//79/A=="), "\xff\xfe\xfd\xfc");
  EXPECT_EQ(Base64Decode("//79/A="), "\xff\xfe\xfd\xfc");
  EXPECT_EQ(Base64Decode("//79/A"), "\xff\xfe\xfd\xfc");

  for (size_t i = 0; i < ArraySize(kPatterns); ++i) {
    const auto& p = kPatterns[i];
    Optional<std::string> dec = Base64Decode(StringView(p.encoded));
    EXPECT_TRUE(dec.has_value());
    EXPECT_EQ(dec.value(), StringView(p.decoded, p.decoded_len).ToStdString());
  }

  // Error cases:
  EXPECT_EQ(Base64Decode("Z"), nullopt);
  EXPECT_EQ(Base64Decode("Zm9vY"), nullopt);

  uint8_t buf[4];
  EXPECT_EQ(Base64Decode("", 0, buf, 2), 0);       // Valid, 0 len.
  EXPECT_EQ(Base64Decode("Z", 1, buf, 1), -1);     // Invalid input.
  EXPECT_EQ(Base64Decode("Zg==", 4, buf, 1), -1);  // Not enough dst space.
}

}  // namespace
}  // namespace base
}  // namespace perfetto
