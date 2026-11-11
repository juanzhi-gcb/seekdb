# MiniVectorDB

第二轮独立开发分支 2026-competition-round2。已接通磁盘表目录、行编解码、SQL 执行、精确向量查询和事务。本轮按官方 f4d4e266 发布的 JSONL 协议修复默认入口，并补齐持久化向量索引；公开自测与隐藏正式准入须分别看待。

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

单键修改仍可能追加 16 个索引页以及记录页。现在 INSERT/UPDATE/DELETE 按不超过 128 条、累计 512 KiB 的小批次共享新建索引路径（单条超大记录单独处理），减少重复复制。旧页面保持不可变，保存点和提交同步语义不变；已增加有界、完整验证后才准入的索引页缓存；SQL 默认 1 MiB，Options.index_cache_bytes=0 可关闭，底层 Store 默认仍关闭。尚无空间回收。不要求全部记录常驻内存，数据页按固定缓存预算淘汰，但单条记录与单条 SQL 仍需要内存。排序预算是工作状态目标，另有当前记录和序列化等开销，不等于进程硬内存限制。进程强杀可能留下临时排序目录，尚未自动回收历史孤儿目录。

向量 TopK 仅在 K 能落入预算时走有界堆，否则走排序路径。查询会检查整行格式、UTF-8 和有限向量，但只为所需列构造值。表扫描路径先过滤，再构造向量和计算距离；索引路径先核验索引向量与表中向量一致，再过滤和计算距离。已校验的 float32 在小端系统使用整块复制，避免重复逐分量解码。UPDATE/DELETE 扫描仍完整解码行。投影按获胜键读取，不能据此推断物理 IO 一定下降。

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

[本轮验证记录](docs/admission-validation.md)区分公开检查与正式评测；[历史验证](docs/validation.md)保留当时的版本范围。仍需隐藏正式资源准入、后续固定错误码映射及写入/空间性能改进。独立测试包括随机参考模型、语句原子性、跨表事务、强制终止恢复、强制外排及真实写盘失败。

上游遗留 .github 流程属于完整 seekdb，尚不是本项目适配的 CI。初赛分支未修改。用户反馈 bdae610 的正式准入未通过；本轮修复仅在本地验证，不自动推送或提测。

页面校验使用 IEEE CRC32 slicing-by-8，字节算法保留作独立对照。新 SQL 数据库默认使用格式03紧凑不可变数据页及跨查询16MiB有界数据页缓存，原扫描缓存默认关闭；单行页面和超大记录自动使用普通布局。既有格式02保持原格式，不自动迁移；旧二进制不能读取03。生产提交同步保持不变。VA 精确剪枝及单次同步日志仍为独立研究原型。详见[格式说明](docs/storage-format03.md)、[数据缓存契约](docs/data-buffer.md)、[本轮结果](docs/buffer-results.md)和[最终验证](docs/buffer-validation.md)。

## 论文与学习材料

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

agent/conversation.json 为源，根目录 conversation.json 为同内容兼容副本，待平台明确路径后固定入口。材料区分用户、Agent 和工具来源，不把 Agent 的实现或解释冒充用户理解，不包含令牌。

本轮已对照平台显示成功的 `8b7054`：两版各公开20/20、95SQL，58条协议响应仅一条排序修复差异；详见[成功版本兼容性复核](docs/successful-version-compatibility.md)。该报告记录上传前验证；最终版本以提交SHA为准，平台两项分数仍未提供。

本轮新增[无索引快照、缓冲复用与临时向量优化的完整实验](docs/unindexed-results.md)，保留三轮迭代、负面结果、38项回归与真实JSONL对照。
