# FloraGPA C++ 迁移进度 — 2026-09-20

已有可编译、可运行的 VS2022 / Qt Widgets 原生工程，能够独立重放当前验证的
GF2 与 BF1 DX11 捕获。**尚未完成原 Python 版本的全部功能迁移。**


## 当前可用

| 区域 | 已迁移功能 | 主要边界 |
|---|---|---|
| 工程 | C++20、VS2022 x64、Qt 6.11.2、CMake、Git、独立 CLI/Worker | 尚未配置远程仓库 |
| 捕获与重放 | 有边界检查的 IGPA v3 读取、六阶段状态、基本 D3D11 资源和 draw/dispatch、Map/更新/复制/清除 | 特殊 replay 路径尚未完全迁移 |
| 主界面 | GPA 式深色三栏、真实 GPU 时间柱状图与概览、可停靠面板、API 筛选、任务取消 | 概览尚无范围拖动；未知功能页禁用 |
| 事件 | draw/dispatch 选择、快照管线、前后边界重放、事件启停 | 部分命令执行及 setter 实验仍待迁移 |
| 捕获命令状态 | 六阶段 setter、CB1 范围、IA/RS/OM/SO/predicate 参数、扩展 UAV 槽元数据；命令前/后、逐字段来源、资源重叠失效与快照校准；Pipeline 页异步读取、筛选、跳转、JSON 导出 | 原始捕获状态，不应用实验；只读 DSV / 模糊 3D 重叠保持未知；SO 偏移是 setter 参数，隐藏 counter 和实时写入位置不由此推断 |
| 命令间输入绑定 | 实际执行 IA layout / VB / IB、六阶段 SRV / sampler、CB / CB1；验证槽位、数组、资源类型、IA bind flags、stride 和 CB1 对齐 / 驱动支持；缺失绑定按阶段 / 槽位追踪 | 缺失资源未被后续 setter / 完整快照 / ClearState 恢复时，选定边界明确失败；setter 编辑仍待迁移 |
| 重放管线状态 | Pipeline > Replay State 通过 Worker 读取实际六阶段绑定、CB1 范围、IA/RS/OM/SO/predicate、视图及状态描述；命令前/后、实验输入克隆、禁用状态、筛选、跳转和 JSON 导出 | 仅覆盖后端已支持的命令和实验；SO 实时写入位置保持未知；SO retained 实验及完整管线编辑仍待迁移 |
| 动态 shader 类链接 | 六阶段 draw/dispatch 快照的 linkage、具名/显式创建实例、有序接口绑定、CB/texture/sampler 偏移；动态/静态 shader 替换；带 SO 的 linked GS；Qt 资源属性、接口槽数和 CLI 元数据导出 | shader setter 实验和专用覆盖分析仍待迁移；本机已复现的空函数表 PS 驱动崩溃改为明确提示使用 WARP |
| Stream output / DrawAuto | SO 声明、五版 context 的目标 setter、普通 GS / VS / DS / 仅输出签名的 passthrough；重复/dirty 快照、显式重置、追加、ClearState；逐流 GPU 查询、按实际写入字节及 IA 范围重建 DrawAuto；Qt SO 属性/目标链接和几何导出 | 输出/setter 实验的 retained 目标和私有诊断重放仍待迁移；无帧内历史时原始重放保留捕获计数并标注未验证，有编辑时拒绝猜测 |
| Predication | occlusion / SO overflow predicate 资源、六组 Begin/End/SetPredication、快照条件绑定、原始 BOOL、实际 GPU 结果；Pipeline > Predicate 前后边界检查与 JSON 导出；辅助预览查询隔离、读回和编辑准备的条件恢复 | active / hint 结果明确不可读；setter 实验及尚未迁移的私有诊断消费者仍待闭合；非标准 BOOL 的驱动差异保留 |
| API 检查 | 捕获字段、偏移/原始位、可选数组、引用跳转、资源/多词筛选、JSON/CSV 导出；getter、annotation、query 与 command-list 调用元数据 | 解码不代表执行；annotation 层级和 view typed-format 预览衔接尚未闭合 |
| Context / command-list | 五种 context 接口身份、immediate/deferred 区分；缺失 context 的严格 Map READ 证据恢复；command-list 资源、owner 和 Execute/Finish 清单；Qt 字段树、证据导航、JSON 导出 | 推断不补造版本/指针/标志；保留指针与 ID 候选冲突；与 Python 相同，仅接受 immediate-context 的 Finish 空操作，未恢复列表执行 |
| 捕获查询值 | 按同 ID、命令顺序使用 GetDesc/GetDataSize/CreateQuery/predicate 元数据；BOOL 完整值与 UINT64 低位、缺失字节、HRESULT/冲突状态 | 表示捕获时保存的内存字，不是新执行的 GPU query；原始高位缺失时保持不完整 |
| 命令编辑 | RTV / DSV / Uint / Float UAV 清除值；资源写入命令启停；UpdateSubresource 紧密排列源数据替换；菜单、右键、撤销/重做；共享 context 校验和恢复 | 特殊 planar 资源执行仍受现有重放限制 |
| 图像 | 实际 GPU 输出、缩放/平移/通道、像素值、PNG 导出 | 全帧输出目前限制单采样 RGBA/BGRA8 |
| 纹理 | GPU 格式转换、BC、浮点/整数、1D/2D/3D、mip/layer/slice、事件边界预览 | MSAA、平面格式及部分查看选项未迁移 |
| Shader | DXBC 反汇编、反射、SPDB/SDBG 内嵌源码、HLSL 编译替换 | 不含 HLSL 恢复、单步调试及全部反射树 |
| Buffer | 初始值、事件前后读回、范围、Hex/ASCII/32 位解释、导出；事件字节编辑/二进制补丁导入、绑定识别、撤销/重做；递归 CB 字段、CB1 范围和类型化编辑；按 UAV view 检查/编辑 Append/Consume/Counter | counter 支持 CS/OM 快照及已恢复 clear/copy/setter 引用；SO retained 实验、完整 setter/getter 与扩展 UAV 重放仍待迁移 |
| 几何 | IA 输入解码、索引与实例、DrawAuto 实际参数及来源、三种顶点表、旋转线框、CSV/OBJ 导出 | 后变换与覆盖未迁移；当前表格上限为 100 万引用 / 1600 万字段 |
| 资源名称 | GenPrivateData 原始名称、非法 UTF-8 转义、列表筛选和属性显示 | 名称记录不表示逐事件重命名时间线 |
| 实验项目 | 原格式 JSON、捕获 SHA-256 绑定、资产校验、uint64 ID、原子保存、撤销/重做 | 接受事件启停、buffer、clear、update_source、事件/初始 UAV counter、全局 shader/texture 替换；其他操作明确拒绝 |
| 性能 | 原生 D3D11 时间戳、disjoint 与 pipeline statistics | 尚未迁移原版多轮调度、Intel 硬件指标/GTPin |

## 验证证据

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

## 尚未闭合的迁移范围

1. 完整 setter/command/context 的重放与实验语义、predicate setter 编辑、SO 的 retained 输出与私有诊断、
   class linkage 的 setter 衔接、扩展 UAV、MSAA 与 planar 等特殊 replay 路径。
2. 其余资源/状态/绑定/命令编辑，完整 counter 命令引用、后变换几何、覆盖率、
   quad 与像素分析。
3. HLSL 恢复、source/instruction 导航、变量/表达式、trace/stack 与 shader 调试。
4. RenderDoc 原生 C++ 后端、Intel Metrics Discovery、GTPin 与完整指标调度。
5. 逐模块原版对照闭合，以及独立机器、更多捕获和完整交互回归。

因此当前可靠性结论限于已验证样本和上述路径，不能推断任意 gpaframe 都能正确
重放，也不能用“两份捕获通过”代表全部 204 个源模块完成迁移。
