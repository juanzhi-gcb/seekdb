/**
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
#pragma once

#include <stdlib.h>
#include <string.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// The AI column definition is carried by the hidden spec column.  It is stored
// in the column comment as a small, self describing key=value list.  Values are
// escaped so that a prompt expression (which may contain any SQL text) can be
// stored without ambiguity.
class AiColumnSpec
{
public:
  static constexpr const char *COMMENT_PREFIX = "AI_COLUMN_SPEC:";
  static constexpr const char *SLOT_STATE = "state";
  static constexpr const char *SLOT_REQUEST = "request";
  static constexpr const char *SLOT_SPEC = "spec";

  enum FunctionType
  {
    FT_COMPLETE = 0,
    FT_EMBED = 1,
  };
  enum ResultType
  {
    RT_JSON = 0,
    RT_INTEGER = 1,
    RT_VECTOR = 2,
    RT_STRING = 3,
  };
  enum TaskState
  {
    TS_PENDING = 1,
    TS_RETRYING = 3,
    TS_READY = 2,
    TS_FAILED = 4,
  };

  AiColumnSpec()
    : column_name_(), model_name_(), function_type_(FT_COMPLETE), result_type_(RT_STRING),
      prompt_expr_(), normalized_expr_(), sync_mode_(), max_retries_(3), embed_dim_(0),
      database_name_(), table_name_()
  {}

  void reset()
  {
    column_name_.reset();
    model_name_.reset();
    function_type_ = FT_COMPLETE;
    result_type_ = RT_STRING;
    prompt_expr_.reset();
    normalized_expr_.reset();
    sync_mode_.reset();
    max_retries_ = 3;
    embed_dim_ = 0;
    database_name_.reset();
    table_name_.reset();
  }

  bool is_valid() const
  {
    return !column_name_.empty() && !model_name_.empty() && !prompt_expr_.empty();
  }

  static int escape(ObIAllocator &alloc, const ObString &in, ObString &out)
  {
    int ret = OB_SUCCESS;
    char *buf = nullptr;
    int64_t pos = 0;
    if (in.empty()) {
      out.reset();
    } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(2 * in.length() + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
        const char c = in.ptr()[i];
        if ('\\' == c || ';' == c || '=' == c) {
          buf[pos++] = '\\';
        }
        buf[pos++] = c;
      }
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
    return ret;
  }

  // Helper used to avoid materializing a NUL terminated copy for every append.
  static int append_field(ObIAllocator &alloc, ObSqlString &buf, const char *key,
                          const ObString &value)
  {
    int ret = OB_SUCCESS;
    ObString escaped;
    if (OB_FAIL(escape(alloc, value, escaped))) {
    } else if (OB_FAIL(buf.append(key))) {
    } else if (OB_FAIL(buf.append("="))) {
    } else if (OB_FAIL(buf.append(escaped))) {
    } else if (OB_FAIL(buf.append(";"))) {
    }
    return ret;
  }

  int encode(ObIAllocator &alloc, ObString &out) const
  {
    int ret = OB_SUCCESS;
    ObSqlString buf;
    if (OB_FAIL(append_field(alloc, buf, "column_name", column_name_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "model_name", model_name_))) {
    } else if (OB_FAIL(buf.append_fmt("function_type=%ld;", function_type_))) {
    } else if (OB_FAIL(buf.append_fmt("result_type=%ld;", result_type_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "prompt_expr", prompt_expr_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "normalized_expr", normalized_expr_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "sync_mode", sync_mode_))) {
    } else if (OB_FAIL(buf.append_fmt("max_retries=%ld;", max_retries_))) {
    } else if (OB_FAIL(buf.append_fmt("embed_dim=%ld;", embed_dim_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "database_name", database_name_))) {
    } else if (OB_FAIL(append_field(alloc, buf, "table_name", table_name_))) {
    } else {
      const int64_t total = buf.length();
      char *p = nullptr;
      if (OB_ISNULL(p = static_cast<char *>(alloc.alloc(total + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(p, buf.ptr(), total);
        p[total] = '\0';
        out.assign_ptr(p, static_cast<int32_t>(total));
      }
    }
    return ret;
  }

  static int unescape_value(ObIAllocator &alloc, const char *str, int64_t len, ObString &out)
  {
    int ret = OB_SUCCESS;
    char *buf = nullptr;
    int64_t pos = 0;
    if (len <= 0) {
      out.reset();
    } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; i < len; ++i) {
        if ('\\' == str[i] && i + 1 < len) {
          buf[pos++] = str[++i];
        } else {
          buf[pos++] = str[i];
        }
      }
      buf[pos] = '\0';
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
    return ret;
  }

  static bool key_is(const char *key, int64_t key_len, const char *expected)
  {
    const int64_t expected_len = static_cast<int64_t>(strlen(expected));
    return key_len == expected_len && 0 == strncasecmp(key, expected, expected_len);
  }

  int decode(ObIAllocator &alloc, const ObString &in)
  {
    int ret = OB_SUCCESS;
    int64_t pos = 0;
    while (OB_SUCC(ret) && pos < in.length()) {
      int64_t eq = -1;
      int64_t end = in.length();
      bool esc = false;
      int64_t i = pos;
      for (; i < in.length(); ++i) {
        const char c = in.ptr()[i];
        if (esc) {
          esc = false;
        } else if ('\\' == c) {
          esc = true;
        } else if (';' == c) {
          end = i;
          break;
        } else if ('=' == c && -1 == eq) {
          eq = i;
        }
      }
      if (-1 != eq) {
        ObString value;
        const char *key = in.ptr() + pos;
        const int64_t key_len = eq - pos;
        const char *val = in.ptr() + eq + 1;
        const int64_t val_len = end - eq - 1;
        if (OB_FAIL(unescape_value(alloc, val, val_len, value))) {
        } else if (key_is(key, key_len, "column_name")) {
          column_name_ = value;
        } else if (key_is(key, key_len, "model_name")) {
          model_name_ = value;
        } else if (key_is(key, key_len, "function_type")) {
          function_type_ = atoi(value.ptr());
        } else if (key_is(key, key_len, "result_type")) {
          result_type_ = atoi(value.ptr());
        } else if (key_is(key, key_len, "prompt_expr")) {
          prompt_expr_ = value;
        } else if (key_is(key, key_len, "normalized_expr")) {
          normalized_expr_ = value;
        } else if (key_is(key, key_len, "sync_mode")) {
          sync_mode_ = value;
        } else if (key_is(key, key_len, "max_retries")) {
          max_retries_ = atoi(value.ptr());
        } else if (key_is(key, key_len, "embed_dim")) {
          embed_dim_ = atoi(value.ptr());
        } else if (key_is(key, key_len, "database_name")) {
          database_name_ = value;
        } else if (key_is(key, key_len, "table_name")) {
          table_name_ = value;
        }
      }
      pos = end + 1;
    }
    return ret;
  }

  static bool has_prefix(const ObString &comment)
  {
    const int64_t prefix_len = static_cast<int64_t>(strlen(COMMENT_PREFIX));
    return comment.length() > prefix_len
           && 0 == strncmp(comment.ptr(), COMMENT_PREFIX, prefix_len);
  }

  static int decode_from_comment(ObIAllocator &alloc, const ObString &comment, AiColumnSpec &spec)
  {
    int ret = OB_SUCCESS;
    const int64_t prefix_len = static_cast<int64_t>(strlen(COMMENT_PREFIX));
    if (!has_prefix(comment)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObString payload(comment.length() - static_cast<int32_t>(prefix_len),
                       comment.ptr() + prefix_len);
      ret = spec.decode(alloc, payload);
    }
    return ret;
  }

  static int build_state_column_name(common::ObIAllocator &alloc, const ObString &column_name,
                                     ObString &name)
  {
    return build_slot_name(alloc, SLOT_STATE, column_name, name);
  }
  static int build_request_column_name(common::ObIAllocator &alloc, const ObString &column_name,
                                       ObString &name)
  {
    return build_slot_name(alloc, SLOT_REQUEST, column_name, name);
  }
  static int build_spec_column_name(common::ObIAllocator &alloc, const ObString &column_name,
                                    ObString &name)
  {
    return build_slot_name(alloc, SLOT_SPEC, column_name, name);
  }

  static int build_slot_name(common::ObIAllocator &alloc, const char *slot,
                             const ObString &column_name, ObString &name)
  {
    int ret = OB_SUCCESS;
    const int64_t slot_len = static_cast<int64_t>(strlen(slot));
    int64_t base_len = column_name.length();
    const int64_t max_len = 64; // keep well below OB_MAX_COLUMN_NAME_LENGTH
    const int64_t overhead = 2 /* __ */ + slot_len + 1 /* _ */;
    if (base_len + overhead > max_len) {
      base_len = max_len - overhead;
    }
    const int64_t total = overhead + base_len;
    char *buf = static_cast<char *>(alloc.alloc(total + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      MEMCPY(buf + pos, "__", 2);
      pos += 2;
      MEMCPY(buf + pos, slot, slot_len);
      pos += slot_len;
      buf[pos++] = '_';
      MEMCPY(buf + pos, column_name.ptr(), base_len);
      pos += base_len;
      buf[pos] = '\0';
      name.assign_ptr(buf, static_cast<int32_t>(pos));
    }
    return ret;
  }

  TO_STRING_KV(K_(column_name), K_(model_name), K_(function_type), K_(result_type),
               K_(prompt_expr), K_(normalized_expr), K_(sync_mode), K_(max_retries),
               K_(embed_dim), K_(database_name), K_(table_name));

  ObString column_name_;
  ObString model_name_;
  int64_t function_type_;
  int64_t result_type_;
  ObString prompt_expr_;
  ObString normalized_expr_;
  ObString sync_mode_;
  int64_t max_retries_;
  int64_t embed_dim_;
  ObString database_name_;
  ObString table_name_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
