# FloraGPA C++ 迁移进度 — 2026-09-20

已有可编译、可运行的 VS2022 / Qt Widgets 原生工程，能够独立重放当前验证的
GF2 与 BF1 DX11 捕获。**尚未完成原 Python 版本的全部功能迁移。**

原目录 `D:\CDXrepo\FloraGPA` 保留；新工程位于 `D:\CDXrepo\FloraGPA-Cpp`。
运行程序为 `out\FloraGPA\FloraGPA.exe`，需要保留同目录的 DLL、插件和 Worker。

## 当前可用

| 区域 | 已迁移功能 | 主要边界 |
|---|---|---|
| 工程 | C++20、VS2022 x64、Qt 6.11.2、CMake、Git、独立 CLI/Worker | 尚未配置远程仓库 |
| 捕获与重放 | 有边界检查的 IGPA v3 读取、六阶段状态、基本 D3D11 资源和 draw/dispatch、Map/更新/复制/清除 | 特殊 replay 路径尚未完全迁移 |
| 主界面 | GPA 式深色三栏、真实 GPU 时间柱状图与概览、可停靠面板、API 筛选、任务取消 | 概览尚无范围拖动；未知功能页禁用 |
| 事件 | draw/dispatch 选择、快照管线、前后边界重放、事件启停 | 非 draw 精确 setter 状态仍不完整 |
| 图像 | 实际 GPU 输出、缩放/平移/通道、像素值、PNG 导出 | 全帧输出目前限制单采样 RGBA/BGRA8 |
| 纹理 | GPU 格式转换、BC、浮点/整数、1D/2D/3D、mip/layer/slice、事件边界预览 | MSAA、平面格式及部分查看选项未迁移 |
| Shader | DXBC 反汇编、反射、SPDB/SDBG 内嵌源码、HLSL 编译替换 | 不含 HLSL 恢复、单步调试及全部反射树 |
| Buffer | 初始值、事件前后读回、字节范围、Hex/ASCII/32 位解释、导出 | CB 递归字段、UAV counter 与编辑仍待迁移 |
| 几何 | IA 输入解码、索引与实例、三种顶点表、旋转线框、CSV/OBJ 导出 | DrawAuto、后变换与覆盖未迁移；当前表格上限为 100 万引用 / 1600 万字段 |
| 资源名称 | GenPrivateData 原始名称、非法 UTF-8 转义、列表筛选和属性显示 | 名称记录不表示逐事件重命名时间线 |
| 实验项目 | 原格式 JSON、捕获 SHA-256 绑定、资产校验、uint64 ID、原子保存、撤销/重做 | 目前接受事件启停、全局 shader/texture 替换；其他操作明确拒绝 |
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
- Release 与 Debug 的六组 CTest 均通过。不是“所有原版测试已通过”。
- 发布目录在仅保留 Windows 系统 PATH 的子进程中完成两份黄金重放和负对照。
  模块列表未发现 Python/Tk、GPA 或 RenderDoc；尚未做另一台干净 Windows 验证。
- Qt 自身窗口渲染已检查 1440×900、1920×1080 及 150% / 200% 缩放。
  发布包还通过了本机 Windows 平台插件的原生窗口启动、重放和截图检查；
  这不等于完成所有窗口操作和多显示器验证。

生成证据位于未纳入 Git 的 `artifacts/`：`validation-delivery/`、
`texture-validation.json`、`geometry-validation-1/`、`geometry-validation-instanced/`、
`buffer-validation-2/`、`name-validation.json`、`ctest-release-delivery.log`、
`ctest-debug-publish.log`、`asset-ui-delivery/`、`ui-portable-*.png`、
`ui-windows-delivery.png`。
源码基线逐模块记录在 `migration.json`。

## 尚未闭合的迁移范围

1. 完整 setter/command/context 语义、predication、stream-output/DrawAuto、
   class linkage、扩展 UAV、MSAA 与 planar 等特殊 replay 路径。
2. 全部资源/状态/绑定/命令编辑，CB 字段、DrawAuto 与后变换几何、覆盖率、
   quad 与像素分析。
3. HLSL 恢复、source/instruction 导航、变量/表达式、trace/stack 与 shader 调试。
4. RenderDoc 原生 C++ 后端、Intel Metrics Discovery、GTPin 与完整指标调度。
5. 逐模块原版对照闭合，以及独立机器、更多捕获和完整交互回归。

因此当前可靠性结论限于已验证样本和上述路径，不能推断任意 gpaframe 都能正确
重放，也不能用“两份捕获通过”代表全部 204 个源模块完成迁移。
