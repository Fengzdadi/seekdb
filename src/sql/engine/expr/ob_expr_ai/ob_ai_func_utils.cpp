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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_ai_func_utils.h"
#include "ob_ai_func_client.h"
#include "query/engine/expr/ob_ai_model_resolver.h"

namespace oceanbase 
{
namespace common 
{

int ObOpenAIUtils::get_header(common::ObIAllocator &allocator,
                              ObString &api_key,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (api_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //["Authorization: Bearer %.*s", "Content-Type: application/json"]
    int auth_header_len = 1024;
    char *auth_header_str = (char *)allocator.alloc(auth_header_len);
    ObString content_type_str("Content-Type: application/json");
    ObString content_type_c_str;
    if (OB_FAIL(ob_write_string(allocator, content_type_str, content_type_c_str, true))) {
    } else if (OB_ISNULL(auth_header_str)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("Failed to allocate memory for auth header string and content type string", K(ret));
    } else {
      int auth_header_pos = snprintf(auth_header_str, auth_header_len,
                         "Authorization: Bearer %.*s", api_key.length(), api_key.ptr());
      if (auth_header_pos < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(headers.push_back(ObString(auth_header_str)))) {
      } else if (OB_FAIL(headers.push_back(content_type_c_str))) {
      }
    }
  }
  return ret;
}
int ObOpenAIUtils::ObOpenAIComplete::get_header(common::ObIAllocator &allocator,
                                                ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObOpenAIUtils::ObOpenAIComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *messages_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(body_obj->add("messages", messages_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonArray *&messages)
{
  int ret = OB_SUCCESS;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //messages: [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "What is the capital of France?"}]
    ObJsonArray *messages_array = nullptr;
    ObJsonObject *sys_message_obj = nullptr;
    ObJsonObject *user_message_obj = nullptr;
    ObString system_str("system");
    ObString user_str("user");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, messages_array))) {
    } else if(!prompt.empty()) {
      if (OB_FAIL(construct_message_obj(allocator, system_str, prompt, sys_message_obj))) {
      } else if (OB_FAIL(messages_array->append(sys_message_obj))) {
      }
    } 
    if (OB_SUCC(ret)) {
      if (OB_FAIL(construct_message_obj(allocator, user_str, content, user_message_obj))) {
      } else if (OB_FAIL(messages_array->append(user_message_obj))) {
      } else {
        messages = messages_array;
      }
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_message_obj(ObIAllocator &allocator, ObString &role, ObString &content, ObJsonObject *&message)
{
  int ret = OB_SUCCESS;
  if (role.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *message_obj = nullptr;
    ObJsonString *role_json_str = nullptr;
    ObJsonString *content_json_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, message_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, role, role_json_str))) {
    } else if (OB_FAIL(message_obj->add("role", role_json_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, content, content_json_str))) {
    } else if (OB_FAIL(message_obj->add("content", content_json_str))) {
    } else {
      message = message_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"response_format":{"type":"json_object"}}
    ObString json_str("json_object");
    ObJsonString *json_str_obj = nullptr;
    ObJsonObject *json_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, json_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(json_obj->add("type", json_str_obj))) {
    } else if (OB_FAIL(config->add("response_format", json_obj))) {
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::parse_output(common::ObIAllocator &allocator, 
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"choices": [{"message": {"role": "assistant", "content": "*"}}]}
    ObJsonNode *choices_node = http_response->get_value("choices");
    ObJsonNode *choice_node = nullptr;
    ObJsonNode *message_node = nullptr;
    ObJsonNode *content_node = nullptr;
    if (OB_ISNULL(choices_node) || choices_node->json_type() != ObJsonNodeType::J_ARRAY
        || choices_node->element_count() == 0) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choice_node = static_cast<ObJsonArray *>(choices_node)->get_value(0))
               || choice_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(message_node = static_cast<ObJsonObject *>(choice_node)->get_value("message"))
               || message_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(content_node = static_cast<ObJsonObject *>(message_node)->get_value("content"))
               || content_node->json_type() != ObJsonNodeType::J_STRING
               || content_node->get_data_length() == 0) {
      // the completion must be a non-empty string
      ret = OB_INVALID_DATA;
    } else {
      result = content_node;
    }
    if (OB_INVALID_DATA == ret) {
      LOG_WARN("invalid completion response", K(ret));
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_header(common::ObIAllocator &allocator,
                                             ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::parse_output(common::ObIAllocator &allocator, 
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"data": [{"index": 1, "embedding": [*]}, {"index": 0, "embedding": [*]}]}, items may be out of order
    ObJsonNode *data_node = nullptr;
    ObJsonArray *result_array = nullptr;
    if (OB_ISNULL(data_node = http_response->get_value("data"))
        || data_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
      LOG_WARN("embedding response has no data array", K(ret));
    } else if (OB_FAIL(ObAIFuncJsonUtils::collect_embeddings_by_index(allocator, static_cast<ObJsonArray *>(data_node),
                                                                      "index", "embedding", result_array))) {
    } else {
      result = result_array;
    }
  }
  return ret;
}

int ObOllamaUtils::get_header(common::ObIAllocator &allocator,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  // ollama needs no api key, only the content type is sent
  ObString content_type_str("Content-Type: application/json");
  ObString content_type_c_str;
  if (OB_FAIL(ob_write_string(allocator, content_type_str, content_type_c_str, true))) {
  } else if (OB_FAIL(headers.push_back(content_type_c_str))) {
  }
  return ret;
}

// the url path (query string and trailing '/' ignored) ends with suffix, case insensitive
static bool url_path_ends_with(const ObString &url, const char *suffix)
{
  ObString path = url;
  const char *query_pos = path.empty() ? nullptr : path.find('?');
  if (OB_NOT_NULL(query_pos)) {
    path.assign_ptr(path.ptr(), static_cast<ObString::obstr_size_t>(query_pos - path.ptr()));
  }
  while (path.length() > 0 && path.ptr()[path.length() - 1] == '/') {
    path.assign_ptr(path.ptr(), path.length() - 1);
  }
  return path.suffix_match_ci(suffix);
}

bool ObOllamaUtils::is_chat_api(const ObString &url)
{
  return url_path_ends_with(url, "/api/chat");
}

bool ObOllamaUtils::is_legacy_embed_api(const ObString &url)
{
  return url_path_ends_with(url, "/api/embeddings");
}

int ObOllamaUtils::add_config_to_body(common::ObIAllocator &allocator,
                                      common::ObJsonObject *config,
                                      common::ObJsonObject *body)
{
  int ret = OB_SUCCESS;
  // sampling parameters belong to "options" in the native protocol, the others stay at the top level
  static const char *OPTION_KEYS[] = {
    "temperature", "top_p", "top_k", "min_p", "seed", "stop", "num_predict", "num_ctx",
    "repeat_penalty", "repeat_last_n", "presence_penalty", "frequency_penalty", "typical_p",
    "mirostat", "mirostat_tau", "mirostat_eta", "num_keep", "num_batch", "num_gpu", "num_thread"
  };
  ObJsonObject *options = nullptr;
  if (OB_ISNULL(body)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(config)) {
    // do nothing
  } else {
    ObJsonNode *user_options = config->get_value("options");
    if (OB_NOT_NULL(user_options) && user_options->json_type() == ObJsonNodeType::J_OBJECT) {
      options = static_cast<ObJsonObject *>(user_options);
    }
    ObString key;
    for (int64_t i = 0; OB_SUCC(ret) && i < config->element_count(); i++) {
      ObJsonNode *value = config->get_value(i);
      bool is_option = false;
      if (OB_ISNULL(value)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(config->get_key(i, key))) {
      } else if (0 == key.case_compare("options") && value == options) {
        // merged below
      } else {
        ObString option_key = key;
        if (0 == key.case_compare("max_tokens") || 0 == key.case_compare("max_completion_tokens")) {
          option_key = ObString("num_predict");
          is_option = true;
        }
        for (int64_t j = 0; !is_option && j < ARRAYSIZEOF(OPTION_KEYS); j++) {
          is_option = (0 == key.case_compare(OPTION_KEYS[j]));
        }
        if (!is_option) {
          if (OB_FAIL(body->add(key, value))) {
          }
        } else if (OB_ISNULL(options) && OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, options))) {
        } else if (OB_NOT_NULL(options->get_value(option_key))) {
          // a value given explicitly in "options" wins
        } else if (OB_FAIL(options->add(option_key, value))) {
        }
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(options) && OB_FAIL(body->add("options", options))) {
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::get_header(common::ObIAllocator &allocator,
                                                common::ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  return ObOllamaUtils::get_header(allocator, headers);
}

int ObOllamaUtils::ObOllamaComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) { 
    ret = OB_INVALID_ARGUMENT;
  } else {
    // /api/generate: {"model": "llama3.1", "prompt": "*", "system": "*", "stream": false, "options": {}}
    // /api/chat: {"model": "llama3.1", "messages": [{"role": "user", "content": "*"}], "stream": false}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *content_str = nullptr;
    ObJsonString *system_str = nullptr;
    ObJsonArray *messages_array = nullptr;
    ObJsonBoolean *stream_false = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (use_chat_api_) {
      if (OB_FAIL(ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(allocator, prompt, content, messages_array))) {
      } else if (OB_FAIL(body_obj->add("messages", messages_array))) {
      }
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, content, content_str))) {
    } else if (OB_FAIL(body_obj->add("prompt", content_str))) {
    } else if (prompt.empty()) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, prompt, system_str))) {
    } else if (OB_FAIL(body_obj->add("system", system_str))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObOllamaUtils::add_config_to_body(allocator, config, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, false, stream_false))) {
    } else if (OB_FAIL(body_obj->add("stream", stream_false))) {
      // ollama streams NDJSON unless stream is false, always ask for a single response
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"format": "json"}
    ObString json_str("json");
    ObJsonString *json_str_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(config->add("format", json_str_obj))) {
    } 
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::parse_output(common::ObIAllocator &allocator,
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // /api/generate returns {"response": "*"}, /api/chat returns {"message": {"content": "*"}}
    ObJsonNode *content_node = http_response->get_value("response");
    ObJsonNode *message_node = nullptr;
    if (OB_ISNULL(content_node)
        && OB_NOT_NULL(message_node = http_response->get_value("message"))
        && message_node->json_type() == ObJsonNodeType::J_OBJECT) {
      content_node = static_cast<ObJsonObject *>(message_node)->get_value("content");
    }
    if (OB_ISNULL(content_node)
        || content_node->json_type() != ObJsonNodeType::J_STRING
        || content_node->get_data_length() == 0) {
      ret = OB_INVALID_DATA;
      LOG_WARN("invalid ollama completion response", K(ret));
    } else {
      result = content_node;
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::get_header(common::ObIAllocator &allocator,
                                             ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  return ObOllamaUtils::get_header(allocator, headers);
}

int ObOllamaUtils::ObOllamaEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (use_legacy_api_ && contents.count() != 1) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("legacy ollama embeddings api takes one input", K(ret), K(contents.count()));
  } else {
    // /api/embed: {"model": "*", "input": ["*"]}, /api/embeddings: {"model": "*", "prompt": "*"}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    ObJsonString *prompt_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (use_legacy_api_) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, contents.at(0), prompt_str))) {
      } else if (OB_FAIL(body_obj->add("prompt", prompt_str))) {
      }
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::parse_output(common::ObIAllocator &allocator,
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // /api/embed returns {"embeddings": [[*]]}, /api/embeddings returns {"embedding": [*]}
    ObJsonNode *embeddings_node = http_response->get_value("embeddings");
    ObJsonNode *embedding_node = nullptr;
    ObJsonArray *result_array = nullptr;
    if (OB_NOT_NULL(embeddings_node)) {
      if (embeddings_node->json_type() != ObJsonNodeType::J_ARRAY) {
        ret = OB_INVALID_DATA;
      } else {
        result_array = static_cast<ObJsonArray *>(embeddings_node);
      }
    } else if (OB_ISNULL(embedding_node = http_response->get_value("embedding"))) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
    } else if (OB_FAIL(result_array->append(embedding_node))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::check_embedding_list(result_array))) {
    } else {
      result = result_array;
    }
    if (OB_INVALID_DATA == ret) {
      LOG_WARN("invalid ollama embedding response", K(ret));
    }
  }
  return ret;
}

int ObDashscopeUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::get_header(common::ObIAllocator &allocator, 
                                                      common::ObString &api_key,
                                                      common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeComplete::get_body(common::ObIAllocator &allocator,
                                                    common::ObString &model,
                                                    common::ObString &prompt,
                                                    common::ObString &content,
                                                    common::ObJsonObject *config,
                                                    common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;

    if (OB_ISNULL(config)) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
      } else if (OB_FAIL(body_obj->add("model", model_str))) {
      } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(allocator, prompt, content, input_obj))) {
      } else if (OB_FAIL(body_obj->add("input", input_obj))) {
      } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(allocator, config))) {
      } else if (OB_FAIL(body_obj->add("parameters", config))) {
      } else {
        body = body_obj;
      }
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(config->get_value("result_format"))) {
    if (OB_FAIL(config->remove("result_format"))) {
    }
  } 
  if (OB_SUCC(ret)) {
    // {"result_format": "message"}
    ObJsonString *result_format_str = nullptr;
    ObString result_format_str_val("message");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, result_format_str_val, result_format_str))) {
    } else if (OB_FAIL(config->add("result_format", result_format_str))) {
    }
  } 
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonObject *&input_obj)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonArray *messages_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(obj->add("messages", messages_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_json_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //{"type": "json_object"}
    ObJsonString *type_str = nullptr;
    ObString type_str_val("json_object");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, type_str_val, type_str))) {
    } else if (OB_FAIL(config->add("type", type_str))) {
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::parse_output(ObIAllocator &allocator, ObJsonObject *http_response, ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonNode *output_node = nullptr;
    ObJsonNode *choices_node = nullptr;
    ObJsonNode *choice_node = nullptr;
    ObJsonNode *message_node = nullptr;
    ObJsonNode *content_node = nullptr;
    if (OB_ISNULL(output_node = http_response->get_value("output"))
        || output_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choices_node = static_cast<ObJsonObject *>(output_node)->get_value("choices"))
               || choices_node->json_type() != ObJsonNodeType::J_ARRAY
               || choices_node->element_count() == 0) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choice_node = static_cast<ObJsonArray *>(choices_node)->get_value(0))
               || choice_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(message_node = static_cast<ObJsonObject *>(choice_node)->get_value("message"))
               || message_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(content_node = static_cast<ObJsonObject *>(message_node)->get_value("content"))
               || content_node->json_type() != ObJsonNodeType::J_STRING
               || content_node->get_data_length() == 0) {
      ret = OB_INVALID_DATA;
    } else {
      result = content_node;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::get_header(common::ObIAllocator &allocator,
                                                   common::ObString &api_key,
                                                   common::ObArray<ObString> &headers) 
{
  return ObDashscopeUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeEmbed::get_body(common::ObIAllocator &allocator,
                                                 common::ObString &model,
                                                 common::ObArray<ObString> &contents,
                                                 common::ObJsonObject *config,
                                                 common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"texts": ["*"]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    ObJsonArray *texts_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, input_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, texts_array))) {
    } else if (OB_FAIL(input_obj->add("texts", texts_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (config != nullptr && config->element_count() > 0) {
      ObJsonNode *dimensions_node = config->get_value("dimensions");
      if (OB_ISNULL(dimensions_node)) {
        // do nothing
      } else if (OB_FAIL(config->rename_key("dimensions", "dimension"))) {
      } 
      if (OB_SUCC(ret)) {
        if (OB_FAIL(body_obj->add("parameters", config))) {
        }
      }
    } 
    if (OB_SUCC(ret)) {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::parse_output(common::ObIAllocator &allocator,
                                                    common::ObJsonObject *http_response,
                                                    common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"output": {"embeddings": [{"text_index": 0, "embedding": ["*"]}]}}
    ObJsonNode *output_node = nullptr;
    ObJsonNode *embeddings_node = nullptr;
    ObJsonArray *result_array = nullptr;
    if (OB_ISNULL(output_node = http_response->get_value("output"))
        || output_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(embeddings_node = static_cast<ObJsonObject *>(output_node)->get_value("embeddings"))
               || embeddings_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncJsonUtils::collect_embeddings_by_index(allocator, static_cast<ObJsonArray *>(embeddings_node),
                                                                      "text_index", "embedding", result_array))) {
    } else {
      result = result_array;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeRerank::get_input_obj(allocator, query, document_array, input_obj))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (config != nullptr && config->element_count() > 0) {
      if (OB_FAIL(body_obj->add("parameters", config))) {
      }
    } 
    if (OB_SUCC(ret)) {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_input_obj(common::ObIAllocator &allocator,
                                                      common::ObString &query,
                                                      common::ObJsonArray *document_array,
                                                      common::ObJsonObject *&input_obj) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonString *query_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(obj->add("query", query_str))) {
    } else if (OB_FAIL(obj->add("documents", document_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonObject *output_obj = nullptr;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(http_response->get_value("output")))
             || output_obj->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(output_obj->get_value("results")))
             || results_array->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}

int ObSiliconflowUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *query_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(body_obj->add("query", query_str))) {
    } else if (OB_FAIL(body_obj->add("documents", document_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(http_response->get_value("results")))
             || results_array->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}


int ObAIFuncUtils::get_header(ObIAllocator &allocator, 
                              const ObAIFuncExprInfo &info,
                              const share::ObAiModelEndpointInfo &endpoint_info,
                              ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  ObString unencrypted_access_key;
  if (OB_FAIL(endpoint_info.get_unencrypted_access_key(allocator, unencrypted_access_key))) {
  } else if (ObAIFuncUtils::is_completion_type(&info)) {
    ObAIFuncIComplete *complete_provider = nullptr;
    if (OB_FAIL(get_complete_provider(allocator, endpoint_info, complete_provider))) {
    } else if (OB_FAIL(complete_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_dense_embedding_type(&info)) {
    ObAIFuncIEmbed *embed_provider = nullptr;
    if (OB_FAIL(get_embed_provider(allocator, endpoint_info, embed_provider))) {
    } else if (OB_FAIL(embed_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_rerank_type(&info)) {
    ObAIFuncIRerank *rerank_provider = nullptr;
    if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
    } else if (OB_FAIL(rerank_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_body(ObIAllocator &allocator, 
                                    const ObAIFuncExprInfo &info, 
                                    const share::ObAiModelEndpointInfo &endpoint_info,
                                    ObString &prompt, 
                                    ObString &content, 
                                    ObJsonObject *config,
                                    ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info, complete_provider))) {
  } else if (OB_FAIL(complete_provider->get_body(allocator, request_model_name, prompt, content, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::set_json_format_config(ObIAllocator &allocator, const ObString &provider, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, provider, complete_provider))) {
  } else if (OB_FAIL(complete_provider->set_config_json_format(allocator, config))) {
  } 
  return ret;
}

int ObAIFuncUtils::get_embed_body(ObIAllocator &allocator, 
                                  const ObAIFuncExprInfo &info, 
                                  const share::ObAiModelEndpointInfo &endpoint_info,
                                  ObArray<ObString> &contents, 
                                  ObJsonObject *config,
                                  ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info, embed_provider))) {
  } else if (OB_FAIL(embed_provider->get_body(allocator, request_model_name, contents, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_body(ObIAllocator &allocator, 
                                   const ObAIFuncExprInfo &info,
                                   const share::ObAiModelEndpointInfo &endpoint_info,
                                   ObString &query,
                                   ObJsonArray *document_array,
                                   ObJsonObject *config,
                                   ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->get_body(allocator, request_model_name, query, document_array, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_complete_output(ObIAllocator &allocator, 
                                        const share::ObAiModelEndpointInfo &endpoint_info,
                                        ObJsonObject *http_response,
                                        ObIJsonBase *&result)                                                 
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info, complete_provider))) {
  } else if (OB_FAIL(complete_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_embed_output(ObIAllocator &allocator, 
                                      const share::ObAiModelEndpointInfo &endpoint_info,
                                      ObJsonObject *http_response,
                                      ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info, embed_provider))) {
  } else if (OB_FAIL(embed_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_rerank_output(ObIAllocator &allocator, 
                                       const share::ObAiModelEndpointInfo &endpoint_info,
                                       ObJsonObject *http_response,
                                       ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object(ObIAllocator &allocator, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObJsonObject *j_obj = OB_NEWx(ObJsonObject, &allocator, &allocator);
  if (OB_ISNULL(j_obj)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    obj_node = j_obj;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_array(ObIAllocator &allocator, ObJsonArray *&array_node)
{
  int ret = OB_SUCCESS;
  ObJsonArray *j_array = OB_NEWx(ObJsonArray, &allocator, &allocator);
  if (OB_ISNULL(j_array)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    array_node = j_array; 
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_string(ObIAllocator &allocator, ObString &str, ObJsonString *&str_node)
{
  int ret = OB_SUCCESS;
  ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator,str);
  if (OB_ISNULL(j_str)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    str_node = j_str;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_int(ObIAllocator &allocator, int64_t num, ObJsonInt *&int_node)
{
  int ret = OB_SUCCESS;
  ObJsonInt *j_int = OB_NEWx(ObJsonInt, &allocator, num);
  if (OB_ISNULL(j_int)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int_node = j_int;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_boolean(ObIAllocator &allocator, bool value, ObJsonBoolean *&boolean_node)
{
  int ret = OB_SUCCESS;
  ObJsonBoolean *j_bool = OB_NEWx(ObJsonBoolean, &allocator, value);
  if (OB_ISNULL(j_bool)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    boolean_node = j_bool;
  }
  return ret;
}

int ObAIFuncJsonUtils::print_json_to_str(ObIAllocator &allocator, ObIJsonBase *base_node, ObString &str)
{
  int ret = OB_SUCCESS;
  ObJsonBuffer j_buf(&allocator);
  if (OB_FAIL(base_node->print(j_buf, 0))) {
  } else {
    str = j_buf.string();
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object_form_str(ObIAllocator &allocator, ObString &str, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, str, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("json text is not an object", K(ret));
  } else {
    obj_node = static_cast<ObJsonObject *>(j_base);
  }
  return ret;
}

int ObAIFuncJsonUtils::compact_json_object(ObIAllocator &allocator, ObJsonObject *obj_node, ObJsonObject *compact_obj)
{
  int ret = OB_SUCCESS;
  // add all members of obj_node to compact_obj
  if (OB_ISNULL(compact_obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(obj_node)) {
    // do nothing
  } else {
    ObString key;
    int64_t count = obj_node->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *j_node = obj_node->get_value(i);
      if (OB_ISNULL(j_node)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(obj_node->get_key(i, key))) {
      } else if (OB_FAIL(compact_obj->add(key, j_node))) {
      }
    }
  }
  return ret;
}

bool ObAIFuncJsonUtils::is_json_number(const ObIJsonBase *node)
{
  return OB_NOT_NULL(node) && node->is_json_number(node->json_type());
}

int ObAIFuncJsonUtils::get_json_integer(const ObIJsonBase *node, int64_t &value)
{
  int ret = OB_SUCCESS;
  double double_value = 0.0;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (ObJsonNodeType::J_INT == node->json_type()) {
    value = node->get_int();
  } else if (ObJsonNodeType::J_UINT == node->json_type()) {
    if (node->get_uint() > static_cast<uint64_t>(INT64_MAX)) {
      ret = OB_INVALID_DATA;
    } else {
      value = static_cast<int64_t>(node->get_uint());
    }
  } else if (!is_json_number(node)) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(node->to_double(double_value))) {
    ret = OB_INVALID_DATA;
  } else if (!(double_value >= static_cast<double>(INT64_MIN) && double_value < static_cast<double>(INT64_MAX))) {
    // also rejects nan
    ret = OB_INVALID_DATA;
  } else if (double_value != static_cast<double>(static_cast<int64_t>(double_value))) {
    // only an integral value like 1.0 is accepted as integer
    ret = OB_INVALID_DATA;
  } else {
    value = static_cast<int64_t>(double_value);
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_number(const ObIJsonBase *node, double &value)
{
  int ret = OB_SUCCESS;
  if (!is_json_number(node)) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(node->to_double(value))) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int ObAIFuncJsonUtils::check_embedding_array(const ObIJsonBase *embedding, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(embedding) || embedding->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
    LOG_WARN("embedding is not an array", K(ret));
  } else if (embedding->element_count() == 0) {
    ret = OB_INVALID_DATA;
    LOG_WARN("embedding is empty", K(ret));
  } else {
    const ObJsonArray *embedding_array = static_cast<const ObJsonArray *>(embedding);
    for (uint64_t i = 0; OB_SUCC(ret) && i < embedding_array->element_count(); i++) {
      if (!is_json_number((*embedding_array)[i])) {
        ret = OB_INVALID_DATA;
        LOG_WARN("embedding element is not a number", K(ret), K(i));
      }
    }
    if (OB_SUCC(ret)) {
      dimension = embedding_array->element_count();
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::check_embedding_list(ObJsonArray *embeddings)
{
  int ret = OB_SUCCESS;
  int64_t expect_dim = 0;
  if (OB_ISNULL(embeddings) || embeddings->element_count() == 0) {
    ret = OB_INVALID_DATA;
    LOG_WARN("no embedding in response", K(ret));
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < embeddings->element_count(); i++) {
    int64_t dim = 0;
    if (OB_FAIL(check_embedding_array(embeddings->get_value(i), dim))) {
    } else if (0 == expect_dim) {
      expect_dim = dim;
    } else if (dim != expect_dim) {
      ret = OB_INVALID_DATA;
      LOG_WARN("embeddings have different dimensions", K(ret), K(i), K(dim), K(expect_dim));
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::collect_embeddings_by_index(ObIAllocator &allocator, ObJsonArray *items,
                                                   const char *index_key, const char *embedding_key,
                                                   ObJsonArray *&result)
{
  int ret = OB_SUCCESS;
  const int64_t count = OB_ISNULL(items) ? 0 : items->element_count();
  ObJsonNode **slots = nullptr;
  ObJsonArray *result_array = nullptr;
  int64_t indexed_count = 0;
  if (OB_ISNULL(items) || OB_ISNULL(index_key) || OB_ISNULL(embedding_key)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("no embedding in response", K(ret));
  } else if (OB_ISNULL(slots = static_cast<ObJsonNode **>(allocator.alloc(sizeof(ObJsonNode *) * count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMSET(slots, 0, sizeof(ObJsonNode *) * count);
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *item = items->get_value(i);
      ObJsonNode *embedding = nullptr;
      ObJsonNode *index_node = nullptr;
      int64_t index = i;
      if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("embedding item is not an object", K(ret), K(i));
      } else if (OB_ISNULL(embedding = static_cast<ObJsonObject *>(item)->get_value(embedding_key))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("embedding item has no embedding", K(ret), K(i));
      } else if (OB_ISNULL(index_node = static_cast<ObJsonObject *>(item)->get_value(index_key))) {
        // without index the items keep the request order
      } else if (OB_FAIL(get_json_integer(index_node, index))) {
        LOG_WARN("embedding index is not an integer", K(ret), K(i));
      } else if (index < 0 || index >= count) {
        ret = OB_INVALID_DATA;
        LOG_WARN("embedding index out of range", K(ret), K(index), K(count));
      } else {
        indexed_count++;
      }
      if (OB_FAIL(ret)) {
      } else if (OB_NOT_NULL(slots[index])) {
        ret = OB_INVALID_DATA;
        LOG_WARN("duplicated embedding index", K(ret), K(index));
      } else {
        slots[index] = embedding;
      }
    }
    if (OB_SUCC(ret) && indexed_count != 0 && indexed_count != count) {
      ret = OB_INVALID_DATA;
      LOG_WARN("only part of embedding items have index", K(ret), K(indexed_count), K(count));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      if (OB_ISNULL(slots[i])) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(result_array->append(slots[i]))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(check_embedding_list(result_array))) {
    } else {
      result = result_array;
    }
  }
  return ret;
}

int ObAIFuncPromptUtils::replace_meta_prompt(ObIAllocator &allocator, ObString &meta_prompt, ObString &key, ObString &content, ObString &result)
{
  int ret = OB_SUCCESS;
  if (meta_prompt.empty() || key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *key_pos = nullptr;
    if (OB_ISNULL(key_pos = STRSTR(meta_prompt.ptr(), key.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t before_key_len = key_pos - meta_prompt.ptr();
      int64_t after_key_len = meta_prompt.length() - before_key_len - key.length();
      int64_t new_len = before_key_len + content.length() + after_key_len;
      char *new_str = static_cast<char *>(allocator.alloc(new_len));
      if (OB_ISNULL(new_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(new_str, meta_prompt.ptr(), before_key_len);
        MEMCPY(new_str + before_key_len, content.ptr(), content.length());
        MEMCPY(new_str + before_key_len + content.length(), key_pos + key.length(), after_key_len);
        result.assign_ptr(new_str, static_cast<ObString::obstr_size_t>(new_len));
      }
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::transform_array_to_json_array(ObIAllocator &allocator, ObArray<ObString> &contents, ObJsonArray *&array)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(array = OB_NEWx(ObJsonArray, &allocator, &allocator))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator, content);
      if (OB_ISNULL(j_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        array->append(j_str);
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIComplete *&complete_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::DEEPSEEK)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)) {
    complete_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    complete_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    complete_provider = OB_NEWx(ObOllamaUtils::ObOllamaComplete, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(complete_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_embed_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIEmbed *&embed_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)) {
    embed_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    embed_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    embed_provider = OB_NEWx(ObOllamaUtils::ObOllamaEmbed, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(embed_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIRerank *&rerank_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
          || ob_provider_check(provider, ObAIFuncProviderUtils::COHERE)) {
    rerank_provider = OB_NEWx(ObSiliconflowUtils::ObSiliconflowRerank, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    rerank_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeRerank, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "rerank support siliconflow, cohere and ailiyun-dashscope, this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(rerank_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_complete_provider(ObIAllocator &allocator,
                                         const share::ObAiModelEndpointInfo &endpoint_info,
                                         ObAIFuncIComplete *&complete_provider)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
  } else if (ob_provider_check(endpoint_info.get_provider(), ObAIFuncProviderUtils::OLLAMA)) {
    static_cast<ObOllamaUtils::ObOllamaComplete *>(complete_provider)->set_use_chat_api(
        ObOllamaUtils::is_chat_api(endpoint_info.get_url()));
  }
  return ret;
}

int ObAIFuncUtils::get_embed_provider(ObIAllocator &allocator,
                                      const share::ObAiModelEndpointInfo &endpoint_info,
                                      ObAIFuncIEmbed *&embed_provider)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
  } else if (ob_provider_check(endpoint_info.get_provider(), ObAIFuncProviderUtils::OLLAMA)) {
    static_cast<ObOllamaUtils::ObOllamaEmbed *>(embed_provider)->set_use_legacy_api(
        ObOllamaUtils::is_legacy_embed_api(endpoint_info.get_url()));
  }
  return ret;
}

int64_t ObAIFuncUtils::get_embed_max_batch_size(const share::ObAiModelEndpointInfo &endpoint_info)
{
  int64_t batch_size = DEFAULT_EMBED_BATCH_SIZE;
  if (ob_provider_check(endpoint_info.get_provider(), ObAIFuncProviderUtils::OLLAMA)
      && ObOllamaUtils::is_legacy_embed_api(endpoint_info.get_url())) {
    batch_size = 1;
  }
  return batch_size;
}

int ObAIFuncUtils::check_rerank_results(ObJsonArray *results, const int64_t doc_count)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_allocator("AIRerankChk");
  bool *seen = nullptr;
  if (OB_ISNULL(results) || results->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else if (doc_count <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (results->element_count() != doc_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("rerank result count mismatch", K(ret), K(results->element_count()), K(doc_count));
  } else if (OB_ISNULL(seen = static_cast<bool *>(tmp_allocator.alloc(sizeof(bool) * doc_count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMSET(seen, 0, sizeof(bool) * doc_count);
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_count; i++) {
      ObJsonNode *item = results->get_value(i);
      int64_t index = 0;
      double score = 0.0;
      if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("rerank result is not an object", K(ret), K(i));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_integer(static_cast<ObJsonObject *>(item)->get_value("index"), index))) {
        LOG_WARN("rerank result has no valid index", K(ret), K(i));
      } else if (index < 0 || index >= doc_count) {
        ret = OB_INVALID_DATA;
        LOG_WARN("rerank result index out of range", K(ret), K(index), K(doc_count));
      } else if (seen[index]) {
        ret = OB_INVALID_DATA;
        LOG_WARN("duplicated rerank result index", K(ret), K(index));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number(static_cast<ObJsonObject *>(item)->get_value("relevance_score"), score))) {
        LOG_WARN("rerank result has no valid relevance_score", K(ret), K(i));
      } else {
        seen[index] = true;
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::check_completion_config(ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(config)) {
    ObJsonNode *temperature = config->get_value("temperature");
    ObJsonNode *top_p = config->get_value("top_p");
    double number = 0.0;
    if (OB_NOT_NULL(temperature)
        && (OB_SUCCESS != ObAIFuncJsonUtils::get_json_number(temperature, number) || number < 0 || number > 2)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, temperature must be a number between 0 and 2");
    } else if (OB_NOT_NULL(top_p)
               && (OB_SUCCESS != ObAIFuncJsonUtils::get_json_number(top_p, number) || number < 0 || number > 1)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, top_p must be a number between 0 and 1");
    } else {
      const char *TOKEN_KEYS[] = {"max_tokens", "max_completion_tokens"};
      for (int64_t i = 0; OB_SUCC(ret) && i < ARRAYSIZEOF(TOKEN_KEYS); i++) {
        ObJsonNode *max_tokens = config->get_value(TOKEN_KEYS[i]);
        int64_t tokens = 0;
        if (OB_NOT_NULL(max_tokens)
            && (OB_SUCCESS != ObAIFuncJsonUtils::get_json_integer(max_tokens, tokens) || tokens <= 0)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, max_tokens must be a positive integer");
        }
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_completion(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info is null");
  } else if (!is_completion_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_dense_embedding(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info is null");
  } else if (!is_dense_embedding_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_rerank(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info is null");
  } else if (!is_rerank_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  }
  return ret;
}

int ObAIFuncUtils::set_string_result(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res, ObString &res_str)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res);
  int64_t res_len = res_str.length();
  if (OB_FAIL(text_result.init(res_len))) {
  } else if (OB_FAIL(text_result.append(res_str))) {
  } else {
    text_result.set_result();
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id,
                                    share::schema::ObSchemaGetterGuard &guard,
                                    ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, model_id is empty");
  } else {
    ObAIFuncExprInfo *info_obj = OB_NEWx(ObAIFuncExprInfo, (&allocator), allocator, T_FUN_SYS_AI_COMPLETE);
    if (OB_ISNULL(info_obj)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(info_obj->init(allocator, model_id, guard))) {
    } else {
      info = info_obj;
    }
  }
  return ret;
}

bool ObAIFuncUtils::is_provider_support_base64(const ObString &provider)
{
  return ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW);
}

int ObAIFuncUtils::decode_base64_embedding_array(const ObIJsonBase &embedding_jbase,
                                                 ObIAllocator &allocator,
                                                 const int64_t dimension,
                                                 float *&vector)
{
  int ret = OB_SUCCESS;
  if (embedding_jbase.json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *encoded_embedding = embedding_jbase.get_data();
    uint64_t encoded_embedding_len = embedding_jbase.get_data_length();
    uint64_t decoded_buf_len = ObBase64Encoder::needed_decoded_length(encoded_embedding_len);
    uint8_t *decoded_buf = nullptr;
    int64_t pos = 0;
    if (decoded_buf_len <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(decoded_buf = static_cast<uint8_t *>(allocator.alloc(decoded_buf_len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(ObBase64Encoder::decode(encoded_embedding, encoded_embedding_len,
                                               decoded_buf, decoded_buf_len, pos))) {
    } else if (pos != dimension * sizeof(float)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      vector = reinterpret_cast<float *>(decoded_buf);
    }
  }
  return ret;
}

int ObAIFuncUtils::decode_float_embedding_array(const ObIJsonBase &embedding_jbase,
                                                ObIAllocator &allocator,
                                                share::ObJsonReaderHelper &json_reader,
                                                const int64_t dimension,
                                                float *&vector)
{
  int ret = OB_SUCCESS;
  float *tmp_vector = nullptr;
  if (!share::ObJsonHelper::is_array_type(&embedding_jbase)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint64_t embedding_size = json_reader.get_array_size(&embedding_jbase);
    if (embedding_size != dimension) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      tmp_vector = static_cast<float*>(allocator.alloc(dimension * sizeof(float)));
      if (OB_ISNULL(tmp_vector)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (uint64_t i = 0; i < dimension && OB_SUCC(ret); i++) {
          ObIJsonBase *value = nullptr;
          if (OB_FAIL(json_reader.get_array_element(&embedding_jbase, i, value))) {
          } else if (!share::ObJsonHelper::is_number_type(value)) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            float f_value = 0.0;
            if (OB_FAIL(json_reader.get_float_value(value, f_value))) {
            } else {
              tmp_vector[i] = f_value;
            }
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    vector = tmp_vector;
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id, ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  share::schema::ObSchemaGetterGuard guard;
  
  if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(get_ai_func_info(allocator, model_id, guard, info))) {
  }
  return ret;
}

void ObAIFuncModel::forward_invalid_response_error(const int ret, ObJsonObject *response)
{
  if (OB_INVALID_DATA == ret) {
    ObString response_str;
    if (OB_NOT_NULL(response)
        && OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %.*s",
               response_str.length(), response_str.ptr());
      FORWARD_USER_ERROR(ret, http_message_str);
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  }
}

int ObAIFuncModel::call_completion(ObString &prompt, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_, complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
  } else if (OB_ISNULL(result_base) || result_base->json_type() != ObJsonNodeType::J_STRING
             || result_base->get_data_length() == 0) {
    // the completion must be a non-empty string
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, result_base, result_str))) {
  } else {
    result = result_str;
  }
  forward_invalid_response_error(ret, response);
  return ret;
}

int ObAIFuncModel::call_completion_vector(ObArray<ObString> &prompts, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_, complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < prompts.count(); i++) {
      ObString prompt = prompts[i];
      if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding(ObString &content, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> contents;
  ObArray<ObString> results;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
  } else if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(contents.push_back(content))) {
  } else if (OB_FAIL(call_dense_embedding_vector_v2(contents, config, results))) {
  } else if (results.count() != 1) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result = results[0];
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding_vector(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIEmbed *embed_provider = nullptr;
  ObAIFuncClient client;
  ObString request_model_name = get_request_model_name();
  ObString unencrypted_access_key;
  if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_, embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObArray<ObString> content_array;
      if (OB_FAIL(content_array.push_back(content))) {
      } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, content_array, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}


int ObAIFuncModel::call_dense_embedding_request(ObAIFuncIEmbed *embed_provider,
                                                ObArray<ObString> &headers,
                                                ObString &request_model_name,
                                                ObArray<ObString> &contents,
                                                ObJsonObject *config,
                                                const int64_t dimension,
                                                ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncClient client;
  if (OB_ISNULL(embed_provider) || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, contents, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
  } else if (OB_ISNULL(result_base) || result_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonArray *result_array = static_cast<ObJsonArray *>(result_base);
    const int64_t count = result_array->element_count();
    if (contents.count() != count) {
      // every input must get exactly one embedding, otherwise rows can not be mapped back
      ret = OB_INVALID_DATA;
      LOG_WARN("embedding count mismatch", K(ret), K(contents.count()), K(count));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObIJsonBase *j_base = result_array->get_value(i);
        ObString result_str;
        int64_t result_dim = 0;
        if (OB_FAIL(ObAIFuncJsonUtils::check_embedding_array(j_base, result_dim))) {
        } else if (dimension > 0 && result_dim != dimension) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, result dimension is not equal to dimension");
        } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, j_base, result_str))) {
        } else if (OB_FAIL(results.push_back(result_str))) {
        }
      }
    }
  }
  forward_invalid_response_error(ret, response);
  return ret;
}

int ObAIFuncModel::call_dense_embedding_vector_v2(ObArray<ObString> &content, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObAIFuncIEmbed *embed_provider = nullptr;
  int64_t dimension = 0;
  if (OB_NOT_NULL(config)) {
    ObJsonNode *dimension_node = config->get_value("dimensions");
    if (OB_ISNULL(dimension_node)) {
      // do nothing
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_integer(dimension_node, dimension)) || dimension <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    }
  }
  
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (OB_FAIL(ret)) {
  } else if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_, embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (ObAIFuncUtils::get_embed_max_batch_size(endpoint_info_) == 1 && content.count() > 1) {
    // the endpoint takes one input per request
    for (int64_t i = 0; OB_SUCC(ret) && i < content.count(); i++) {
      ObArray<ObString> single_content;
      if (OB_FAIL(single_content.push_back(content.at(i)))) {
      } else if (OB_FAIL(call_dense_embedding_request(embed_provider, headers, request_model_name,
                                                      single_content, config, dimension, results))) {
      }
    }
  } else if (OB_FAIL(call_dense_embedding_request(embed_provider, headers, request_model_name,
                                                  content, config, dimension, results))) {
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding_batch(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = ObAIFuncUtils::get_embed_max_batch_size(endpoint_info_);
  ObArray<ObString> batch_contents;
  ObArray<ObString> batch_results;
  results.reuse();
  for (int64_t start = 0; OB_SUCC(ret) && start < contents.count(); start += batch_size) {
    const int64_t end = MIN(start + batch_size, contents.count());
    batch_contents.reuse();
    batch_results.reuse();
    for (int64_t i = start; OB_SUCC(ret) && i < end; i++) {
      if (OB_FAIL(batch_contents.push_back(contents.at(i)))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(call_dense_embedding_vector_v2(batch_contents, config, batch_results))) {
    } else if (batch_results.count() != batch_contents.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(append(results, batch_results))) {
    }
  }
  return ret;
}

int ObAIFuncModel::call_rerank(ObString &query, ObJsonArray *contents, ObJsonArray *&results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIRerank *rerank_provider = nullptr;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_rerank_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  } else if (OB_ISNULL(contents) || contents->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncUtils::get_rerank_provider(*allocator_, endpoint_info_.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(rerank_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(rerank_provider->get_body(*allocator_, request_model_name, query, contents, nullptr, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(rerank_provider->parse_output(*allocator_, response, result_base))) {
  } else if (OB_ISNULL(result_base) || result_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncUtils::check_rerank_results(static_cast<ObJsonArray *>(result_base),
                                                         contents->element_count()))) {
    // count, index range, duplicated index and score type are checked before mapping
  } else {
    results = static_cast<ObJsonArray *>(result_base);
  }
  forward_invalid_response_error(ret, response);
  return ret;
}

bool ObAIFuncJsonUtils::ob_is_json_array_all_str(ObJsonArray* json_array)
{
  bool is_all_str = true;
  if (OB_ISNULL(json_array)) {
    is_all_str = false;
  }
  for (int64_t i = 0; is_all_str && i < json_array->element_count(); i++) {
    ObJsonNode *node = json_array->get_value(i);
    if (OB_ISNULL(node)) {
      is_all_str = false;
    } else if (node->json_type() != ObJsonNodeType::J_STRING) {
      is_all_str = false;
    }
  }
  return is_all_str;
}

int ObAIFuncPromptObjectUtils::construct_prompt_object(ObIAllocator &allocator, ObString &template_str, ObJsonArray *args_array, ObJsonObject *&prompt_object)
{
  INIT_SUCC(ret);
  ObJsonObject *prompt_obj = NULL;
  ObJsonString *template_json_str = NULL;
  if (template_str.empty() || args_array == NULL) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, template_str, template_json_str))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, prompt_obj))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_template_key, template_json_str))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_args_key, args_array))) {
  }
  if (OB_SUCC(ret)) {
    prompt_object = prompt_obj;
  }
  return ret;
}

bool ObAIFuncPromptObjectUtils::is_valid_prompt_object(ObJsonObject *prompt_object)
{
  bool is_valid = true;
  ObJsonNode *template_node = NULL;
  ObJsonNode *args_node = NULL;
  if (OB_ISNULL(prompt_object)) {
    is_valid = false;
  } else if (prompt_object->element_count() != 2) {
    is_valid = false;
  } else if (OB_ISNULL(template_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key)) ||
            (OB_ISNULL(args_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key)))) {
    is_valid = false;
  } else if (template_node->json_type() != ObJsonNodeType::J_STRING || args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    is_valid = false;
  }
  if (is_valid) {
    // args can be any json value: strings, numbers, arrays, objects and nested prompt objects
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    for (int64_t i = 0; is_valid && i < args_array->element_count(); i++) {
      if (OB_ISNULL(args_array->get_value(i))) {
        is_valid = false;
      }
    }
  }
  return is_valid;
}

int ObAIFuncPromptObjectUtils::render_prompt_arg(ObIAllocator &allocator, ObJsonNode *arg_node,
                                                 const int64_t depth, ObString &arg_str)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(arg_node)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (arg_node->json_type() == ObJsonNodeType::J_STRING) {
    // a string replaces the placeholder as is, without quotes
    arg_str = static_cast<ObJsonString *>(arg_node)->get_str();
  } else if (arg_node->json_type() == ObJsonNodeType::J_OBJECT
             && is_valid_prompt_object(static_cast<ObJsonObject *>(arg_node))) {
    // nested AI_PROMPT
    ObStringBuffer nested_buf(&allocator);
    if (OB_FAIL(render_prompt_object(allocator, static_cast<ObJsonObject *>(arg_node), depth + 1, nested_buf))) {
    } else if (OB_FAIL(nested_buf.get_result_string(arg_str))) {
      // take over the buffer, it is freed when nested_buf is destructed otherwise
    }
  } else {
    // other json values replace the placeholder with their json text, e.g. [1, true] or {"k": 2}
    ObJsonBuffer j_buf(&allocator);
    if (OB_FAIL(arg_node->print(j_buf, false))) {
    } else if (OB_FAIL(j_buf.get_result_string(arg_str))) {
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::render_prompt_object(ObIAllocator &allocator, ObJsonObject *prompt_object,
                                                    const int64_t depth, ObStringBuffer &result)
{
  INIT_SUCC(ret);
  ObJsonNode *template_node = NULL;
  ObJsonNode *args_node = NULL;
  ObString template_str;
  if (depth > MAX_PROMPT_NESTED_DEPTH) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: nested prompt is too deep");
  } else if (OB_ISNULL(prompt_object)
             || OB_ISNULL(template_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key))
             || OB_ISNULL(args_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key))
             || template_node->json_type() != ObJsonNodeType::J_STRING
             || args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FALSE_IT(template_str = static_cast<ObJsonString *>(template_node)->get_str())) {
  } else if (template_str.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    const int64_t args_count = args_array->element_count();
    // args are rendered once even if a placeholder is repeated
    ObString *rendered_args = NULL;
    bool *is_rendered = NULL;
    if (args_count > 0
        && (OB_ISNULL(rendered_args = static_cast<ObString *>(allocator.alloc(sizeof(ObString) * args_count)))
            || OB_ISNULL(is_rendered = static_cast<bool *>(allocator.alloc(sizeof(bool) * args_count))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (args_count > 0) {
      MEMSET(is_rendered, 0, sizeof(bool) * args_count);
      for (int64_t i = 0; i < args_count; i++) {
        new (rendered_args + i) ObString();
      }
    }
    const char *template_ptr = template_str.ptr();
    const int64_t template_len = template_str.length();
    int64_t literal_start = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < template_len; i++) {
      if (template_ptr[i] != '{') {
        continue;
      }
      // a placeholder is '{' digits '}', anything else is copied as is
      int64_t j = i + 1;
      while (j < template_len && template_ptr[j] >= '0' && template_ptr[j] <= '9') {
        j++;
      }
      if (j == i + 1 || j >= template_len || template_ptr[j] != '}') {
        continue;
      }
      int64_t index = 0;
      bool overflow = false;
      for (int64_t k = i + 1; k < j && !overflow; k++) {
        index = index * 10 + (template_ptr[k] - '0');
        overflow = index >= args_count;
      }
      if (overflow || index >= args_count) {
        ObString placeholder(static_cast<ObString::obstr_size_t>(j - i + 1), template_ptr + i);
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
        LOG_WARN("invalid placeholder index", K(ret), K(args_count), K(placeholder));
      } else if (!is_rendered[index]
                 && OB_FAIL(render_prompt_arg(allocator, args_array->get_value(index), depth, rendered_args[index]))) {
      } else if (OB_FALSE_IT(is_rendered[index] = true)) {
      } else if (OB_FAIL(result.append(template_ptr + literal_start, i - literal_start))) {
      } else if (OB_FAIL(result.append(rendered_args[index]))) {
      } else {
        i = j;
        literal_start = j + 1;
      }
    }
    if (OB_SUCC(ret) && literal_start < template_len
        && OB_FAIL(result.append(template_ptr + literal_start, template_len - literal_start))) {
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(ObIAllocator &allocator, ObJsonObject* prompt_object, ObString& replaced_prompt_str)
{
  INIT_SUCC(ret);
  ObStringBuffer result_buf(&allocator);
  if (OB_ISNULL(prompt_object)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(render_prompt_object(allocator, prompt_object, 0, result_buf))) {
  } else if (OB_FAIL(result_buf.get_result_string(replaced_prompt_str))) {
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::render_prompt(ObIAllocator &allocator, ObIJsonBase *prompt, ObString &prompt_str)
{
  INIT_SUCC(ret);
  ObIJsonBase *prompt_tree = prompt;
  if (OB_ISNULL(prompt)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (prompt->is_bin()
             && OB_FAIL(ObJsonBaseFactory::transform(&allocator, prompt, ObJsonInType::JSON_TREE, prompt_tree))) {
    LOG_WARN("failed to transform prompt to tree", K(ret));
  } else if (OB_ISNULL(prompt_tree)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (prompt_tree->json_type() == ObJsonNodeType::J_STRING) {
    prompt_str.assign_ptr(prompt_tree->get_data(), static_cast<ObString::obstr_size_t>(prompt_tree->get_data_length()));
  } else if (prompt_tree->json_type() != ObJsonNodeType::J_OBJECT
             || !is_valid_prompt_object(static_cast<ObJsonObject *>(prompt_tree))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "prompt is not valid");
  } else if (OB_FAIL(replace_all_str_args_in_template(allocator, static_cast<ObJsonObject *>(prompt_tree), prompt_str))) {
  }
  return ret;
}

const ObString ObAIFuncModel::get_request_model_name()
{
  ObString request_model_name = info_.model_;
  if (!endpoint_info_.get_request_model_name().empty()) {
    request_model_name = endpoint_info_.get_request_model_name();
  }
  return request_model_name;
}

} // namespace common

namespace query
{

int ObAIModelResolver::resolve_model_name(
    common::ObIAllocator &allocator,
    const common::ObString &model_id,
    common::ObString &model_name)
{
  int ret = OB_SUCCESS;
  common::ObAIFuncExprInfo *info = nullptr;
  if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(
          allocator, model_id, info))) {
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_string(allocator, info->model_, model_name))) {
  }
  return ret;
}

} // namespace query
} // namespace oceanbase
