// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <cstddef>

#include "google/protobuf/generated_message_tctable_impl.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/types/optional.h"
#include "google/protobuf/generated_message_tctable_lite_test.pb.h"
#include "google/protobuf/wire_format_lite.h"


// Must come last:
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {

namespace {

using ::testing::AllOf;
using ::testing::AnyOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::Not;
using ::testing::Optional;
using ::testing::SizeIs;

// The fast parser's dispatch table Xors two bytes of incoming data with
// the data in TcFieldData, so we reproduce that here:
TcFieldData Xor2SerializedBytes(TcFieldData tfd, const char* ptr) {
  uint64_t twobytes = 0xFF & ptr[0];
  twobytes |= (0xFF & ptr[1]) << 8;
  tfd.data ^= twobytes;
  return tfd;
}

absl::optional<const char*> fallback_ptr_received;
absl::optional<uint64_t> fallback_hasbits_received;
absl::optional<uint64_t> fallback_tag_received;
const char* FastParserGaveUp(::google::protobuf::MessageLite*, const char* ptr,
                             ::google::protobuf::internal::ParseContext*,
                             ::google::protobuf::internal::TcFieldData data,
                             const ::google::protobuf::internal::TcParseTableBase*,
                             uint64_t hasbits) {
  fallback_ptr_received = ptr;
  fallback_hasbits_received = hasbits;
  fallback_tag_received = data.tag();
  return nullptr;
}

// To test that we aren't storing too much data, we set up a fake message area
// and fill all its bytes with kDND.
constexpr char kDND = 0x5A;  // "Do Not Disturb"

// To retrieve data and see if it matches what we expect, we have this routine
// which simultaneously reads the data we want, and sets it back to what it was
// before the test, that is, to kDND.  This makes it easier to test at the end
// that all the original data is undisturbed.
template <typename T>
T ReadAndReset(char* p) {
  T result;
  memcpy(&result, p, sizeof(result));
  memset(p, kDND, sizeof(result));
  return result;
}

template <typename Proto>
void TestFastVFunction(const absl::string_view expected_function) {
  uint8_t serialize_buffer[64];

  int size = 8 * sizeof(Proto{}.field());
  auto next_i = [](uint64_t i) {
    // if i + 1 is a power of two, return that.
    // (This will also match when i == -1, but for this loop we know that will
    // not happen.)
    if ((i & (i + 1)) == 0) return i + 1;
    // otherwise, i is already a power of two, so advance to one less than the
    // next power of two.
    return i + (i - 1);
  };
  for (uint64_t i = 0; i + 1 != 0; i = next_i(i)) {
    auto serialize_ptr = WireFormatLite::WriteUInt64ToArray(
        /* field_number= */ 1, i, serialize_buffer);
    absl::string_view serialized{
        reinterpret_cast<char*>(&serialize_buffer[0]),
        static_cast<size_t>(serialize_ptr - serialize_buffer)};

    Proto msg;

    absl::optional<char> fallback_ptr_received;
    absl::optional<uint64_t> fallback_hasbits_received;
    absl::optional<uint64_t> fallback_tag_received;

    std::vector<absl::string_view> trace;
    TcParser::RunWithTrace(
        [&](auto func_name, PROTOBUF_TC_PARAM_DECL) {
          trace.push_back(func_name);
          if (func_name == "MpVarint") {
            fallback_ptr_received = *ptr;
            fallback_hasbits_received = hasbits;
            fallback_tag_received = data.tag();
          }
        },
        [&] { ASSERT_TRUE(msg.ParseFromString(serialized)); });

    SCOPED_TRACE(absl::StrCat("Trace <", testing::PrintToString(trace), ">"));

    ASSERT_THAT(trace, SizeIs(Gt(0)));

    // The fast bool parser fallsback to MiniParse for non-canonical.
    const bool is_noncanonical_bool = size == 8 && i != 0 && i != 1;

    if (is_noncanonical_bool) {
      ASSERT_THAT(trace,
                  ElementsAre(expected_function, "MiniParse", "MpVarint"));

      // If falled_into_miniparse the FastParser gave up and
      // tried to pass control to MiniParse.... which is expected anytime
      // we encounter something other than 0 or 1 encodings.  (Since
      // FastV8S1 is only used for `bool` fields.)
      ASSERT_THAT(fallback_hasbits_received, Optional(0));
      // Like the mini-parser functions, and unlike the fast-parser
      // functions, the fallback receives a ptr already incremented past
      // the tag, and receives the actual tag in the `data` parameter.
      ASSERT_THAT(fallback_ptr_received, Optional(serialized[1]));
      ASSERT_THAT(fallback_tag_received, Optional(0x7F & serialized[0]));
    } else {
      ASSERT_THAT(trace, ElementsAre(expected_function));

      EXPECT_TRUE(!fallback_ptr_received);
      EXPECT_TRUE(!fallback_hasbits_received);
      EXPECT_TRUE(!fallback_tag_received);
    }

    auto actual_field = msg.field();
    EXPECT_EQ(actual_field, static_cast<decltype(actual_field)>(i))
        << " hex: " << absl::StrCat(absl::Hex(actual_field));
    EXPECT_TRUE(msg.has_field());
  }
}

class FastVarintTest : public ::testing::Test {
 public:
  void SetUp() override {
#if !defined(PROTOBUF_TAIL_CALL_TRACING_ENABLED)
    GTEST_SKIP() << "Tracing disabled";
#endif
  }
};

TEST_F(FastVarintTest, FastV8S1) {
  TestFastVFunction<protobuf::internal::FastV8S1Proto>("FastV8S1");
}
TEST_F(FastVarintTest, FastV32S1) {
  TestFastVFunction<protobuf::internal::FastV32S1Proto>("FastV32S1");
}
TEST_F(FastVarintTest, FastV64S1) {
  TestFastVFunction<protobuf::internal::FastV64S1Proto>("FastV64S1");
}

MATCHER_P3(IsEntryForFieldNum, table, field_num, field_numbers_table,
           absl::StrCat(negation ? "isn't " : "",
                        "the field entry for field number ", field_num)) {
  if (arg == nullptr) {
    *result_listener << "which is nullptr";
    return false;
  }
  // Use the entry's index to compare field numbers.
  size_t index = static_cast<const TcParseTableBase::FieldEntry*>(arg) -
                 &table->field_entries[0];
  uint32_t actual_field_num = field_numbers_table[index];
  if (actual_field_num != field_num) {
    *result_listener << "which is the entry for " << actual_field_num;
    return false;
  }
  return true;
}

TEST(IsEntryForFieldNumTest, Matcher) {
  // clang-format off
  TcParseTable<0, 3, 0, 0, 2> table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          0,     // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF - 7,  // 7 = fields 1, 2, and 3.
          offsetof(decltype(table), field_names),
          0,           // num_field_entries
          0, 0,        // num_aux_entries, aux_offset,
          nullptr,     // default instance
          nullptr,     // fallback function
      }};
  // clang-format on
  int table_field_numbers[] = {1, 2, 3};
  table.field_lookup_table = {{65535, 65535}};

  auto& entries = table.field_entries;
  EXPECT_THAT(&entries[0], IsEntryForFieldNum(&table, 1, table_field_numbers));
  EXPECT_THAT(&entries[2], IsEntryForFieldNum(&table, 3, table_field_numbers));
  EXPECT_THAT(&entries[1],
              Not(IsEntryForFieldNum(&table, 3, table_field_numbers)));

  EXPECT_THAT(nullptr, Not(IsEntryForFieldNum(&table, 1, table_field_numbers)));
}

}  // namespace

class FindFieldEntryTest : public ::testing::Test {
 public:
  // Calls the private `FindFieldEntry` function.
  template <size_t kFastTableSizeLog2, size_t kNumEntries, size_t kNumFieldAux,
            size_t kNameTableSize, size_t kFieldLookupTableSize>
  static const TcParseTableBase::FieldEntry* FindFieldEntry(
      const TcParseTable<kFastTableSizeLog2, kNumEntries, kNumFieldAux,
                         kNameTableSize, kFieldLookupTableSize>& table,
      uint32_t tag) {
    return TcParser::FindFieldEntry(&table.header, tag);
  }

  // Calls the private `FieldName` function.
  template <size_t kFastTableSizeLog2, size_t kNumEntries, size_t kNumFieldAux,
            size_t kNameTableSize, size_t kFieldLookupTableSize>
  static absl::string_view FieldName(
      const TcParseTable<kFastTableSizeLog2, kNumEntries, kNumFieldAux,
                         kNameTableSize, kFieldLookupTableSize>& table,
      const TcParseTableBase::FieldEntry* entry) {
    return TcParser::FieldName(&table.header, entry);
  }

  // Calls the private `MessageName` function.
  template <size_t kFastTableSizeLog2, size_t kNumEntries, size_t kNumFieldAux,
            size_t kNameTableSize, size_t kFieldLookupTableSize>
  static absl::string_view MessageName(
      const TcParseTable<kFastTableSizeLog2, kNumEntries, kNumFieldAux,
                         kNameTableSize, kFieldLookupTableSize>& table) {
    return TcParser::MessageName(&table.header);
  }

  // Returns the number of fields scanned during a small scan.
  static constexpr int small_scan_size() { return TcParser::kMtSmallScanSize; }
};

TEST_F(FindFieldEntryTest, SequentialFieldRange) {
  // Look up fields that are within the range of `lookup_table_offset`.
  // clang-format off
  TcParseTable<0, 5, 0, 0, 8> table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          111,   // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF - (1 << 1) - (1 << 2)   // fields 2, 3
                     - (1 << 3) - (1 << 4),  // fields 4, 5
          offsetof(decltype(table), field_entries),
          5,           // num_field_entries
          0, 0,        // num_aux_entries, aux_offset,
          nullptr,     // default instance
          {},          // fallback function
      },
      {},  // fast_entries
      // field_lookup_table for 2, 3, 4, 5, 111:
      {{
        111,      0,                  // field 111
        1,                            // 1 skip entry
        0xFFFE,   4,                  // 1 field, entry 4.
        65535, 65535,                 // end of table
      }},
  };
  // clang-format on
  int table_field_numbers[] = {2, 3, 4, 5, 111};

  for (int i : table_field_numbers) {
    EXPECT_THAT(FindFieldEntry(table, i),
                IsEntryForFieldNum(&table, i, table_field_numbers));
  }
  for (int i : {0, 1, 6, 7, 110, 112, 500000000}) {
    ABSL_LOG(WARNING) << "Field " << i;
    EXPECT_THAT(FindFieldEntry(table, i), Eq(nullptr));
  }
}

TEST_F(FindFieldEntryTest, SmallScanRange) {
  // Look up fields past `lookup_table_offset`, but before binary search.
  ASSERT_THAT(small_scan_size(), Eq(4)) << "test needs to be updated";
  // clang-format off
  TcParseTable<0, 6, 0, 0, 8> table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          111,   // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF - (1<<0) - (1<<2) - (1<<3) - (1<<4) - (1<<6),  // 1,3-5,7
          offsetof(decltype(table), field_entries),
          6,           // num_field_entries
          0, 0,        // num_aux_entries, aux_offset,
          nullptr,     // default instance
          {},          // fallback function
      },
      {},  // fast_entries
      // field_lookup_table for 1, 3, 4, 5, 7, 111:
      {{
        111, 0,                                              // field 111
        1,                                                   // 1 skip entry
        0xFFFE, 5,                                           // 1 field, entry 5
        65535, 65535                                         // end of table
      }},
  };
  // clang-format on
  int table_field_numbers[] = {// Sequential entries:
                               1,
                               // Small scan range:
                               3, 4, 5, 7,
                               // Binary search range:
                               111};

  for (int i : table_field_numbers) {
    EXPECT_THAT(FindFieldEntry(table, i),
                IsEntryForFieldNum(&table, i, table_field_numbers));
  }
  for (int i : {0, 2, 6, 8, 9, 110, 112, 500000000}) {
    EXPECT_THAT(FindFieldEntry(table, i), Eq(nullptr));
  }
}

TEST_F(FindFieldEntryTest, BinarySearchRange) {
  // Fields after the sequential and small-scan ranges are looked up using
  // binary search.
  ASSERT_THAT(small_scan_size(), Eq(4)) << "test needs to be updated";

  // clang-format off
  TcParseTable<0, 10, 0, 0, 8> table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          70,    // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF - (1<<0) - (1<<2) - (1<<3) - (1<<4)   // 1, 3, 4, 5, 6
                     - (1<<5) - (1<<7) - (1<<8) - (1<<10)  // 8, 9, 11, 12
                     - (1<<11),
          offsetof(decltype(table), field_entries),
          10,          // num_field_entries
          0, 0,        // num_aux_entries, aux_offset,
          nullptr,     // default instance
          {},          // fallback function
      },
      {},  // fast_entries
      // field_lookup_table for 1, 3, 4, 5, 6, 8, 9, 11, 12, 70
      {{
        70, 0,                                              // field 70
        1,                                                  // 1 skip entry
        0xFFFE, 9,                                          // 1 field, entry 9
        65535, 65535                                        // end of table
      }},
  };
  int table_field_numbers[] = {
        // Sequential entries:
        1,
        // Small scan range:
        3, 4, 5, 6,
        // Binary search range:
        8, 9, 11, 12, 70
  };
  // clang-format on
  for (int i : table_field_numbers) {
    EXPECT_THAT(FindFieldEntry(table, i),
                IsEntryForFieldNum(&table, i, table_field_numbers));
  }
  for (int i : {0, 2, 7, 10, 13, 69, 71, 112, 500000000}) {
    EXPECT_THAT(FindFieldEntry(table, i), Eq(nullptr));
  }
}

TEST_F(FindFieldEntryTest, OutOfRange) {
  // Look up tags that are larger than the maximum in the message.
  // clang-format off
  TcParseTable<0, 3, 0, 15, 2> table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          3,     // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF - (1<<0) - (1<<1) - (1<<2),  // fields 1, 2, 3
          offsetof(decltype(table), field_entries),
          3,           // num_field_entries
          0,           // num_aux_entries
          offsetof(decltype(table), field_names),  // no aux_entries
          nullptr,     // default instance
          {},          // fallback function
      },
      {},  // fast_entries
      {{// field lookup table
        65535, 65535                       // end of table
      }},
    {},  // "mini" table
    // auxiliary entries (none in this test)
    {{  // name lengths
        "\0\1\2\3\0\0\0\0"
          // names
        "1"
        "02"
        "003"}},
  };
  // clang-format on
  int table_field_numbers[] = {1, 2, 3};

  for (int field_num : table_field_numbers) {
    auto* entry = FindFieldEntry(table, field_num);
    EXPECT_THAT(entry,
                IsEntryForFieldNum(&table, field_num, table_field_numbers));

    absl::string_view name = FieldName(table, entry);
    EXPECT_EQ(name.length(), field_num);
    while (name[0] == '0') name.remove_prefix(1);  // strip leading zeores
    EXPECT_EQ(name, absl::StrCat(field_num));
  }
  for (int field_num : {0, 4, 112, 500000000}) {
    EXPECT_THAT(FindFieldEntry(table, field_num), Eq(nullptr));
  }
}

TEST_F(FindFieldEntryTest, EmptyMessage) {
  // Ensure that tables with no fields are handled correctly.
  using TableType = TcParseTable<0, 0, 0, 20, 2>;
  // clang-format off
  TableType table = {
      // header:
      {
          0, 0,  // has_bits_offset, extensions
          0,     // max_field_number
          0,     // fast_idx_mask,
          offsetof(decltype(table), field_lookup_table),
          0xFFFFFFFF,       // no fields
          offsetof(decltype(table), field_names),  // no field_entries
          0,           // num_field_entries
          0,           // num_aux_entries
          offsetof(TableType, field_names),
          nullptr,     // default instance
          nullptr,     // fallback function
      },
      {},  // fast_entries
      {{// empty field lookup table
        65535, 65535
      }},
      {{
          "\13\0\0\0\0\0\0\0"
          "MessageName"
      }},
  };
  // clang-format on

  for (int i : {0, 4, 112, 500000000}) {
    EXPECT_THAT(FindFieldEntry(table, i), Eq(nullptr));
  }
  EXPECT_THAT(MessageName(table), Eq("MessageName"));
}

// Make a monster with lots of field numbers

int32_t test_all_types_table_field_numbers[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,   //
    11,  12,  13,  14,  15,  18,  19,  21,  22,  24,   //
    25,  27,  31,  32,  33,  34,  35,  36,  37,  38,   //
    39,  40,  41,  42,  43,  44,  45,  48,  49,  51,   //
    52,  54,  55,  56,  57,  58,  59,  60,  61,  62,   //
    63,  64,  65,  66,  67,  68,  69,  70,  71,  72,   //
    73,  74,  75,  76,  77,  78,  79,  80,  81,  82,   //
    83,  84,  85,  86,  87,  88,  89,  90,  91,  92,   //
    93,  94,  95,  96,  97,  98,  99,  100, 101, 102,  //
    111, 112, 113, 114, 115, 116, 117, 118, 119, 201,  //
    241, 242, 243, 244, 245, 246, 247, 248, 249, 250,  //
    251, 252, 253, 254, 255, 321, 322, 401, 402, 403,  //
    404, 405, 406, 407, 408, 409, 410, 411, 412, 413,  //
    414, 415, 416, 417};

// clang-format off
const TcParseTable<5, 134, 5, 2176, 55> test_all_types_table = {
    // header:
    {
        0, 0,  // has_bits_offset, extensions
        418, 248,    // max_field_number, fast_idx_mask
        offsetof(decltype(test_all_types_table), field_lookup_table),
        977895424,  // skipmap for fields 1-15,18-19,21-22,24-25,27,31-32
        offsetof(decltype(test_all_types_table), field_entries),
        135,         // num_field_entries
        5,           // num_aux_entries
        offsetof(decltype(test_all_types_table), aux_entries),
        nullptr,     // default instance
        nullptr,     // fallback function
    },
    {{
        // tail-call table
    }},
    {{  // field lookup table
        //
        // fields 33-417, over 25 skipmap / offset pairs
      33, 0, 25,
      24576,  24,   18,     38,   0,      52,   0,      68,   16320,  84,
      65408,  92,   65535,  99,   65535,  99,   65535,  99,   65535,  99,
      65279,  99,   65535,  100,  65535,  100,  32768,  100,  65535,  115,
      65535,  115,  65535,  115,  65535,  115,  65532,  115,  65535,  117,
      65535,  117,  65535,  117,  65535,  117,  0,      117,  65532,  133,
      // end of table
      65535, 65535
  }},
  {{
      // "mini" table
  }},
  {{  // auxiliary entries (not used in this test)
      {-1, 4},
      {-1, 4},
      {-1, 4},
      {-1, 4},
      {-1, 4},
    }}, {{  // name lengths
      "\1"  // message name
      "\16\16\17\17\17\17\20\20\21\21\16\17\15\17\16\27\30\24\25\25"
      "\15\21\16\16\17\17\17\17\20\20\21\21\16\17\15\17\16\27\30\24"
      "\25\25\15\17\17\21\21\21\21\23\23\25\25\17\20\15\21\20\31\32"
      "\26\27\14\14\15\15\15\15\16\16\17\17\14\15\13\22\16\16\17\17"
      "\17\17\20\20\21\21\16\17\15\24\14\24\14\13\12\14\13\14\12\4"
      "\15\15\16\16\16\16\17\17\20\20\15\16\14\16\15\25\25\12\13\14"
      "\15\13\15\12\12\13\14\14\14\16\16\15\15\16\0"
        // names
      "M"
      "optional_int32"
      "optional_int64"
      "optional_uint32"
      "optional_uint64"
      "optional_sint32"
      "optional_sint64"
      "optional_fixed32"
      "optional_fixed64"
      "optional_sfixed32"
      "optional_sfixed64"
      "optional_float"
      "optional_double"
      "optional_bool"
      "optional_string"
      "optional_bytes"
      "optional_nested_message"
      "optional_foreign_message"
      "optional_nested_enum"
      "optional_foreign_enum"
      "optional_string_piece"
      "optional_cord"
      "recursive_message"
      "repeated_int32"
      "repeated_int64"
      "repeated_uint32"
      "repeated_uint64"
      "repeated_sint32"
      "repeated_sint64"
      "repeated_fixed32"
      "repeated_fixed64"
      "repeated_sfixed32"
      "repeated_sfixed64"
      "repeated_float"
      "repeated_double"
      "repeated_bool"
      "repeated_string"
      "repeated_bytes"
      "repeated_nested_message"
      "repeated_foreign_message"
      "repeated_nested_enum"
      "repeated_foreign_enum"
      "repeated_string_piece"
      "repeated_cord"
      "map_int32_int32"
      "map_int64_int64"
      "map_uint32_uint32"
      "map_uint64_uint64"
      "map_sint32_sint32"
      "map_sint64_sint64"
      "map_fixed32_fixed32"
      "map_fixed64_fixed64"
      "map_sfixed32_sfixed32"
      "map_sfixed64_sfixed64"
      "map_int32_float"
      "map_int32_double"
      "map_bool_bool"
      "map_string_string"
      "map_string_bytes"
      "map_string_nested_message"
      "map_string_foreign_message"
      "map_string_nested_enum"
      "map_string_foreign_enum"
      "packed_int32"
      "packed_int64"
      "packed_uint32"
      "packed_uint64"
      "packed_sint32"
      "packed_sint64"
      "packed_fixed32"
      "packed_fixed64"
      "packed_sfixed32"
      "packed_sfixed64"
      "packed_float"
      "packed_double"
      "packed_bool"
      "packed_nested_enum"
      "unpacked_int32"
      "unpacked_int64"
      "unpacked_uint32"
      "unpacked_uint64"
      "unpacked_sint32"
      "unpacked_sint64"
      "unpacked_fixed32"
      "unpacked_fixed64"
      "unpacked_sfixed32"
      "unpacked_sfixed64"
      "unpacked_float"
      "unpacked_double"
      "unpacked_bool"
      "unpacked_nested_enum"
      "oneof_uint32"
      "oneof_nested_message"
      "oneof_string"
      "oneof_bytes"
      "oneof_bool"
      "oneof_uint64"
      "oneof_float"
      "oneof_double"
      "oneof_enum"
      "data"
      "default_int32"
      "default_int64"
      "default_uint32"
      "default_uint64"
      "default_sint32"
      "default_sint64"
      "default_fixed32"
      "default_fixed64"
      "default_sfixed32"
      "default_sfixed64"
      "default_float"
      "default_double"
      "default_bool"
      "default_string"
      "default_bytes"
      "optional_lazy_message"
      "repeated_lazy_message"
      "fieldname1"
      "field_name2"
      "_field_name3"
      "field__name4_"
      "field0name5"
      "field_0_name6"
      "fieldName7"
      "FieldName8"
      "field_Name9"
      "Field_Name10"
      "FIELD_NAME11"
      "FIELD_name12"
      "__field_name13"
      "__Field_name14"
      "field__name15"
      "field__Name16"
      "field_name17__"
  }},
};
// clang-format on

TEST_F(FindFieldEntryTest, BigMessage) {
  EXPECT_THAT(MessageName(test_all_types_table), Eq("M"));
  for (int field_num :
       {1, 12, 31, 42, 57, 68, 79, 90, 101, 119, 249, 402, 412}) {
    auto* entry = FindFieldEntry(test_all_types_table, field_num);
    absl::string_view name = FieldName(test_all_types_table, entry);
    switch (field_num) {
      case 1:
        EXPECT_THAT(name, Eq("optional_int32"));
        break;
      case 12:
        EXPECT_THAT(name, Eq("optional_double"));
        break;
      case 31:
        EXPECT_THAT(name, Eq("repeated_int32"));
        break;
      case 42:
        EXPECT_THAT(name, Eq("repeated_double"));
        break;
      case 57:
        EXPECT_THAT(name, Eq("map_int64_int64"));
        break;
      case 68:
        EXPECT_THAT(name, Eq("map_bool_bool"));
        break;
      case 79:
        EXPECT_THAT(name, Eq("packed_sint32"));
        break;
      case 90:
        EXPECT_THAT(name, Eq("unpacked_int64"));
        break;
      case 101:
        EXPECT_THAT(name, Eq("unpacked_bool"));
        break;
      case 119:
        EXPECT_THAT(name, Eq("oneof_enum"));
        break;
      case 249:
        EXPECT_THAT(name, Eq("default_sfixed32"));
        break;
      case 402:
        EXPECT_THAT(name, Eq("field_name2"));
        break;
      case 412:
        EXPECT_THAT(name, Eq("FIELD_name12"));
        break;
    }
  }
}

}  // namespace internal
}  // namespace protobuf
}  // namespace google
