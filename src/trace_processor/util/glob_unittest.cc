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

#include "src/trace_processor/util/glob.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

TEST(GlobUnittest, EmptyPattern) {
  GlobMatcher matcher = GlobMatcher::FromPattern("");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches(""));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("A"));
  ASSERT_FALSE(matcher.Matches("AXBC"));
  ASSERT_FALSE(matcher.Matches("ABXC"));
}

TEST(GlobUnittest, JustStar) {
  GlobMatcher matcher = GlobMatcher::FromPattern("*");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches(""));
  ASSERT_TRUE(matcher.Matches("A"));
  ASSERT_TRUE(matcher.Matches("ABCD"));
}

TEST(GlobUnittest, NoStars) {
  GlobMatcher matcher = GlobMatcher::FromPattern("ABC");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABC"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AXBC"));
  ASSERT_FALSE(matcher.Matches("ABXC"));
  ASSERT_FALSE(matcher.Matches("ABABABBC"));
  ASSERT_FALSE(matcher.Matches("AAAAAAABABABBC"));
  ASSERT_FALSE(matcher.Matches("ABCD"));
  ASSERT_FALSE(matcher.Matches("ABBBBBB"));
  ASSERT_FALSE(matcher.Matches("BCA"));
}

TEST(GlobUnittest, InteriorOnly) {
  GlobMatcher matcher = GlobMatcher::FromPattern("A*B*C");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABC"));
  ASSERT_TRUE(matcher.Matches("AXBC"));
  ASSERT_TRUE(matcher.Matches("ABXC"));
  ASSERT_TRUE(matcher.Matches("ABABABBC"));
  ASSERT_TRUE(matcher.Matches("AAAAAAABABABBC"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABCD"));
  ASSERT_FALSE(matcher.Matches("ABBBBBB"));
  ASSERT_FALSE(matcher.Matches("BCA"));
}

TEST(GlobUnittest, ComplexInterior) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB*CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABCAB"));
  ASSERT_TRUE(matcher.Matches("ABCCAB"));
  ASSERT_TRUE(matcher.Matches("ABCABCAB"));
  ASSERT_TRUE(matcher.Matches("ABCABCABCABABABCAB"));
  ASSERT_TRUE(matcher.Matches("ABXCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABXCABCABCA"));
  ASSERT_FALSE(matcher.Matches("ABXCABCABAB"));
  ASSERT_FALSE(matcher.Matches("ABXCABCABCB"));
}

TEST(GlobUnittest, LeadingAndTrailing) {
  GlobMatcher matcher = GlobMatcher::FromPattern("*BC*");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABC"));
  ASSERT_TRUE(matcher.Matches("ABABABBC"));
  ASSERT_TRUE(matcher.Matches("AAAAAAABABABBC"));
  ASSERT_TRUE(matcher.Matches("ABCD"));
  ASSERT_TRUE(matcher.Matches("BCA"));
  ASSERT_TRUE(matcher.Matches("AXBC"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABXC"));
  ASSERT_FALSE(matcher.Matches("ABBBBBB"));
}

TEST(GlobUnittest, Leading) {
  GlobMatcher matcher = GlobMatcher::FromPattern("*BC");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABC"));
  ASSERT_TRUE(matcher.Matches("AAAAAAABABABBC"));
  ASSERT_TRUE(matcher.Matches("ABABABBC"));
  ASSERT_TRUE(matcher.Matches("AXBC"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABXC"));
  ASSERT_FALSE(matcher.Matches("ABCD"));
  ASSERT_FALSE(matcher.Matches("ABBBBBB"));
  ASSERT_FALSE(matcher.Matches("BCA"));
}

TEST(GlobUnittest, Trailing) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB*");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABC"));
  ASSERT_TRUE(matcher.Matches("ABXC"));
  ASSERT_TRUE(matcher.Matches("ABABABBC"));
  ASSERT_TRUE(matcher.Matches("ABCD"));
  ASSERT_TRUE(matcher.Matches("ABBBBBB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AAAAAAABABABBC"));
  ASSERT_FALSE(matcher.Matches("AXBC"));
  ASSERT_FALSE(matcher.Matches("BCA"));
}

TEST(GlobUnittest, QuestionMarks) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB?*CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABCCAB"));
  ASSERT_TRUE(matcher.Matches("ABDCAB"));
  ASSERT_TRUE(matcher.Matches("ABCABDDDDDCAB"));
  ASSERT_TRUE(matcher.Matches("ABXCABCAB"));
  ASSERT_TRUE(matcher.Matches("ABXCABCABCABABABCAB"));
  ASSERT_TRUE(matcher.Matches("ABCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABXCA"));
  ASSERT_FALSE(matcher.Matches("ABXCABCABCA"));
}

TEST(GlobUnittest, CharacterClassRange) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[a-zA-Z]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABaCAB"));
  ASSERT_TRUE(matcher.Matches("ABcCAB"));
  ASSERT_TRUE(matcher.Matches("ABzCAB"));
  ASSERT_TRUE(matcher.Matches("ABACAB"));
  ASSERT_TRUE(matcher.Matches("ABDCAB"));
  ASSERT_TRUE(matcher.Matches("ABZCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AB1CAB"));
  ASSERT_FALSE(matcher.Matches("ABaaCAB"));
  ASSERT_FALSE(matcher.Matches("ABaACAB"));
  ASSERT_FALSE(matcher.Matches("AB-CAB"));
}

TEST(GlobUnittest, CharacterClassNormal) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[abcAZe]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABaCAB"));
  ASSERT_TRUE(matcher.Matches("ABcCAB"));
  ASSERT_TRUE(matcher.Matches("ABACAB"));
  ASSERT_TRUE(matcher.Matches("ABZCAB"));
  ASSERT_TRUE(matcher.Matches("ABeCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABBCAB"));
  ASSERT_FALSE(matcher.Matches("ABCCAB"));
  ASSERT_FALSE(matcher.Matches("ABCABaCAB"));
}

TEST(GlobUnittest, CharacterClassMultiple) {
  GlobMatcher matcher = GlobMatcher::FromPattern("*[rR][eE][nN]*");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("renderScreenImplLock"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("updateVrFlinger"));
  ASSERT_FALSE(matcher.Matches("waitForever"));
}

TEST(GlobUnittest, CharacterClassMixed) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[abcf-zA-DEFG-Z]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABaCAB"));
  ASSERT_TRUE(matcher.Matches("ABbCAB"));
  ASSERT_TRUE(matcher.Matches("ABhCAB"));
  ASSERT_TRUE(matcher.Matches("ABACAB"));
  ASSERT_TRUE(matcher.Matches("ABHCAB"));
  ASSERT_TRUE(matcher.Matches("ABZCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABeCAB"));
}

TEST(GlobUnittest, CharacterClassInvert) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[^a-zA]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABHCAB"));
  ASSERT_TRUE(matcher.Matches("ABZCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABhCAB"));
  ASSERT_FALSE(matcher.Matches("ABaCAB"));
  ASSERT_FALSE(matcher.Matches("ABbCAB"));
  ASSERT_FALSE(matcher.Matches("ABACAB"));
}

TEST(GlobUnittest, CharacterClassNestedDash) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[-]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("AB-CAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AB]CAB"));
}

TEST(GlobUnittest, CharacterClassNestedOpenSquare) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[[]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("AB[CAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AB]CAB"));
}

TEST(GlobUnittest, CharacterClassNestedClosedSquare) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB[]]CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("AB]CAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("AB[CAB"));
}

TEST(GlobUnittest, Complex) {
  GlobMatcher matcher = GlobMatcher::FromPattern("AB*[C-D]?*F*CAB");

  // Matching patterns.
  ASSERT_TRUE(matcher.Matches("ABDDDDDDCIFJKNFCAB"));

  // Non-matching patterns.
  ASSERT_FALSE(matcher.Matches("ABDDDDDDCIFJKNFAB"));
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
