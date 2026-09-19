# FloraGPA C++ 迁移进度 — 2026-09-20

已有可编译、可运行的 VS2022 / Qt Widgets 原生工程，能够独立重放当前验证的
GF2 与 BF1 DX11 捕获。**尚未完成原 Python 版本的全部功能迁移。**


## 当前可用

| 区域 | 已迁移功能 | 主要边界 |
|---|---|---|
| 工程 | C++20、VS2022 x64、Qt 6.11.2、CMake、Git、独立 CLI/Worker | 尚未配置远程仓库 |
| 捕获与重放 | 有边界检查的 IGPA v3 读取、六阶段状态、基本 D3D11 资源和 draw/dispatch、Map/更新/复制/清除 | 特殊 replay 路径尚未完全迁移 |
| 主界面 | GPA 式深色三栏、真实 GPU 时间柱状图与概览、可停靠面板、API 筛选、任务取消 | 概览尚无范围拖动；未知功能页禁用 |
| 事件 | draw/dispatch 选择、快照管线、前后边界重放、事件启停 | 非 draw 精确 setter 状态仍不完整 |
| 命令编辑 | RTV / DSV / Uint / Float UAV 清除值；资源写入命令启停；UpdateSubresource 紧密排列源数据替换；菜单、右键、撤销/重做 | 未覆盖缺失 context 的推断恢复；特殊 planar 资源执行仍受现有重放限制 |
| 图像 | 实际 GPU 输出、缩放/平移/通道、像素值、PNG 导出 | 全帧输出目前限制单采样 RGBA/BGRA8 |
| 纹理 | GPU 格式转换、BC、浮点/整数、1D/2D/3D、mip/layer/slice、事件边界预览 | MSAA、平面格式及部分查看选项未迁移 |
| Shader | DXBC 反汇编、反射、SPDB/SDBG 内嵌源码、HLSL 编译替换 | 不含 HLSL 恢复、单步调试及全部反射树 |
| Buffer | 初始值、事件前后读回、范围、Hex/ASCII/32 位解释、导出；事件字节编辑/二进制补丁导入、绑定识别、撤销/重做；递归 CB 字段、CB1 范围和类型化编辑；按 UAV view 检查/编辑 Append/Consume/Counter | counter 支持 CS/OM 快照及已恢复 clear/copy/setter 引用；SO、完整 setter/getter 与扩展 UAV 重放仍待迁移 |
| 几何 | IA 输入解码、索引与实例、三种顶点表、旋转线框、CSV/OBJ 导出 | DrawAuto、后变换与覆盖未迁移；当前表格上限为 100 万引用 / 1600 万字段 |
| 资源名称 | GenPrivateData 原始名称、非法 UTF-8 转义、列表筛选和属性显示 | 名称记录不表示逐事件重命名时间线 |
| 实验项目 | 原格式 JSON、捕获 SHA-256 绑定、资产校验、uint64 ID、原子保存、撤销/重做 | 接受事件启停、buffer、clear、update_source、事件/初始 UAV counter、全局 shader/texture 替换；其他操作明确拒绝 |
| 性能 | 原生 D3D11 时间戳、disjoint 与 pipeline statistics | 尚未迁移原版多轮调度、Intel 硬件指标/GTPin |

## 验证证据

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
- Release 与 Debug 的十组 CTest 均通过。不是“所有原版测试已通过”。
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
源码基线逐模块记录在 `migration.json`。

## 尚未闭合的迁移范围

1. 完整 setter/command/context 语义、predication、stream-output/DrawAuto、
   class linkage、扩展 UAV、MSAA 与 planar 等特殊 replay 路径。
2. 其余资源/状态/绑定/命令编辑，完整 counter 命令引用、DrawAuto 与后变换几何、覆盖率、
   quad 与像素分析。
3. HLSL 恢复、source/instruction 导航、变量/表达式、trace/stack 与 shader 调试。
4. RenderDoc 原生 C++ 后端、Intel Metrics Discovery、GTPin 与完整指标调度。
5. 逐模块原版对照闭合，以及独立机器、更多捕获和完整交互回归。

因此当前可靠性结论限于已验证样本和上述路径，不能推断任意 gpaframe 都能正确
重放，也不能用“两份捕获通过”代表全部 204 个源模块完成迁移。
