# FloraGPA C++ 迁移进度 — 2026-09-22

已有可编译、可运行的 VS2022 / Qt Widgets 原生工程，能够独立重放当前验证的
GF2 与 BF1 DX11 捕获。**尚未完成原 Python 版本的全部功能迁移。**
当前独立包为 `out/FloraGPA-coverage-ui/FloraGPA.exe`。

最新一批补齐 Quad 串行诊断的原生字节码准备：拆分绘制的 VertexID／InstanceID
补偿，以及计数器 UAV 写入前的数组索引筛选。4,342 项 Python／C++ 字节码和
元数据对照通过，覆盖 395 份着色器及 2,099 项预期拒绝输入。原版测试驱动使用
C++ 生成字节码通过 803 项 GPU 检查，覆盖硬件／WARP、实例步长、点线三角形、
strip 重启／退化图元、间接绘制、数组和 MSAA。**这些检查验证的是字节码处理，
原生 Quad 调度和 Qt 面板仍待迁移**。完整 Release 构建、6 套相关 CTest 与
4 项黄金帧／负对照通过。系统 ID 模块更新为 `ported`，Quad 目标模块为 `partial`；
当前总数为 **21 `ported`、117 `partial`、66 `pending`**，不是工作量百分比。
详见 [QUAD_SHADER_MIGRATION.md](QUAD_SHADER_MIGRATION.md)。可用界面包仍为上述 Coverage 版本。

上一批接通中央 **Coverage** 页：Fragment／Geometry、有效 RTV／DSV、层／索引、
深度测试、Fit／1:1、尺寸／缩放、纹理像素与缓冲区字节范围联动、取消重试及四项
原版产物的 ZIP 导出。当前实验和硬件／WARP 设置共用原生 Worker；切换事件、
实验或设备即清除过期结果。原版没有保存 Coverage 控件设置，迁移版保持这一边界。
完整 Release 构建和 7 套相关 CTest 通过；原主窗口 54 项、Coverage 6 项均无
失败或跳过。Windows-only PATH 发布包通过同样 6 项交互检查、16 项 CLI／Worker
检查、4 项黄金帧／负对照和 10 份依赖审计。Qt 导出的完整报告和三张图像与原版
真实 GF2 绘制逐项一致；四个程序与 Release 哈希一致，包中已移除临时测试文件。
四个独立 Coverage 模块完成职责审计并更新为 `ported`，共享模块保留剩余缺口。
该阶段总数为 **20 `ported`、116 `partial`、68 `pending`**，不是工作量百分比。
**Quad 执行、界面及其他待迁模块仍未完成**。详见
[COVERAGE_UI_MIGRATION.md](COVERAGE_UI_MIGRATION.md)。

上一批迁入 Coverage 原生执行与 CLI/Worker 导出：fragment／geometry、
RTV／DSV／viewport、数组层／mip／体纹理切片、缓冲区 RTV、MSAA、UAV
重定位与私有输出隔离均已接通，保留实验编辑、条件绘制和 SO／DrawAuto 语义。
744 组合成与集成 GPU 对照共 6,624 项检查通过，另有 GF2／BF1 的 8 组真实
绘制对照、80 项检查通过。WARP 深度预览的初始 CPU 转换差异已按原版独立
硬件 GPU 预览修正，没有放宽像素对照。完整 Release 构建、7 套相关 CTest、
16 组独立包 CLI／Worker 检查、4 项黄金帧／负对照和 10 份运行依赖审计通过。
四个产品程序与 Release 哈希一致。当时 Coverage 的 Qt 面板与 Quad 执行仍待迁移，
相关模块保留 `partial`；该阶段总数为 16 `ported`、120 `partial`、68 `pending`，
不是工作量百分比。详见 [COVERAGE_EXECUTION_MIGRATION.md](COVERAGE_EXECUTION_MIGRATION.md)。

上一批补齐 Coverage／Quad 所依赖的 DXBC 改写：保留原 PS 的新增标记输出、
main 返回处替换颜色、保留 alpha、数组索引筛选、六阶段 SM5 UAV 重定位、
反射范围和 64 槽 feature flag 均已迁入原生代码。3,564 组逐字节 Python 对照
通过，覆盖 378 份独立着色器和 501 组拒绝输入。原版 GPU 测试驱动使用 C++
生成的字节码通过 660 项检查，覆盖硬件／WARP、MSAA、双源混合、动态 PS、
数组索引及各前置阶段的 UAV 副作用。此项证据不代表整个 Python 诊断引擎已迁移。
Release 全量构建、五套相关 CTest 和四项黄金帧／负对照通过。三个底层模块
更新为 `ported`，该阶段为 16 `ported`、117 `partial`、71 `pending`，不等同于
工作量百分比。当时完整 Coverage／Quad 后端与 Qt 页面仍待迁移，独立包为
GPU Timing 版本。详见 [COVERAGE_SHADER_MIGRATION.md](COVERAGE_SHADER_MIGRATION.md)。

上一批将原版重复 GPU 采样迁入原生后端和中央 **GPU Timing** 页：预热、
包含端点的 API 范围、可选资源写入、耗时分布、逐轮真实时间轴、API 跳转、
实验设置和 JSON／CSV／ZIP 导出均已接通。取消和过期结果检查沿用隔离 worker；
每轮恢复捕获资源，保留 disabled／predication／DrawAuto 的原版测量边界。
发布包 232 组 Python 对照通过，其中 26 组执行 GPU 重放，覆盖硬件／WARP、
修改着色器后的 DrawAuto、真实帧禁用绘制及恢复。原始计数、来源信息、每条
CSV、资源字节和从实际 ticks 重算的分布均已检查；不要求两次独立采样耗时相同。
13 组命令行检查、发布包 Qt 6 项检查、四项黄金帧／负对照和九份依赖审计通过。
完整 Release 构建通过，相关九套 CTest 在初次与修正后的回归中通过；原主窗口
54 项无失败或跳过。四个程序与 Release 哈希一致，发布包不含测试程序。
两个独立模块更新为 `ported`，共享模块仍保留各自缺口：当前为 13 `ported`、
119 `partial`、72 `pending`，这不是工作量百分比。详见
[GPU_PROFILE_MIGRATION.md](GPU_PROFILE_MIGRATION.md)。

上一批迁移了原版可选外部反编译器回退、DXBC 汇编编辑、CLI profile／优化
选项和按资源保存的 HLSL／ASM 草稿。Qt 的 **Shader → DXBC** 提供 Read、
Import ASM、Assemble & Apply 和 External Tool；草稿与入口名随实验保存，
切换资源及撤销／重做不会丢失。外部工具在隔离进程中执行，超时或取消时清理
子进程树。未捆绑第三方工具，原生恢复仍优先使用。
发布包 21 组原 Python／CLI 对照通过，涵盖六个阶段汇编字节、回退成功／失败、
三个 profile × 三个优化模式、Shader 导出及汇编往返；四项黄金帧／负对照通过。
完整 Release 构建及相关 10 套 CTest 通过（含更新只读汇编旧假设后的回归），
原主窗口 54 项无失败或跳过。Windows-only PATH 的发布包模型／Qt 检查为
14／4 项通过；16 份依赖报告通过审计，四个程序与 Release 哈希一致。
共享模块仍保持 `partial`；数量维持 11 `ported`、120 `partial`、73 `pending`。
详见 [EXTERNAL_SHADER_MIGRATION.md](EXTERNAL_SHADER_MIGRATION.md)。

上一批将原版四个 DXBC→HLSL lowering 模块完整迁入 C++，并接入
**Shader → Source → Recover HLSL** 和 CLI `shader --recover`。复用源码编辑、
Compile & Apply、实验保存与撤销／重做；保存源码只有在重新编译后的 DXBC
逐字节一致时才标为已验证。重建源码保留“非原始源码／语义未验证”的标记，
详细信息放在 tooltip 和折叠日志内。
发布包 3,685 项原 Python 对照通过（931 项拒绝输入），577 份独立 DXBC 中，
原版可恢复的 375 份全部生成一致源码和重新编译字节；六阶段原 CLI 导出完全一致。
六套专门 CPU／GPU 对照通过，覆盖整数、共享内存、插值／深度、GS 多流、Gather、
计数器和立方体比较采样。BF1 的 47 个 CS／HS／DS 同时替换后仍符合黄金图像。
完整 Release 构建和相关 8 套 CTest 通过（含修正测试控件定位后的回归）；原主窗口
54 项无失败或跳过。Windows-only PATH 下的发布包模型 10 项、Qt 套件 3 项通过，
4 项黄金帧／负对照和 11 份运行时依赖审计通过，四个程序与 Release 哈希一致。
四个模块更新为 `ported`，清单为 11 `ported`、120 `partial`、73 `pending`；
这不是工作量百分比。外部反编译器回退和汇编编辑入口已在上述后续批次迁移，
相关共享模块保持 `partial`。详见 [HLSL_RECOVERY_MIGRATION.md](HLSL_RECOVERY_MIGRATION.md)。

上一批将多文件 **Shader Project** 接入 Shader 页：根文件选择、源码添加／删除、
Settings、JSON 导入／保存及隔离的 Compile & Apply 已迁入 Qt。原生编译器仅解析
项目内保存的 include，保留宏、编译选项和原版的展开上限；显式使用系统
D3DCompiler 47。项目随实验保存，支持撤销／重做，重开时验证当前 DXBC，过期
草稿不能覆盖更新的实验，但仍可导出。
发布包通过 2,057 组原 Python 对照（263 组拒绝输入）、2 组原 CLI 导出对照、
10 项模型与 7 项界面／重放检查。六个 shader 阶段和原 GPA 三文件样本均有验证；
硬件／WARP 的修改后像素与 CPU 预期一致。Release 全量构建及 7 套相关 CTest
通过，其中原主窗口 54 项回归无失败或跳过；4 项黄金帧／负对照及 9 份发布包
依赖审计通过，四个程序与构建哈希一致。两个项目模块更新为 `ported`，共享模块
仍按缺口保留状态。清单现为 7 `ported`、120 `partial`、77 `pending`，不等同于
工作量百分比。详见 [SHADER_PROJECT_MIGRATION.md](SHADER_PROJECT_MIGRATION.md)。

上一批补齐 `FloraGPA.Cli rdc-analyze`，覆盖原版八种 RDC 分析，以及事件／
资源／实例／线程参数、超时、日志、失败报告和进程清理。Qt 四类分析页与命令行
共用原生作业准备；修复完整 uint64 ID 检查，并补齐模块清单和反汇编 CRLF 导出。
最终包 15 组原 CLI 对照、33 项进程／参数测试、4 项黄金帧／负对照和 19 份运行时
审计通过。完整 Release 构建及 8 套相关 CTest 通过，包含原主窗口 54 项检查；
最终诊断调整后的 3 套相关 worker 测试再次通过。四个程序与构建哈希一致。
经逐项职责核对，`rdc_analyze.py` 和 `rdc_jobs.py` 更新为 `ported`，其余模块按
各自证据保留状态。详见 [RDC_CLI_MIGRATION.md](RDC_CLI_MIGRATION.md)。

上一批将原 Python 的 `postmesh` 接入 **Geometry → Replay Mesh**：支持 VS／
最终 GS 或 DS 输出、实例选择、网格预览、原始位置表和完整 JSON／CSV／OBJ 导出。
独立 Geometry 检查保留在相邻页签；详情默认收起。与 Pixel History／Shader Debug／
Replay Metrics 共享原生再捕获、缓存与取消流程，已读出的导出不依赖临时目录存续。
发布包对照覆盖 18 种真实配置和 4 种拒绝请求：3,676 个顶点、19,432 个索引、
715,705 字节导出完全一致，另有 78 组执行原 Python 分支的模型边界对照。
发布包界面 6 项通过，无失败和跳过；原主窗口 54 项回归通过。完整 Release
构建、4 项黄金帧／负对照和 34 份运行时依赖审计通过，四个程序与构建哈希一致。
共享模块仍保留 `partial`，不据此宣称全部 Python 功能完成迁移。
详见 [REPLAY_MESH_MIGRATION.md](REPLAY_MESH_MIGRATION.md)。

上一批补齐原生 RenderDoc `inventory`／`texture` 分析接口：完整资源关系、纹理／
缓冲描述和原始子资源字节均已接通。支持无 Draw／Dispatch 的目录查询，保留原版
三维纹理读取整个 mip 体积的语义，以及注解联合值的高位字符转义、NaN 和负零。
发布包 34 组对照通过：2,982 项资源描述、703 项纹理描述、646 项缓冲描述，
22,593,876 字节纹理数据逐项／逐字节与 Python 一致；另有 8 项注解序列化对照。
这些入口不依赖 Python／Tk／GPA 运行时，普通 Qt 资源视图继续使用既有原生实现。
完整 Release 构建、相关 5 套 CTest、发布包 5 项序列化测试、4 项黄金帧／负对照
及 38 份运行时模块审计通过。四个程序与构建哈希一致，临时测试程序已从包中移除。
该批次未包含 `postmesh`，后续已由上述 Replay Mesh 批次接通。
详见 [RDC_ASSETS_MIGRATION.md](RDC_ASSETS_MIGRATION.md)。



上一批将原 Python 的通用重放计数器接入右侧 **Replay Metrics**：原生测量、
Selection／Frame 范围、过滤、API 定位、指标目录、详情和完整 JSON 导出已接通，
与 Pixel History／Shader Debug 共享实验感知的再捕获缓存及取消流程。
数值保留原始整数精度，默认窄面板可同时显示指标、数值和单位；详情默认收起。
九组原 Python 对照共检查 18,928 条结果：16,157 条数值精确一致，1,456 条独立
GPU 时间检查有效性，1,315 条 BF1 PS 测量值单独审计。重复运行两版均观察到
BF1 PS 调用次数波动；不将这部分计入数值完全一致，也不修改产品输出掩盖差异。
完整 Release 构建及相关 7 套 CTest 通过，原主窗口 54 项无失败、无跳过。发布包
在 Windows-only PATH 下通过 6 项界面检查、4 项黄金帧／负对照和 10 份运行时
模块审计；四个程序与 Release 构建哈希一致。实际发布包 Qt 布局已检查。
本批不完成厂商硬件指标调度、GTPin 或剩余 RenderDoc 分析分支；共享模块仍为
`partial`。详见 [REPLAY_METRICS_MIGRATION.md](REPLAY_METRICS_MIGRATION.md)。

上一批将 PS／VS／CS 调试接入 Qt：指令／源码前后步进、步过／步出、
运行到指令、条件和次数断点、监视、调用栈、带类型的源码变量、配置导入／保存及
完整 JSON 导出已接通。三个阶段与 Pixel History 共用原生再捕获缓存和取消流程；
事件、实验、驱动或调用参数变化会使旧轨迹失效。界面按 VS／HS／DS／GS／PS／CS
排序，源码使用等宽字体，详细 JSON 默认收起。
模型在 21 份真实轨迹、67 个场景、8,170 次操作中与原 Python 对照通过；包含真实
循环局部变量条件、反向步进、稀疏源码映射、配置身份和失败回滚。集成界面测试
7 项通过、无失败和跳过，覆盖 Hardware／WARP 三阶段、禁用／撤销、取消重试、
后端缺失恢复、输出像素选择及跨分析缓存复用。
完整 Release 构建通过；48 套 CTest 中其余 47 套通过，新调试套件的测试产物命名
冲突修复后复测通过。原 Qt 回归 54 项无失败、无跳过。最终发布包在 Windows-only
PATH 下再次通过 7 项调试界面检查、4 项黄金帧／负对照及 14 份运行时模块审计，
四个程序与构建哈希一致。完整回归早于最后的调试器显示调整，发布包检查包含调整。
本批并不完成硬件指标调度、GTPin、剩余 RenderDoc 分析分支等迁移；相关模块仍
如实保留未完成状态。详见 [REPLAY_DEBUG_UI_MIGRATION.md](REPLAY_DEBUG_UI_MIGRATION.md)。

上一批迁入原生 PS／VS／CS 调试后端：像素调用、索引／非索引顶点和计算线程
可通过独立 `FloraGPA.Rdc.exe` 取得完整步骤、源码、反汇编、调用栈和变量变化。
发布包 171 项检查通过，包含 21 份完整轨迹／279 个步骤与 7 种失败选择；覆盖
Hardware／WARP、多文件源码、循环、16／32 位索引偏移、负 base vertex、显式索引
及原 GF2／BF1 捕获。RenderDoc 1.45 未初始化的全局映射偏移明确标为不可用，
其余轨迹字段逐项与 Python 原版对照。完整 Release 构建、相关 4 套 CTest、
4 项黄金帧及负对照、44 份运行时依赖审计通过；4 个程序与构建哈希一致。
该批当时 PS／VS／CS 的 Qt 消费者待接通，现已由最新界面批次接通；整个迁移目标未完成。
详见 [RDC_DEBUG_MIGRATION.md](RDC_DEBUG_MIGRATION.md)。

上一批将 Pixel History 接入 Qt 主窗口：独立再捕获、Hardware／WARP、
实验缓存与撤销复用、取消／重试、原始 API 定位、精确整数值、折叠详情和完整
JSON 导出均已接通；Output／Texture 像素选择可填写历史查询坐标。
45 套 Release CTest 通过，含原 Qt 54 项回归，无失败、无跳过。
最终 Details 布局／提示清理后重新完整构建，在发布包、Windows-only PATH 下
重跑新 Qt 测试，6 项通过且无跳过（含 22 份后端历史报告、真实 CPU 写入和
Hardware／WARP 再捕获）。新包 4 项黄金帧及负对照、10 份运行时模块审计通过，
4 个程序与最终构建哈希一致，实际 Qt 截图已检查。共享 RenderDoc worker 的
像素／顶点／计算 shader 调试在该批仍待迁移（现已迁入后端，Qt 消费者待接通），
计数器等其他消费者仍待迁移，相关模块保持 `partial`。
详见 [PIXEL_HISTORY_UI_MIGRATION.md](PIXEL_HISTORY_UI_MIGRATION.md)。

上一批接通原生 RenderDoc 重放控制器、PixelHistory 结果和 CPU 写入补充。
独立 `FloraGPA.Rdc.exe` 通过 JSON 任务运行，不调用 Python/qrenderdoc。
发布包 152 项检查通过：22 组完整像素历史对照（35 条记录、其中 10 条 CPU
快照）、9 份真实 RDC 事件索引、5 种非法选择，覆盖 MSAA、Resolve、复制／清除／
深度／mip 操作、Update 区域、1D／数组／体纹理 Map、整数值及未变化的局部写入。
完整 Release 构建、相关 2 套 CTest、4 项黄金帧及负对照、44 份运行时报告审计通过。
该批当时 Qt Pixel History 界面、缓存、实验联动、取消和导航／导出待接通，
现已由最新界面批次接通；共享 worker 的其他分析和调试分支仍未迁移。
详见 [RDC_HISTORY_MIGRATION.md](RDC_HISTORY_MIGRATION.md)。

上一批迁入可选 RenderDoc 原生再捕获与精确命令来源映射。CLI 可将独立重放
导出为 RDC，保留 GPA 命令与资源身份，正常重放不加载 RenderDoc。独立包九份
捕获的 99 项检查、709 组原 Python 映射对照、43 套 Release CTest、4 项黄金帧
及负对照、18 份运行时模块审计全部通过；原 Qt 54 项回归无失败、无跳过。
BF1 的 5,491 个已执行命令均获得唯一可选择的原生事件映射。
该批是 Pixel History 的依赖迁移；当时生产版 RenderDoc 重放控制器、像素历史结果、
CPU 写入补充与 Qt 视图均待接通，现已分别由后端与界面批次迁入。
详见 [RDC_CAPTURE_MIGRATION.md](RDC_CAPTURE_MIGRATION.md)。

上一批将 GS／HS／DS 检查点调试接入主窗口：读取原始 shader、单点与完整轨迹
采集、精确输入重新采集、HS 阶段选择、指令／源码步进、条件断点、源码帧变量、
SDBG 历史监视、配置导入／保存和完整 ZIP 导出。44 份实测轨迹的 3,246 项
界面对照通过，包含 1,752 次按钮导航、变量／帧／监视与归档检查；原 Qt 54 项
回归无失败、无跳过。新包 164 项源码捕获对照、4 项黄金帧及负对照、166 份
运行时模块审计通过。完整 Release 构建和实际 Qt 布局检查通过。
界面保持 GPA 风格布局与简洁英文控件；未接通的其他调试阶段仍留空。
详见 [CHECKPOINT_UI_MIGRATION.md](CHECKPOINT_UI_MIGRATION.md)。

清单中的 `ported` 表示该 Python 模块的功能已有接通并验证的原生对应实现；
`partial` 表示共享模块仍有未迁移或未充分验证的分支／消费者，`pending` 表示待迁移。
检查点界面批次将检查点界面、原生调试配置和断点／监视组件三项记为 `ported`；这不代表
整个应用迁移完成，也不代表已验证任意捕获或另一台干净 Windows 机器。

上一批迁入原生调试配置与 Qt 断点／监视面板组件。984 项配置对照通过，
覆盖 164 份捕获报告的身份、配置往返、规则限制、编码和文件边界；原生事务回滚、
Qt 按钮／文件对话框、源码帧切换与 SDBG 历史监视检查通过，实际 Qt 渲染已检查。
完整 Release 构建通过。该批当时组件尚未接入主窗口，也未替换独立包；
完整检查点视图与 Worker 协调已由检查点界面批次接通。
详见 [NATIVE_DEBUG_CONFIG_MIGRATION.md](NATIVE_DEBUG_CONFIG_MIGRATION.md)。

上一批迁入 SDBG 变量符号与逐次赋值历史，正式 checkpoint 导出已包含完整符号。
13,790 项符号／历史对照通过；新捕获的 6,220 条快照用于 442 项精确历史对照，
包含数组写入前索引、双精度完整分量、反向／随机查看及缺失历史拒绝。
独立包 164 项 Hardware/WARP 捕获对照、15,742 项 SPDB 回归、相关 4 套 CTest、
4 项黄金帧和 164 份运行时审计通过。该批当时 Qt 调试视图和配置持久化仍待接通，
未将底层完成计作界面功能完成。详见 [SDBG_VARIABLES_MIGRATION.md](SDBG_VARIABLES_MIGRATION.md)。

上一批迁入 C++ 源码导航、条件断点、命中次数规则和监视表达式底层 API。
15,821 项表达式／作用域对照通过；73 个导航场景中的 28,998 次操作对照通过，
包括实测快照、嵌套内联、调用／HS 阶段边界、条件断点优先停靠、缓存与截断预览。
完整 Release 构建及相关 4 套 CTest 通过。该批尚未接通 Qt 调试控件和配置持久化，
当时未替换独立包；SDBG 赋值重建现已补齐，SDBG 表达式适配层只接受已验证的赋值值。
详见 [SOURCE_NAVIGATION_MIGRATION.md](SOURCE_NAVIGATION_MIGRATION.md)。

上一批接通原生源码调用栈、逐函数源码位置和 HS 阶段作用域，并纳入正式
checkpoint 导出。3,492 项源码栈检查通过，涵盖 28,702 次查询和 1,388 个独立
Microsoft DIA 源码位置对照。92 项独立包 Hardware/WARP 捕获对照通过。
原始 shader → C++ 符号／源码栈 → 变量值的 15,742 项对照通过，包含 11,606 条
实测快照，HS 阶段归属由 C++ 自行构建。相关 4 套 CTest、4 项黄金帧检查和
92 份运行时模块审计通过；应用不加载 DIA，独立测试工具使用 DIA 作为对照。
该批当时 SDBG 赋值重建、源码单步导航、断点／监视和 Qt 源码调试视图仍待迁移。
详见 [SOURCE_STACK_MIGRATION.md](SOURCE_STACK_MIGRATION.md)。

上一批接通 SPDB 源码变量的 C++ 符号／类型／存活区间解析，并纳入正式
checkpoint 导出；原生变量值解析 API 保留逐分量有效性、原始位值和冲突状态。
14,060 项对照通过，包含 9,940 条 Hardware/WARP 快照记录，7 项仅诊断措辞不同。
独立包 76 项源码捕获对照、相关 4 套 CTest、4 项黄金帧检查和 76 份运行时审计通过。
该批当时 SDBG 赋值重建、HS 阶段作用域归属、源码调用栈及 Qt 源码变量窗口待迁移；
该批 HS 解析对照使用参考版提供的阶段作用域，原生阶段归属由最新一批补齐。
详见 [SOURCE_VARIABLES_MIGRATION.md](SOURCE_VARIABLES_MIGRATION.md)。

上一批接通 SPDB C13／SDBG 原始源码行映射与 checkpoint 指令源码位置。
6,515 项解析、路径、文本与边界对照全部通过；独立包 Hardware/WARP 的
60 项 SPDB/SDBG GS/HS/DS 捕获对照通过，源码行与指令目录也参与完整比较。
修正了 Qt 部署旧编译器漏报 `#line` 的问题，源码导航明确使用系统编译器。
相关 CTest 与 53 项 Qt 回归通过；最终包 4 项黄金帧检查及 60 份运行时审计通过。
该批当时源码变量、源码调用栈以及 Qt 源码调试入口仍待迁移，完整目标保持未完成。
详见 [SOURCE_LINES_MIGRATION.md](SOURCE_LINES_MIGRATION.md)。

上一批接通 GS/DS/HS checkpoint／trace 的 C++ 正式采集与 CLI 导出：
原始 Draw、私有输出／SO 隔离、HS 运行时筛选、容量重试、完整命中序列和
寄存器位值／有效性，均复用原生重放路径。开发版 540 项 Python 对照全部通过
（324 项成功、216 项一致拒绝），其中 298 项捕获的原始快照字节也完全一致。
独立包另有 60 项间接绘制、输入／参数编辑、禁用绘制对照通过。Release 41 套
CTest 全部通过，原 Qt 53 项无失败、无跳过；GF2/BF1 黄金帧及负对照 4 项通过。
旧 VS/DS 写入与 GS 发射 100 项回归通过；154 份独立包成功报告的模块审计通过，
三个 EXE 与最终 Release 构建一致，仍未替代另一台干净机器的验证。
该批当时源码行／变量／源码调用栈与 Qt checkpoint 调试界面均未迁移；
源码行现已接通，其余依赖仍 pending，清单维持 partial。
详见 [CHECKPOINT_CAPTURE_MIGRATION.md](CHECKPOINT_CAPTURE_MIGRATION.md)。

上一批完成 GS/DS/HS checkpoint／trace 的完整 C++ DXBC 插桩层，包括
寄存器值／有效性、动态数组与双结果指令、原始调用、HS 运行时 token／输入筛选。
1090 项插桩对照全部通过：875 项 DXBC 字节与元数据一致，215 项一致拒绝；
覆盖 27 个 shader，其中 4 个来自 GF2/BF1。Hardware/WARP 原生测试核对
GS 数组浮点位值、HS 控制点整数、DS 快照与下游 SO 字节、管线计数，以及
计数器回绕、索引越界和容量边界。**当时生产采集协调、导出和 Qt 调试界面尚待接通**，
没有将插桩层完成计作完整功能可用；该批未替换独立包。
详见 [DXBC_CHECKPOINT_MIGRATION.md](DXBC_CHECKPOINT_MIGRATION.md)。

上一批补齐 checkpoint／trace 的 C++ 基础层：原始指令目录、HS 阶段与
HS/DS 声明输入、动态数组寄存器寻址、双结果指令的地址依赖、静态调用图、
完整输入筛选及返回记录校验。1476 项检查全部通过，其中 1403 项成功结果
与 Python 精确一致，72 项一致拒绝，1 项为单独计数的记录长度边界增强；
包含编译器生成的 GS 4.0/4.1/5.0、HS/DS，以及真实帧的 4 个着色器资源。
该批当时尚未接通 GPU checkpoint／trace 插桩、采集、导出或 Qt 调试界面，
不代表这些功能已经可用，也未替换当前独立包。完整迁移目标保持未完成。
详见 [CHECKPOINT_MODEL_MIGRATION.md](CHECKPOINT_MODEL_MIGRATION.md)。

上一批实现补齐原生 HS 输出采集、VS→HS 实例身份传递、控制点与 patch constants
双表、逐分量有效性、完整导出和 Qt 实验项目选择恢复。GPU 原始输入和原始绘制
参数保持不变；多实例数据从完整绘制结果切片，不改写 InstanceID。
开发版插桩 226 项对照通过（216 项成功、10 项预期拒绝），GPU 112 项对照通过
（100 项成功、12 项预期拒绝），包括 Hardware/WARP 上的 BF1 HS 输出。
原生测试核对 CPU 预期值、下游 DS 字节、原 UAV/隐藏计数器以及后续 Draw；
Release 40 套 CTest 全部通过，原有 Qt 53 项无失败、无跳过。最终独立包 HS
112 项对照通过，旧几何 140 项、写入／发射 100 项、黄金帧与负对照 4 项均通过。
324 份成功报告的模块审计未发现 Python/Tk/GPA/RenderDoc，Qt 来自包内，三个
EXE 与最终 Release 构建一致。证据见 `artifacts/hull-portable-final/validation.json`、
`artifacts/ctest-hull-final.log`、`artifacts/hull-post-regression/validation.json`、
`artifacts/hull-log-regression/validation.json`、`artifacts/hull-golden-final/validation.json`
和 `artifacts/hull-runtime-audit.json`；仍未替代独立机器验证。
该 HS 输出批次当时尚未接通 checkpoint、trace 的生产采集和调试器；完整目标仍未完成。
详见 [HULL_OUTPUT_MIGRATION.md](HULL_OUTPUT_MIGRATION.md)。

上一批接通原生 VS/DS 写入与 GS 发射的采集、导出和 Qt Geometry 入口。
沿用原 Draw、下游管线与类实例，在私有 RTV/DSV/UAV/SO 上执行并恢复原 SO 游标；
记录逐分量有效性、原始已知身份、Emit/Cut 顺序和 GS 条带连接关系。VS/DS 写入
使用记录表，GS 发射保留网格预览，导出包含有效性二进制与图元 CSV。
开发版 100 项对照通过（90 项成功、10 项预期拒绝），含 Hardware/WARP 的
GF2/BF1 六项真实捕获对照；BF1 DS 使用实际启用曲面细分的事件 11276。
原子分配顺序的三项差异保留于证据中，核对完整调用记录和 GS 局部序列。
最终独立包同样通过 100 项对照，旧 Final/VS/DS/GS 的 140 项对照继续通过。
Release 40 套 CTest 全部通过，含原有 Qt 53 项及几何套件 10 项，无失败、无跳过；
GF2/BF1 黄金帧及关闭 Draw 负对照 4 项通过。224 份成功报告的模块审计未发现
Python/Tk/GPA/RenderDoc，Qt 来自独立包，三个 EXE 与 Release 构建一致。
证据为 `artifacts/output-log-portable-final/validation.json`、
`artifacts/output-log-post-regression/validation.json`、
`artifacts/ctest-output-log-final.log`、`artifacts/output-log-golden-final/validation.json`
及 `artifacts/output-log-runtime-audit.json`；仍未替代独立机器验证。
Checkpoint、trace 和 HS 分支仍未迁移，整体目标保持未完成。
详见 [OUTPUT_LOG_MIGRATION.md](OUTPUT_LOG_MIGRATION.md)。

前一批完成 VS/DS 写入、GS 发射记录的 C++ DXBC 插桩基础层。完整插桩字节及
元数据 461 项对照全部通过（444 项成功、17 项预期拒绝），其中包含 24 项既有
VS 身份插桩回归，以及 GF2/BF1 的 163 个不同原始着色器（161 VS、2 DS）。
原生 Hardware/WARP 执行测试覆盖记录值、有效性、调用数、溢出边界及 GS Emit
之后的稀疏写入。该批仅完成基础层；Worker、导出和 Qt 接入现已由最新批次补齐，checkpoint/trace 等分支仍未迁移。
Release 40 套 CTest 全部通过，原有 Qt 53 项无失败、无跳过；新增着色器套件
6 项通过（含 setup/cleanup），GF2/BF1 黄金图像和关闭 Draw 负对照 4 项通过。
详见 [DXBC_OUTPUT_LOG_MIGRATION.md](DXBC_OUTPUT_LOG_MIGRATION.md)。

前一批迁移 VS 索引关联：通过原生 DXBC 插桩携带真实 VertexID/InstanceID，
将组装后的 VS 输出关联到原始输入索引和实例，支持展开表、唯一输出表及引用映射。
同一身份的不同输出字节保留为独立变体，NaN 载荷和正负零不会被数值去重合并。
原始 Shader 字节、插桩后的 DXBC 哈希、映射和两份二进制表均纳入 Python 对照，
引用表必须能无损重建完整输出。Qt 支持三表切换、完整导出和实验项目恢复。
独立包 108 项合成对照全部通过（100 项成功、8 项预期拒绝），覆盖 SM4/SM5、
列表/条带/邻接/控制点、输出分量复用、语义冲突、间接参数、UAV 限制、空绘制、
关闭事件及编辑后的当前 GPU 索引。Release 39 套 CTest、原有 Qt 53 项及扩展后的
几何套件 9 项均通过。证据为 `artifacts/vs-identity-portable-final/validation.json`、
`artifacts/ctest-vs-identity-final.log` 和 `artifacts/vs-identity-tests-final.txt`。
独立包真实 GF2/BF1 16 项对照通过，包括完整输出和最后一个实例，覆盖 BF1
事件 1415 的第九个实例及事件 11276 的曲面控制点。原有 Final/VS/DS/GS 的
140 项对照继续通过。对应证据为 `artifacts/vs-identity-real-portable-final/validation.json`
和 `artifacts/vs-identity-post-regression/validation.json`。
GF2/BF1 黄金帧与关闭 Draw 负对照共 4 项通过。250 份成功报告的模块审计未发现
Python/Tk/GPA/RenderDoc，Qt 来自独立包，三个 EXE 与最终 Release 构建一致。
证据为 `artifacts/vs-identity-golden-final/validation.json` 及
`artifacts/vs-identity-runtime-audit.json`；仍未替代独立机器验证。
详情见 [VS_IDENTITY_MIGRATION.md](VS_IDENTITY_MIGRATION.md)。

前一批迁移原生 Final/VS/DS/GS 后变换几何、四路输出流和指定实例采集。
Geometry 页整合 IA 与 Shader 输出、类型化属性表、线框预览及 JSON/CSV/BIN/OBJ
导出，并保存/恢复实验项目中的已迁移阶段、流和实例选择。界面保留紧凑的工具栏，
未迁移的 HS、VS 索引、逐次写入和 GS 发射功能不显示为可用。
私有 RTV/DSV/UAV 快照及隐藏计数器隔离诊断写入，保留原 SO 缓冲区和游标；
原生测试验证了重复采集、扩容后超限失败、重试及随后直接绘制的存储一致性。
已修复 Qt 网格与新数据的索引基准差异，并用实际渲染及导出测试覆盖。
Release 39 套 CTest 全部通过，原有 Qt 53 项无失败、无跳过；新增几何套件 8 项
通过。独立包 140 项 Python 对照全部通过，其中 130 项成功采集、10 项预期拒绝。
对照包含精确元数据、原始字节、类型化 CSV 和 OBJ，覆盖实例步进、间接参数、
GS/DS、四路流、高槽位 UAV、扩容、输入替换、着色器替换/撤销及关闭事件。
证据见 `artifacts/ctest-post-transform-final.log`、`artifacts/post-transform-tests-final.txt`
和 `artifacts/post-transform-portable-final/validation.json`。
独立包真实 GF2/BF1 14 项对照也全部通过，包含首尾绘制以及 BF1 的 Final/VS/DS
曲面细分阶段，均覆盖 Hardware/WARP。整帧黄金图像与关闭 Draw 负对照 4 项通过。
148 份成功报告的模块审计确认 Qt 来自独立包，未加载 Python/Tk/GPA/RenderDoc；
发布的三个 EXE 与验证后的 Release 构建逐字节一致。对应证据为
`artifacts/post-transform-real-portable-final/validation.json`、
`artifacts/post-transform-golden-final/validation.json` 和
`artifacts/post-transform-runtime-audit.json`。验证在本机完成，未代替独立机器验证。
范围及验证缺口见 [POST_TRANSFORM_MIGRATION.md](POST_TRANSFORM_MIGRATION.md)。

前一批迁移原生单事件、包含端点的命令区间和整帧 GPU 统计。Qt GPU Statistics
页提供管线调用数、遮挡样本、四路 SO、溢出、时间及 JSON/CSV 导出，沿用全局
Hardware/WARP 选择；Annotations 的 Range Metrics 预填闭合分组范围。
独立包最终 92 项 Python 对照通过（78 项成功采样、14 项预期拒绝），Release 38 套
CTest 和原有 Qt 53 项全部通过。早期旧夹具误将拒绝计为正向覆盖的证据已被替代。
新增 Qt 测试还验证实际 Worker 测量、取消后重试、旧结果失效和 uint64 精确导出。
真实 GF2/BF1 22 项严格对照中 21 项完全匹配；BF1 硬件整帧 PS 调用数在两个实现
中均有跨次小幅波动，WARP 整帧和其余字段一致。保留严格比较差异，不伪装成全绿。
查询结果是单次重放采样，区间时间包含准备和辅助操作，并非原应用运行时间。
独立包的 GF2/BF1 黄金帧及关闭 Draw 负对照也全部通过。
详见 [GPU_STATISTICS_MIGRATION.md](GPU_STATISTICS_MIGRATION.md)。

前一批迁移 annotation 对象层级、Begin/End 配对、显式 QueryInterface context
关联、已证明的 Draw/Dispatch 成员，以及完整 JSON/CSV 导出。Qt Annotations 页
支持异步读取、名称/ID 筛选、证据详情和起止/Draw API 跳转。重放校验并计数七类
annotation 命令，损坏记录明确拒绝，不创建额外 GPU 对象。
64 项开发版对照通过：44 份报告（360 条 annotation、310 个节点）和 20 项
硬件/WARP 重放检查，其中 14 项为预期拒绝。范围统计交接已在最新批次补齐；先前列出的
图表分组不在原 Python 面板中，不作为本轮迁移要求。
Release 全套 37 项 CTest 通过，原有 Qt 53 项无失败、无跳过，新增 annotation
套件验证实际主窗口导航、筛选后完整导出及异步结果失效。
独立包同样通过 64 项对照及 GF2/BF1 黄金帧、关闭 draw 负对照；50 份成功报告
确认 Qt 来自独立包，没有加载 Python/Tk/GPA/RenderDoc。
详见 [ANNOTATION_MIGRATION.md](ANNOTATION_MIGRATION.md)。

前一批补齐原生实验执行报告：当前历史位置、总版本数、已应用／待执行事件、
最终 shader／初始纹理资产哈希、合并后的视图描述符及 Update 源信息。
Replay、Buffer 和事件 Texture 导出使用真实遍历记录，Qt 折叠任务日志显示简短摘要。
Release 36 项 CTest 全部通过，其中 Qt 53 项无失败、无跳过。
独立包 252 项 Python 报告对照通过，覆盖硬件／WARP、历史游标、事件前后、
完整帧、禁用／抑制事件、uint64 ID 和 buffer／UAV counter 编辑。
217 项 Texture 字节／像素／元数据回归、GF2/BF1 黄金帧及关闭 draw 负对照通过。
详见 [EXPERIMENT_REPORT_MIGRATION.md](EXPERIMENT_REPORT_MIGRATION.md)。

此前迁移 NV12/P010/P016 Map 的 Y-only 写入、旧行填充去除、原生行距适配，
以及 NV12 Update 和完整实验 Y/UV 源替换。写入来源与 UV 保留／未定义状态进入
Texture 工具提示及 JSON，事件前检查不增加该事件的写入记录。
独立包 192 项 Python 对照全部通过：125 个成功字节／像素／来源比较，67 个受控拒绝，
包括实际捕获、硬件/WARP、完整帧、非法输入和 uint64 ID 精度。Release 35 项 CTest
全部通过，其中 Qt 53 项无失败、无跳过。详见
[PLANAR_WRITE_MIGRATION.md](PLANAR_WRITE_MIGRATION.md)。
独立包的既有 217 项 Texture 检查／导出回归、GF2/BF1 黄金帧和关闭 draw 负对照也全部通过。

旧式 P010/P016 捕获的 UV 缺失仍无法凭空恢复；初始重放需要完整实验纹理替换，
捕获的 Update 仍明确拒绝，显式完整 Update 资产可用。这与原 Python 恢复的行为一致。

前两批已接入 Texture 的 MSAA/typed/plane 检查、DDS/RAW/PNG 导出和逐事件输入／输出
RAW 编辑，验证见 [TEXTURE_INSPECTION_MIGRATION.md](TEXTURE_INSPECTION_MIGRATION.md)
及 [EVENT_TEXTURE_MIGRATION.md](EVENT_TEXTURE_MIGRATION.md)。完整迁移仍包含尚未闭合的
私有分析／调试、shader 工具和指标等模块；实验报告已在本批接入。

## 当前可用

| 区域 | 已迁移功能 | 主要边界 |
|---|---|---|
| 工程 | C++20、VS2022 x64、Qt 6.11.2、CMake、Git、独立 CLI/Worker | 尚未配置远程仓库 |
| 捕获与重放 | 有边界检查的 IGPA v3 读取、六阶段状态、基本 D3D11 资源和 draw/dispatch、Map/更新/复制/清除 | 特殊 replay 路径尚未完全迁移 |
| 主界面 | GPA 式深色三栏、真实 GPU 时间柱状图与概览、可停靠面板、API 筛选、任务取消 | 概览尚无范围拖动；未知功能页禁用 |
| 事件 | draw/dispatch 选择、快照管线、前后边界重放、事件启停 | 部分命令执行和私有诊断仍待迁移 |
| 捕获命令状态 | 六阶段 setter、CB1 范围、IA/RS/OM/SO/predicate 参数、扩展 UAV 槽元数据；命令前/后、逐字段来源、资源重叠失效与快照校准；Pipeline 页异步读取、筛选、跳转、JSON 导出 | 原始捕获状态，不应用实验；只读 DSV / 模糊 3D 重叠保持未知；SO 偏移是 setter 参数，隐藏 counter 和实时写入位置不由此推断 |
| 命令间输入绑定 | 实际执行 IA layout / VB / IB、六阶段 SRV / sampler、CB / CB1；验证槽位、数组、资源类型、IA bind flags、stride 和 CB1 对齐 / 驱动支持；缺失绑定按阶段 / 槽位追踪 | 缺失资源未被后续 setter / 完整快照 / ClearState 恢复时，选定边界明确失败；已恢复的 setter 家族编辑均已接入，私有诊断仍待迁移 |
| 重放管线状态 | Pipeline > Replay State 通过 Worker 读取实际六阶段绑定、CB1 范围、IA/RS/OM/SO/predicate、视图及状态描述；命令前/后、实验输入克隆、禁用状态、筛选、跳转和 JSON 导出 | 仅覆盖后端已支持的命令和实验；SO 实时写入位置保持未知；完整管线编辑仍待迁移 |
| 动态 shader 类链接 | 六阶段 draw/dispatch 快照的 linkage、具名/显式创建实例、有序接口绑定、CB/texture/sampler 偏移；动态/静态 shader 替换；带 SO 的 linked GS；Qt 资源属性、接口槽数和 CLI 元数据导出 | 六阶段 shader setter 实验已接入；专用覆盖分析仍待迁移；本机已复现的空函数表 PS 驱动崩溃改为明确提示使用 WARP |
| Stream output / DrawAuto | SO 声明、五版 context 的目标 setter、普通 GS / VS / DS / 仅输出签名的 passthrough；重复/dirty 快照、显式重置、追加、ClearState；逐流 GPU 查询、按实际写入字节及 IA 范围重建 DrawAuto；Qt SO 属性/目标链接和几何导出 | SO setter 的已知 native cursor 保留已接入；私有诊断重放仍待迁移；无帧内历史时原始重放保留捕获计数并标注未验证，有编辑时拒绝猜测 |
| Predication | occlusion / SO overflow predicate 资源、六组 Begin/End/SetPredication、快照条件绑定、原始 BOOL、实际 GPU 结果；Pipeline > Predicate 前后边界检查与 JSON 导出；辅助预览查询隔离、读回和编辑准备的条件恢复 | active / hint 结果明确不可读；predicate setter 编辑、项目保存与撤销/重做已迁移；尚未迁移的私有诊断消费者仍待闭合；非标准 BOOL 的驱动差异保留 |
| API 检查 | 捕获字段、偏移/原始位、可选数组、引用跳转、资源/多词筛选、JSON/CSV 导出；getter、annotation、query 与 command-list 调用元数据 | 解码不代表所有命令均可执行 |
| Annotations | 对象独立的层级/配对、生命周期内 QueryInterface 身份、精确 context 成员、未知/冲突/未闭合边界、JSON/CSV；Qt 筛选、证据、API 跳转和范围统计交接；有效记录重放计数 | 仍需更多原始捕获验证；关联 deferred context 的记录不代表执行 command list |
| GPU Statistics | Draw/Dispatch、包含端点的区间和整帧；管线/遮挡/四路 SO/溢出/时间查询，实验与设备元数据，Qt Worker 测量及 JSON/CSV 导出 | 单次采样含查询和重放开销；BF1 硬件 PS 调用数存在跨次波动；私有指标与诊断消费者仍待迁移 |
| Context / command-list | 五种 context 接口身份、immediate/deferred 区分；缺失 context 的严格 Map READ 证据恢复；command-list 资源、owner 和 Execute/Finish 清单；Qt 字段树、证据导航、JSON 导出 | 推断不补造版本/指针/标志；保留指针与 ID 候选冲突；与 Python 相同，仅接受 immediate-context 的 Finish 空操作，未恢复列表执行 |
| 捕获查询值 | 按同 ID、命令顺序使用 GetDesc/GetDataSize/CreateQuery/predicate 元数据；BOOL 完整值与 UINT64 低位、缺失字节、HRESULT/冲突状态 | 表示捕获时保存的内存字，不是新执行的 GPU query；原始高位缺失时保持不完整 |
| 命令编辑 | RTV / DSV / Uint / Float UAV 清除值；资源写入命令启停；UpdateSubresource 紧密排列源数据替换；菜单、右键、撤销/重做；共享 context 校验和恢复 | 已支持 planar Map / NV12 Update / 显式完整 Update 资产；旧式 P010/P016 捕获 UV 仍不可重建 |
| 深度 / 模板实验 | 每 draw 的 depth enable/write/func、stencil enable/read/write masks、完整正反面四字段和 uint32 reference；字段递归合并、旧 preset、Qt 三页参数编辑、保存与撤销/重做 | 仅 graphics draw；下一未编辑 draw 恢复捕获状态；可与 rasterizer/blend 编辑混合，私有 coverage / 诊断仍待迁移 |
| 光栅 / 视口 / 裁剪实验 | 十二项 rasterizer 字段、0–16 槽 viewport/scissor、旧 wireframe/cull_none preset；原生 State/State1/State2、forced sample count 和 conservative raster；Qt 三页编辑、字段合并、历史和保存/加载 | 受设备能力与合法 draw 组合限制；强制采样检查 DSV、深度、RTV 采样数和替换后的 PS；私有 coverage 消费者仍待迁移 |
| 混合 / 采样实验 | BlendState/BlendState1、八槽独立 color/alpha 运算与因子、双源混合、写掩码、16 种 logic op、blend constant、uint32 sample mask、alpha-to-coverage；Qt General/RT 参数页、旧 preset、字段合并、保存与撤销/重做 | 逻辑运算要求设备与每个 RTV 格式支持；不兼容组合明确拒绝；MSAA 执行及 Output/Texture 样本查看已验证；私有 coverage 诊断仍待迁移 |
| Sampler 实验 | 六阶段 × 16 槽 descriptor 编辑：36 种过滤、地址/比较/各向异性、border RGBA、LOD；sampler setter 的起始槽/资源数组、部分覆盖、空调用、ClearState、范围移动时保留前序绑定；Qt 编辑、保存、撤销/重做 | descriptor 只作用于选定 draw/dispatch；继承最终 setter 绑定；缺失前序观察和不支持的 min/max filtering 明确拒绝；私有 coverage/quad 消费者仍待迁移 |
| SRV 描述符实验 | 六阶段 × 128 槽、十一种维度、格式与 mip/array/buffer 范围；逐事件字段合并、维度切换、原生视图创建、输入 buffer 克隆归属；Qt 编辑、保存与撤销/重做 | 继承最终 SRV setter 编辑；原生驱动处理 SRV/UAV 重叠；texture 输入克隆已接入；私有诊断消费者仍待迁移 |
| 全局视图实验 | SRV / RTV / DSV / UAV 的格式、维度、mip / 数组层 / 3D 切片 / buffer 范围与标志；不可变捕获副本；Clear / Draw / Dispatch / GenerateMips、绑定冲突与 counter 使用最终描述符；Qt 编辑、项目历史、保存重开、撤销重做；主输出跟随最终视图范围 | 原始捕获字段保持不变；私有 coverage/debug 消费者仍待迁移 |
| 纹理原始存储读回 | 非 MSAA 的完整 mip / 数组层 / 3D 切片紧密存储；CLI texture-storage 导出；WARP 绑定中的 mip 检查复制保留 RTV 和 UAV counter | 图像转换独立于原始字节；Qt DDS/RAW/PNG 导出已接入；旧式 P010/P016 初始数据仅恢复 Y；planar 写入来源已接入 |
| SRV setter 实验 | 六阶段 × 128 槽的起始槽/视图数组；逐槽跨快照继承、部分覆盖、范围移动/缩短、空调用、ClearState、缺失资源恢复；mip/layer 与只读 depth/stencil 冲突、原生解绑保持；Qt 编辑、保存与撤销/重做 | 移动范围要求可确认的前序绑定；描述符和 buffer 补丁/导出继承最终绑定；texture 输入克隆及私有诊断依赖尚未闭合 |
| CB / CB1 setter 实验 | 六阶段、18 种编码；槽范围、buffer 数组、可选 first/count 窗口、跨快照保持、普通 setter/部分覆盖/ClearState 恢复；常量反射和 buffer patch 使用最终绑定；Qt 编辑、保存及撤销/重做 | 快照不能提供缺失的窗口历史；未知移出槽拒绝猜测；偏移窗口需驱动支持；私有诊断消费者仍待迁移 |
| IA setter 实验 | input layout、VB 槽范围、stride/offset、IB 格式/offset；跨快照保持、原始 setter/ClearState 恢复、输出冲突、几何和 buffer patch；紧凑 Qt 编辑器、项目保存与撤销/重做 | 移动范围需已知前序绑定；缺失输出影响冲突判断时明确拒绝；覆盖/后变换等消费者仍待迁移 |
| Output / SO setter 实验 | RTV/DSV、OM/CS UAV、SO 目标/偏移；KEEP、独立可选数组、64 槽 UAV、跨快照绑定和已知 SO cursor 保留；紧凑 Qt 表格、项目保存与撤销/重做 | buffer、counter、SRV 描述符及 IA 几何使用最终绑定；未知 SO cursor 拒绝猜测；texture 实验及私有诊断仍待迁移 |
| 图像 | 自动/指定 swap chain、RT0–7、深度/模板；RTV/DSV mip、数组层、3D 切片、buffer 范围；浮点/整数/packed 格式、通道/范围、原始存储与 PNG 导出；MSAA resolve/指定样本、整数位保真、深度/模板 resolve；显示设置、设备、事件与边界保存/恢复；像素关联 API/资源和 buffer 元素字节 | before-draw 边界语义统一、像素历史/覆盖/调试消费者尚未完成；MSAA 帧前逐样本内容不声称恢复 |
| 纹理 | GPU 格式转换、BC、浮点/整数、1D/2D/3D、mip/layer/slice、事件边界预览；MSAA resolve/sample、typed format、Y/UV、通道/范围、DDS/RAW/PNG、输入/输出导入、实验及平面写入报告 | 旧式 P010/P016 缺失 UV 不可恢复；私有分析消费者仍待迁移 |
| Shader | DXBC 反汇编、反射、SPDB/SDBG 内嵌源码、HLSL 编译替换 | 不含 HLSL 恢复、单步调试及全部反射树 |
| Buffer | 初始值、事件前后读回、范围、Hex/ASCII/32 位解释、导出；事件字节编辑/二进制补丁导入、绑定识别、撤销/重做；递归 CB 字段、CB1 范围和类型化编辑；按 UAV view 检查/编辑 Append/Consume/Counter | counter 支持 CS/OM 快照及已恢复 clear/copy/setter 引用；输出编辑和扩展 UAV 已接入；完整 setter/getter 与私有诊断仍待迁移 |
| 几何 | IA 输入解码、索引与实例、DrawAuto 实际参数及来源、三种顶点表、旋转线框、CSV/OBJ 导出 | 后变换与覆盖未迁移；当前表格上限为 100 万引用 / 1600 万字段 |
| 资源名称 | GenPrivateData 原始名称、非法 UTF-8 转义、列表筛选和属性显示 | 名称记录不表示逐事件重命名时间线 |
| 实验项目 | 原格式 JSON、捕获 SHA-256 绑定、资产校验、uint64 ID、原子保存、撤销/重做 | 接受事件启停、buffer、clear、update_source、predicate/sampler/SRV/output/SO/shader 与 topology/RS/OM/IA/CB/CB1 setter、全局 view、sampler / SRV descriptor、depth/stencil、rasterizer/viewport/scissor 与 blend pipeline、事件/初始 UAV counter、全局 shader/texture 替换；其他操作明确拒绝 |
| 性能 | 原生 D3D11 时间戳、disjoint 与 pipeline statistics | 尚未迁移原版多轮调度、Intel 硬件指标/GTPin |

## 验证证据

六阶段 shader setter 已支持程序／类实例替换和解绑、跨快照持续生效、原始
setter／ClearState 恢复、项目撤销／重做。常量反射和 Pipeline 的 shader 导航
使用编辑后的资源，Compile & Apply 可继续修改该 shader。详见
`docs/SHADER_SETTER_MIGRATION.md`；私有覆盖／quad／调试消费者仍待迁移。
本轮 29 个 CTest 套件、428 项综合对照（含 576 个参数／布局用例）通过。
GF2 的 50 条和 BF1 的 665 条 PS setter 全部编辑后的输出与 Python 一致，
撤销恢复金标准；最终独立包的原始帧及禁用 draw 负对照也全部通过。

六阶段 CB／CB1 setter 已迁移，涵盖 18 种命令编码、逐槽保持、窗口范围、
移动／缩短范围时的前序窗口保留、普通 setter 重置和 ClearState。常量反射、
buffer patch 与 Pipeline 导航使用编辑后的绑定。870 项综合对照通过，包含
2,196 个 CPU 用例（354 接受、1,842 拒绝）以及硬件／WARP 六阶段实际绑定与输出。
GF2 的 144 条和 BF1 的 542 条 CB setter 全部编辑后与 Python 输出一致，
撤销恢复金标准；真实帧六项检查通过。Release 全部 31 个 CTest 套件通过；
补充缺失窗口数组的编辑修复后，完整 UI 再次通过（49 项、无跳过）。最终包
原始帧及禁用 draw 负对照四项通过。
870 份运行时报告无 Python/GPA/RenderDoc，Qt 来自独立包。详见
`docs/CONSTANT_BUFFER_SETTER_MIGRATION.md`；私有诊断消费者仍待迁移。

Edit Setter 已接入 IA 输入布局、顶点缓冲区和索引缓冲区的持久编辑。
133 项综合对照通过，包含 411 个 CPU 参数／布局用例（66 接受、345 拒绝）、
硬件／WARP getter、像素、几何 CSV/OBJ、buffer patch 和 output/SO 组合实验。
GF2 分别编辑 72 条 VB、72 条 IB、16 条 layout；BF1 分别编辑 1,194、641、645 条。
18 项真实帧检查全部通过，编辑输出与 Python 一致，撤销精确恢复金标准。
本轮 Release 的 29 个非 UI 套件通过；修正旧 blendHistory 测试对延迟预览的等待后，
完整 UI 套件再次通过（46 项、无失败和跳过），覆盖当前全部 30 个 CTest 套件。
最终包的原始帧及禁用 draw 负对照四项通过；139 份综合／真实帧运行时报告
未加载 Python/GPA/RenderDoc，Qt 来自独立包。详见
`docs/IA_SETTER_MIGRATION.md`；CB/CB1 已接入，私有诊断消费者仍待迁移。

Edit Setter 新增拓扑、光栅化状态、视口、裁剪矩形、混合状态和深度／模板
状态六类持久编辑；后续同类 setter / ClearState 结束覆盖，逐 draw 实验继承
修改后的默认值。748 项参数／布局对照与 269 项综合检查通过，包含硬件／WARP
真实 getter、输出像素／原始存储、几何表格／OBJ，以及 SO→DrawAuto 链路。
Qt 实际 Worker 验证了编辑、撤销、重做；GF2 的 9 条和 BF1 的 208 条视口
setter 全部修改后图像与 Python 一致，撤销后恢复原始金标准。详见
`docs/PIPELINE_SETTER_MIGRATION.md`。IA 资源和 CB/CB1 setter 已接入。

普通 Output 的 Before event 已与 Python 遍历边界对齐：在所选 draw/dispatch
之前停止；Pipeline／输入检查继续准备所选快照。缺失 RTV、输入布局或 SRV
可以由完整快照恢复，但不提交所选命令。成功 Map 写入现会更新输出导航的
最后工作事件；失败或无写入数据的 Map 不会覆盖它。GF2／BF1 首、中、末
draw/dispatch 的前后边界共 12 项独立包对照通过，像素、原始存储、选择及
导航元数据均与 Python 一致。详见 `docs/BEFORE_BOUNDARY_MIGRATION.md`。

- 输出会话已接入项目 JSON，保存保持原实验历史并保留未迁移页面的未知字段。
  uint64 ID、数值范围文本、非法设置拒绝、菜单保存重开、WARP/事件/样本恢复、
  点击与拖动区分、过期图像拒绝和 buffer 元素导航均通过测试。
  Release 全套 CTest **27/27**，Qt UI **41 项**无跳过；最后一次像素有效性修正的
  Release / Debug 四项交互回归均通过。独立包 GF2/BF1 黄金帧及负对照四项通过。
  详见 `docs/OUTPUT_SESSION_MIGRATION.md`。

- 主输出和 MSAA 的验证说明见 `docs/FRAME_OUTPUT_MIGRATION.md`。
  CPU **6,515 用例**在 Release / Debug 中通过；完整 Python / native 对照
  **1,560 项**通过，包含硬件 / WARP 和直接 D3D11 样本图案验证。
  Release 全量 CTest **27/27** 通过，其中 Qt UI **38 项**全部通过；
  新增逐样本切换、空输出清除、通道/范围及捕获切换重置测试。
  反复读取 MSAA 样本后的 RTV 身份和引用计数保持不变。
  独立包 GF2/BF1 黄金帧与禁用 draw 负对照四项通过，真实视图编辑六项通过。
  318 份成功报告的 85 个加载模块路径未包含 Python / GPA / RenderDoc；
  打包的三个 EXE 与验证后的 Release 构建哈希一致。
  Python 工具仅用于开发对照，不作为应用运行时后端。

- 全局 view 描述符完成 **9,155 项** Python 规则对照，Release / Debug 均通过。
  原生执行完成 **358 项**对照，覆盖硬件 / WARP、纹理范围、buffer UAV、
  counter、只读 DSV、sRGB 清除、GenerateMips、拒绝无效 native 描述符，
  以及真实 GF2 / BF1 编辑输出和原始哈希恢复。Qt 新增两项交互测试通过，
  包括菜单 → Worker → 图像变化 → 撤销 / 重做。
  Release 全量 CTest **26/26** 通过，其中 Qt UI **37 项**全部通过；
  Debug view 测试 **13 项**和新增 Qt 两项交互测试通过。
  独立包的 GF2/BF1 黄金帧及禁用 draw 负对照共四项通过。
  详见 `docs/VIEW_MIGRATION.md`；仍按部分迁移标记依赖其他消费者的功能。


- 本轮 output/SO 的原生执行和 Qt 编辑入口已接通。首轮 `output-replay-full-01`
  完成 1,142 项对照；`output-replay-extended-02` 完成 132 项，包括 counter 连续性、
  buffer/counter 组合顺序、OM KEEP/slot 63、六阶段连带绑定、混合 SRV 编辑，
  以及 GF2/BF1 在硬件和 WARP 上的真实编辑边界。最终隔离发布包完成 **1,992 项**
  对照，见 `artifacts/output-replay-package-full/validation.json`；黄金帧及禁用 draw
  负对照通过，见 `artifacts/validation-output-editors-golden/validation.json`。
  1,242 份成功原生报告的 75 个加载模块路径未出现 Python/GPA/RenderDoc，
  三个打包 EXE 与 Release 哈希一致，见 `artifacts/output-editors-audit.json`。
  Release/Debug 输出绑定各 9 项测试及新增 Qt 三项测试通过，涵盖重复重放、
  保存重开、撤销重做和菜单到 Worker 的完整操作。
  最终 Release 全量 CTest **25/25** 通过，其中 Qt UI **35 项**全部通过；
  日志为 `artifacts/ctest-output-editors-final-release.log`。

- 输出/SO setter 的参数与历史缓存继续迁移：完整参数集合、严格整数/数组、KEEP、
  五种 SO 编码、重复目标及偏移检查；按 context 单次推进、只缓存差异、反向读取、
  缺失输出的 UAV counter 重置拒绝，以及 retained SO 的 KEEP 偏移标记。
  `tools/validate_output_history_port.py` 的 65 组、2,480 项 Python 对照通过，
  其中 548 项为参数/编码，1,932 项为历史/状态读取，包含 490 项预期拒绝。
  Release 证据为 `artifacts/output-history-combined/validation.json`，
  Debug 证据为 `artifacts/output-history-debug/validation.json`。
  此前 8,152 个边界的模型回归也通过，见
  `artifacts/output-history-model-regression/validation.json`。
  这些接口现已接入实验项目、原生 SO cursor 保留、native getter 校验及 Qt 编辑表单。
  texture 实验与私有诊断消费者仍待迁移。

- 输出绑定核心新增 `OutputBindingModel`，迁移原始/实验双状态、KEEP、跨输出冲突、
  SRV/IA 连带解绑、缺失记录后的受影响字段和跨快照差异。最终 Python 对照覆盖
  42 条流、8,152 个边界，每处比较全部 1,008 个字段、dirty 集合及 overlay。
  输出命令的共享检查解析器已接入原生重放和 SRV 历史。
  后续批次已补齐历史缓存、counter 缺口检查及 output/SO 的项目和 Qt 编辑入口。
  此处数据仍是底层模型验证，完整消费者迁移尚未完成。
  硬件/WARP 的 288 处 getter 对照发现四处空 VB 的 stride/offset 与 Python
  ClearState 模型不同，已明确记录，不能据此声称所有 GPU 状态完全一致。
  具体边界与后续接入要求见 [输出绑定迁移记录](OUTPUT_BINDING_MIGRATION.md)。
  本轮新包 GF2/BF1 黄金帧及两项禁用 draw 的负检查均通过；Release 25/25
  CTest、Debug output/input/SRV binding 三套检查通过。证据位于
  `artifacts/output-model-verified/validation.json`、
  `artifacts/validation-output-model-golden/validation.json` 和
  `artifacts/ctest-output-model-*.log`。
  共享解析器变更后的 164 项 SRV 边界回归也全部通过，见
  `artifacts/output-model-srv-edges/validation.json`。

- SRV setter 的最终 `out/FloraGPA-srv-bindings-final/` 包通过 1,272 项 Python 对照：
  `artifacts/srv-setters-verified/validation.json` 中 1,104 项合成与四项
  真实帧对照，以及 `artifacts/srv-setter-edges-verified/validation.json` 的 164 项
  组合边界检查。覆盖六阶段 × 128 槽、前后边界、禁用事件、部分覆盖、空调用、
  移动/缩短范围、ClearState、槽位 127 与 shader 未使用槽位、缺失资源恢复，
  以及 RTV/UAV 解绑后不恢复已清空的 SRV。
  GF2 的 151 条、BF1 的 2,173 条 setter 置空编辑与 Python 整帧逐字节相同，
  移除编辑后恢复原黄金帧。全部十一种维度（包括 MSAA）的描述符继承，
  setter / descriptor 操作顺序、只读 depth/stencil 四种标志、未知输出冲突，
  新绑定 buffer 的输入克隆、事件前/后导出字节与绑定角色均已对照。
  CPU 测试覆盖无前序观察的拒绝、历史回退、无效 ClearState 保持未知、
  保存/加载与失败重绑定的项目回滚。Debug 增量链接曾出现损坏 COFF 中间文件，
  清理后完整重建成功；Qt 紧凑视图数组编辑器与 sampler 共用资源模型，
  截图 `artifacts/srv-setter-ui-final/srv-setter-editor.png` 已检查。
  context 记录交错时，原生离线检查曾将有效的继承绑定误判为空；修正后
  硬件/WARP、两种操作顺序和 CPU 检查全部通过。捕获前序观察按 context 分开，
  编辑覆盖按原版单一重放 context 的命令顺序继承，不代表新增了多设备重放。
  最终 Release 24 套测试、Debug 六套相关测试均通过，见
  `artifacts/ctest-srv-setters-verified-release.log` 与 `ctest-srv-setters-verified-debug.log`。
  Debug 的 SRV setter / descriptor 与 sampler setter 三项 Worker 交互通过
  （`artifacts/srv-setter-debug-verified-interactions.txt`）；buffer 编辑交互另在配置
  外部 GF2 夹具后复查（`artifacts/srv-setter-debug-buffer-history.txt`）。
  最终黄金帧及关闭 draw 负对照共四项通过
  （`artifacts/validation-srv-setters-verified/validation.json`）。
  1,232 份成功原生报告的 75 种模块未发现 Python、GPA 或 RenderDoc，
  三个 EXE 与最终 Release 哈希一致（`artifacts/srv-setters-module-audit.json`）。
  texture 输入克隆和私有诊断消费者仍待迁移。

- SRV 描述符迁移的最终独立包通过 288 项 Python 对照，记录为
  `artifacts/srv-final-comparison/validation.json`。覆盖六阶段全部 768 个实际槽位、
  十一种维度、typed/raw/structured buffer、格式切换、剩余 mip、共享视图、
  shader 未使用的高槽位、前后边界、禁用事件、下一事件恢复、非法描述符及输入克隆。
  SRV 与 UAV 覆盖相同 mip 时由原生 D3D11 解除绑定，不同 mip 可以共存；
  getter 结果和计算输出均经过硬件/WARP 对照。
  GF2 的 15 项、BF1 的 9,817 项多 mip 编辑与 Python 整帧输出逐字节一致，
  移除编辑后恢复各自黄金帧。原生测试另验证描述符创建失败时恢复输出补丁与输入存储。
  Release 全部 23 套测试通过（`artifacts/ctest-srv-final-release.log`）；
  Debug 三套相关测试及 SRV Apply/Undo/Redo、参数校验、sampler 回归交互通过
  （`artifacts/ctest-srv-final-debug.log`、`artifacts/srv-debug-ui.txt`）。
  Qt 的维度切换、目标重载、溢出、无改动与过期提交检查通过；紧凑编辑器截图
  `artifacts/srv-ui/srv-editor.png` 与 `srv-array-editor.png` 已检查。
  最终包的黄金帧及关闭 draw 负对照共四项通过
  （`artifacts/validation-srv-final/validation.json`）。268 份报告的模块审计
  未发现 Python、GPA 或 RenderDoc，三个发布 EXE 与 Release 构建哈希一致
  （`artifacts/srv-module-audit.json`）。texture 输入克隆及私有诊断依赖尚未闭合，因此相关模块仍标记为 partial。

- Sampler descriptor / setter 迁移的最终独立包通过 954 项合成 Python 对照，
  覆盖六阶段 × 16 槽实际绑定、输出字节、命令前/后、禁用 draw、部分覆盖、空调用、
  范围移动/缩短、ClearState、操作顺序和非法输入；全部 36 种 filter、五种寻址、
  comparison、LOD 限制和偏移均包含硬件/WARP 对照。
  证据为 `artifacts/sampler-final-comparison/validation.json`。
  整帧追加验证共 6 项通过（`artifacts/sampler-real-comparison/validation.json`）：
  GF2 的 576 项 LOD 编辑 / 98 项空绑定编辑、BF1 的 27,373 项 LOD 编辑 /
  476 项空绑定编辑均与原版输出逐字节相同，撤销实验后恢复原黄金帧哈希。
  第一轮整帧验证因脚本误传 `--event 0` 被 CLI 拒绝；修正为省略该参数后，
  以上六项单独重跑通过，954 项合成结果未受影响。
  原生前序记录、范围、历史、保存/加载和事务回滚的 Release / Debug CPU 检查
  通过；Qt 紧凑参数/资源数组编辑器已经接入菜单和 API Log 右键。
  936 份成功重放报告的运行模块审计未发现 Python、GPA 或 RenderDoc，
  记录为 `artifacts/sampler-module-audit.json`。私有 coverage/quad 消费者仍待迁移。
  Release 22 套测试最终均通过：首轮的 21 套非 UI 回归和原生 sampler GPU 测试
  见 `artifacts/ctest-sampler-final-release.log`；新 setter UI 夹具在首个 draw 前
  没有可识别输出目标，补入零顶点 draw 建立目标后，完整 UI 套件复跑通过
  （`artifacts/ctest-sampler-final-ui.log`）。Debug 四套相关测试通过
  （`artifacts/ctest-sampler-debug.log`）；五项 Qt 交互的结果见
  `artifacts/sampler-debug-ui.txt` 及夹具修正后的 `sampler-debug-setter-ui.txt`。
  最终包的两份黄金帧和关闭 draw 负对照共四项通过，记录为
  `artifacts/validation-sampler-final/validation.json`。界面截图已检查，位于
  `artifacts/sampler-ui-verified/`；三个发布 EXE 与最终 Release 构建哈希一致。

- 混合/采样迁移初轮 639 项 Python 对照通过，逐字段比较实际管线描述、draw 计数
  与 RGBA8 纹理预览；覆盖普通/双源因子、颜色/alpha 运算、全部写掩码、八槽 MRT、
  16 种 logic op、捕获的逻辑状态、旧 BlendState、历史合并与非法输入拒绝。
  证据为 `artifacts/blend-first-comparison-v2/validation.json`。
  `tests/BlendTests.cpp` 另直接读回浮点/整数原始存储，以算术和按位运算独立核对；
  MSAA 测试逐样本读取，不用 resolve 冒充采样验证。alpha-to-coverage 与单独创建的
  D3D11 状态逐样本一致。Release 全部 21 套 CTest 与 Debug 五套相关测试通过，
  记录为 `artifacts/ctest-blend-release.log` 和 `ctest-blend-debug.log`。
  Qt Apply/Undo/Redo、uint32 溢出、只记录改动槽位、无改动和过期提交检查通过；
  Debug 另复查了 rasterizer/depth 编辑交互（`artifacts/blend-debug-ui.txt`）。
  已检查 `artifacts/blend-ui/blend-general-editor.png` 和 `blend-target-editor.png`。
  首版独立包的扩展对照共 665 项通过，另覆盖捕获 enable=true 的旧 preset 恢复，
  以及真实 GF2/BF1 draw 的提交前、原始输出和零写掩码实验：编辑后保留提交前
  颜色存储，draw 次数不变。证据为 `artifacts/blend-package-comparison/validation.json`。
  源码复核后补齐了原版对捕获 BOOL 的真值归一化，原生测试新增 264/328 字节
  描述符、非标准 BOOL、截断/多余字节的检查。更新后 Release 21 套再次全部通过
  （`artifacts/ctest-blend-final-release.log`），Debug blend 套件与两项编辑交互通过
  （`artifacts/ctest-blend-final-debug.log`、`blend-final-debug-ui.txt`）。
  最终 `out/FloraGPA-blend-v2/` 包通过全部 671 项对照，包括硬件/WARP 的非标准
  捕获 BOOL，以及再次执行的真实帧编辑验证；GF2/BF1 黄金帧和关闭 draw 负对照
  全部通过。证据为 `artifacts/blend-final-comparison/validation.json` 和
  `artifacts/validation-blend-final/validation.json`。本批另通过 309 项光栅化回归
  （`artifacts/blend-rasterizer-regression/validation.json`）。

- 光栅/视口/裁剪迁移的最终包通过 309 项 Python 对照，覆盖硬件/WARP 的实际画面、
  UAV 字节与管线字段，十二项状态、空/满槽数组、字段合并、命令前/后、下一 draw
  恢复，以及捕获 State/State1/State2。强制采样覆盖 0/1/2/4/8/16、无 RTV、
  conservative raster、设备拒绝、DSV/深度/MSAA RTV/逐样本或深度输出 PS 限制，
  包括替换 PS 后重新检查。非法组合仍允许 before/disabled 边界检查。
  证据为 `artifacts/rasterizer-package-comparison-v2/validation.json`；原生子进程
  仅使用 Windows 系统 PATH，未加载 Python、GPA 或 RenderDoc。
  Release 全部 20 个 CTest 套件通过；Debug 五个相关套件与三项编辑器交互通过，
  记录为 `artifacts/ctest-rasterizer-release.log`、`ctest-rasterizer-debug.log` 和
  `rasterizer-debug-ui.txt`。Qt 测试验证实际画面的 Apply/Undo/Redo、只保存改动字段、
  数组增加/删除/上限、无改动不写历史、非法值和过期提交拒绝。
  截图为 `artifacts/rasterizer-ui/rasterizer-state-editor.png` 与
  `rasterizer-scissors-editor.png`。私有 coverage 仍待迁移；blend 编辑见上述后续批次。
  最终独立包还通过 517 项深度/模板回归与 GF2/BF1 黄金帧及关闭 draw 负对照，
  证据为 `artifacts/rasterizer-depth-regression/validation.json` 和
  `artifacts/validation-rasterizer-package/validation.json`。

- 深度/模板编辑初轮 517 项 Python 对照通过，覆盖 D24S8 / D32S8、硬件/WARP、
  八种深度比较函数、模板比较、正反面三条分支的八种操作、mask/reference、字段合并、
  命令前边界与下一 draw 恢复；证据为 `artifacts/depth-compare/validation.json`。
  `tests/DepthStencilTests.cpp` 另直接读回 96 组原生模板操作的存储字节，并检查
  深度保持/写入、项目保存/加载、撤销/重做与非法输入不改变历史。Qt Worker 的
  Apply/Undo/Redo 验证画面黑/红切换，截图为 `artifacts/depth-ui/depth-stencil-editor.png`。

- Predicate setter 编辑已接入 API Log 右键与 Edit 菜单、资源选择和原始 uint32 BOOL 输入、
  原格式 setter 项目操作、保存/加载及撤销/重做。覆盖会持续影响后续 draw/dispatch，
  直到原始 setter 或 ClearState；失败的绑定不会提前更新覆盖状态。首轮 1,233 项
  Python 对照通过，证据为 `artifacts/predicate-setter-first/validation.json`。
  原生测试验证重复重放和历史分支；Qt Worker 验证输入拒绝及 apply/undo/redo。
  完整 setter 家族与尚未迁移的私有分析路径仍待完成。

- Predication 的 Python 对照共 624 项通过：硬件/WARP、false/true、原始 BOOL
  0/1/7/0xffffffff、六组 context 命令、前后边界、空查询初值、hint、ClearState、
  shader 替换/生产者禁用/跳过 dispatch 的输出编辑、四流溢出和非法调用拒绝。
  16 个完整管线边界的每个字段也与原版一致。非标准 BOOL 的硬件/WARP 差异
  同样由原 Python 复现，未将原始值归一化。Qt Worker 验证 active/ready/false、
  绑定状态和过期结果丢弃；界面截图为 `artifacts/predicate-ui/predicate-inspector.png`。
  初轮对照证据为 `artifacts/predicate-comparison-first/validation.json`。

- SO / DrawAuto 原生回归覆盖硬件和 WARP、重复重放、dirty 快照重置、输出签名、
  点/线/三角形计数、输入克隆和禁用生产者、未知历史的实验拒绝，以及声明/数组的
  截断和非法字段。逐流 GPU 查询提供实际写入与所需容量；GPU 结束时间戳在 SO
  CPU 读回等待之前提交。Qt Worker 测试验证 SO 声明属性、反射条目、DrawAuto 的
  六顶点 IA 表和计数来源，截图位于 `artifacts/so-ui/`。
  最终发布包仅系统 PATH 的 690 项对照全部通过，覆盖五种 context setter、四流、
  GS/VS/DS/签名 passthrough、linked GS、溢出/零输出、shader 与 buffer 实验，
  以及五份原始 GPA SO 捕获的图像/缓冲区基准、逐生产者禁用、帧尾遍历和时间戳。
  原始线输出捕获保存计数 6，硬件/WARP 均重建为实际 12 个顶点；没有直接相信捕获值。

- 动态类链接的 Python 对照覆盖六个 shader 阶段、具名和显式创建实例、实例数组顺序、
  CB/texture/sampler 偏移、动态/静态替换以及命令前后边界。发布包 207 组对照全部通过：
  71 组 GPU 输出逐字节一致，70 个管线边界共 91,420 个字段一致，60 组元数据/DXBC
  导出及 6 组明确拒绝检查通过。C++ 单元测试另覆盖资源记录
  逐字节截断、非法名称/owner/flag、重复读取和空函数表 PS。Qt 测试通过真实 Worker
  验证资源属性、静态 HLSL 替换改变输出和撤销恢复动态 shader；截图位于
  `artifacts/class-linkage-ui-final/`。保留 Python 原版的快照绑定语义；未启用编辑时的
  shader setter 不额外执行，避免把 BF1 中缺失资源的 dormant setter 当作有效快照。
  原 Python 与早期 C++ 均在本机 NVIDIA 10de:249d 上复现空函数表 PS 访问违例；
  当前 C++ 在该适配器上明确拒绝此路径并提示 WARP，WARP 保留原始 DXBC 且输出正确。
  正常动态 shader 的硬件路径另外验证；不据此宣称其他 GPU 的该缺陷已验证。

- 命令间输入绑定：硬件与 WARP 的六阶段 native getter 对照，涵盖 SRV 127、sampler 15、
  CB 13、VB 31、IA layout / IB、CB1 窗口与 legacy 重置、空数组和重复 replay；
  SRV / UAV 冲突实际解绑、缺失 SRV 整组失效及同阶段局部覆盖、缺失 layout / 输出恢复。
  加上 GF2 / BF1 的实际边界，131 组捕获与拒绝路径对照通过，77 个有效边界共
  87,164 个字段与 Python 一致；BF1 25572 之后缺失输出绑定的拒绝行为也与原版一致。
  KEEP-RTV 调用忽略缺失 DSV 参数，错误视图类型、非法 KEEP 和 RTV/UAV 槽位重叠明确拒绝。
  这不代表 setter 实验或任意捕获已完成迁移；原生管线检查的证据另列如下。

- 重放管线检查的 180 组 Python 对照全部通过：153 个有效命令边界共 199,818 个字段，
  包括值、来源、对象标记、描述和限制；另有 27 个预期拒绝边界。覆盖硬件/WARP、
  六阶段绑定、扩展 UAV 槽、rasterizer 扩展描述、共享 sampler 身份、CB/SRV 临时克隆、
  禁用与撤销、ClearState，以及 GF2/BF1 边界。命令前检查不提交 draw/dispatch，重复
  读取不累计 GPU 写入；回调异常恢复实验存储。Qt 测试覆盖 Worker 读取、完整 JSON
  导出、筛选、资源跳转、禁用/撤销及过期结果丢弃。截图位于
  `artifacts/replay-pipeline-ui/`，对照结果位于 `artifacts/replay-pipeline-comparison-2/`。

- GF2：72 draw、0 dispatch、44 Map；BF1：1202 draw、73 dispatch、4151 Map。
  两者 RGBA SHA-256 均与 Python 基线相同，详见 `NATIVE_BASELINE.md`。
- 关闭 draw 后，两份输出均发生变化；Map/dispatch 数量仍符合预期，排除了
  直接展示捕获参考图的替代路径。
- 13 个纹理案例覆盖 BC、sRGB、浮点、整数、A8、mip、array、1D 与 3D slice，
  均与原版 GPU 预览逐字节一致。
- GUI 测试覆盖打开、图像哈希、事件导航、管线、指标、纹理、HLSL 编译应用、
  撤销精确恢复与重做复现、IA 几何、buffer 边界和范围。项目测试覆盖资产/捕获绑定及大整数，shader 测试
  覆盖本机 compiler 47 的 SPDB 和 compiler 43 的 SDBG。
- 六个 IA 几何案例的参数、三个 CSV 表和 OBJ 坐标/拓扑与原版一致，包含 BF1
  的 9 实例 draw。间接参数解析已有代码，但这六个实测案例没有覆盖间接 draw。
- 五个 buffer 案例的二进制及逐字数值与原版一致；错误资源、范围溢出、事件 0
  和 uint32 索引溢出均返回明确错误。
- GF2 的 141 条资源名称及 BF1 的空名称目录与原版一致。
- GF2 / BF1 的五个命令实验案例，涵盖 RTV、Float UAV buffer 清除和两种纹理格式的
  UpdateSubresource 源替换；事件前、事件后及撤销后的 15 组数据与 Python 逐字节一致，
  且五个案例的修改结果均不同于原始结果。
- 命令测试覆盖四种 clear 的数值和记录布局、11 种可启停命令布局、越界/校验和/延迟
  context 拒绝；WARP 验证 buffer 区域更新、Uint UAV 清除和撤销/重做。mip/array、3D、
  BC 边界、planar 对齐检查属于布局测试，不代表这些格式全部完成 GPU 实测。
- Qt 交互测试验证清除值编辑、Worker 实际执行、当前纹理刷新与撤销/重做；
  窗口截图为 `command-ui/clear-dialog.png` 和 `command-ui/clear-edited.png`。
- Buffer 实验区分事件输入克隆与输出原位修改；输入在提交后恢复，输出仅在真正提交后
  保留。WARP 合成 compute 捕获验证 CB/SRV 重定向、下一条 dispatch 不受输入污染、
  输出持续修改、禁用/预览/异常回滚、重复 replay、重叠补丁、项目保存与撤销/重做。
  Append UAV 经两次 dispatch 和 CopyStructureCount 后计数仍正确，未重新创建原 UAV。
- GF2 / BF1 的 CB、VB、IB、SRV、UAV 五类真实缓冲区，20 组事件前/后、禁用、撤销
  数据及绑定元数据与 Python 一致；另对 GF2 可见 draw 430 验证修改后的 VB/IB 几何表、
  实际渲染图与 Python 一致，且渲染结果区别于未修改版本。
- Qt Buffer 编辑器经 Worker 验证修改、撤销/重做以及纯输入在事件后恢复原始字节；
  `buffer-edit-ui/` 保留编辑窗口与缓冲区表格截图。
- CB 递归反射与编辑：bool/int/uint/float/double、向量、行/列主序矩阵、数组和结构体；
  在整个缓冲区上按 CB1 范围计算字段，字节导出范围不改变字段偏移。字段不可用、越界及
  不支持的布局明确标记；不推测被剥离的变量名。
- 编译 DXBC 的 22 个递归叶字段、数值和编辑补丁与 Python 一致；GF2 / BF1 的原始 shader、
  带反射替换、字段修改、精确位模式、事件后恢复与撤销共 12 组检查也一致。BF1 原始 shader 本身有
  3 个具名字段，CB1 起始常量 2304、数量 16；GF2 所选原始 shader 没有具名反射，因此
  该部分通过实际 HLSL 编译替换验证，并未声称恢复了剥离的名称。
- 类型化编辑保留未变组件、填充、NaN payload、负零和非规范 bool 位；非连续矩阵补丁
  一次撤销。CLI 输出与 Qt 编辑器均验证负零符号位不被 JSON 规范化丢失。WARP 验证保存/载入后的 typed CB 编辑确实改变 compute 输出，同时后续事件
  的输入恢复。Qt 测试验证矩阵编辑、读回、撤销/重做和过期字段失效。
- UAV counter 按 view 独立管理，支持事件前临时编辑和初始值；捕获中的显式 reset 仍优先。
  `0xffffffff` 通过 GPU helper 写入实际计数，不会误作 D3D11 setter 的 KEEP 标志。
  CS 与 OM/像素着色器路径分别覆盖 Append 和 Counter，WARP 与硬件设备均通过；
  验证预览/禁用/异常回滚、提交后保留、同 buffer 多 view 独立计数、CopyStructureCount、
  buffer 补丁组合、保存/载入及撤销/重做。扩展槽 metadata 检查不代表扩展 UAV 重放已完成。
- UAV counter 的 32 组 Python 对照通过：29 组缓冲区逐字节一致；BF1 的 3 组并行 Append
  输出按完整元素多重集合比较，未写入区域逐字节一致，计数和元数据全部一致。
  Qt 测试覆盖溢出拒绝、最大 uint32 编辑、实际 Worker 读回、撤销/重做与过期结果失效。
- 当前 Release / Debug 各 17 项 CTest 全部通过，包含完整 Qt UI、类链接与新增 SO 测试。
  不是“所有原版测试已通过”。
- API 检查与 Python 对照：GF2 全部 920 条、BF1 全部 18,024 条记录的字段、偏移、原始位、
  引用、状态、query 元数据及解析后的 CSV 一致；另验证 BF1 资源/多词组合筛选。
  合成捕获的 3,948 条记录覆盖固定/可选数组布局、逐字节截断、尾随字节、无效 flag、
  大计数、64 位引用、非有限浮点、损坏 UTF-16 和 DrawAuto 捕获参数；另有 58 条查询顺序、
  类型与结果状态记录。全部对照通过；invalid 诊断的文字允许与 Python 不同，其余结构仍比较。
- Qt API 交互测试覆盖字段/偏移显示、引用纹理跳转、资源 ID 筛选与清除筛选。
  捕获字段保持原始值，实验清除值单独显示。纹理导航会取消尚未开始的帧输出预览。
- Context 对照覆盖 GF2、BF1 和 25 份合成捕获：身份、恢复报告、command-list 清单及 API
  元数据与 Python 一致，包含 21 类拒绝/冲突证据。保留原始索引顺序、64 位捕获 owner、
  原播放器的 32 位 parent/owner 以及不确定的 Execute 候选，不把文件清单当成 GPU 执行序列。
  缺失 context 的 compute 捕获在 WARP 和硬件上均执行成功；两份输出 buffer 与 Python 逐字节一致。
  五个接口版本的 Finish 空操作经 GPU 路径验证；deferred、缺少 Unmap、非零返回引用、
  正 HRESULT 和两种 Execute 记录经实际 Replay 入口拒绝。测试未证明原版尚未恢复的列表执行。
- Qt Context 测试验证捕获身份、推断缺失字段、双页清单和 Map 证据导航。较长说明只放在
  工具提示与导出 JSON，界面保持字段树布局。跳转到被隐藏的证据事件时会清除阻碍定位的
  API 筛选并切换到 API Log。
- 捕获命令状态的逐快照检查：GF2 的 86,380 个、BF1 的 1,545,910 个已知字段预测与
  合格快照一致。BF1 的四处不完整 view 描述仍产生与 Python 相同的未知状态；Dispatch
  快照中省略的 VS/PS 不会抹去此前观察，CB1 范围仅在绑定仍一致时保留。
- 194 组命令状态前/后对照逐项比较值、已知标志、来源事件、原因、资源 ID、notes 和字段顺序；
  包含原 Python validator 的 KEEP/绑定冲突捕获，以及 C++ 测试生成的多 context、
  缺失 SO 偏移、只读 DSV、3D 重叠、扩展槽、CB1 拒绝顺序、非有限浮点和未知命令案例。
  Qt 测试覆盖前/后切换、Known/Unknown 筛选、来源/资源跳转、过期结果失效及真实 GF2 主窗口。
  这些是捕获状态检查证据，不代表这些 setter 的实验编辑和特殊 GPU 重放已全部迁移。
- 发布目录在仅保留 Windows 系统 PATH 的子进程中完成两份黄金重放和负对照。
  模块列表未发现 Python/Tk、GPA 或 RenderDoc；尚未做另一台干净 Windows 验证。
- Qt 自身窗口渲染已检查 1440×900、1920×1080 及 150% / 200% 缩放。
  发布包还通过了本机 Windows 平台插件的原生窗口启动、重放和截图检查；
  这不等于完成所有窗口操作和多显示器验证。

生成证据位于未纳入 Git 的 `artifacts/`：`validation-delivery/`、
`texture-validation.json`、`geometry-validation-1/`、`geometry-validation-instanced/`、
`buffer-validation-2/`、`name-validation.json`、`ctest-release-delivery.log`、
`ctest-debug-publish.log`、`asset-ui-delivery/`、`ui-portable-*.png`、
`ui-windows-delivery.png`、`command-validation-2/validation.json`、`command-ui/`、
`ctest-command-final-release.log`、`ctest-command-debug.log`、`validation-command-package/`。
Buffer 编辑证据为 `buffer-edit-validation-1/`、`buffer-edit-visible-validation/`、
`buffer-edit-ui/`、`ctest-buffer-release.log`、`ctest-buffer-debug.log`、`validation-buffer-package/`。
常量字段证据为 `constant-fixture/`、`constant-validation-2/validation.json`、`constant-ui/`、
`ctest-constants-release.log`、`ctest-constants-debug.log`、`validation-constants-package/`。
UAV counter 证据为 `uav-counter-fixture/`、`uav-counter-validation-4/validation.json`、
`uav-counter-ui/`、`ctest-counter-release.log`、`ctest-counter-debug.log`、
`validation-counters-package/`；对应发布目录为 `out/FloraGPA-counters/`。
API 检查证据为 `api-fixture/`、`api-validation-3/validation.json`、`api-tests.txt`、
`api-ui-test.txt`、`api-ui/api-fields.png`、`ctest-api-release.log`、`ctest-api-debug.log`。
完整 Qt 测试日志保存于 `build/vs2022/ui-Release.txt` 和 `ui-Debug.txt`。
发布包的 API 对照保存在 `api-validation-package/`，完整帧与负对照保存在
`validation-api-package/`；本批发布目录为 `out/FloraGPA-api/`。中文捕获文件名导出
另经 Release / Debug 的 API 测试验证（`ctest-api-unicode-release.log`、
`ctest-api-unicode-debug.log`）。
源码基线逐模块记录在 `migration.json`。
Context 证据为 `context-fixture/`、`context-validation-1/validation.json`、`context-ui/`；
相应 CTest 日志为 `ctest-context-release-final.log`、`ctest-context-debug.log`，发布包为
`out/FloraGPA-contexts/`，隔离 PATH 的黄金重放证据为 `validation-context-package/`。
本批 API 元数据回归保存在 `api-validation-contexts/validation.json`。
捕获命令状态证据为 `state-fixture/`、`state-validation-2/validation.json`、`state-ui/`、
`state-ui-debug/`、`ctest-state-release.log`、`ctest-state-debug.log`；发布包为
`out/FloraGPA-state/`，黄金帧回归为 `validation-state-package/`。
主窗口新增交互的最终 Release 检查另存于 `state-ui-final-release.txt`；仅系统 PATH 的
命令状态读取保存在 `state-isolated-package/`。
输入绑定的最终证据为 `input-bindings-keep/`、`input-bindings-keep-release.txt`、
`input-bindings-keep-debug.txt`、`input-binding-comparison-keep/validation.json`；发布包为
`out/FloraGPA-bindings-final/`，仅系统 PATH 的黄金帧和关闭 draw 的负对照为
`validation-bindings-final-package/`。本批 Release / Debug 各 14 项完整回归日志为
`ctest-input-bindings-release-final.log`、`ctest-input-bindings-debug.log`；随后 KEEP 修正
另经上述 Release / Debug 输入绑定测试、Python 对照和最终发布包重放检查。
重放管线检查的 CTest 日志为 `ctest-replay-pipeline-release.log`、
`ctest-replay-pipeline-debug.log` 与 `ctest-replay-pipeline-debug-ui-fixed.log`。
Debug 首轮暴露旧 UI 测试按类型取错页面的问题，改为按对象名定位后完整 UI 回归通过。
发布包为 `out/FloraGPA-pipeline/`；仅系统 PATH 的 180 组管线对照保存在
`replay-pipeline-package-comparison/validation.json`，两份黄金帧及关闭 draw 的负对照为
`validation-pipeline-package/validation.json`。
同样隔离 PATH 的发布版 GUI/Worker 已完成打开捕获、重放、GPU 指标采集和离屏窗口
渲染，截图为 `replay-pipeline-ui/package-isolated.png`。

动态类链接的发布包为 `out/FloraGPA-classes/`。仅系统 PATH 的 207 组对照为
`class-linkage-package-comparison/validation.json`；两份黄金帧和关闭 draw 的负对照为
`validation-class-package/validation.json`。`class-package-smoke/validation.json` 验证
非纹理空函数表 PS 的 WARP 原字节码输出、硬件明确拒绝，以及发布版 GUI/Worker
打开正常动态 shader 捕获；对应窗口截图为 `class-package-smoke/package-gui.png`。
本批 Release / Debug 回归为 `ctest-class-release.log` 和 `ctest-class-debug.log`；
类资源的截断/非法字段和快照测试详见 `build/vs2022/class-linkage-Release.txt` 与
`class-linkage-Debug.txt`。

SO / DrawAuto 的发布包为 `out/FloraGPA-stream-output-captures/`。最终对照为
`so-captured-package-comparison/validation.json`，两份黄金帧和关闭 draw 的负对照为
`validation-so-captured-package/validation.json`。Release / Debug 的完整回归日志为
`ctest-so-release-timing.log` 与 `ctest-so-debug.log`；各自的 SO 测试包含 18 个通过项，
详见 `build/vs2022/stream-output-Release.txt` 和 `stream-output-Debug.txt`。
帧尾检查记录的补齐另经 Release / Debug 各四项相关回归验证，日志为
`ctest-so-inspection-release.log` 和 `ctest-so-inspection-debug.log`。
新增校验覆盖 15 种 getter/lifetime 记录，保留捕获返回值且不修改 GPU 绑定。
发布版 GUI/Worker 的完整原始线输出重放、时间戳采集及离屏渲染为
`so-package-smoke-fixed/validation.json`，截图为 `so-package-smoke-fixed/package-gui.png`。

Predication 发布包为 `out/FloraGPA-predication/`。Release / Debug 各 18 项完整
CTest 回归全部通过，日志为 `artifacts/ctest-predication-release.log` 和
`artifacts/ctest-predication-debug.log`；各自的原生 predicate 测试有 39 项通过。
仅系统 PATH 的发布包 624 项对照为
`artifacts/predicate-package-comparison/validation.json`；GF2/BF1 黄金帧与
关闭 draw 的负对照为 `artifacts/validation-predication-package/validation.json`。

新增 predication 后，最终发布包的 690 项 SO / DrawAuto 回归仍全部通过，证据为
`artifacts/so-predication-package-comparison/validation.json`。仅系统 PATH 的
GUI/Worker 已完成 GF2 打开、重放和 GPU 指标采集，窗口截图为
`artifacts/predicate-package-smoke/package-gui.png`，运行记录为同目录 `validation.json`。

Predicate setter 发布包为 `out/FloraGPA-predicate-setters/`。仅系统 PATH 的
1,265 项 Python 对照全部通过，记录为
`artifacts/predicate-setters-package/validation.json`。覆盖六种记录族、
硬件/WARP、替换与解除绑定、原始 BOOL、前后边界、后续覆盖、非法输入和查询配对；
条件编辑后的 SO 输出历史与 DrawAuto 顶点数也一致。Qt 的 Apply/Undo/Redo
通过真实 Worker 读取后续 dispatch 的绑定，输入错误不会写入项目历史。

本批 Release 18 项完整回归通过（`artifacts/ctest-predicate-setters-release.log`）；
Debug 的 predicate、experiment、管线、SO 和 buffer edit 五项相关回归通过
（`artifacts/ctest-predicate-setters-debug.log`），编辑/检查的 Qt Worker 测试也通过
（`artifacts/predicate-setters-debug-ui.txt`）。最终发布包的 GF2/BF1 黄金帧及
关闭 draw 负对照均通过，证据为 `artifacts/validation-predicate-setters-package/validation.json`。

深度/模板编辑的 Release 完整回归共 19 项通过，记录为
`artifacts/ctest-depth-release.log`；Debug 的五项相关回归通过，记录为
`artifacts/ctest-depth-debug.log`，另有 `artifacts/depth-debug-ui.txt` 的编辑交互验证。
原生存储测试在两个配置下均通过，未将驱动对关闭 stencil 时无效操作的规范化
误判成字段丢失。该轮验证未包含 MSAA 检查和混合 rasterizer 管线编辑；它们随后
在上述光栅化批次验证；blend 管线编辑见后续混合批次，私有 coverage 仍待迁移。

该轮独立包为 `out/FloraGPA-depth-stencil/`。仅系统 PATH 的 517 项对照再次全部
通过（`artifacts/depth-package-comparison/validation.json`），GF2/BF1 黄金帧及
关闭 draw 负对照通过（`artifacts/validation-depth-package/validation.json`）。

## 尚未闭合的迁移范围

1. 其余 setter/command/context 的重放与实验语义、SO 的 retained 输出与私有诊断、
   扩展 UAV、私有 MSAA/planar 消费者等特殊 replay 路径。
2. 其余资源/状态/绑定/命令编辑，完整 counter 命令引用、HS/写入/发射几何、覆盖率、
   quad 与像素分析。
3. HLSL 恢复、source/instruction 导航、变量/表达式、trace/stack 与 shader 调试。
4. RenderDoc 原生 C++ 后端、Intel Metrics Discovery、GTPin 与完整指标调度。
5. 逐模块原版对照闭合，以及独立机器、更多捕获和完整交互回归。

因此当前可靠性结论限于已验证样本和上述路径，不能推断任意 gpaframe 都能正确
重放，也不能用“两份捕获通过”代表全部 204 个源模块完成迁移。
