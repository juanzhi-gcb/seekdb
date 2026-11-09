# 精确检索局部基准

此程序仅测内存中的距离计算与 Top10 选择，含过滤；不含 SQL、持久化、索引构建、物理 I/O 或正式容器资源统计，不能换算赛事分数。

数据：固定种子20261109，10,000条128维float32向量，1,000条查询，Top10，500条查询带通过率10%的等值标签过滤。比较全排序参考实现与流式小堆；三轮交替先后顺序，每轮在计时外核对所有1,000条结果。

~~~sh
g++ -O3 -std=c++20 -I include bench/exact_scan.cpp src/vector/exact.cpp -o /mnt/d/Codex/MiniVectorDB/build/exact-scan-bench
/mnt/d/Codex/MiniVectorDB/build/exact-scan-bench
~~~

初次记录在 results/exact-scan-before.json，GCC14.2.0、Alpine3.22.8、WSL2，未设置CPU绑核。全排序中位数783.687ms，流式堆551.126ms，局部时间比1.42198。两组都在过滤后选择TopK，不存在先取全表K再过滤的错误路径。

该数值仅说明当前程序和当前机器上的局部差异，不能外推端到端SQL或正式排名。改动内核后应保留新结果文件，不覆盖原测量。距离细微差别的精确排名以平方距离为准，欧氏距离仅在输出时开方；旧基线在候选计算阶段即开方，因此新结果需要注明实现版本差异。

最终口径一致的复测保存在 results/exact-scan-squared-reference.json：两种方案均按平方距离排名、结果输出时开方。全排序中位数765.482ms，流式堆541.186ms，时间比1.41445，三轮各1000条结果全部一致。此轮未与其他本项目测试并行运行。前面记录是开发过程探索，不用于混合计算最终成绩。