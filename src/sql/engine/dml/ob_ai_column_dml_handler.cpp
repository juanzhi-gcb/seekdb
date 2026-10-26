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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_dml_handler.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_ai_column_spec.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_mod_define.h"
#include "lib/time/ob_time_utility.h"

using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace sql
{

namespace
{

const char *AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
const int64_t AI_REQUEST_ID_BUF_LEN = 64;
const char *AI_PENDING_STATE = "PENDING";

struct AiColumnRuntimeInfo
{
  AiColumnRuntimeInfo()
    : spec_(), result_column_(nullptr), state_column_name_(), request_column_name_(),
      prompt_text_(), stored_request_(), prompt_is_null_(true), invalidate_(false)
  {}
  AiColumnSpec spec_;
  const ObColumnSchemaV2 *result_column_;
  ObString state_column_name_;
  ObString request_column_name_;
  ObString prompt_text_;
  ObString stored_request_;
  bool prompt_is_null_;
  bool invalidate_;
};

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

int append_string_literal(ObIAllocator &alloc,
                          const ObObjPrintParams &print_params,
                          const bool is_null,
                          const ObString &value,
                          ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  int64_t buf_len = 0;
  int64_t pos = 0;
  ObObj obj;
  if (is_null) {
    ret = sql.append("NULL");
  } else {
    obj.set_varchar(value);
    obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, alloc, print_params))) {
    } else if (OB_FAIL(sql.append(buf, pos))) {
    }
  }
  return ret;
}

int assign_cstring(ObIAllocator &alloc, const ObSqlString &sql, ObString &out)
{
  int ret = OB_SUCCESS;
  const int64_t len = sql.length();
  char *buf = static_cast<char *>(alloc.alloc(len + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, sql.ptr(), len);
    buf[len] = '\0';
    out.assign_ptr(buf, static_cast<int32_t>(len));
  }
  return ret;
}

int datum_to_literal(ObIAllocator &alloc,
                     ObDatum &datum,
                     const ObObjMeta &meta,
                     const ObObjPrintParams &print_params,
                     ObString &literal)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  int64_t buf_len = 0;
  int64_t pos = 0;
  ObObj obj;
  if (OB_FAIL(datum.to_obj(obj, meta))) {
  } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, alloc, print_params))) {
  } else {
    literal.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int find_column_index(const ObDMLBaseCtDef &dml_ctdef,
                      const uint64_t column_id,
                      int64_t &idx)
{
  int ret = OB_SUCCESS;
  idx = -1;
  for (int64_t i = 0; -1 == idx && i < dml_ctdef.column_ids_.count(); ++i) {
    if (dml_ctdef.column_ids_.at(i) == column_id) {
      idx = i;
    }
  }
  if (-1 == idx) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// Builds the "`pk1` = value1 and `pk2` = value2" predicate that identifies one
// row.  The very same text is stored as the task table rowkey and reused as the
// WHERE clause of the inner statements.
int build_rowkey_predicate(ObTableModifyOp &op,
                           const ObDMLBaseCtDef &dml_ctdef,
                           const ObTableSchema &table_schema,
                           const ExprFixedArray &row,
                           ObIAllocator &alloc,
                           ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSEArray<uint64_t, 4> rowkey_column_ids;
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  const ObObjPrintParams &print_params = op.get_obj_print_params();
  if (OB_FAIL(table_schema.get_rowkey_column_ids(rowkey_column_ids))) {
  } else if (rowkey_column_ids.empty()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("ai column requires the table to have a primary key", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_column_ids.count(); ++i) {
    const uint64_t column_id = rowkey_column_ids.at(i);
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_id);
    int64_t idx = -1;
    ObDatum *datum = nullptr;
    ObString literal;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(find_column_index(dml_ctdef, column_id, idx))) {
      LOG_WARN("rowkey column is missing from the dml ctdef", K(ret), K(column_id));
    } else if (idx >= row.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(row.at(idx)->eval(eval_ctx, datum))) {
    } else if (OB_ISNULL(datum) || datum->is_null()) {
      ret = OB_ERR_NULL_VALUE;
    } else if (OB_FAIL(datum_to_literal(alloc, *datum, row.at(idx)->obj_meta_,
                                        print_params, literal))) {
    } else if (!sql.empty() && OB_FAIL(sql.append(" and "))) {
    } else if (OB_FAIL(sql.append_fmt("`%.*s` = %.*s",
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr(),
                                      literal.length(), literal.ptr()))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(assign_cstring(alloc, sql, predicate))) {
    }
  }
  return ret;
}

int collect_ai_columns(ObIAllocator &alloc,
                       const ObTableSchema &table_schema,
                       ObIArray<AiColumnRuntimeInfo> &columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!column->is_hidden() || !AiColumnSpec::has_prefix(column->get_comment_str())) {
      // not a hidden AI spec column
    } else {
      AiColumnRuntimeInfo info;
      if (OB_FAIL(AiColumnSpec::decode_from_comment(alloc, column->get_comment_str(),
                                                    info.spec_))) {
      } else if (!info.spec_.is_valid()) {
        LOG_WARN("skip invalid ai column spec", K(info.spec_));
      } else if (OB_ISNULL(info.result_column_ =
                               table_schema.get_column_schema(info.spec_.column_name_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column result column not found", K(ret), K(info.spec_));
      } else if (OB_FAIL(AiColumnSpec::build_state_column_name(alloc, info.spec_.column_name_,
                                                              info.state_column_name_))) {
      } else if (OB_FAIL(AiColumnSpec::build_request_column_name(alloc, info.spec_.column_name_,
                                                                 info.request_column_name_))) {
      } else if (OB_FAIL(columns.push_back(info))) {
      }
    }
  }
  return ret;
}

int build_prompt_select(ObIAllocator &alloc,
                        const ObIArray<AiColumnRuntimeInfo> &columns,
                        const ObString &database_name,
                        const ObString &table_name,
                        const ObString &predicate,
                        ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString builder;
  if (OB_FAIL(builder.append("SELECT "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (OB_FAIL(builder.append_fmt("(%.*s), ",
                                   columns.at(i).spec_.prompt_expr_.length(),
                                   columns.at(i).spec_.prompt_expr_.ptr()))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (OB_FAIL(append_identifier(builder, columns.at(i).request_column_name_))) {
    } else if (OB_FAIL(builder.append(i + 1 < columns.count() ? ", " : " "))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(builder.append("FROM "))) {
    } else if (OB_FAIL(append_identifier(builder, database_name))) {
    } else if (OB_FAIL(builder.append("."))) {
    } else if (OB_FAIL(append_identifier(builder, table_name))) {
    } else if (OB_FAIL(builder.append(" WHERE "))) {
    } else if (OB_FAIL(builder.append(predicate))) {
    } else if (OB_FAIL(assign_cstring(alloc, builder, sql))) {
    }
  }
  return ret;
}

int build_invalidation_update(ObIAllocator &alloc,
                              const ObObjPrintParams &print_params,
                              const ObIArray<AiColumnRuntimeInfo> &columns,
                              const ObString &database_name,
                              const ObString &table_name,
                              const ObString &predicate,
                              const ObString &request_id,
                              ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString builder;
  bool first = true;
  if (OB_FAIL(builder.append("UPDATE "))) {
  } else if (OB_FAIL(append_identifier(builder, database_name))) {
  } else if (OB_FAIL(builder.append("."))) {
  } else if (OB_FAIL(append_identifier(builder, table_name))) {
  } else if (OB_FAIL(builder.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const AiColumnRuntimeInfo &info = columns.at(i);
    ObString generation_column;
    if (!info.invalidate_) {
    } else if (OB_FAIL(builder.append(first ? "" : ", "))) {
    } else if (OB_FAIL(append_identifier(builder, info.result_column_->get_column_name_str()))) {
    } else if (OB_FAIL(builder.append(" = NULL, "))) {
    } else if (OB_FAIL(append_identifier(builder, info.state_column_name_))) {
    } else if (OB_FAIL(builder.append_fmt(" = '%s', ", AI_PENDING_STATE))) {
    } else if (OB_FAIL(append_identifier(builder, info.request_column_name_))) {
    } else if (OB_FAIL(builder.append(" = "))) {
    } else if (OB_FAIL(append_string_literal(alloc, print_params, info.prompt_is_null_,
                                             info.prompt_text_, builder))) {
    } else if (OB_FAIL(AiColumnSpec::build_spec_column_name(alloc,
                    info.result_column_->get_column_name_str(), generation_column))) {
    } else if (OB_FAIL(builder.append(", "))) {
    } else if (OB_FAIL(append_identifier(builder, generation_column))) {
    } else if (OB_FAIL(builder.append(" = "))) {
    } else if (OB_FAIL(append_string_literal(alloc, print_params, false, request_id, builder))) {
    } else {
      first = false;
    }
  }
  if (OB_SUCC(ret)) {
    if (first) {
      // nothing to invalidate
    } else if (OB_FAIL(builder.append(" WHERE "))) {
    } else if (OB_FAIL(builder.append(predicate))) {
    } else if (OB_FAIL(assign_cstring(alloc, builder, sql))) {
    }
  }
  return ret;
}

int build_task_cleanup(ObIAllocator &alloc,
                       const ObObjPrintParams &print_params,
                       const uint64_t table_id,
                       const ObString &predicate,
                       ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString builder;
  if (OB_FAIL(builder.append_fmt("DELETE FROM %s WHERE table_id = %lu AND rowkey_predicate = ",
                                 AI_TASK_TABLE_NAME, table_id))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false, predicate, builder))) {
  } else if (OB_FAIL(assign_cstring(alloc, builder, sql))) {
  }
  return ret;
}

int build_task_replace(ObIAllocator &alloc,
                       const ObObjPrintParams &print_params,
                       const AiColumnRuntimeInfo &info,
                       const uint64_t table_id,
                       const ObString &database_name,
                       const ObString &table_name,
                       const ObString &predicate,
                       const ObString &request_id,
                       ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString builder;
  if (OB_FAIL(builder.append_fmt(
          "REPLACE INTO %s (table_id, column_id, rowkey_predicate, database_name, "
          "table_name, column_name, request_id, request_text, state, retry_count, eligible_ts) "
          "VALUES (%lu, %lu, ",
          AI_TASK_TABLE_NAME, table_id, info.result_column_->get_column_id()))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false, predicate, builder))) {
  } else if (OB_FAIL(builder.append(", "))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false,
                                           database_name, builder))) {
  } else if (OB_FAIL(builder.append(", "))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false,
                                           table_name, builder))) {
  } else if (OB_FAIL(builder.append(", "))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false,
                                           info.spec_.column_name_, builder))) {
  } else if (OB_FAIL(builder.append(", "))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, false, request_id, builder))) {
  } else if (OB_FAIL(builder.append(", "))) {
  } else if (OB_FAIL(append_string_literal(alloc, print_params, info.prompt_is_null_,
                                           info.prompt_text_, builder))) {
  } else if (OB_FAIL(builder.append_fmt(", %ld, 0, now(6))",
                                        static_cast<int64_t>(AiColumnSpec::TS_PENDING)))) {
  } else if (OB_FAIL(assign_cstring(alloc, builder, sql))) {
  }
  return ret;
}

int build_request_id(char *buf, const int64_t buf_len, ObString &request_id)
{
  int ret = OB_SUCCESS;
  static volatile int64_t request_seq = 0;
  const int64_t seq = __sync_add_and_fetch(&request_seq, 1);
  const int64_t len = snprintf(buf, static_cast<size_t>(buf_len), "ai-%ld-%ld-%ld",
                               ObTimeUtility::current_time(),
                               GETTID(),
                               seq);
  if (OB_UNLIKELY(len <= 0 || len >= buf_len)) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    request_id.assign_ptr(buf, static_cast<int32_t>(len));
  }
  return ret;
}

} // namespace

bool AiColumnDmlHandler::is_ai_column_nested_sql(ObExecContext *ctx)
{
  bool bret = false;
  if (OB_NOT_NULL(ctx) && OB_NOT_NULL(ctx->get_parent_ctx())
      && ctx->get_parent_ctx()->get_das_ctx().ai_column_processing_) {
    bret = true;
  }
  return bret;
}

// Reads the current prompt values and the prompts recorded by the previous
// request of every AI column of the row.  The inner read result must be
// destroyed before the hook writes through the same inner connection, so the
// read is kept in its own scope.
int read_ai_column_prompts(ObTableModifyOp &op,
                           const ObString &select_sql,
                           ObIAllocator &allocator,
                           ObIArray<AiColumnRuntimeInfo> &columns)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    if (OB_FAIL(op.execute_read(select_sql.ptr(), res))) {
      LOG_WARN("failed to read ai column prompts", K(ret), K(select_sql));
    } else if (OB_ISNULL(res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int next_ret = res.get_result()->next();
      if (OB_ITER_END == next_ret) {
        // the row vanished while the statement was running
        ret = OB_SUCCESS;
      } else if (OB_FAIL(next_ret)) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
          AiColumnRuntimeInfo &info = columns.at(i);
          ObObj obj;
          if (OB_FAIL(res.get_result()->get_obj(i, obj))) {
            LOG_WARN("failed to read prompt value", K(ret), K(i));
          } else if (obj.is_null()) {
            info.prompt_is_null_ = true;
            info.prompt_text_.reset();
          } else if (OB_FAIL(ob_write_string(allocator, obj.get_string(),
                                             info.prompt_text_))) {
          } else {
            info.prompt_is_null_ = false;
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
          AiColumnRuntimeInfo &info = columns.at(i);
          ObObj obj;
          if (OB_FAIL(res.get_result()->get_obj(i + columns.count(), obj))) {
            LOG_WARN("failed to read stored request", K(ret), K(i));
          } else if (obj.is_null()) {
            info.stored_request_.reset();
          } else if (OB_FAIL(ob_write_string(allocator, obj.get_string(),
                                             info.stored_request_))) {
          }
        }
      }
      int close_ret = res.get_result()->close();
      if (OB_SUCCESS != close_ret && OB_SUCCESS == ret) {
        ret = close_ret;
      }
    }
  }
  return ret;
}

int AiColumnDmlHandler::process_row(ObTableModifyOp &op,
                                    const ObDMLBaseCtDef &dml_ctdef,
                                    const ObDMLModifyRowNode &modify_row,
                                    const ObDmlEventType dml_event)
{
  int ret = OB_SUCCESS;
  ObExecContext &ctx = op.get_exec_ctx();
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  ObSchemaGetterGuard *schema_guard = nullptr;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *database_schema = nullptr;
  // table_id_ of the DAS ctdef is the table location id, the reference table
  // id (the table id in the schema) is carried by index_tid_, which equals the
  // table id for the primary index.
  const uint64_t table_id = dml_ctdef.das_base_ctdef_.index_tid_;
  const bool is_delete = ObDmlEventType::DE_DELETING == dml_event;
  const bool is_insert = ObDmlEventType::DE_INSERTING == dml_event;
  const bool is_update = ObDmlEventType::DE_UPDATING == dml_event;
  if (!dml_ctdef.is_ai_column_table_ || !dml_ctdef.is_primary_index_) {
    // nothing to do
  } else if (is_ai_column_nested_sql(&ctx)) {
    // the row was written by the AI column hook itself, do not recurse
  } else if (OB_ISNULL(session) || OB_ISNULL(sql_ctx)
             || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid session or schema guard", K(ret));
  } else if (session->is_real_inner_session()) {
    // The ai column worker and the other server background tasks maintain the
    // hidden columns explicitly, the row hook must not run for their writes.
  } else if ((!is_insert && !is_update && !is_delete)
             || OB_FAIL(schema_guard->get_table_schema(table_id, table_schema))
             || OB_ISNULL(table_schema)) {
    if (OB_SUCC(ret)) {
      ret = OB_ERR_UNEXPECTED;
    }
    LOG_WARN("unexpected dml event or table schema", K(ret), K(table_id), K(dml_event));
  } else if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(),
                                                       database_schema))
             || OB_ISNULL(database_schema)) {
    if (OB_SUCC(ret)) {
      ret = OB_ERR_UNEXPECTED;
    }
    LOG_WARN("failed to get database schema", K(ret), K(table_id));
  } else {
    // The rows handled here were buffered by the DML operator while the
    // statement was running, so the ctdef expressions only carry the values of
    // the last row that went through the operator.  The stored shadow rows say
    // which expression array still holds this particular row.
    const bool has_old_shadow = OB_NOT_NULL(modify_row.old_row_);
    const bool has_new_shadow = OB_NOT_NULL(modify_row.new_row_);
    const bool has_full_shadow = OB_NOT_NULL(modify_row.full_row_);
    const bool is_single_row = !has_old_shadow && !has_new_shadow && !has_full_shadow;
    // Positions [0, column_ids_.count()) of full_row_ hold the values the row
    // had before the update, which is what the old predicate needs when the
    // primary key did not change.
    const ExprFixedArray &old_source = (has_old_shadow || is_single_row)
                                       ? dml_ctdef.old_row_ : dml_ctdef.full_row_;
    const ExprFixedArray &new_source = (has_new_shadow || is_single_row)
                                       ? dml_ctdef.new_row_ : dml_ctdef.full_row_;
    ObArenaAllocator allocator(ObModIds::OB_SQL_EXPR);
    ObSEArray<AiColumnRuntimeInfo, 4> ai_columns;
    const ObString database_name = database_schema->get_database_name_str();
    const ObString table_name = table_schema->get_table_name_str();
    const ObObjPrintParams &print_params = op.get_obj_print_params();
    ObString old_predicate;
    ObString new_predicate;
    bool pk_changed = false;
    if (OB_FAIL(collect_ai_columns(allocator, *table_schema, ai_columns))) {
      LOG_WARN("failed to collect ai columns", K(ret), K(table_id));
    } else if (ai_columns.empty()) {
      // nothing to maintain
    } else if (!is_insert && OB_FAIL(build_rowkey_predicate(op, dml_ctdef, *table_schema,
                                                            old_source, allocator,
                                                            old_predicate))) {
      LOG_WARN("failed to build old rowkey predicate", K(ret), K(table_id));
    } else if (!is_delete && OB_FAIL(build_rowkey_predicate(op, dml_ctdef, *table_schema,
                                                            new_source, allocator,
                                                            new_predicate))) {
      LOG_WARN("failed to build new rowkey predicate", K(ret), K(table_id));
    } else if (is_update) {
      // A moved primary key leaves the old task row behind and has to be
      // treated as a value change even when the prompt text is unchanged.
      pk_changed = 0 != old_predicate.compare(new_predicate);
    }
    if (OB_SUCC(ret) && OB_FAIL(op.check_stack())) {
      LOG_WARN("failed to check stack", K(ret));
    } else if (OB_SUCC(ret) && OB_FAIL(op.open_inner_conn())) {
      LOG_WARN("failed to open inner connection", K(ret));
    } else if (OB_SUCC(ret) && OB_FAIL(op.begin_nested_session(true))) {
      LOG_WARN("failed to begin nested session", K(ret));
    } else if (OB_SUCC(ret)) {
      ctx.get_das_ctx().ai_column_processing_ = true;
      if (is_delete) {
        ObString cleanup_sql;
        if (OB_FAIL(build_task_cleanup(allocator, print_params, table_id,
                                       old_predicate, cleanup_sql))) {
        } else if (OB_FAIL(op.execute_write(cleanup_sql.ptr()))) {
          LOG_WARN("failed to clean ai column tasks", K(ret), K(cleanup_sql));
        }
      } else {
        ObString select_sql;
        if (OB_FAIL(build_prompt_select(allocator, ai_columns, database_name, table_name,
                                        new_predicate, select_sql))) {
        } else if (OB_FAIL(read_ai_column_prompts(op, select_sql, allocator, ai_columns))) {
        }
        if (OB_SUCC(ret)) {
            bool any_invalidated = false;
            for (int64_t i = 0; i < ai_columns.count(); ++i) {
              AiColumnRuntimeInfo &info = ai_columns.at(i);
              info.invalidate_ = info.result_column_->get_column_id() == dml_ctdef.ai_retry_column_id_
                                 || is_insert || pk_changed || info.prompt_is_null_
                                 || info.stored_request_.empty()
                                 || 0 != info.stored_request_.compare(info.prompt_text_);
              any_invalidated = any_invalidated || info.invalidate_;
            }
            if (any_invalidated) {
              char request_buf[AI_REQUEST_ID_BUF_LEN];
              ObString request_id;
              ObString cleanup_sql;
              ObString update_sql;
              if (OB_FAIL(build_request_id(request_buf, AI_REQUEST_ID_BUF_LEN, request_id))) {
              } else if (!pk_changed) {
                // keep the existing task rows untouched
              } else if (OB_FAIL(build_task_cleanup(allocator, print_params, table_id,
                                                    old_predicate, cleanup_sql))) {
              } else if (OB_FAIL(op.execute_write(cleanup_sql.ptr()))) {
              }
              if (OB_FAIL(ret)) {
              } else if (OB_FAIL(build_invalidation_update(allocator, print_params, ai_columns,
                                                           database_name, table_name,
                                                           new_predicate, request_id, update_sql))) {
              } else if (update_sql.empty()) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(op.execute_write(update_sql.ptr()))) {
                LOG_WARN("failed to invalidate ai column", K(ret), K(update_sql));
              }
              for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
                ObString task_sql;
                if (!ai_columns.at(i).invalidate_) {
                } else if (OB_FAIL(build_task_replace(allocator, print_params,
                                                      ai_columns.at(i), table_id,
                                                      database_name, table_name,
                                                      new_predicate, request_id, task_sql))) {
                } else if (OB_FAIL(op.execute_write(task_sql.ptr()))) {
                  LOG_WARN("failed to enqueue ai column task", K(ret), K(task_sql));
                }
              }
            }
          }
        }
      }
      ctx.get_das_ctx().ai_column_processing_ = false;
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret && OB_SUCCESS == ret) {
        ret = end_ret;
      }
    }
    int close_ret = op.close_inner_conn();
    if (OB_SUCCESS != close_ret && OB_SUCCESS == ret) {
      ret = close_ret;
    }
    return ret;
  }
} // namespace sql
} // namespace oceanbase
