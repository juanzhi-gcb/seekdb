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

#define USING_LOG_PREFIX SERVER

#include <ctype.h>
#include <cmath>
#include <string.h>
#include "observer/ai_service/ob_ai_column_worker.h"

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/object/ob_object.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_mod_define.h"
#include "lib/utility/ob_smart_var.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_ai_column_spec.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

struct AiColumnTask
{
  AiColumnTask()
    : table_id_(OB_INVALID_ID), column_id_(OB_INVALID_ID), rowkey_predicate_(),
      database_name_(), table_name_(), column_name_(), request_id_(), request_text_(),
      request_is_null_(false), retry_count_(0), model_name_(), function_type_(0),
      result_type_(AiColumnSpec::RT_STRING), max_retries_(0), embed_dim_(0),
      state_column_name_(), request_column_name_(), ai_result_(), ai_succeeded_(false)
  {}
  uint64_t table_id_;
  uint64_t column_id_;
  ObString rowkey_predicate_;
  ObString database_name_;
  ObString table_name_;
  ObString column_name_;
  ObString request_id_;
  ObString request_text_;
  bool request_is_null_;
  int64_t retry_count_;
  ObString model_name_;
  int64_t function_type_;
  int64_t result_type_;
  int64_t max_retries_;
  int64_t embed_dim_;
  ObString state_column_name_;
  ObString request_column_name_;
  ObString ai_result_;
  bool ai_succeeded_;
};

namespace
{

const char *AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
const char *AI_COLUMN_TABLE_NAME = "oceanbase.__all_ai_column";
// One do_work() round handles at most this many requests so that a burst of
// user statements cannot starve the idle waits.
const int64_t AI_WORKER_MAX_REQUEST_PER_ROUND = 16;
// The number of AI columns one row can normally carry is small; the limit only
// protects the worker against corrupted task rows.
const int64_t AI_WORKER_MAX_TASK_PER_REQUEST = 128;
const int64_t AI_WORKER_RETRY_DELAY_SEC = 1;

const char *AI_JSON_SYSTEM_PROMPT =
    "You convert the user input into a single valid JSON value. "
    "Return only the JSON value, without explanations or code fences.";
const char *AI_INTEGER_SYSTEM_PROMPT =
    "You answer the user input with a single base-10 integer. "
    "Return only the integer, without explanations or units.";
const char *AI_STRING_SYSTEM_PROMPT =
    "You answer the user input directly. Return only the answer text.";

int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int append_string_literal(ObIAllocator &alloc, const ObString &value, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  int64_t buf_len = 0;
  int64_t pos = 0;
  ObObj obj;
  ObObjPrintParams print_params;
  obj.set_varchar(value);
  obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, alloc, print_params))) {
    LOG_WARN("failed to print sql literal", K(ret));
  } else if (OB_FAIL(sql.append(buf, pos))) {
    LOG_WARN("failed to append sql literal", K(ret));
  }
  return ret;
}

// Compare the generation stored in the user row itself. A task-table subquery
// could be evaluated before waiting for a concurrent row update to commit.
int append_task_guard(ObIAllocator &alloc, const AiColumnTask &task, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObString generation_column;
  if (OB_FAIL(AiColumnSpec::build_spec_column_name(alloc, task.column_name_, generation_column))) {
  } else if (OB_FAIL(append_identifier(sql, generation_column))) {
  } else if (OB_FAIL(sql.append(" <=> "))) {
  } else if (OB_FAIL(append_string_literal(alloc, task.request_id_, sql))) {
  }
  return ret;
}

// Returns the [begin, end) range of the text without the surrounding blanks.
void trim_range(const ObString &text, int64_t &begin, int64_t &end)
{
  begin = 0;
  end = text.length();
  while (begin < end && isspace(static_cast<unsigned char>(text.ptr()[begin]))) {
    ++begin;
  }
  while (end > begin && isspace(static_cast<unsigned char>(text.ptr()[end - 1]))) {
    --end;
  }
}

bool is_integer_text(const ObString &text)
{
  int64_t begin = 0;
  int64_t end = 0;
  trim_range(text, begin, end);
  bool bret = begin < end;
  if (bret && ('+' == text.ptr()[begin] || '-' == text.ptr()[begin])) {
    ++begin;
    bret = begin < end;
  }
  for (int64_t i = begin; bret && i < end; ++i) {
    bret = text.ptr()[i] >= '0' && text.ptr()[i] <= '9';
  }
  return bret;
}

int get_uint64_field(ObMySQLResult &result, const char *name, uint64_t &value)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  if (OB_FAIL(result.get_obj(name, obj))) {
    LOG_WARN("failed to get field", K(ret), K(name));
  } else if (obj.is_null()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null field", K(ret), K(name));
  } else {
    value = static_cast<uint64_t>(obj.get_int());
  }
  return ret;
}

int get_int_field(ObMySQLResult &result, const char *name, int64_t &value)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  if (OB_FAIL(result.get_obj(name, obj))) {
    LOG_WARN("failed to get field", K(ret), K(name));
  } else if (obj.is_null()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null field", K(ret), K(name));
  } else {
    value = obj.get_int();
  }
  return ret;
}

int get_string_field(ObIAllocator &alloc, ObMySQLResult &result, const char *name,
                     ObString &value, bool &is_null)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  if (OB_FAIL(result.get_obj(name, obj))) {
    LOG_WARN("failed to get field", K(ret), K(name));
  } else if (obj.is_null()) {
    is_null = true;
    value.reset();
  } else if (OB_FAIL(ob_write_string(alloc, obj.get_string(), value))) {
    LOG_WARN("failed to copy field", K(ret), K(name));
  } else {
    is_null = false;
  }
  return ret;
}

int get_string_field(ObIAllocator &alloc, ObMySQLResult &result, const char *name,
                     ObString &value)
{
  bool is_null = false;
  return get_string_field(alloc, result, name, value, is_null);
}

const char *system_prompt_for_result_type(const int64_t result_type)
{
  const char *prompt = AI_STRING_SYSTEM_PROMPT;
  if (AiColumnSpec::RT_JSON == result_type) {
    prompt = AI_JSON_SYSTEM_PROMPT;
  } else if (AiColumnSpec::RT_INTEGER == result_type) {
    prompt = AI_INTEGER_SYSTEM_PROMPT;
  }
  return prompt;
}

} // namespace

AiColumnWorker::AiColumnWorker()
  : is_inited_(false)
{
}

AiColumnWorker::~AiColumnWorker()
{
  destroy();
}

int AiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(create("AiColumnWorker", 1))) {
    LOG_WARN("failed to create ai column worker thread", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

void AiColumnWorker::destroy()
{
  if (is_inited_) {
    ObServerThreadHelper::stop();
    ObServerThreadHelper::wait();
    ObServerThreadHelper::destroy();
    is_inited_ = false;
  }
}

void AiColumnWorker::do_work()
{
  int ret = OB_SUCCESS;
  int64_t processed = 0;
  ObArenaAllocator allocator(ObModIds::OB_SQL_EXPR);
  while (OB_SUCC(ret) && processed < AI_WORKER_MAX_REQUEST_PER_ROUND) {
    int64_t task_count = 0;
    allocator.reuse();
    if (OB_FAIL(process_next_request(task_count))) {
      LOG_WARN("failed to process ai column tasks", K(ret));
    } else if (0 == task_count) {
      break;
    } else {
      ++processed;
    }
  }
  if (OB_FAIL(ret)) {
    idle(200 * 1000);
  } else if (0 == processed) {
    idle(100 * 1000);
  } else {
    idle(10 * 1000);
  }
}

int AiColumnWorker::process_next_request(int64_t &task_count)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObModIds::OB_SQL_EXPR);
  ObSqlString sql;
  ObString request_id;
  bool found = false;
  task_count = 0;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (OB_ISNULL(sql_proxy)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is not ready", K(ret));
  } else if (OB_FAIL(sql.assign_fmt(
      "SELECT request_id FROM %s WHERE state IN (%ld, %ld) AND eligible_ts <= now(6) "
      "ORDER BY request_id LIMIT 1",
      AI_TASK_TABLE_NAME,
      static_cast<int64_t>(AiColumnSpec::TS_PENDING),
      static_cast<int64_t>(AiColumnSpec::TS_RETRYING)))) {
    LOG_WARN("failed to build request query", K(ret));
  } else {
    HEAP_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy->read(res, sql.ptr()))) {
        LOG_WARN("failed to query ai column requests", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else {
        int next_ret = result->next();
        if (OB_ITER_END == next_ret) {
          found = false;
        } else if (OB_FAIL(next_ret)) {
          LOG_WARN("failed to iterate ai column requests", K(ret));
        } else if (OB_FAIL(get_string_field(allocator, *result, "request_id", request_id))) {
          LOG_WARN("failed to read request id", K(ret));
        } else {
          found = true;
        }
      }
    }
  }
  if (OB_SUCC(ret) && found) {
    ObSEArray<AiColumnTask, 8> tasks;
    if (OB_FAIL(sql.assign_fmt(
        "SELECT task.table_id, task.column_id, task.rowkey_predicate, task.database_name, "
        "task.table_name, task.column_name, task.request_id, task.request_text, "
        "task.retry_count, col.model_name, col.function_type, col.result_type, "
        "col.max_retries, col.embed_dim "
        "FROM %s task JOIN %s col ON col.table_id = task.table_id "
        "AND col.column_id = task.column_id "
        "WHERE task.request_id = '%.*s' AND task.state IN (%ld, %ld) "
        "AND task.eligible_ts <= now(6)",
        AI_TASK_TABLE_NAME, AI_COLUMN_TABLE_NAME,
        request_id.length(), request_id.ptr(),
        static_cast<int64_t>(AiColumnSpec::TS_PENDING),
        static_cast<int64_t>(AiColumnSpec::TS_RETRYING)))) {
      LOG_WARN("failed to build task query", K(ret));
    } else {
      HEAP_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = nullptr;
        if (OB_FAIL(sql_proxy->read(res, sql.ptr()))) {
          LOG_WARN("failed to query ai column tasks", K(ret), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result is null", K(ret));
        } else {
          while (OB_SUCC(ret)) {
            int next_ret = result->next();
            if (OB_ITER_END == next_ret) {
              break;
            } else if (OB_FAIL(next_ret)) {
              LOG_WARN("failed to iterate ai column tasks", K(ret));
            } else if (tasks.count() >= AI_WORKER_MAX_TASK_PER_REQUEST) {
              LOG_WARN("too many tasks for one request, skip the rest", K(request_id));
              break;
            } else {
              AiColumnTask task;
              if (OB_FAIL(get_uint64_field(*result, "table_id", task.table_id_))) {
              } else if (OB_FAIL(get_uint64_field(*result, "column_id", task.column_id_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "rowkey_predicate",
                                                  task.rowkey_predicate_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "database_name",
                                                  task.database_name_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "table_name",
                                                  task.table_name_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "column_name",
                                                  task.column_name_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "request_id",
                                                  task.request_id_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "request_text",
                                                  task.request_text_,
                                                  task.request_is_null_))) {
              } else if (OB_FAIL(get_int_field(*result, "retry_count", task.retry_count_))) {
              } else if (OB_FAIL(get_string_field(allocator, *result, "model_name",
                                                  task.model_name_))) {
              } else if (OB_FAIL(get_int_field(*result, "function_type",
                                               task.function_type_))) {
              } else if (OB_FAIL(get_int_field(*result, "result_type", task.result_type_))) {
              } else if (OB_FAIL(get_int_field(*result, "max_retries", task.max_retries_))) {
              } else if (OB_FAIL(get_int_field(*result, "embed_dim", task.embed_dim_))) {
              } else if (OB_FAIL(tasks.push_back(task))) {
                LOG_WARN("failed to push task", K(ret));
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      task_count = tasks.count();
      if (tasks.empty()) {
        // The request row disappeared or lost its column definition between
        // the two queries.  Drop the leftovers so that they cannot starve the
        // other requests, and let the next round look at the next request.
        ObSqlString cleanup_sql;
        int64_t affected_rows = 0;
        if (OB_FAIL(cleanup_sql.assign_fmt("DELETE FROM %s WHERE request_id = '%.*s'",
                                           AI_TASK_TABLE_NAME,
                                           request_id.length(), request_id.ptr()))) {
          LOG_WARN("failed to build orphan task cleanup", K(ret));
        } else if (OB_FAIL(write_sql(cleanup_sql, affected_rows))) {
          LOG_WARN("failed to clean orphan ai column tasks", K(ret), K(cleanup_sql));
        } else {
          task_count = (affected_rows > 0) ? 1 : 0;
        }
      } else if (OB_FAIL(process_tasks(allocator, tasks))) {
        LOG_WARN("failed to process ai column request", K(ret), K(request_id));
      }
    }
  }
  return ret;
}

// Computes the model output for one task.  The result is written into the task
// allocator and the task is flagged as computed on success.
int AiColumnWorker::compute_task(ObIAllocator &alloc, AiColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver =
      share::server_service<query::ObIAiEndpointResolver>();
  if (task.request_is_null_) {
    // A null prompt produces a null result without touching the model.
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, task.model_name_, info))) {
    LOG_WARN("failed to get ai func info", K(ret), K(task.model_name_));
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai endpoint resolver is not ready", K(ret));
  } else if (OB_FAIL(resolver->resolve_by_model_name(task.model_name_, alloc, endpoint))) {
    LOG_WARN("failed to resolve ai endpoint", K(ret), K(task.model_name_));
  } else {
    ObAIFuncModel model(alloc, *info, endpoint);
    ObString prompt = task.request_text_;
    if (AiColumnSpec::FT_EMBED == task.function_type_) {
      ObJsonObject *config = nullptr;
      ObJsonInt *dimension = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(alloc, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(alloc, task.embed_dim_, dimension))) {
      } else if (OB_FAIL(config->add("dimensions", dimension))) {
      } else if (OB_FAIL(model.call_dense_embedding(prompt, config, task.ai_result_))) {
        LOG_WARN("failed to embed ai column prompt", K(ret), K(task.table_id_),
                 K(task.column_id_));
      }
    } else {
      const char *prompt_text = system_prompt_for_result_type(task.result_type_);
      ObString system_prompt(static_cast<int32_t>(strlen(prompt_text)),
                             const_cast<char *>(prompt_text));
      if (OB_FAIL(model.call_completion_with_system_prompt(system_prompt, prompt, nullptr,
                                                           task.ai_result_))) {
        LOG_WARN("failed to complete ai column prompt", K(ret), K(task.table_id_),
                 K(task.column_id_));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString validated;
    ret = append_task_result(alloc, task, validated);
  }
  return ret;
}

int AiColumnWorker::process_tasks(ObIAllocator &alloc, ObIArray<AiColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  bool has_success = false;
  bool updated = false;
  ObSqlString sql;
  // DDL may remove or rename a table while its model call is in flight. A
  // missing table cannot be marked FAILED and must not block the queue forever.
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  if (tasks.empty()) {
    return ret;
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    return OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    return ret;
  } else if (OB_FAIL(guard.get_table_schema(tasks.at(0).table_id_, table))) {
    return ret;
  }
  if (table == nullptr || table->is_in_recyclebin()) {
    int64_t affected = 0;
    const uint64_t tid = tasks.at(0).table_id_;
    OZ(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu", AI_TASK_TABLE_NAME, tid));
    OZ(write_sql(sql, affected));
    OZ(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu", AI_COLUMN_TABLE_NAME, tid));
    OZ(write_sql(sql, affected));
    return ret;
  }
  OZ(guard.get_database_schema(table->get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr) { ret = OB_ERR_UNEXPECTED; }
  for (int64_t i = tasks.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    AiColumnTask &task = tasks.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
    if (column == nullptr) {
      int64_t affected = 0;
      OZ(delete_task(alloc, task));
      OZ(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND column_id = %lu",
          AI_COLUMN_TABLE_NAME, task.table_id_, task.column_id_));
      OZ(write_sql(sql, affected));
      OZ(tasks.remove(i));
    } else {
      OZ(ob_write_string(alloc, database->get_database_name_str(), task.database_name_));
      OZ(ob_write_string(alloc, table->get_table_name_str(), task.table_name_));
      OZ(ob_write_string(alloc, column->get_column_name_str(), task.column_name_));
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  sql.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    AiColumnTask &task = tasks.at(i);
    if (OB_FAIL(AiColumnSpec::build_state_column_name(alloc, task.column_name_,
                                                      task.state_column_name_))) {
      LOG_WARN("failed to build state column name", K(ret), K(task.column_name_));
    } else if (OB_FAIL(AiColumnSpec::build_request_column_name(alloc, task.column_name_,
                                                               task.request_column_name_))) {
      LOG_WARN("failed to build request column name", K(ret), K(task.column_name_));
    } else if (OB_FAIL(compute_task(alloc, task))) {
      // A model failure is a normal retry condition, keep the other columns of
      // the same row going.
      LOG_WARN("ai column task failed", K(ret), K(task.table_id_), K(task.column_id_));
      ret = OB_SUCCESS;
      task.ai_succeeded_ = false;
    } else {
      task.ai_succeeded_ = true;
      has_success = true;
    }
  }
  if (OB_SUCC(ret) && has_success) {
    int64_t affected_rows = 0;
    if (OB_FAIL(build_apply_update(alloc, tasks, sql))) {
      LOG_WARN("failed to build result update", K(ret));
    } else if (OB_FAIL(write_sql(sql, affected_rows))) {
      LOG_WARN("failed to apply ai column results", K(ret), K(sql));
    } else {
      updated = true;
    }
  }
  // A write/type-conversion error must not leave the head request spinning
  // forever and starving all other rows. Use the same bounded retry policy.
  if (OB_FAIL(ret)) {
    ret = OB_SUCCESS;
    updated = false;
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      AiColumnTask &task = tasks.at(i);
      if (task.ai_succeeded_ && updated) {
        if (OB_FAIL(delete_task(alloc, task))) {
          LOG_WARN("failed to delete finished ai column task", K(ret), K(task.table_id_),
                   K(task.column_id_), K(task.request_id_));
        }
      } else if (OB_FAIL(handle_task_failure(alloc, task))) {
        LOG_WARN("failed to handle ai column task failure", K(ret), K(task.table_id_),
                 K(task.column_id_), K(task.request_id_));
      }
    }
  }
  return ret;
}

int AiColumnWorker::build_apply_update(ObIAllocator &alloc, const ObIArray<AiColumnTask> &tasks,
                                       ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  bool first = true;
  if (tasks.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(append_identifier(sql, tasks.at(0).database_name_))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_identifier(sql, tasks.at(0).table_name_))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    const AiColumnTask &task = tasks.at(i);
    if (!task.ai_succeeded_) {
      continue;
    }
    if (OB_FAIL(sql.append(first ? "" : ", "))) {
    } else if (OB_FAIL(append_identifier(sql, task.column_name_))) {
    } else if (OB_FAIL(sql.append(" = IF("))) {
    } else if (OB_FAIL(append_task_guard(alloc, task, sql))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_task_result(alloc, task, sql))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_identifier(sql, task.column_name_))) {
    } else if (OB_FAIL(sql.append("), "))) {
    } else if (OB_FAIL(append_identifier(sql, task.state_column_name_))) {
    } else if (OB_FAIL(sql.append(" = IF("))) {
    } else if (OB_FAIL(append_task_guard(alloc, task, sql))) {
    } else if (OB_FAIL(sql.append(", 'READY', "))) {
    } else if (OB_FAIL(append_identifier(sql, task.state_column_name_))) {
    } else if (OB_FAIL(sql.append(")"))) {
    } else {
      first = false;
    }
  }
  if (OB_SUCC(ret)) {
    if (first) {
      // no successful task, nothing to update
    } else if (OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_FAIL(sql.append(tasks.at(0).rowkey_predicate_))) {
    }
  }
  return ret;
}

int AiColumnWorker::append_task_result(ObIAllocator &alloc, const AiColumnTask &task,
                                       ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (task.request_is_null_) {
    ret = sql.append("NULL");
  } else if (AiColumnSpec::RT_INTEGER == task.result_type_) {
    int64_t begin = 0;
    int64_t end = 0;
    trim_range(task.ai_result_, begin, end);
    ObString text(end - begin, task.ai_result_.ptr() + begin);
    if (!is_integer_text(task.ai_result_)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("ai column result is not an integer", K(task.ai_result_));
    } else {
      const bool negative = text.ptr()[0] == '-';
      const uint64_t limit = negative ? (uint64_t(1) << 63) : uint64_t(INT64_MAX);
      uint64_t number = 0;
      for (int64_t i = (text.ptr()[0] == '-' || text.ptr()[0] == '+') ? 1 : 0;
           OB_SUCC(ret) && i < text.length(); ++i) {
        const uint64_t digit = text.ptr()[i] - '0';
        if (number > (limit - digit) / 10) { ret = OB_INVALID_DATA; }
        else { number = number * 10 + digit; }
      }
      if (OB_SUCC(ret)) { ret = sql.append(text.ptr(), text.length()); }
    }
  } else if (AiColumnSpec::RT_VECTOR == task.result_type_) {
    ObIJsonBase *value = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, task.ai_result_,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, value))) {
    } else if (OB_ISNULL(value) || value->json_type() != ObJsonNodeType::J_ARRAY
               || value->element_count() != task.embed_dim_) {
      ret = OB_INVALID_DATA;
    } else {
      ObJsonArray *vector = static_cast<ObJsonArray *>(value);
      for (uint64_t i = 0; OB_SUCC(ret) && i < vector->element_count(); ++i) {
        ObJsonNode *number = vector->get_value(i);
        double converted = 0;
        if (OB_ISNULL(number) || !number->is_number()
            || OB_FAIL(number->to_double(converted)) || !std::isfinite(converted)) {
          ret = OB_INVALID_DATA;
        }
      }
      // A native array literal keeps IF's result type compatible with VECTOR.
      // Only a validated numeric JSON array is allowed into SQL text.
      if (OB_SUCC(ret)) {
        ret = sql.append(task.ai_result_);
      }
    }
  } else {
    if (AiColumnSpec::RT_JSON == task.result_type_) {
      ObIJsonBase *value = nullptr;
      ret = ObJsonBaseFactory::get_json_base(&alloc, task.ai_result_,
          ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, value);
    }
    if (OB_SUCC(ret)) { ret = append_string_literal(alloc, task.ai_result_, sql); }
  }
  return ret;
}

int AiColumnWorker::write_sql(const ObSqlString &sql, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  affected_rows = 0;
  if (OB_ISNULL(sql_proxy)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is not ready", K(ret));
  } else if (OB_FAIL(sql_proxy->write(sql.string(), affected_rows))) {
    LOG_WARN("failed to execute sql", K(ret));
  }
  return ret;
}

int AiColumnWorker::delete_task(ObIAllocator &alloc, const AiColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt(
      "DELETE FROM %s WHERE table_id = %lu AND column_id = %lu "
      "AND rowkey_predicate = ",
      AI_TASK_TABLE_NAME, task.table_id_, task.column_id_))) {
  } else if (OB_FAIL(append_string_literal(alloc, task.rowkey_predicate_, sql))) {
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
  } else if (OB_FAIL(append_string_literal(alloc, task.request_id_, sql))) {
  } else if (OB_FAIL(write_sql(sql, affected_rows))) {
    LOG_WARN("failed to delete ai column task", K(ret), K(sql));
  }
  return ret;
}

int AiColumnWorker::handle_task_failure(ObIAllocator &alloc, const AiColumnTask &task)
{
  int ret = OB_SUCCESS;
  if (task.retry_count_ < task.max_retries_) {
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(sql.assign_fmt(
        "UPDATE %s SET state = %ld, retry_count = retry_count + 1, "
        "eligible_ts = date_add(now(6), interval %ld second) "
        "WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = ",
        AI_TASK_TABLE_NAME,
        static_cast<int64_t>(AiColumnSpec::TS_RETRYING), AI_WORKER_RETRY_DELAY_SEC,
        task.table_id_, task.column_id_))) {
    } else if (OB_FAIL(append_string_literal(alloc, task.rowkey_predicate_, sql))) {
    } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    } else if (OB_FAIL(append_string_literal(alloc, task.request_id_, sql))) {
    } else if (OB_FAIL(write_sql(sql, affected_rows))) {
      LOG_WARN("failed to retry ai column task", K(ret), K(sql));
    }
  } else {
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(sql.append("UPDATE "))) {
    } else if (OB_FAIL(append_identifier(sql, task.database_name_))) {
    } else if (OB_FAIL(sql.append("."))) {
    } else if (OB_FAIL(append_identifier(sql, task.table_name_))) {
    } else if (OB_FAIL(sql.append(" SET "))) {
    } else if (OB_FAIL(append_identifier(sql, task.state_column_name_))) {
    } else if (OB_FAIL(sql.append(" = IF("))) {
    } else if (OB_FAIL(append_task_guard(alloc, task, sql))) {
    } else if (OB_FAIL(sql.append(", 'FAILED', "))) {
    } else if (OB_FAIL(append_identifier(sql, task.state_column_name_))) {
    } else if (OB_FAIL(sql.append(") WHERE "))) {
    } else if (OB_FAIL(sql.append(task.rowkey_predicate_))) {
    } else if (OB_FAIL(write_sql(sql, affected_rows))) {
      LOG_WARN("failed to mark ai column as failed", K(ret), K(sql));
    } else if (OB_FAIL(delete_task(alloc, task))) {
      LOG_WARN("failed to delete failed ai column task", K(ret));
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
