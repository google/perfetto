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

#include "src/trace_processor/dataframe/type_set.h"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {
namespace {

// Basic type tags for testing
struct A {};
struct B {};
struct C {};
struct D {};
struct E {};

TEST(TypeSet, Construction) {
  // Test construction with valid types
  TypeSet<A, B, C> abc(A{});
  EXPECT_TRUE(abc.Is<A>());
  EXPECT_FALSE(abc.Is<B>());
  EXPECT_FALSE(abc.Is<C>());

  TypeSet<A, B, C> abc2(B{});
  EXPECT_FALSE(abc2.Is<A>());
  EXPECT_TRUE(abc2.Is<B>());
  EXPECT_FALSE(abc2.Is<C>());

  TypeSet<A, B, C> abc3(C{});
  EXPECT_FALSE(abc3.Is<A>());
  EXPECT_FALSE(abc3.Is<B>());
  EXPECT_TRUE(abc3.Is<C>());
}

TEST(TypeSet, Index) {
  // Check that index values are consistent
  TypeSet<A, B, C> a_set(A{});
  TypeSet<A, B, C> b_set(B{});
  TypeSet<A, B, C> c_set(C{});

  EXPECT_EQ(a_set.index(), 0u);
  EXPECT_EQ(b_set.index(), 1u);
  EXPECT_EQ(c_set.index(), 2u);

  // Check that index values are preserved for same types in different sets
  TypeSet<C, B, A> c_first(A{});
  EXPECT_EQ(c_first.index(), 2u);
}

TEST(TypeSet, IsMethod) {
  TypeSet<A, B, C> abc(B{});

  EXPECT_FALSE(abc.Is<A>());
  EXPECT_TRUE(abc.Is<B>());
  EXPECT_FALSE(abc.Is<C>());

  // Type E is not in the TypeSet, so this should not compile
  // Uncomment to test compile-time check:
  // EXPECT_FALSE(abc.Is<E>());
}

TEST(TypeSet, IsAnyOf) {
  TypeSet<A, B, C, D> abcd(B{});

  // Test with various target TypeSets
  // Use temporary variables to avoid template syntax in macros
  bool is_in_ab = abcd.IsAnyOf<TypeSet<A, B>>();
  EXPECT_TRUE(is_in_ab);

  bool is_in_bc = abcd.IsAnyOf<TypeSet<B, C>>();
  EXPECT_TRUE(is_in_bc);

  bool is_in_ac = abcd.IsAnyOf<TypeSet<A, C>>();
  EXPECT_FALSE(is_in_ac);

  bool is_in_cd = abcd.IsAnyOf<TypeSet<C, D>>();
  EXPECT_FALSE(is_in_cd);

  bool is_in_abcd = abcd.IsAnyOf<TypeSet<A, B, C, D>>();
  EXPECT_TRUE(is_in_abcd);
}

TEST(TypeSet, ImplicitUpcast) {
  // Create more specific TypeSets
  TypeSet<A, B> ab(A{});
  TypeSet<A, C> ac(C{});

  // Upcast to more general TypeSets
  auto abc1 = ab.Upcast<TypeSet<A, B, C>>();
  auto abcd1 = ab.Upcast<TypeSet<A, B, C, D>>();

  // Check that types are preserved
  EXPECT_TRUE(abc1.Is<A>());
  EXPECT_FALSE(abc1.Is<B>());
  EXPECT_FALSE(abc1.Is<C>());

  EXPECT_TRUE(abcd1.Is<A>());
  EXPECT_FALSE(abcd1.Is<B>());
  EXPECT_FALSE(abcd1.Is<C>());
  EXPECT_FALSE(abcd1.Is<D>());

  // Upcast from another TypeSet
  auto abc2 = ac.Upcast<TypeSet<A, B, C>>();
  EXPECT_FALSE(abc2.Is<A>());
  EXPECT_FALSE(abc2.Is<B>());
  EXPECT_TRUE(abc2.Is<C>());
}

TEST(TypeSet, ExplicitUpcast) {
  // Test explicit casting with operator TypeSet<...>()
  TypeSet<A, B> ab(B{});

  auto abc = ab.Upcast<TypeSet<A, B, C>>();
  EXPECT_FALSE(abc.Is<A>());
  EXPECT_TRUE(abc.Is<B>());
  EXPECT_FALSE(abc.Is<C>());

  // The following should not compile because D is not in the target set:
  // Uncomment to test compile-time check:
  // TypeSet<A, D> ad(D{});
  // auto abc_fail = ad.Upcast<TypeSet<A, B, C>>();
  // base::ignore_result(abc_fail);
}

TEST(TypeSet, TryDowncast) {
  // Create a general TypeSet
  TypeSet<A, B, C, D> abcd(B{});

  // Try valid downcasts
  auto maybe_ab = abcd.TryDowncast<TypeSet<A, B>>();
  ASSERT_TRUE(maybe_ab.has_value());
  EXPECT_TRUE(maybe_ab->Is<B>());

  auto maybe_bc = abcd.TryDowncast<TypeSet<B, C>>();
  ASSERT_TRUE(maybe_bc.has_value());
  EXPECT_TRUE(maybe_bc->Is<B>());

  // Try invalid downcasts
  auto maybe_ac = abcd.TryDowncast<TypeSet<A, C>>();
  EXPECT_FALSE(maybe_ac.has_value());

  auto maybe_cd = abcd.TryDowncast<TypeSet<C, D>>();
  EXPECT_FALSE(maybe_cd.has_value());

  // Create a different general TypeSet
  TypeSet<A, B, C, D> abcd2(D{});

  // This downcast should work
  auto maybe_cd2 = abcd2.TryDowncast<TypeSet<C, D>>();
  ASSERT_TRUE(maybe_cd2.has_value());
  EXPECT_TRUE(maybe_cd2->Is<D>());

  // This downcast should fail
  auto maybe_ab2 = abcd2.TryDowncast<TypeSet<A, B>>();
  EXPECT_FALSE(maybe_ab2.has_value());
}

TEST(TypeSet, GetTypeIndex) {
  // Check that GetTypeIndex returns correct indices
  EXPECT_EQ((TypeSet<A, B, C>::GetTypeIndex<A>()), 0u);
  EXPECT_EQ((TypeSet<A, B, C>::GetTypeIndex<B>()), 1u);
  EXPECT_EQ((TypeSet<A, B, C>::GetTypeIndex<C>()), 2u);

  // Different order of types should yield different indices
  EXPECT_EQ((TypeSet<C, B, A>::GetTypeIndex<A>()), 2u);
  EXPECT_EQ((TypeSet<C, B, A>::GetTypeIndex<B>()), 1u);
  EXPECT_EQ((TypeSet<C, B, A>::GetTypeIndex<C>()), 0u);

  // The following should not compile because D is not in the TypeSet:
  // Uncomment to test compile-time check:
  // TypeSet<A, B, C>::GetTypeIndex<D>();
}

TEST(TypeSet, VariantTypeAtIndex) {
  // Test that VariantTypeAtIndex correctly maps TypeSet indices to variant
  // types
  using AString = std::pair<A, std::string>;
  using BInt = std::pair<B, int>;
  using CDouble = std::pair<C, double>;

  using MyVariant = std::variant<AString, BInt, CDouble>;

  // Type at index 0 should be AString
  using Type0 = TypeSet<A, B, C>::VariantTypeAtIndex<A, MyVariant>;
  static_assert(std::is_same_v<Type0, AString>);

  // Type at index 1 should be BInt
  using Type1 = TypeSet<A, B, C>::VariantTypeAtIndex<B, MyVariant>;
  static_assert(std::is_same_v<Type1, BInt>);

  // Type at index 2 should be CDouble
  using Type2 = TypeSet<A, B, C>::VariantTypeAtIndex<C, MyVariant>;
  static_assert(std::is_same_v<Type2, CDouble>);

  // With a different type order
  using Type2Alt = TypeSet<C, B, A>::VariantTypeAtIndex<C, MyVariant>;
  static_assert(std::is_same_v<Type2Alt, AString>);
}

TEST(TypeSet, ContainsMethod) {
  // Test the Contains method
  static_assert(TypeSet<A, B, C>::Contains<A>());
  static_assert(TypeSet<A, B, C>::Contains<B>());
  static_assert(TypeSet<A, B, C>::Contains<C>());
  static_assert(!TypeSet<A, B, C>::Contains<D>());
  static_assert(!TypeSet<A, B, C>::Contains<E>());

  // Test with different type order
  static_assert(TypeSet<C, B, A>::Contains<A>());
  static_assert(TypeSet<C, B, A>::Contains<B>());
  static_assert(TypeSet<C, B, A>::Contains<C>());
}

TEST(TypeSet, ComplexHierarchy) {
  // Test a more complex type hierarchy

  // Create base TypeSets
  TypeSet<A, B> ab(A{});
  TypeSet<C, D> cd(C{});

  // Upcast to a unified TypeSet
  auto abcd1 = ab.Upcast<TypeSet<A, B, C, D>>();
  auto abcd2 = cd.Upcast<TypeSet<A, B, C, D>>();

  // Check types
  EXPECT_TRUE(abcd1.Is<A>());
  EXPECT_FALSE(abcd1.Is<B>());
  EXPECT_FALSE(abcd1.Is<C>());
  EXPECT_FALSE(abcd1.Is<D>());

  EXPECT_FALSE(abcd2.Is<A>());
  EXPECT_FALSE(abcd2.Is<B>());
  EXPECT_TRUE(abcd2.Is<C>());
  EXPECT_FALSE(abcd2.Is<D>());

  // Try various downcasts
  auto maybe_ab = abcd1.TryDowncast<TypeSet<A, B>>();
  ASSERT_TRUE(maybe_ab.has_value());
  EXPECT_TRUE(maybe_ab->Is<A>());

  auto maybe_cd = abcd1.TryDowncast<TypeSet<C, D>>();
  EXPECT_FALSE(maybe_cd.has_value());

  auto maybe_cd2 = abcd2.TryDowncast<TypeSet<C, D>>();
  ASSERT_TRUE(maybe_cd2.has_value());
  EXPECT_TRUE(maybe_cd2->Is<C>());

  auto maybe_ab2 = abcd2.TryDowncast<TypeSet<A, B>>();
  EXPECT_FALSE(maybe_ab2.has_value());
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
