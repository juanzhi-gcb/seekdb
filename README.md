# MiniVectorDB

第二轮独立开发分支 2026-competition-round2。当前交付是可构建、可测试的数据库核心库，尚不是完整 SQL 数据库或可正式提测版本。

## 当前能力

- BIGINT 范围、严格 UTF-8 字符计数/比较、有限 float32 向量验证。
- 欧氏距离、流式过滤 TopK、按主键消除真实距离并列的不确定性。
- 4096 字节磁盘页、显式小端编码、校验、溢出值和磁盘键索引。
- 写时复制、提交/回滚、事务内读取和语句保存点。
- 双版本提交记录、同步后确认、重启忽略未发布数据。
- 随机参考模型对照、进程退出/重启、写盘失败及损坏检测测试。

首版索引使用 8 层、256 分支的 radix 页树，作为正确性基线。它是有界内存的磁盘树，但每次修改可能追加 8 个索引页以及值页，写放大显著。B+树、批内页复用和空间回收仍是后续工作，不能据此声称已满足正式资源限制或性能目标。

## 尚未实现

SQL 解析/执行、表目录和行编解码、官方 stdin/stdout 协议、ANN 索引、空间回收、批量页复用、外部排序和完整资源准入测试。正式驱动、公开用例及冻结限额尚未获取。

上游的遗留 .github 流程属于完整 seekdb，不是本项目已适配的 CI。未修改初赛分支，也未推送本轮代码或提测。

## 开发

Windows 工作根目录 D:/Codex/MiniVectorDB。专用 WSL 发行版 MiniVectorDB 的系统盘、Docker 镜像和项目数据均在 D 盘。用户选择保留全局 WSL 交换设置，不改动其他发行版。

通过 scripts/dev.ps1 的 Status、Build、Test、Image、ContainerTest 动作操作。详见 [环境说明](docs/environment.md)。

其他 Linux 环境可使用：
~~~sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
~~~

Dockerfile 当前的 test 目标用于在容器内验证核心库；最终运行入口将在 SQL 协议确定后接入。正式运行的数据、日志、索引和临时文件需统一在 /data。

## 资料和材料

- [文献与实验建议](docs/performance-research.md)
- [扫描基准说明](bench/README.md)
- [实际学习记录](agent/conversation.json)

学习材料以 agent/conversation.json 为源，根目录 conversation.json 为当前兼容占位的相同副本，待平台明确路径后固定单一读取入口。材料区分用户、Agent 和工具来源，不把 Agent 的代码/解释冒充用户理解，不包含令牌。
