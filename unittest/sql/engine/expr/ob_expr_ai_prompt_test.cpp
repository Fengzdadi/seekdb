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

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"


using namespace oceanbase::common;
using namespace oceanbase::sql;

class ObAIPromptTest: public ::testing::Test
{
public:
  ObAIPromptTest();
  virtual ~ObAIPromptTest();
  virtual void SetUp();
  virtual void TearDown();
private:
  // disallow copy
  ObAIPromptTest(const ObAIPromptTest &other);
  ObAIPromptTest& operator=(const ObAIPromptTest &other);
protected:
  int parse_prompt(const char *json_text, ObJsonObject *&prompt_object);
  // render the prompt object given as json text
  int render(const char *json_text, ObString &result);
  // wrap the prompt object into depth levels of {"template": "{0}", "args": [prompt_object]}
  int wrap_prompt(const int64_t depth, ObJsonObject *&prompt_object);
protected:
  // data members
  ObArenaAllocator allocator_;
};

ObAIPromptTest::ObAIPromptTest()
  : allocator_(ObModIds::TEST)
{
}

ObAIPromptTest::~ObAIPromptTest()
{
}

void ObAIPromptTest::SetUp()
{
}

void ObAIPromptTest::TearDown()
{
  allocator_.reset();
}

int ObAIPromptTest::parse_prompt(const char *json_text, ObJsonObject *&prompt_object)
{
  ObString raw_str(json_text);
  return ObAIFuncJsonUtils::get_json_object_form_str(allocator_, raw_str, prompt_object);
}

int ObAIPromptTest::render(const char *json_text, ObString &result)
{
  int ret = OB_SUCCESS;
  ObJsonObject *prompt_object = NULL;
  if (OB_FAIL(parse_prompt(json_text, prompt_object))) {
  } else {
    ret = ObAIFuncPromptObjectUtils::render_prompt(allocator_, prompt_object, result);
  }
  return ret;
}

int ObAIPromptTest::wrap_prompt(const int64_t depth, ObJsonObject *&prompt_object)
{
  int ret = OB_SUCCESS;
  ObString wrapper_template("{0}");
  for (int64_t i = 0; OB_SUCC(ret) && i < depth; i++) {
    ObJsonArray *args = NULL;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator_, args))) {
    } else if (OB_FAIL(args->append(prompt_object))) {
    } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, wrapper_template,
                                                                          args, prompt_object))) {
    }
  }
  return ret;
}

TEST_F(ObAIPromptTest, test_ob_is_vaild_prompt_object)
{
  ObArenaAllocator allocator(ObModIds::TEST);
  ObString a("a");
  ObJsonString a_str(a);
  ObString raw_str("{\"template\": \"{0}+{1}={2} 吗？请回答true或false\", \"args\": [\"1\", \"2\", \"3\"]}");
  ObJsonObject *prompt_object = NULL;
  ObString prompt_str;
  ObAIFuncJsonUtils::get_json_object_form_str(allocator, raw_str, prompt_object);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  prompt_object->add("a",&a_str);
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
}

TEST_F(ObAIPromptTest, test_replace_all_str_args_in_template)
{
  ObArenaAllocator allocator(ObModIds::TEST);
  ObString a("a");
  ObJsonString a_str(a);
  ObString raw_str("{\"template\": \"{0}+{1}={2} 吗？请回答true或false\", \"args\": [\"1\", \"2\", \"3\"]}");
  ObJsonObject *prompt_object = NULL;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncJsonUtils::get_json_object_form_str(allocator, raw_str, prompt_object);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, prompt_str));
  ob_write_string(allocator, prompt_str, result_str,true);
  ASSERT_EQ(result_str, "1+2=3 吗？请回答true或false");
}

TEST_F(ObAIPromptTest, test_is_valid_prompt_object_with_json_args)
{
  ObJsonObject *prompt_object = NULL;
  // args can be any json value
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}{1}{2}{3}{4}\", \"args\": [1, true, null, [1, 2], {\"k\": \"v\"}]}", prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"no placeholder\", \"args\": []}", prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  // template must be a string and args must be an array
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": 1, \"args\": []}", prompt_object));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}\", \"args\": \"a\"}", prompt_object));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}\"}", prompt_object));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}\", \"argv\": [\"a\"]}", prompt_object));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(NULL));
}

TEST_F(ObAIPromptTest, test_render_json_args)
{
  ObString result;
  // json args replace the placeholder with their json text
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}|{1}|{2}|{3}|{4}|{5}\", \"args\": [[1, true], {\"k\": 2}, 3.5, null, false, -7]}", result));
  ASSERT_EQ(result, "[1, true]|{\"k\": 2}|3.5|null|false|-7");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"rows: {0}\", \"args\": [[{\"id\": 1, \"tags\": [\"a\", \"b\"]}, []]]}", result));
  ASSERT_EQ(result, "rows: [{\"id\": 1, \"tags\": [\"a\", \"b\"]}, []]");
}

TEST_F(ObAIPromptTest, test_render_repeated_placeholder)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}/{0}/{1}\", \"args\": [[1, true], {\"k\": 2}]}", result));
  ASSERT_EQ(result, "[1, true]/[1, true]/{\"k\": 2}");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{1}{0}{1}{1}\", \"args\": [\"a\", \"b\"]}", result));
  ASSERT_EQ(result, "babb");
  // args that are not referenced are ignored
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{1}\", \"args\": [\"a\", \"b\", \"c\"]}", result));
  ASSERT_EQ(result, "b");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{01}\", \"args\": [\"a\", \"b\"]}", result));
  ASSERT_EQ(result, "b");
}

TEST_F(ObAIPromptTest, test_render_nested_prompt)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"Q: {0}\", \"args\": [{\"template\": \"{0} and {1}\", \"args\": [\"x\", {\"template\": \"<{0}>\", \"args\": [\"y\"]}]}]}", result));
  ASSERT_EQ(result, "Q: x and <y>");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}{0}\", \"args\": [{\"template\": \"[{0}]\", \"args\": [1]}]}", result));
  ASSERT_EQ(result, "[1][1]");
  // an object that is not a prompt object is printed as json
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}\", \"args\": [{\"template\": \"{0}\"}]}", result));
  ASSERT_EQ(result, "{\"template\": \"{0}\"}");
}

TEST_F(ObAIPromptTest, test_render_invalid_placeholder_index)
{
  ObString result;
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"{0} {1}\", \"args\": [\"a\"]}", result));
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"{0}\", \"args\": []}", result));
  // an index that overflows int64 is invalid as well
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"{18446744073709551617}\", \"args\": [\"a\"]}", result));
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"{99999999999999999999999999999}\", \"args\": [\"a\"]}", result));
  // the error of a nested prompt is returned
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"{0}\", \"args\": [{\"template\": \"{1}\", \"args\": [\"a\"]}]}", result));
}

TEST_F(ObAIPromptTest, test_render_literal_braces)
{
  ObString result;
  // only '{' digits '}' is a placeholder, other braces are kept as is
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{} {a} {-1} {0 }{ 0}{0\", \"args\": [\"x\"]}", result));
  ASSERT_EQ(result, "{} {a} {-1} {0 }{ 0}{0");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{{0}} {\\\"k\\\": {0}}\", \"args\": [\"x\"]}", result));
  ASSERT_EQ(result, "{x} {\"k\": x}");
  // a placeholder in an arg is not replaced again
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}{1}\", \"args\": [\"{1}\", \"b\"]}", result));
  ASSERT_EQ(result, "{1}b");
}

TEST_F(ObAIPromptTest, test_render_unicode_and_escape)
{
  ObString result;
  // string args are inserted without quotes or escaping, json args keep their json escaping
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"{0}|{1}|{2}|{3}|😀\", \"args\": [\"你好，世界\", \"say \\\"hi\\\"\\n\\\\\", [\"a\\\"b\"], \"\\u4e2d\"]}", result));
  ASSERT_EQ(result, "你好，世界|say \"hi\"\n\\|[\"a\\\"b\"]|中|😀");
  ASSERT_EQ(OB_SUCCESS, render("{\"template\": \"请总结：{0}\", \"args\": [\"数据库\"]}", result));
  ASSERT_EQ(result, "请总结：数据库");
}

TEST_F(ObAIPromptTest, test_render_nested_depth_limit)
{
  ObString result;
  ObJsonObject *prompt_object = NULL;
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"x\", \"args\": []}", prompt_object));
  ASSERT_EQ(OB_SUCCESS, wrap_prompt(ObAIFuncPromptObjectUtils::MAX_PROMPT_NESTED_DEPTH, prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::render_prompt(allocator_, prompt_object, result));
  ASSERT_EQ(result, "x");
  ASSERT_EQ(OB_SUCCESS, wrap_prompt(1, prompt_object));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::render_prompt(allocator_, prompt_object, result));
}

TEST_F(ObAIPromptTest, test_render_prompt_input)
{
  ObString result;
  // a json string is the prompt itself
  ObString plain("plain prompt");
  ObJsonString plain_node(plain);
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::render_prompt(allocator_, &plain_node, result));
  ASSERT_EQ(result, "plain prompt");
  // a binary json prompt object
  ObJsonObject *prompt_object = NULL;
  ObIJsonBase *j_bin = NULL;
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}-{1}\", \"args\": [\"a\", [1]]}", prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::transform(&allocator_, prompt_object, ObJsonInType::JSON_BIN, j_bin));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::render_prompt(allocator_, j_bin, result));
  ASSERT_EQ(result, "a-[1]");
  // other json values are not prompts
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::render_prompt(allocator_, NULL, result));
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"k\": 1}", prompt_object));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::render_prompt(allocator_, prompt_object, result));
  ASSERT_EQ(OB_INVALID_ARGUMENT, render("{\"template\": \"\", \"args\": []}", result));
}

TEST_F(ObAIPromptTest, test_construct_prompt_object)
{
  ObString result;
  ObString template_str("{0}?");
  ObString arg("why");
  ObString empty_template;
  ObJsonArray *args = NULL;
  ObJsonString *arg_node = NULL;
  ObJsonObject *prompt_object = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, arg, arg_node));
  ASSERT_EQ(OB_SUCCESS, args->append(arg_node));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, template_str, args, prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt_object, result));
  ASSERT_EQ(result, "why?");
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, empty_template, args, prompt_object));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, template_str, NULL, prompt_object));
}

TEST_F(ObAIPromptTest, test_get_json_object_form_str)
{
  ObJsonObject *obj = NULL;
  ASSERT_EQ(OB_SUCCESS, parse_prompt("{\"template\": \"{0}\", \"args\": [\"a\"]}", obj));
  ASSERT_TRUE(NULL != obj);
  ASSERT_EQ(OB_INVALID_ARGUMENT, parse_prompt("[\"template\", \"args\"]", obj));
  ASSERT_NE(OB_SUCCESS, parse_prompt("{\"template\": ", obj));
}
