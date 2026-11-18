# MiniVectorDB 当前提交材料

正式入口是 `agent/conversation.json`，根目录 `conversation.json` 是相同字节的镜像。当前材料采用 `minivectordb-evidence-and-implementation-v3`，包含 61 条对话记录（含 q01–q03 本会话用户逐字原话）、52 条实验摘要、20 条证据链、20 条 rubric 实例和内嵌实施规格；不是全文对话，也不是官方评分报告。当前源码身份绑定 runtime_blob_sha1；本地验证为 ctest 70/70、JSONL 协议 20/20、SnapshotProjection 双向量回归通过，均非官方评测。

运行源码、测试与构建文件固定为 `a24d2cd764027adec7371f268f99ccdca2307032`。JSON 开头的 `version_ledger` 区分 a24 实现、后续未采用实验和未执行计划；不能把后续 CRC、压缩段或并行绑定实验当作当前运行代码。

主提示词、算法契约、输入契约、七条实验推导指令与外部审查后的澄清均在 JSON 内。`implementation-prompt.txt` 仅为方便人工阅读的完整副本。专业解释标记为 Agent 当前整理，未补造历史用户解释或测试结果；未证明空白独立重建、相同性能或达到目标评分。

校验命令：

```
python3 -B scripts/check-submission-contract.py --check-internal-references
python3 -B scripts/check-learning-material.py
```

第二条命令按材料版本选择校验器，核验归档原文、当前记录上下文、42 处内部引用、镜像、规格副本和 97 个 a24 文件的 Git blob 身份。旧结构的检查逻辑和回归测试保留。`material-digest.json` 只记录完整性，不是平台评测输出。

历史材料、归档和旧版本文档保留供追溯；当前状态以本文件及同一提交的 JSON 为准。此次没有提交 API 密钥、API 调用脚本或模拟评委报告，也没有触发比赛正式提测。
