#include "cachex/line_buffer.hpp"

#include <string>

#include "test_framework.hpp"

// These tests are the whole reason framing lives in its own class: every
// awkward case a real socket produces at random can be reproduced here exactly,
// by choosing where to split the input.

CACHEX_TEST(empty_buffer_yields_no_line) {
  cachex::LineBuffer buffer;
  CHECK(!buffer.next_line().has_value());
  CHECK(buffer.empty());
}

CACHEX_TEST(incomplete_line_is_withheld_until_the_newline_arrives) {
  cachex::LineBuffer buffer;
  buffer.append("GET fo");

  CHECK(!buffer.next_line().has_value());
  CHECK_EQ(buffer.buffered(), 6u);

  buffer.append("o\n");
  CHECK_EQ(buffer.next_line().value_or(""), "GET foo");
  CHECK_EQ(buffer.buffered(), 0u);
}

// The case that breaks naive servers: one recv() delivers several commands.
// Serving only the first would leave the rest sitting in the buffer while both
// sides waited for the other.
CACHEX_TEST(one_read_can_contain_several_lines) {
  cachex::LineBuffer buffer;
  buffer.append("SET a 1\nSET b 2\nGET a\n");

  CHECK_EQ(buffer.next_line().value_or(""), "SET a 1");
  CHECK_EQ(buffer.next_line().value_or(""), "SET b 2");
  CHECK_EQ(buffer.next_line().value_or(""), "GET a");
  CHECK(!buffer.next_line().has_value());
}

CACHEX_TEST(a_line_split_across_many_reads_is_reassembled) {
  cachex::LineBuffer buffer;
  const std::string command = "SET key value\n";
  // One byte at a time -- the pathological case, and entirely legal for TCP.
  for (const char c : command) {
    CHECK(!buffer.next_line().has_value() || false);
    buffer.append(&c, 1);
  }
  CHECK_EQ(buffer.next_line().value_or(""), "SET key value");
}

CACHEX_TEST(trailing_partial_line_is_kept_for_next_time) {
  cachex::LineBuffer buffer;
  buffer.append("GET a\nGET b");

  CHECK_EQ(buffer.next_line().value_or(""), "GET a");
  CHECK(!buffer.next_line().has_value());
  CHECK_EQ(buffer.buffered(), 5u);  // "GET b" is still waiting

  buffer.append("c\n");
  CHECK_EQ(buffer.next_line().value_or(""), "GET bc");
}

CACHEX_TEST(crlf_is_accepted_and_stripped) {
  cachex::LineBuffer buffer;
  buffer.append("PING\r\nGET a\r\n");

  CHECK_EQ(buffer.next_line().value_or("x"), "PING");
  CHECK_EQ(buffer.next_line().value_or("x"), "GET a");
}

CACHEX_TEST(lf_and_crlf_can_be_mixed_on_one_connection) {
  cachex::LineBuffer buffer;
  buffer.append("PING\nPING\r\nPING\n");

  CHECK_EQ(buffer.next_line().value_or("x"), "PING");
  CHECK_EQ(buffer.next_line().value_or("x"), "PING");
  CHECK_EQ(buffer.next_line().value_or("x"), "PING");
}

CACHEX_TEST(a_cr_split_from_its_lf_still_works) {
  cachex::LineBuffer buffer;
  buffer.append("PING\r");
  CHECK(!buffer.next_line().has_value());  // "\r" alone does not terminate

  buffer.append("\n");
  CHECK_EQ(buffer.next_line().value_or("x"), "PING");
}

CACHEX_TEST(empty_lines_are_real_lines) {
  cachex::LineBuffer buffer;
  buffer.append("\n\n");

  // Two blank lines, not one and not zero. The server turns these into
  // "-ERR empty command" rather than silently ignoring them.
  const auto first = buffer.next_line();
  const auto second = buffer.next_line();
  CHECK(first.has_value());
  CHECK(second.has_value());
  CHECK_EQ(first.value_or("x"), "");
  CHECK_EQ(second.value_or("x"), "");
  CHECK(!buffer.next_line().has_value());
}

CACHEX_TEST(a_lone_cr_is_not_a_terminator) {
  cachex::LineBuffer buffer;
  buffer.append("A\rB\n");
  // Only '\n' ends a line; an interior '\r' is ordinary data.
  CHECK_EQ(buffer.next_line().value_or("x"), "A\rB");
}

CACHEX_TEST(buffered_reports_bytes_not_yet_forming_a_line) {
  cachex::LineBuffer buffer;
  buffer.append("abc");
  CHECK_EQ(buffer.buffered(), 3u);
  buffer.append("de\nxy");
  // "abcde\n" is consumed; "xy" remains.
  CHECK_EQ(buffer.next_line().value_or(""), "abcde");
  CHECK_EQ(buffer.buffered(), 2u);
}

CACHEX_TEST(clear_discards_everything) {
  cachex::LineBuffer buffer;
  buffer.append("partial");
  buffer.clear();
  CHECK(buffer.empty());
  CHECK_EQ(buffer.buffered(), 0u);
}

CACHEX_TEST(a_very_long_line_accumulates_so_the_caller_can_enforce_a_limit) {
  cachex::LineBuffer buffer;
  const std::string chunk(1024, 'x');
  for (int i = 0; i < 100; ++i) {
    buffer.append(chunk);
    CHECK(!buffer.next_line().has_value());
  }
  // The buffer does not police this itself; it reports the size so the
  // connection can decide. That keeps policy out of the framing code.
  CHECK_EQ(buffer.buffered(), 102400u);
}
