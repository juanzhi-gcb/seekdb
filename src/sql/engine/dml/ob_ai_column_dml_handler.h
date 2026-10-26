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
#include "share/ob_define.h"

namespace oceanbase
{
namespace sql
{

class ObExecContext;
class ObTableModifyOp;
struct ObDMLBaseCtDef;
struct ObDMLBaseRtDef;
struct ObDMLModifyRowNode;

// The AI column DML hook maintains the hidden AI column state and the async
// task rows after a row has been written.  It runs inside the user
// transaction through the nested inner SQL session, so every change it makes
// is rolled back together with the outer statement.
class AiColumnDmlHandler
{
public:
  static int process_row(ObTableModifyOp &op,
                         const ObDMLBaseCtDef &dml_ctdef,
                         const ObDMLModifyRowNode &modify_row,
                         const common::ObDmlEventType dml_event);
  static bool is_ai_column_nested_sql(ObExecContext *ctx);
private:
  DISALLOW_COPY_AND_ASSIGN(AiColumnDmlHandler);
  AiColumnDmlHandler();
  ~AiColumnDmlHandler();
};

} // namespace sql
} // namespace oceanbase
