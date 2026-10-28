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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class ObOpenAIUtilsTest: public ::testing::Test
{
public:
    ObOpenAIUtilsTest();
    virtual ~ObOpenAIUtilsTest();
    virtual void SetUp();
    virtual void TearDown();
private:
    // disallow copy
    ObOpenAIUtilsTest(const ObOpenAIUtilsTest &other);
    ObOpenAIUtilsTest& operator=(const ObOpenAIUtilsTest &other);
protected:
    int parse_json(const char *json_text, ObJsonObject *&obj);
    // print a json node, a string is printed without quotes
    ObString to_string(ObIJsonBase *node);
    int parse_embed_output(ObAIFuncIEmbed &embed, const char *response_text, ObJsonArray *&embeddings);
protected:
    // data members
    ObArenaAllocator allocator_;
};

ObOpenAIUtilsTest::ObOpenAIUtilsTest()
    : allocator_(ObModIds::TEST)
{
}

ObOpenAIUtilsTest::~ObOpenAIUtilsTest()
{
}

void ObOpenAIUtilsTest::SetUp()
{
}

void ObOpenAIUtilsTest::TearDown()
{
    allocator_.reset();
}

int ObOpenAIUtilsTest::parse_json(const char *json_text, ObJsonObject *&obj)
{
    ObString raw_str(json_text);
    return ObAIFuncJsonUtils::get_json_object_form_str(allocator_, raw_str, obj);
}

ObString ObOpenAIUtilsTest::to_string(ObIJsonBase *node)
{
    ObString str;
    ObJsonBuffer j_buf(&allocator_);
    if (OB_NOT_NULL(node) && OB_SUCCESS == node->print(j_buf, false)) {
        j_buf.get_result_string(str);
    }
    return str;
}

int ObOpenAIUtilsTest::parse_embed_output(ObAIFuncIEmbed &embed, const char *response_text, ObJsonArray *&embeddings)
{
    int ret = OB_SUCCESS;
    ObJsonObject *http_response = nullptr;
    ObIJsonBase *result = nullptr;
    embeddings = nullptr;
    if (OB_FAIL(parse_json(response_text, http_response))) {
    } else if (OB_FAIL(embed.parse_output(allocator_, http_response, result))) {
    } else if (OB_ISNULL(result) || result->json_type() != ObJsonNodeType::J_ARRAY) {
        ret = OB_ERR_UNEXPECTED;
    } else {
        embeddings = static_cast<ObJsonArray *>(result);
    }
    return ret;
}

TEST_F(ObOpenAIUtilsTest, test_complete_get_header)
{
    ObArray<ObString> headers;
    ObString api_key("sk-1234567890");
    ObString authorization("Authorization: Bearer sk-1234567890");
    ObString content_type("Content-Type: application/json");
    ObArenaAllocator allocator(ObModIds::TEST);
    ObOpenAIUtils::ObOpenAIComplete completion;
    ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator, api_key, headers));
    ASSERT_EQ(2, headers.count());
    ASSERT_EQ(authorization, headers[0]);
    ASSERT_EQ(content_type, headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_complete_get_body)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString model("gpt-4o-mini");
    ObString prompt("You are a helpful assistant.");
    ObString content("oceanbase is a distributed database");
    ObString messages("[{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"what is oceanbase?\"}]");
    ObOpenAIUtils::ObOpenAIComplete completion;
    ObJsonObject *body = nullptr;
    ObJsonObject *config = nullptr;
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator, model, prompt, content, config, body));
    ObJsonNode *model_node = body->get_value("model");
    ObStringBuffer model_buf(&allocator);
    model_node->print(model_buf, 0);
    ASSERT_EQ(model, model_buf.string());

    ObJsonNode *messages_node = body->get_value("messages");
    ObJsonArray *messages_array = static_cast<ObJsonArray *>(messages_node);
    ASSERT_EQ(2, messages_array->element_count());

    ObJsonNode *message_node1 = messages_array->get_value(0);
    ObJsonObject *message_obj1 = static_cast<ObJsonObject *>(message_node1);
    ObJsonNode *prompt_node = message_obj1->get_value("content");
    ObStringBuffer prompt_buf(&allocator);
    prompt_node->print(prompt_buf, 0);
    ASSERT_EQ(prompt, prompt_buf.string());

    ObJsonNode *message_node2 = messages_array->get_value(1);
    ObJsonObject *message_obj2 = static_cast<ObJsonObject *>(message_node2);
    ObJsonNode *content_node = message_obj2->get_value("content");
    ObStringBuffer content_buf(&allocator);
    content_node->print(content_buf, 0);
    ASSERT_EQ(content, content_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString content("天气真好。");
    ObString response(
        "{"
        "\"choices\": [ "
            "{"
                "\"message\": {"
                    "\"role\": \"assistant\","
                    "\"content\": \"天气真好。\""
                "},"
                "\"finish_reason\": \"stop\","
                "\"index\": 0,"
                "\"logprobs\": null"
            "}"
        "],"
        "\"object\": \"chat.completion\","
        "\"usage\": {"
            "\"prompt_tokens\": 3019,"
            "\"completion_tokens\": 104,"
            "\"total_tokens\": 3123,"
            "\"prompt_tokens_details\": {"
                "\"cached_tokens\": 2048"
            "}"
        "},"
        "\"created\": 1735120033,"
        "\"system_fingerprint\": null,"
        "\"model\": \"qwen-plus\","
        "\"id\": \"chatcmpl-6ada9ed2-7f33-9de2-8bb0-78bd4035025a\""
    "}"
    );
    ObOpenAIUtils::ObOpenAIComplete completion;
    ObIJsonBase *j_base = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
    ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator, http_response, result));
    ObStringBuffer content_buf(&allocator);
    result->print(content_buf, 0);
    ASSERT_EQ(content, content_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output_empty)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString content("天气真好。");
    ObString response(
        "{"
        "\"choices\": [ "
            "{"
                "\"message\": {"
                    "\"role\": \"assistant\""

                "},"
                "\"finish_reason\": \"stop\","
                "\"index\": 0,"
                "\"logprobs\": null"
            "}"
        "],"
        "\"object\": \"chat.completion\","
        "\"usage\": {"
            "\"prompt_tokens\": 3019,"
            "\"completion_tokens\": 104,"
            "\"total_tokens\": 3123,"
            "\"prompt_tokens_details\": {"
                "\"cached_tokens\": 2048"
            "}"
        "},"
        "\"created\": 1735120033,"
        "\"system_fingerprint\": null,"
        "\"model\": \"qwen-plus\","
        "\"id\": \"chatcmpl-6ada9ed2-7f33-9de2-8bb0-78bd4035025a\""
    "}"
    );
    ObOpenAIUtils::ObOpenAIComplete completion;
    ObIJsonBase *j_base = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
    ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_INVALID_DATA, completion.parse_output(allocator, http_response, result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_header)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString api_key("sk-1234567890");
    ObString authorization("Authorization: Bearer sk-1234567890");
    ObString content_type("Content-Type: application/json");
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObArray<ObString> headers;
    ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator, api_key, headers));
    ASSERT_EQ(2, headers.count());
    ASSERT_EQ(authorization, headers[0]);
    ASSERT_EQ(content_type, headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_body)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString model("text-embedding-3-small");
    ObString input("oceanbase is a distributed database");
    ObArray<ObString> input_array;
    input_array.push_back(input);
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObJsonObject *body = nullptr;
    ObJsonObject *config = nullptr;
    ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator, model, input_array, config, body));

    ObJsonNode *model_node = body->get_value("model");
    ObStringBuffer model_buf(&allocator);
    model_node->print(model_buf, 0);
    ASSERT_EQ(model, model_buf.string());

    ObJsonNode *input_node = body->get_value("input");
    ObJsonArray *input_array_node = static_cast<ObJsonArray *>(input_node);
    ASSERT_EQ(1, input_array_node->element_count());
    ObJsonNode *input_node2 = input_array_node->get_value(0);
    ObStringBuffer input_buf(&allocator);
    input_node2->print(input_buf, 0);
    ASSERT_EQ(input, input_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString content("[0.0023064255, -0.009327292, -0.0028842222]");
    ObString response(
        "{"
            "\"data\": ["
                "{"
                    "\"embedding\": [0.0023064255, -0.009327292, -0.0028842222],"
                    "\"index\": 0,"
                    "\"object\": \"embedding\""
                "}"
            "],"
            "\"model\":\"text-embedding-v4\","
            "\"object\":\"list\","
            "\"usage\":{\"prompt_tokens\":23,\"total_tokens\":23},"
            "\"id\":\"f62c2ae7-0906-9758-ab34-47c5764f07e2\""
        "}"
    );
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObIJsonBase *j_base = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
    ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator, http_response, result));

    ObJsonArray *embeddings_array = static_cast<ObJsonArray *>(result);
    ASSERT_EQ(1, embeddings_array->element_count());

    ObJsonNode *embedding_node = embeddings_array->get_value(0);
    ObJsonArray *embedding_array = static_cast<ObJsonArray *>(embedding_node);
    ObStringBuffer embedding_buf(&allocator);
    embedding_array->print(embedding_buf, 0);
    ASSERT_EQ(content, embedding_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_empty)
{
    ObArenaAllocator allocator(ObModIds::TEST);
    ObString content("[0.0023064255, -0.009327292, -0.0028842222]");
    ObString response(
        "{"
            "\"data\": ["
                "{"
                    "\"index\": 0,"
                    "\"object\": \"embedding\""
                "}"
            "],"
            "\"model\":\"text-embedding-v4\","
            "\"object\":\"list\","
            "\"usage\":{\"prompt_tokens\":23,\"total_tokens\":23},"
            "\"id\":\"f62c2ae7-0906-9758-ab34-47c5764f07e2\""
        "}"
    );
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObIJsonBase *j_base = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
    ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator, http_response, result));
}

TEST_F(ObOpenAIUtilsTest, test_complete_get_body_with_config)
{
    ObString model("gpt-4o-mini");
    ObString prompt;
    ObString content("hello");
    ObJsonObject *config = nullptr;
    ObJsonObject *body = nullptr;
    ObOpenAIUtils::ObOpenAIComplete completion;
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"temperature\": 0.3, \"max_tokens\": 64}", config));
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, prompt, content, config, body));
    // without a system prompt only the user message is sent
    ObJsonNode *messages = body->get_value("messages");
    ASSERT_TRUE(nullptr != messages);
    ASSERT_EQ(1, messages->element_count());
    ASSERT_EQ(ObString("0.3"), to_string(body->get_value("temperature")));
    ASSERT_EQ(ObString("64"), to_string(body->get_value("max_tokens")));
    ObString empty_model;
    ObString empty_content;
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.get_body(allocator_, empty_model, prompt, content, config, body));
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.get_body(allocator_, model, prompt, empty_content, config, body));
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output_invalid)
{
    ObOpenAIUtils::ObOpenAIComplete completion;
    ObJsonObject *http_response = nullptr;
    ObIJsonBase *result = nullptr;
    const char *invalid_responses[] = {
        "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": \"\"}}]}",
        "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": 42}}]}",
        "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": null}}]}",
        "{\"choices\": [{\"message\": \"hello\"}]}",
        "{\"choices\": [\"hello\"]}",
        "{\"choices\": []}",
        "{\"choices\": {\"message\": {\"content\": \"hello\"}}}",
        "{\"error\": {\"message\": \"rate limited\"}}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_SUCCESS, parse_json(invalid_responses[i], http_response));
        ASSERT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, http_response, result)) << invalid_responses[i];
    }
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.parse_output(allocator_, nullptr, result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_body_batch)
{
    ObString model("text-embedding-3-small");
    ObArray<ObString> inputs;
    ObArray<ObString> empty_inputs;
    ObJsonObject *config = nullptr;
    ObJsonObject *body = nullptr;
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("first")));
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("second")));
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("third")));
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"dimensions\": 256}", config));
    // all inputs go into one request
    ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator_, model, inputs, config, body));
    ASSERT_EQ(ObString("[\"first\", \"second\", \"third\"]"), to_string(body->get_value("input")));
    ASSERT_EQ(ObString("256"), to_string(body->get_value("dimensions")));
    ASSERT_EQ(OB_INVALID_ARGUMENT, embedding.get_body(allocator_, model, empty_inputs, config, body));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_out_of_order)
{
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObJsonArray *embeddings = nullptr;
    // the items are mapped back to the inputs by index
    ASSERT_EQ(OB_SUCCESS, parse_embed_output(embedding,
        "{\"data\": ["
            "{\"object\": \"embedding\", \"index\": 2, \"embedding\": [0.3, 3]},"
            "{\"object\": \"embedding\", \"index\": 0, \"embedding\": [0.1, 1]},"
            "{\"object\": \"embedding\", \"index\": 1, \"embedding\": [0.2, 2]}"
        "], \"model\": \"text-embedding-3-small\", \"object\": \"list\"}", embeddings));
    ASSERT_EQ(3, embeddings->element_count());
    ASSERT_EQ(ObString("[0.1, 1]"), to_string(embeddings->get_value(0)));
    ASSERT_EQ(ObString("[0.2, 2]"), to_string(embeddings->get_value(1)));
    ASSERT_EQ(ObString("[0.3, 3]"), to_string(embeddings->get_value(2)));
    // an integral float index is accepted
    ASSERT_EQ(OB_SUCCESS, parse_embed_output(embedding,
        "{\"data\": [{\"index\": 1.0, \"embedding\": [2]}, {\"index\": 0.0, \"embedding\": [1]}]}", embeddings));
    ASSERT_EQ(ObString("[1]"), to_string(embeddings->get_value(0)));
    ASSERT_EQ(ObString("[2]"), to_string(embeddings->get_value(1)));
    // without index the items keep the request order
    ASSERT_EQ(OB_SUCCESS, parse_embed_output(embedding,
        "{\"data\": [{\"embedding\": [2]}, {\"embedding\": [1]}]}", embeddings));
    ASSERT_EQ(ObString("[2]"), to_string(embeddings->get_value(0)));
    ASSERT_EQ(ObString("[1]"), to_string(embeddings->get_value(1)));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_invalid_index)
{
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObJsonArray *embeddings = nullptr;
    const char *invalid_responses[] = {
        // duplicated index
        "{\"data\": [{\"index\": 0, \"embedding\": [1]}, {\"index\": 0, \"embedding\": [2]}]}",
        // index out of range
        "{\"data\": [{\"index\": 0, \"embedding\": [1]}, {\"index\": 2, \"embedding\": [2]}]}",
        "{\"data\": [{\"index\": -1, \"embedding\": [1]}]}",
        // index is not an integer
        "{\"data\": [{\"index\": 0.5, \"embedding\": [1]}]}",
        "{\"data\": [{\"index\": \"0\", \"embedding\": [1]}]}",
        "{\"data\": [{\"index\": null, \"embedding\": [1]}]}",
        // only part of the items have index
        "{\"data\": [{\"index\": 0, \"embedding\": [1]}, {\"embedding\": [2]}]}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_INVALID_DATA, parse_embed_output(embedding, invalid_responses[i], embeddings)) << invalid_responses[i];
    }
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_invalid_vector)
{
    ObOpenAIUtils::ObOpenAIEmbed embedding;
    ObJsonArray *embeddings = nullptr;
    const char *invalid_responses[] = {
        "{\"object\": \"list\"}",
        "{\"data\": []}",
        "{\"data\": {\"index\": 0, \"embedding\": [1]}}",
        "{\"data\": [\"embedding\"]}",
        "{\"data\": [{\"index\": 0, \"embedding\": []}]}",
        "{\"data\": [{\"index\": 0, \"embedding\": \"0.1,0.2\"}]}",
        "{\"data\": [{\"index\": 0, \"embedding\": [0.1, \"0.2\"]}]}",
        "{\"data\": [{\"index\": 0, \"embedding\": [0.1, null]}]}",
        "{\"data\": [{\"index\": 0, \"embedding\": [[0.1]]}]}",
        // all embeddings of a response have the same dimension
        "{\"data\": [{\"index\": 0, \"embedding\": [0.1, 0.2]}, {\"index\": 1, \"embedding\": [0.3]}]}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_INVALID_DATA, parse_embed_output(embedding, invalid_responses[i], embeddings)) << invalid_responses[i];
    }
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_INVALID_ARGUMENT, embedding.parse_output(allocator_, nullptr, result));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_get_header)
{
    // ollama needs no api key, the content type is the only header
    ObString api_key;
    ObString content_type("Content-Type: application/json");
    ObArray<ObString> complete_headers;
    ObArray<ObString> embed_headers;
    ObOllamaUtils::ObOllamaComplete completion;
    ObOllamaUtils::ObOllamaEmbed embedding;
    ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator_, api_key, complete_headers));
    ASSERT_EQ(1, complete_headers.count());
    ASSERT_EQ(content_type, complete_headers[0]);
    ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator_, api_key, embed_headers));
    ASSERT_EQ(1, embed_headers.count());
    ASSERT_EQ(content_type, embed_headers[0]);
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_get_body_generate)
{
    ObString model("llama3.1");
    ObString prompt("You are a helpful assistant.");
    ObString empty_prompt;
    ObString content("what is oceanbase?");
    ObJsonObject *config = nullptr;
    ObJsonObject *body = nullptr;
    ObOllamaUtils::ObOllamaComplete completion;
    // /api/generate: {"model": *, "prompt": *, "system": *, "stream": false, "options": {*}}
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"temperature\": 0.2, \"max_tokens\": 16, \"top_k\": 5, \"format\": \"json\"}", config));
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, prompt, content, config, body));
    ASSERT_EQ(model, to_string(body->get_value("model")));
    ASSERT_EQ(content, to_string(body->get_value("prompt")));
    ASSERT_EQ(prompt, to_string(body->get_value("system")));
    ASSERT_TRUE(nullptr == body->get_value("messages"));
    ObJsonNode *stream = body->get_value("stream");
    ASSERT_TRUE(nullptr != stream);
    ASSERT_EQ(ObJsonNodeType::J_BOOLEAN, stream->json_type());
    ASSERT_FALSE(stream->get_boolean());
    // sampling parameters go into options, max_tokens is called num_predict
    ObJsonNode *options = body->get_value("options");
    ASSERT_TRUE(nullptr != options);
    ASSERT_EQ(ObJsonNodeType::J_OBJECT, options->json_type());
    ObJsonObject *options_obj = static_cast<ObJsonObject *>(options);
    ASSERT_EQ(ObString("0.2"), to_string(options_obj->get_value("temperature")));
    ASSERT_EQ(ObString("16"), to_string(options_obj->get_value("num_predict")));
    ASSERT_EQ(ObString("5"), to_string(options_obj->get_value("top_k")));
    ASSERT_TRUE(nullptr == body->get_value("temperature"));
    ASSERT_TRUE(nullptr == body->get_value("max_tokens"));
    ASSERT_TRUE(nullptr == body->get_value("top_k"));
    ASSERT_EQ(ObString("json"), to_string(body->get_value("format")));
    // no system prompt and no config
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, empty_prompt, content, nullptr, body));
    ASSERT_TRUE(nullptr == body->get_value("system"));
    ASSERT_TRUE(nullptr == body->get_value("options"));
    ASSERT_TRUE(nullptr != body->get_value("stream"));
    // a value given in options explicitly wins
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"options\": {\"temperature\": 0.9}, \"temperature\": 0.1, \"seed\": 7}", config));
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, empty_prompt, content, config, body));
    options = body->get_value("options");
    ASSERT_TRUE(nullptr != options);
    options_obj = static_cast<ObJsonObject *>(options);
    ASSERT_EQ(ObString("0.9"), to_string(options_obj->get_value("temperature")));
    ASSERT_EQ(ObString("7"), to_string(options_obj->get_value("seed")));
    ObString empty_model;
    ObString empty_content;
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.get_body(allocator_, empty_model, prompt, content, nullptr, body));
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.get_body(allocator_, model, prompt, empty_content, nullptr, body));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_get_body_chat)
{
    ObString model("llama3.1");
    ObString prompt("You are a helpful assistant.");
    ObString content("what is oceanbase?");
    ObJsonObject *body = nullptr;
    ObOllamaUtils::ObOllamaComplete completion;
    // /api/chat: {"model": *, "messages": [{"role": "system", *}, {"role": "user", *}], "stream": false}
    completion.set_use_chat_api(true);
    ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, prompt, content, nullptr, body));
    ASSERT_EQ(model, to_string(body->get_value("model")));
    ASSERT_TRUE(nullptr == body->get_value("prompt"));
    ObJsonNode *messages = body->get_value("messages");
    ASSERT_TRUE(nullptr != messages);
    ASSERT_EQ(ObJsonNodeType::J_ARRAY, messages->json_type());
    ASSERT_EQ(2, messages->element_count());
    ObJsonObject *system_message = static_cast<ObJsonObject *>(static_cast<ObJsonArray *>(messages)->get_value(0));
    ObJsonObject *user_message = static_cast<ObJsonObject *>(static_cast<ObJsonArray *>(messages)->get_value(1));
    ASSERT_EQ(ObString("system"), to_string(system_message->get_value("role")));
    ASSERT_EQ(prompt, to_string(system_message->get_value("content")));
    ASSERT_EQ(ObString("user"), to_string(user_message->get_value("role")));
    ASSERT_EQ(content, to_string(user_message->get_value("content")));
    ObJsonNode *stream = body->get_value("stream");
    ASSERT_TRUE(nullptr != stream);
    ASSERT_FALSE(stream->get_boolean());
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_parse_output)
{
    ObOllamaUtils::ObOllamaComplete completion;
    ObJsonObject *http_response = nullptr;
    ObIJsonBase *result = nullptr;
    // /api/generate
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"model\": \"llama3.1\", \"created_at\": \"2025-01-01T00:00:00Z\", "
                                     "\"response\": \"天气真好。\", \"done\": true, \"done_reason\": \"stop\"}", http_response));
    ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator_, http_response, result));
    ASSERT_EQ(ObString("天气真好。"), to_string(result));
    // /api/chat
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"model\": \"llama3.1\", \"message\": {\"role\": \"assistant\", "
                                     "\"content\": \"Paris\"}, \"done\": true}", http_response));
    ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator_, http_response, result));
    ASSERT_EQ(ObString("Paris"), to_string(result));
    const char *invalid_responses[] = {
        "{\"model\": \"llama3.1\", \"done\": true}",
        "{\"response\": \"\", \"done\": true}",
        "{\"response\": 1, \"done\": true}",
        "{\"response\": null, \"done\": true}",
        "{\"message\": {\"role\": \"assistant\"}, \"done\": true}",
        "{\"message\": {\"role\": \"assistant\", \"content\": \"\"}, \"done\": true}",
        "{\"message\": \"Paris\", \"done\": true}",
        "{\"error\": \"model 'llama3.1' not found\"}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_SUCCESS, parse_json(invalid_responses[i], http_response));
        ASSERT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, http_response, result)) << invalid_responses[i];
    }
    ASSERT_EQ(OB_INVALID_ARGUMENT, completion.parse_output(allocator_, nullptr, result));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_embed_get_body)
{
    ObString model("nomic-embed-text");
    ObArray<ObString> inputs;
    ObJsonObject *config = nullptr;
    ObJsonObject *body = nullptr;
    ObOllamaUtils::ObOllamaEmbed embedding;
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("first")));
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("second")));
    // /api/embed: {"model": *, "input": [*]}
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"dimensions\": 4}", config));
    ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator_, model, inputs, config, body));
    ASSERT_EQ(model, to_string(body->get_value("model")));
    ASSERT_EQ(ObString("[\"first\", \"second\"]"), to_string(body->get_value("input")));
    ASSERT_EQ(ObString("4"), to_string(body->get_value("dimensions")));
    ASSERT_TRUE(nullptr == body->get_value("prompt"));
    // the legacy /api/embeddings takes one "prompt"
    ObArray<ObString> single_input;
    ASSERT_EQ(OB_SUCCESS, single_input.push_back(ObString("only")));
    embedding.set_use_legacy_api(true);
    ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator_, model, single_input, nullptr, body));
    ASSERT_EQ(ObString("only"), to_string(body->get_value("prompt")));
    ASSERT_TRUE(nullptr == body->get_value("input"));
    ASSERT_EQ(OB_INVALID_ARGUMENT, embedding.get_body(allocator_, model, inputs, nullptr, body));
    ObArray<ObString> empty_inputs;
    ASSERT_EQ(OB_INVALID_ARGUMENT, embedding.get_body(allocator_, model, empty_inputs, nullptr, body));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_embed_parse_output)
{
    ObOllamaUtils::ObOllamaEmbed embedding;
    ObJsonArray *embeddings = nullptr;
    // /api/embed
    ASSERT_EQ(OB_SUCCESS, parse_embed_output(embedding,
        "{\"model\": \"nomic-embed-text\", \"embeddings\": [[0.1, 0.2], [0.3, 0.4]], \"total_duration\": 1}", embeddings));
    ASSERT_EQ(2, embeddings->element_count());
    ASSERT_EQ(ObString("[0.1, 0.2]"), to_string(embeddings->get_value(0)));
    ASSERT_EQ(ObString("[0.3, 0.4]"), to_string(embeddings->get_value(1)));
    // the legacy /api/embeddings
    ASSERT_EQ(OB_SUCCESS, parse_embed_output(embedding, "{\"embedding\": [0.5, -0.5, 1]}", embeddings));
    ASSERT_EQ(1, embeddings->element_count());
    ASSERT_EQ(ObString("[0.5, -0.5, 1]"), to_string(embeddings->get_value(0)));
    const char *invalid_responses[] = {
        "{\"model\": \"nomic-embed-text\"}",
        "{\"embeddings\": []}",
        "{\"embeddings\": {\"0\": [0.1]}}",
        "{\"embeddings\": [[]]}",
        "{\"embeddings\": [[0.1], [0.2, 0.3]]}",
        "{\"embeddings\": [[\"0.1\"]]}",
        "{\"embeddings\": [0.1, 0.2]}",
        "{\"embedding\": []}",
        "{\"embedding\": \"0.1\"}",
        "{\"error\": \"model not found\"}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_INVALID_DATA, parse_embed_output(embedding, invalid_responses[i], embeddings)) << invalid_responses[i];
    }
}

TEST_F(ObOpenAIUtilsTest, test_ollama_api_url)
{
    ASSERT_TRUE(ObOllamaUtils::is_chat_api(ObString("http://127.0.0.1:11434/api/chat")));
    ASSERT_TRUE(ObOllamaUtils::is_chat_api(ObString("http://127.0.0.1:11434/api/chat/")));
    ASSERT_TRUE(ObOllamaUtils::is_chat_api(ObString("http://ollama.local/API/Chat?keep_alive=5m")));
    ASSERT_FALSE(ObOllamaUtils::is_chat_api(ObString("http://127.0.0.1:11434/api/generate")));
    ASSERT_FALSE(ObOllamaUtils::is_chat_api(ObString("http://127.0.0.1:11434/v1/chat/completions")));
    ASSERT_FALSE(ObOllamaUtils::is_chat_api(ObString()));
    ASSERT_TRUE(ObOllamaUtils::is_legacy_embed_api(ObString("http://127.0.0.1:11434/api/embeddings")));
    ASSERT_FALSE(ObOllamaUtils::is_legacy_embed_api(ObString("http://127.0.0.1:11434/api/embed")));
    ASSERT_FALSE(ObOllamaUtils::is_legacy_embed_api(ObString("http://127.0.0.1:11434/v1/embeddings/x")));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_provider)
{
    // the provider name is case insensitive
    const char *names[] = {"ollama", "Ollama", "OLLAMA"};
    for (int64_t i = 0; i < ARRAYSIZEOF(names); i++) {
        ObString provider(names[i]);
        ObAIFuncIComplete *complete_provider = nullptr;
        ObAIFuncIEmbed *embed_provider = nullptr;
        ObArray<ObString> headers;
        ObString api_key;
        ASSERT_TRUE(oceanbase::share::ObAiModelEndpointInfo::is_ollama_provider(provider));
        ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, provider, complete_provider));
        ASSERT_TRUE(nullptr != complete_provider);
        ASSERT_EQ(OB_SUCCESS, complete_provider->get_header(allocator_, api_key, headers));
        ASSERT_EQ(1, headers.count());
        ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, provider, embed_provider));
        ASSERT_TRUE(nullptr != embed_provider);
    }
    ASSERT_FALSE(oceanbase::share::ObAiModelEndpointInfo::is_ollama_provider(ObString("openai")));
}

TEST_F(ObOpenAIUtilsTest, test_check_rerank_results)
{
    ObJsonObject *response = nullptr;
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"results\": [{\"index\": 1, \"relevance_score\": 0.9}, "
                                     "{\"index\": 0, \"relevance_score\": 0.1}]}", response));
    ObJsonArray *results = static_cast<ObJsonArray *>(response->get_value("results"));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_rerank_results(results, 2));
    // every document gets exactly one score
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_rerank_results(results, 3));
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_rerank_results(nullptr, 2));
    const char *invalid_responses[] = {
        "{\"results\": [{\"index\": 0, \"relevance_score\": 0.9}, {\"index\": 0, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"index\": 0, \"relevance_score\": 0.9}, {\"index\": 2, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"index\": -1, \"relevance_score\": 0.9}, {\"index\": 0, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"index\": 0, \"relevance_score\": \"high\"}, {\"index\": 1, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"index\": 0}, {\"index\": 1, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"relevance_score\": 0.9}, {\"index\": 1, \"relevance_score\": 0.1}]}",
        "{\"results\": [{\"index\": 0.5, \"relevance_score\": 0.9}, {\"index\": 1, \"relevance_score\": 0.1}]}",
        "{\"results\": [1, 0]}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_responses); i++) {
        ASSERT_EQ(OB_SUCCESS, parse_json(invalid_responses[i], response));
        results = static_cast<ObJsonArray *>(response->get_value("results"));
        ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_rerank_results(results, 2)) << invalid_responses[i];
    }
}

TEST_F(ObOpenAIUtilsTest, test_check_completion_config)
{
    ObJsonObject *config = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_completion_config(nullptr));
    const char *valid_configs[] = {
        "{}",
        "{\"temperature\": 0, \"top_p\": 1, \"max_tokens\": 1}",
        "{\"temperature\": 0.7, \"max_tokens\": 256.0, \"max_completion_tokens\": 100}",
        "{\"temperature\": 2, \"stop\": [\"\\n\"], \"response_format\": {\"type\": \"text\"}}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(valid_configs); i++) {
        ASSERT_EQ(OB_SUCCESS, parse_json(valid_configs[i], config));
        ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_completion_config(config)) << valid_configs[i];
    }
    const char *invalid_configs[] = {
        "{\"temperature\": -0.1}",
        "{\"temperature\": 2.5}",
        "{\"temperature\": \"0.5\"}",
        "{\"temperature\": null}",
        "{\"top_p\": 1.5}",
        "{\"max_tokens\": 0}",
        "{\"max_tokens\": -1}",
        "{\"max_tokens\": 1.5}",
        "{\"max_tokens\": \"10\"}",
        "{\"max_completion_tokens\": 0}",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(invalid_configs); i++) {
        ASSERT_EQ(OB_SUCCESS, parse_json(invalid_configs[i], config));
        ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_completion_config(config)) << invalid_configs[i];
    }
}

TEST_F(ObOpenAIUtilsTest, test_get_json_integer)
{
    ObJsonObject *obj = nullptr;
    int64_t value = 0;
    ASSERT_EQ(OB_SUCCESS, parse_json("{\"a\": 3, \"b\": 3.0, \"c\": 3.5, \"d\": \"3\", \"e\": 1e30, \"f\": -2}", obj));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_integer(obj->get_value("a"), value));
    ASSERT_EQ(3, value);
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_integer(obj->get_value("b"), value));
    ASSERT_EQ(3, value);
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_integer(obj->get_value("f"), value));
    ASSERT_EQ(-2, value);
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::get_json_integer(obj->get_value("c"), value));
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::get_json_integer(obj->get_value("d"), value));
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::get_json_integer(obj->get_value("e"), value));
    ASSERT_EQ(OB_INVALID_DATA, ObAIFuncJsonUtils::get_json_integer(nullptr, value));
}
