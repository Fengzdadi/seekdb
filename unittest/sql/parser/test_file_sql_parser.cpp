/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Grammar of file sql: read_csv / read_json / read_ndjson / read_parquet / glob table
// functions with aliases and named parameters, DESCRIBE SELECT and COPY ... TO.

#define USING_LOG_PREFIX SQL_PARSER

#include <gtest/gtest.h>

#include "lib/allocator/page_arena.h"
#include "common/sql_mode/ob_sql_mode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace file_sql_parser_test
{

class FileSqlParserTest : public ::testing::Test
{
public:
  FileSqlParserTest() : alloc_("FileSqlParserUt") {}
  virtual void TearDown() override { alloc_.reset(); }
protected:
  // Parse one statement; stmt is the statement node under T_STMT_LIST.
  int parse(const char *sql, const ParseNode *&stmt)
  {
    int ret = OB_SUCCESS;
    ObParser parser(alloc_, SMO_DEFAULT);
    ParseResult result;
    stmt = NULL;
    if (OB_SUCC(parser.parse(ObString(sql), result))) {
      if (NULL == result.result_tree_ || result.result_tree_->num_child_ < 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        stmt = result.result_tree_->children_[0];
      }
    }
    return ret;
  }
  static const ParseNode *find(const ParseNode *node, const ObItemType type)
  {
    const ParseNode *found = NULL;
    if (NULL != node) {
      if (type == node->type_) {
        found = node;
      }
      for (int64_t i = 0; NULL == found && i < node->num_child_; ++i) {
        found = find(node->children_[i], type);
      }
    }
    return found;
  }
  static int64_t count(const ParseNode *node, const ObItemType type)
  {
    int64_t n = 0;
    if (NULL != node) {
      n = type == node->type_ ? 1 : 0;
      for (int64_t i = 0; i < node->num_child_; ++i) {
        n += count(node->children_[i], type);
      }
    }
    return n;
  }
  static bool str_is(const ParseNode *node, const char *s)
  {
    return NULL != node && NULL != node->str_value_
           && ObString(node->str_len_, node->str_value_) == ObString(s);
  }
  // Check the four children of T_FILE_TABLE_EXPRESSION: name, path, options, alias.
  void expect_file_table(const char *sql, const char *func, const char *path, const char *alias,
                         const int64_t option_count)
  {
    const ParseNode *stmt = NULL;
    ASSERT_EQ(OB_SUCCESS, parse(sql, stmt)) << sql;
    ASSERT_EQ(T_SELECT, stmt->type_) << sql;
    const ParseNode *table = find(stmt, T_FILE_TABLE_EXPRESSION);
    ASSERT_TRUE(NULL != table) << sql;
    ASSERT_EQ(4, table->num_child_);
    EXPECT_TRUE(str_is(table->children_[0], func)) << sql;
    EXPECT_TRUE(str_is(table->children_[1], path)) << sql;
    if (0 == option_count) {
      EXPECT_TRUE(NULL == table->children_[2]) << sql;
    } else {
      ASSERT_TRUE(NULL != table->children_[2]) << sql;
      EXPECT_EQ(T_FILE_TABLE_OPTION_LIST, table->children_[2]->type_);
      EXPECT_EQ(option_count, table->children_[2]->num_child_) << sql;
    }
    ASSERT_TRUE(NULL != table->children_[3]) << sql;
    EXPECT_EQ(0, ObString(table->children_[3]->str_len_, table->children_[3]->str_value_)
                 .compare(alias)) << sql;
  }
  void expect_error(const char *sql)
  {
    const ParseNode *stmt = NULL;
    EXPECT_NE(OB_SUCCESS, parse(sql, stmt)) << sql;
  }
protected:
  ObArenaAllocator alloc_;
};

TEST_F(FileSqlParserTest, table_functions_and_aliases)
{
  expect_file_table("select * from read_csv('/d/a.csv')", "read_csv", "/d/a.csv", "", 0);
  expect_file_table("select * from read_csv('/d/a.csv') t", "read_csv", "/d/a.csv", "t", 0);
  expect_file_table("select t.id from read_csv('/d/a.csv') as t", "read_csv", "/d/a.csv", "t", 0);
  expect_file_table("select * from READ_CSV_AUTO('/d/a.csv') x", "READ_CSV_AUTO", "/d/a.csv", "x", 0);
  expect_file_table("select * from read_json('/d/a.jsonl') j", "read_json", "/d/a.jsonl", "j", 0);
  expect_file_table("select * from read_json_auto('/d/a.json') j", "read_json_auto", "/d/a.json", "j", 0);
  expect_file_table("select * from read_ndjson('/d/a.ndjson') j", "read_ndjson", "/d/a.ndjson", "j", 0);
  expect_file_table("select * from read_parquet('/d/a.parquet') p", "read_parquet", "/d/a.parquet", "p", 0);
  expect_file_table("select * from parquet_scan('/d/a.parquet')", "parquet_scan", "/d/a.parquet", "", 0);
  expect_file_table("select file from glob('/d/*.csv')", "glob", "/d/*.csv", "", 0);

  // file tables combine with joins, subqueries and ordinary tables
  const ParseNode *stmt = NULL;
  ASSERT_EQ(OB_SUCCESS, parse("select a.id, b.v from read_csv('/d/a.csv') a "
                              "join read_parquet('/d/b.parquet') b on a.id = b.id "
                              "left join t1 on t1.c1 = a.id "
                              "where a.id in (select id from read_json('/d/c.jsonl'))", stmt));
  EXPECT_EQ(3, count(stmt, T_FILE_TABLE_EXPRESSION));
}

TEST_F(FileSqlParserTest, named_parameters)
{
  expect_file_table("select * from read_csv('/d/a.csv', header = true, delim = ';', quote = '''', "
                    "escape = '\\\\', nullstr = 'NA', sample_size = -1, skip = 2) t",
                    "read_csv", "/d/a.csv", "t", 7);
  expect_file_table("select * from read_json('/d/a.jsonl', format = 'newline_delimited', "
                    "ignore_errors = false)", "read_json", "/d/a.jsonl", "", 2);
  const ParseNode *stmt = NULL;
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/d/a.csv', sample_size = -1, header = false)",
                              stmt));
  const ParseNode *option = find(stmt, T_FILE_TABLE_OPTION);
  ASSERT_TRUE(NULL != option);
  ASSERT_EQ(2, option->num_child_);
  EXPECT_TRUE(str_is(option->children_[0], "sample_size"));
  ASSERT_TRUE(NULL != option->children_[1]);
  EXPECT_EQ(T_INT, option->children_[1]->type_);
  EXPECT_EQ(-1, option->children_[1]->value_);
}

TEST_F(FileSqlParserTest, invalid_calls)
{
  // only the file functions are table functions, the path is a string literal and
  // further arguments must be named
  expect_error("select * from read_xml('/d/a.xml')");
  expect_error("select * from read_csv('/d/a.csv', 'xml')");
  expect_error("select * from read_csv(concat('/d', '/a.csv'))");
  expect_error("select * from read_csv()");
  expect_error("select * from read_csv('/d/a.csv', header)");
  expect_error("select * from read_csv('/d/a.csv', header = (select 1))");
  // the names stay usable as identifiers
  const ParseNode *stmt = NULL;
  EXPECT_EQ(OB_SUCCESS, parse("select glob, read_csv, copy from t1", stmt));
  EXPECT_EQ(OB_SUCCESS, parse("create table copy (glob int, read_csv int)", stmt));
  EXPECT_EQ(OB_SUCCESS, parse("select read_csv('/d/a.csv') from dual", stmt));
}

TEST_F(FileSqlParserTest, describe)
{
  const ParseNode *stmt = NULL;
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/d/a.csv')", stmt));
  EXPECT_EQ(T_FILE_SQL_DESCRIBE, stmt->type_);
  ASSERT_EQ(1, stmt->num_child_);
  EXPECT_EQ(T_SELECT, stmt->children_[0]->type_);
  ASSERT_EQ(OB_SUCCESS, parse("desc select id from read_parquet('/d/a.parquet') p", stmt));
  EXPECT_EQ(T_FILE_SQL_DESCRIBE, stmt->type_);
  // without a file table DESCRIBE keeps its meaning, EXPLAIN is never a DESCRIBE
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from t1", stmt));
  EXPECT_EQ(T_EXPLAIN, stmt->type_);
  ASSERT_EQ(OB_SUCCESS, parse("explain select * from read_csv('/d/a.csv')", stmt));
  EXPECT_EQ(T_EXPLAIN, stmt->type_);
  ASSERT_EQ(OB_SUCCESS, parse("explain basic select * from read_csv('/d/a.csv')", stmt));
  EXPECT_EQ(T_EXPLAIN, stmt->type_);
  ASSERT_EQ(OB_SUCCESS, parse("describe t1", stmt));
  EXPECT_EQ(T_SHOW_COLUMNS, stmt->type_);
}

TEST_F(FileSqlParserTest, copy_to)
{
  const ParseNode *stmt = NULL;
  ASSERT_EQ(OB_SUCCESS, parse("copy t1 to '/d/out.csv'", stmt));
  EXPECT_EQ(T_SELECT, stmt->type_);
  const ParseNode *into = find(stmt, T_COPY_INTO);
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(2, into->num_child_);
  EXPECT_TRUE(str_is(into->children_[0], "/d/out.csv"));
  EXPECT_TRUE(NULL == into->children_[1]);

  ASSERT_EQ(OB_SUCCESS, parse("copy (select id, name from read_csv('/d/a.csv') order by id) "
                              "to '/d/out.csv' (format csv, header, delimiter '|', quote '\"', "
                              "escape '\\\\', nullstr 'NULL')", stmt));
  EXPECT_EQ(1, count(stmt, T_FILE_TABLE_EXPRESSION));
  into = find(stmt, T_COPY_INTO);
  ASSERT_TRUE(NULL != into && NULL != into->children_[1]);
  EXPECT_EQ(T_COPY_OPTION_LIST, into->children_[1]->type_);
  EXPECT_EQ(6, into->children_[1]->num_child_);

  ASSERT_EQ(OB_SUCCESS, parse("copy (select 1 as a) to '/d/out.csv' with (header true, null 'N')",
                              stmt));
  into = find(stmt, T_COPY_INTO);
  ASSERT_TRUE(NULL != into && NULL != into->children_[1]);
  EXPECT_EQ(2, into->children_[1]->num_child_);
  ASSERT_EQ(OB_SUCCESS, parse("copy test.t1 to '/d/out.csv' (header = false, delimiter = ',')",
                              stmt));
  ASSERT_EQ(OB_SUCCESS, parse("copy (select * from t1 union all select * from t2) to '/d/u.csv'",
                              stmt));

  expect_error("copy t1 to out_file");
  expect_error("copy (select 1 into outfile '/d/x') to '/d/out.csv'");
  expect_error("copy t1 from '/d/in.csv'");
  expect_error("copy to '/d/out.csv'");
}

} // namespace file_sql_parser_test
} // namespace sql
} // namespace oceanbase
