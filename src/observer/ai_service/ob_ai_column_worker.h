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

#include "lib/ob_define.h"
#include "lib/container/ob_iarray.h"
#include "rootserver/ob_server_thread_helper.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
class ObSqlString;
}
namespace observer
{

// One row of __all_ai_column_task joined with its __all_ai_column definition.
// It is an implementation detail of the worker and is only defined in the
// worker source file.
struct AiColumnTask;

// The AI column worker turns the committed rows of
// oceanbase.__all_ai_column_task into model calls and writes the results back
// into the hidden AI column state and request columns.  It is hosted by
// ObAiService and runs as a single background thread.
class AiColumnWorker : public rootserver::ObServerThreadHelper
{
public:
  AiColumnWorker();
  virtual ~AiColumnWorker();
  int init();
  void destroy();
  virtual void do_work() override;
  virtual void deactivate() override {}
  virtual int activate() override { return OB_SUCCESS; }
private:
  int process_next_request(int64_t &task_count);
  int process_tasks(common::ObIAllocator &alloc, common::ObIArray<AiColumnTask> &tasks);
  int compute_task(common::ObIAllocator &alloc, AiColumnTask &task);
  int build_apply_update(common::ObIAllocator &alloc,
                         const common::ObIArray<AiColumnTask> &tasks,
                         common::ObSqlString &sql);
  int append_task_result(common::ObIAllocator &alloc, const AiColumnTask &task,
                         common::ObSqlString &sql);
  int write_sql(const common::ObSqlString &sql, int64_t &affected_rows);
  int delete_task(common::ObIAllocator &alloc, const AiColumnTask &task);
  int handle_task_failure(common::ObIAllocator &alloc, const AiColumnTask &task);
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(AiColumnWorker);
};

} // namespace observer
} // namespace oceanbase
