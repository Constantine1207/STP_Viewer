# Mac 拆分 STEP 导出可见性修复

2026-10-03。仅更新 Mac；Windows 应用和便携包保持原样。

## 故障与原因

用户报告 本地工程装配样例 拆分到 选定的导出父目录 后目录看起来没有内容。检查已有批次发现 STEP 和 `manifest.xlsx` 实际存在，`STP_parts_*` 目录带 `UF_HIDDEN`，文件标志为 `0x8040`。Apple 将 [`UF_HIDDEN`](https://developer.apple.com/documentation/system/fileflags/hidden) 定义为在图形界面中隐藏文件的标志，所以文件存在检查通过不等于 Finder 可见。

此前导出在目标父目录创建点号开头的暂存目录，写完全部文件和清单后改名发布。当前 FileProvider 环境给暂存树赋予隐藏属性；去掉点号、改名为正常结果名称未清除此属性。读取文件触发物化后，还观察到隐藏属性再次应用。已有恢复记录中，首次清理后有 37 个条目在物化期间被重新赋予隐藏位，读完校验并再次清理后全部可见。

## 修复行为

Mac 暂存目录改为可见的 `STP_exporting-*` 前缀，从创建时避免进入隐藏树。仍在全部 STEP 和清单写完后发布为新的 `STP_parts_日期时间_批次`，每批独立，取消或写入失败自动清理未发布的暂存目录。

发布后，对本次新建的每个 STEP、`manifest.xlsx` 和结果目录清除 `UF_HIDDEN`；FinderInfo 存在隐藏位 `kIsInvisible` 时只清除该位。其余 BSD 标志、Finder 标签和元数据字节保持原样。可见性检查或修改失败会报告“文件已写入，但无法恢复 Finder 可见性”及具体路径，不冒充全部完成。处理不遍历或修改目标目录中其他文件。

完成对话框显示实际结果目录，并提供 **在 Finder 中打开**。状态栏和导入诊断保留完整路径及 STEP 文件数量，便于对照选择的父目录与实际批次目录。左侧勾选范围、按定义去重、原始几何、自身坐标、文件属性和 PMI 诊断规则保持不变；人工材料/密度仍只用于当前会话和 Excel，不写入拆分 STEP。

## 已核验的证据

- 核心服务新增 Mac 专项：注入 `UF_HIDDEN` 和 FinderInfo 隐藏位，发布后确认全部输出可见，其他标志和元数据字节保留；同时覆盖可见暂存前缀及取消清理。核心、PMI、串行作业队列共 3/3 通过，耗时 1.50 秒，记录为 `dist/validation/mac-split-export/ctest.log`。
- 选定的导出父目录 既有结果恢复覆盖 92 个条目，包括 7 个目录和 85 个文件。85 个文件内容 SHA-256 未改变；读取物化后的最终可见性核验全部通过，记录为 `dist/validation/mac-split-export/existing-results-recovery.json`。
- 修复版开发应用原生界面实测：打开用户指定的 本地工程装配样例，加载 54 个零件定义、65 个实例；通过原生目录选择器选择 选定的导出父目录，完成框显示导出 54 种，结果为 新建的拆分批次目录。点击 **在 Finder 中打开** 后，Finder 确实打开对应目录，原生界面列出 54 个 STEP 和 `manifest.xlsx`，状态显示 55 个项目。

- Mac 原生界面回归通过，覆盖窗口布局、人工属性编辑、勾选范围、Excel 与 STEP 实际导出内容，记录为 `dist/validation/mac-split-export/layout/layout-report.json`。
- 最终 Mac ZIP 独立解压后，严格深度签名核验通过，四组移位自检通过，审计 47 个 Mach-O。直接运行包内应用，再通过原生目录选择器将同一本地工程装配样例导出到 新建的拆分批次目录；完成框及 Finder 均确认 54 个 STEP 与清单共 55 项。清单包含 54 行零件、65 行实例，全部文件封装完整且没有隐藏属性，记录为 `dist/validation/mac-split-export/portable-ui-launch.json`。

开发版和最终便携包的实际文件导出与 Finder 查看均已分别核验。当前交付哈希与源码清单见 `dist/DELIVERY_MANIFEST.json`，详细结果见 `dist/MAC_SPLIT_EXPORT_FIX.md`。验证范围仍限当前 Apple Silicon Mac，不新增 macOS 12、另一台干净 Mac 或真实 200 MB 大装配的验收结论。
