# MiniVectorDB

第二轮独立开发分支 2026-competition-round2。支持磁盘表目录、行编解码、SQL 执行、精确向量查询、持久化向量索引和事务，默认使用官方 f4d4e266 发布的 JSONL 协议。用户已反馈提交 `524133baf0a4ed947805236c02cce9cecffc0be3` 通过正式 MySQL 准入；当前得分优化以它为基线，新的本地改动仍须独立验证和正式评测。

## 当前能力

- CREATE TABLE；BIGINT、VARCHAR(n)、VECTOR(n)；一个 BIGINT 主键，每表最多一个向量列。
- INSERT（列清单及多行 VALUES）、SELECT（普通投影、*、L2_DISTANCE）、UPDATE 常量赋值、DELETE。
- 普通列 =、<、<=、>、>= 及 AND；多列 ORDER BY、ASC/DESC、LIMIT；并列按主键升序。
- VARCHAR 按 Unicode 字符计数和码点比较；向量校验有限 float32 和维度，距离使用存储后的值。
- 精确和带过滤的 TopK；按返回的欧氏距离排序，并列按主键升序；事务中读取自己的向量更新。
- CREATE VECTOR INDEX；磁盘平坦向量索引与表数据一起提交、回滚，新增、更新和删除同步维护。exact 和 ann 均使用完整精确检索，不宣称近似剪枝或索引提速。
- 单个磁盘提交根覆盖表目录和所有表；自动提交、显式 BEGIN/COMMIT/ROLLBACK、语句失败保存点。
- 双版本提交记录、同步后确认、正常关闭回滚及异常进程退出后恢复。
- 流式扫描和输出；主键等值直接查找；一般排序按预算生成临时段并做两路归并。

完整语法仍以正式题包为准。不支持的 NULL、JOIN、OR、聚合、子查询、ALTER/DROP 和主键更新会报错，不会伪装成功。

## 存储与内存边界

4096 字节页、显式小端编码、校验及跨页记录。格式 02/03 用 16 层 radix 路径定位“表命名空间+有符号主键”，所有表共享一个文件。旧格式 01 开发数据明确拒绝打开，不自动迁移或覆盖。

单键修改仍可能追加 16 个索引页以及记录页。INSERT/UPDATE/DELETE 按不超过 128 条、累计 512 KiB 的小批次共享新建索引路径（单条超大记录单独处理），减少重复复制。旧页面保持不可变，语句保存点仍恢复共同工作根。SQL 默认使用 1 MiB 索引页缓存和 4 MiB 数据页缓存，只有完整校验后的页面才能准入；相应预算设为 0 可关闭，底层 Store 默认仍关闭这两项缓存。尚无空间回收。不要求全部记录常驻内存，数据页按固定缓存预算淘汰，但单条记录与单条 SQL 仍需要内存。排序预算是工作状态目标，另有当前记录和序列化等开销，不等于进程硬内存限制。进程强杀可能留下临时排序目录，尚未自动回收历史孤儿目录。

向量 TopK 仅在 K 能落入预算时走有界堆，否则走排序路径。从存储读取行时检查整行格式、UTF-8 和有限向量，默认同步索引路径还核验索引与原表的主键集合及向量一致性。默认开启 `typed_scan_cache`：对于无过滤或仅含 BIGINT 过滤条件的合适 TopK，将完整验证后的向量和 BIGINT 列保存在自有对齐数组中，后续查询复用验证结果，避免重复解析无关文本和复制向量；过滤、距离和赢家仍每次重新计算。VARCHAR 过滤等不适用的路径保留原始行快照或流式扫描。

数值列缓存与原始行快照互斥，共用 `scan_snapshot_bytes` 的默认及硬上限 8 MiB，不是各占 8 MiB。首次符合条件的扫描仅观察，第二次完整扫描才构建；预算不足、超容量或构建失败不会发布部分缓存。每条已解析的非 SELECT 在绑定/执行前失效缓存，包括失败的写入、DDL 和事务命令。数值缓存预算包含向量、BIGINT 数组及列映射；固定对象、分配器、当前行与查询临时状态另计。缓存不持久化，不能代替磁盘表和索引。

同表、同索引一旦选择原始行快照，后续无过滤或整数过滤查询继续使用它，直到失效或扫描身份切换。观察、构建及超预算拒绝状态都遵守这一选择，避免字符串过滤与无过滤交替时反复清空两套缓存。它也意味着一次字符串查询后，纯数值查询暂时不再切回类型化缓存；这是避免重建抖动的保守取舍。

`validated_vector_kernel=true` 可在 SQL 已证明候选向量有限时选用内部距离入口：查询向量每次仍验证一次，每个候选仍检查维度，省去重复的逐分量有限性检查。double 累加顺序、sqrt 后排序及有符号主键并列规则保持不变；公开 `exact_top_k` / `l2_distance` 仍完整检查输入。设为 false 使用检查版对照。UPDATE/DELETE 仍完整解码行，赢家投影仍回查原表；不能据此推断物理 I/O 一定下降。

SQL 默认提交策略为 `CommitSyncPolicy::ProcessCrash`：完成所有新页的写入循环后写提交根，再对整个文件执行一次 `fsync`，成功返回后才确认提交。恢复承诺针对进程被 SIGKILL、内核及文件系统继续存活的正式崩溃模型；不将未确认提交遇到断电、内核或设备故障纳入同一保证。`Ordered` 保留先同步数据、再写根并同步的两道屏障，底层 Store 默认仍使用 Ordered。提交 I/O 失败后数据库进入结果不确定状态，须关闭并重新打开，不能继续使用可能失真的事务视图。

## 开发和运行

本机工作根目录 D:/Codex/MiniVectorDB。专用 WSL 发行版 MiniVectorDB 的系统盘、Docker 镜像和项目数据均在 D 盘。保留用户现有全局 WSL 交换配置。

scripts/dev.ps1 提供 Status、Build、Test、Image、ContainerTest、RuntimeImage、RuntimeTest；详情见 [环境说明](docs/environment.md)。

其他 Linux 环境：

~~~sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
MVDB_TEST_DIR=/data/core-tests ctest --test-dir build --output-on-failure
./build/minivectordb --protocol jsonl --data-dir /data
~~~

测试使用 Python 3 和 C++20 编译器。Dockerfile 的 test 目标运行验收测试，最终 runtime 目标直接启动数据库前台进程；运行不需要联网。数据库、临时排序文件和大结果的临时序列化文件均放在 /data。默认 JSONL，开发用 TSV 须显式选择，详见 [输入输出说明](docs/dev-protocol.md)和[官方协议复核](docs/admission-protocol-review.md)。

## 验证与剩余工作

[协议与评分测量复核](docs/submission-contract-results.md)记录最新检查及混合过滤修复；[此前得分优化记录](docs/score-first-results.md)、[准入修复记录](docs/admission-validation.md)与[历史验证](docs/validation.md)保留各自当时的版本范围。基线正式通过不能代替当前改动的准入验证，也不能由本地耗时直接换算正式 E 或排名。独立测试包括随机参考模型、语句原子性、跨表事务、强制终止恢复、强制外排及真实写盘失败。

公开协议检查器及功能用例按上游 `f4d4e266` 原样保存在 `bench/vendor/seekdb-round2-f4d4e26`，逐文件身份见 `upstream-lock.json`。`bench/run_contract_benchmark.py` 使用其传输层，分开测量批量写入和 TopK，支持整数与字符串过滤；它是本地对照工具，公开仓库未提供本赛题的官方性能基准。`scripts/check-submission-contract.py` 可对工作区或指定完整提交检查材料路径和格式；它不评估 Prompt 分数。

上游遗留 .github 流程属于完整 seekdb，尚不是本项目适配的 CI。初赛分支未修改。当前新改动不自动推送或提测。

页面校验使用 IEEE CRC32 slicing-by-8，字节算法保留作独立对照。新 SQL 数据库默认使用格式03紧凑不可变数据页，每次存储扫描的 packed-page 临时缓存默认关闭（`packed_scan_cache_bytes=0`）；单行页面和超大记录自动使用普通布局。既有格式02保持原格式，不自动迁移；旧二进制不能读取03。本轮没有更改磁盘格式。VA 精确剪枝及追加日志仍为独立研究原型，不能与当前 COW 提交策略混为一谈。详见[格式说明](docs/storage-format03.md)、[当前数据缓存契约](docs/data-buffer.md)及记录旧 16 MiB 配置的[历史缓存结果](docs/buffer-results.md)和[历史验证](docs/buffer-validation.md)。

## 论文与学习材料

- [按得分项重排的查询、内存与提交优化](docs/score-first-results.md)
- [上传版0e954的真实JSONL性能基线](docs/jsonl-baseline-0e954.md)
- [同步扫描优化、负面结果和验证](docs/paired-scan-results.md)
- [有界扫描快照、数值修复与交错读写实验](docs/scan-snapshot-results.md)
- [旧版正式失败复核与版本区别](docs/reported-admission-failure.md)
- [文本校验优化、负面实验与学习证据](docs/text-validation-results.md)
- [按收益排序的行 CPU 优化、真实旧新对照与回归](docs/row-cpu-results.md)
- [刁钻测试、提交确认漏洞修复与故障注入](docs/adversarial-results.md)
- [跨查询缓存优化与超容量对照](docs/buffer-results.md)
- [紧凑页生产集成与独立日志实验](docs/compact-results.md)
- [缓存、CRC 与紧凑页/VA 实验](docs/portfolio-results.md)
- [实验方法和限制](docs/portfolio-method.md)
- [耗时拆分与D盘路径对照](docs/path-profile-results.md)
- [页面校验优化](docs/checksum-results.md)
- [优化对照结果](docs/optimization-results.md)
- [实验方法](bench/optimization-method.md)
- [SQL 执行研究](docs/sql-execution-research.md)
- [向量与存储文献](docs/performance-research.md)
- [局部扫描基准](bench/README.md)
- [真实学习记录](agent/conversation.json)

agent/conversation.json 为材料入口，根目录 conversation.json 保留同内容兼容副本。材料区分用户、Agent 和工具来源，不把 Agent 的实现或解释冒充用户理解，不包含令牌。本地文件哈希摘要不等于平台评审输出中的“材料摘要”，本项目不自行生成正式分数或换算评分。

历史上曾对照平台显示成功的 `8b7054`：两版各公开20/20、95SQL，58条协议响应仅一条排序修复差异；详见[成功版本兼容性复核](docs/successful-version-compatibility.md)。该报告保留当时上传前的证据，不代表当前版本再次通过。最新已知正式反馈属于 `524133`：MySQL 通过，性能原始 E=856.552980，Prompt 返回无效/待复核等合并提示；具体 Prompt 原因及当前新改动的正式成绩尚未确认。

上一轮的[无索引快照、缓冲复用与临时向量优化实验](docs/unindexed-results.md)保留当时三轮迭代、负面结果、38项回归与真实 JSONL 对照，不作为本轮最终验证结论。
