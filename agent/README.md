# MiniVectorDB 开发与学习材料

入口 `agent/conversation.json` 与根目录镜像字节相同，UTF-8 JSON对象，125783字节，低于128KiB。包含83条用户记录、49条实验、23条Agent观察，以及当前代码模块映射。基础记录、当前结果、原文引用及限制均内嵌在JSON，外部文档只便于人工复现。

当前运行源码与已完成原生验证的71de1c64一致；正式历史成绩属于a24（3977.995012），当前正式分数待提测。新增解释明确归Agent，不改写为用户理解，不自填分数或模拟三轮评审。

`material-digest.json`为本地完整性清单；`scripts/check-submission-contract.py --check-internal-references`检查包装与引文，`scripts/check-learning-material.py`核验历史记录、归档、摘要和来源。旧`build-prompt-rubric.py`只构建早期版本，不用于覆盖当前材料。完整说明见`docs/submission-evidence-explanation.txt`。
