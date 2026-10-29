# 零件位置编辑、STEP 另存与背景颜色

开发基线：`Xiaoyang899/STPViewer` 的 `7e207119f22670d7c5eab35aad4a34c8add00ff7`。Linux 验证日期：2026-10-29；Windows 云端验证日期：2026-10-30（北京时间）。

## 在原界面中使用

| 功能 | 界面入口 | 操作结果 |
|---|---|---|
| 平移、旋转 | 顶部“移动 / 旋转”；或零件右键“移动 / 旋转…” | 打开右侧“属性与标注 → 位置编辑”页 |
| 平移 | 位置编辑页选择“平移”，输入 ΔX、ΔY、ΔZ | 按全局坐标、mm、相对当前位置移动选中的一个实例 |
| 旋转 | 选择“旋转”，设置 X/Y/Z 轴、角度和中心 | 按全局轴右手规则旋转；中心可选当前包围盒中心、全局原点或指定坐标 |
| 预览、确认 | “预览 / 应用 / 取消” | 预览不进入导出；应用后计入导出，输入增量归零 |
| 撤销与恢复 | 页内撤销、重做、恢复选中零件导入位置 | 位置编辑历史在各文件标签中分别保存 |
| STEP 另存 | 顶部“另存 STEP…”或“文件 → 另存 STEP…” | 保存整个装配，包括全部叶级零件及已应用的位置编辑 |
| 背景颜色 | “视图 → 背景颜色” | 切换原有渐变、白色、浅灰、深灰、自定义颜色；所有标签同步，下次启动保留 |

选中装配节点或多个零件时，位置输入不可用；首版支持单个叶级零件实例。面板在小窗口下可滚动。进入位置编辑时，原有爆炸和抓手位移会复位，避免临时显示位置与实际编辑混淆。

应用编辑后，文件标签出现 `*`；关闭文件时可保存、放弃或取消。保存成功后清除未保存标记。保存不会重新加载当前文档，继续编辑仍以本次导入位置为“恢复”基准；若要以新文件为基准，请重新打开另存后的文件。预览期间必须先应用或取消，才能另存。

## 保存范围与边界

- 输出为 AP242 STEP，使用精确 BRep 几何，长度单位 mm；显示网格精度不会改变输出的精确几何。
- 对同一个零件定义的不同实例分别记录变换。移动一个实例不改变其他同源实例。
- 导出采用独立的 XDE 文档，保留零件名称、原有颜色和面颜色、原有材料及通过单位核验的密度。会话中补填的材料/密度不写入此整装导出。
- 输出增加一个 `<源文件名>_edited` 总装根节点。重复子装配在输出中分别建立装配定义，以隔离实例内的位置调整；叶级零件定义仍可共享。因此不能保证原文件的内部实体编号、字节内容或装配定义共享方式完全不变。
- 此版本整装另存**不导出 PMI 标注**；有已识别 PMI 时会提示确认。界面仍可查看原有 PMI。跨零件标注的重映射需后续单独开发。
- 抓手、爆炸、临时隐藏、临时透明度和背景颜色不写入 STEP；隐藏或取消勾选的零件也包含在“整装另存”中。
- 原有 Excel BOM、工程信息表、拆分 STEP 和“原始位置”测量继续使用导入数据。需要核对新位置的 BOM 或测量时，请重新打开另存文件；“当前显示位置”测量可包含编辑位置。
- 默认另存为 `_modified.stp`；禁止覆盖当前导入的源文件。文件采用原子写入，导出失败或取消不替换已存在的目标文件。
- 未保存的位置编辑及撤销历史属于当前会话；背景颜色属于全局持久设置。

## 代码接入点

| 文件 | 职责 |
|---|---|
| `src/stp/part_placement.*` | 实例变换、刚体变换校验、精确包围盒中心 |
| `src/stp/part_transform_panel.*` | 数值面板、旋转中心、预览、撤销和重做 |
| `src/stp/assembly_export.*` | 重建调整后的装配并原子写出 STEP |
| `src/stp/viewer_widget.*` | 显示编辑变换、网格替换时保留变换、背景设置 |
| `src/stp/main_window.*` | 工具栏、右键、属性页、保存与未保存提示 |
| `src/stp/document_workspace.*` | 多文件隔离、当前标签菜单转发、未保存星号及安全关闭 |
| `cmake/stp_app.cmake` | 新源码和测试目标注册 |
| `scripts/build-windows.ps1` | 新增核心、面板与 GUI 流程测试及截图收集 |

## 编译

依赖版本保持 Qt 6.8.3、OCCT 7.9.3、libxlsxwriter 1.2.4。Windows 使用 MSVC 2022 x64、Windows SDK 和 Python 3.12+，无需更换原编译链。

将本分支源码解压到新目录（例如 `C:\STPViewer-editing`），在已登录、有 OpenGL 图形桌面的 Windows 主机运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Jobs 4
```

也可双击根目录 `build-windows.cmd`。脚本准备固定依赖、编译、执行测试并打包。成功后检查 `dist/windows-build-report.json`、`dist/validation/windows/`，再使用 `dist/STPViewer-1.0.0-windows-x64.zip` 或对应单文件 EXE。源码包不含二进制程序；已构建的 Windows 包可从下方 GitHub Actions 运行下载。

已有 `.deps/paths.cmake` 和依赖时，可单独编译并验证新增功能：

```sh
cmake -C .deps/paths.cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTP_BUILD_TESTS=ON
cmake --build build --target STPViewer stp_placement_tests stp_transform_panel_tests stp_editing_integration_tests --parallel 4
ctest --test-dir build -R 'stp_(placement|transform_panel)_tests' --output-on-failure
build/stp_editing_integration_tests samples/colored-assembly.step editing-evidence
```

Windows 下最后一行使用 `build\stp_editing_integration_tests.exe`。GUI 回归必须具备可用显示与 OpenGL；`-SkipGuiTests` 不代表界面功能已验收。

## 本次验证结果

环境：Ubuntu 24.04 x86_64、GCC 13.3、CMake 3.31.10、Qt 6.8.3、OCCT 7.9.3；图形测试通过 Xvfb 和 Mesa llvmpipe OpenGL 4.5 运行。

| 检查 | 结果 |
|---|---|
| 完整 `STPViewer` 编译与链接 | 通过，生成 Linux ELF 程序 |
| CTest：核心、测量、PMI、任务队列、新增位置变换、新增面板 | 6/6 通过 |
| 新增原生 GUI 集成回归 | 通过：选择、预览、应用、网格重建、多标签背景、文档隔离、关闭取消、菜单保存、重新导入逐实例核对 |
| 原有 Viewer、Appearance、Layout 图形回归 | 通过 |
| 原有 Workspace 图形回归 | 未全部通过：1000×700 两个模型截图的前景采样阈值断言失败；基线在同一环境出现相同失败，其他 `passed` 检查通过 |
| 本分支 Windows x64 云端编译及打包 | 已通过，范围及下载见下节 |
| 本分支 Windows 实机 / macOS 验收 | 尚未执行；不可用上游发行版历史实测代替 |

STEP 核心测试覆盖：嵌套/重复子装配、多个实例独立编辑、平移与非原点旋转组合、空编辑、源文档不变、名称/零件色/面色/材料/密度、体积/面积/位置、中文路径、取消保持已有文件、禁止覆盖源文件、带固有位置的自由零件。真实样例的 GUI 导出测试还逐个匹配重名实例，防止只按名称核对造成误判。

基线对照使用相同上游提交，仅将已存在的 `m_measurementPanel` 指针声明移出 Windows/macOS 条件分支以允许 Linux 编译；本分支也包含此声明可见性修正。没有为了通过测试修改原有截图断言。软件渲染测试不等于 Windows 实机 GPU、200 MB 大装配或 CATIA 的验收；建议在目标 Windows 主机用实际工程 STEP 完成最后一轮导入、编辑、另存和 CAD 回读。

## Windows 云端构建结果

[成功运行 #2](https://github.com/Constantine1207/STP_Viewer/actions/runs/40949370930) 对应源码提交 `4a867000d99cb54a925238ba712c816e6b900bdc`。环境为 Windows Server 2022 x64、MSVC 14.44、Qt 6.8.3、OCCT 7.9.3、Mesa 26.0.3 软件 OpenGL。首轮失败原因是托管机器的 GDI Generic / OpenGL 1.1，第二轮安装软件驱动后完成全部已启用检查。

| 检查 | 结果 |
|---|---|
| 主程序编译与链接、PowerShell 脚本及参数检查 | 通过 |
| 核心、测量、PMI、任务队列、位置变换、变换面板 | CTest 6/6 通过 |
| 便携 ZIP 搬移后模型导入、截图、Excel 和 STEP 拆分 | 6 个样例全部通过，清除 Qt/OCCT 开发环境后执行 |
| 单 EXE 首次解压校验、热缓存启动、缓存损坏恢复 | 通过 |
| 单 EXE 中文、空格、相对模型路径与 7 项测量记录 | 通过；与上行合计 4/4 项单文件检查 |
| Windows 完整独立 GUI 交互回归 | 本次使用 `-SkipGuiTests`，未执行；不能据此宣称已通过 |

[下载构建产物](https://github.com/Constantine1207/STP_Viewer/actions/runs/40949370930/artifacts/12931630750)，解压外层文件后运行 `STPViewer-1.0.0-windows-x64-portable.exe`。文件大小 48,389,120 字节；SHA-256 为 `66803c1d7e18995f01c0d0918526eadb158a78ff11223ffba68643d3543ca2f9`。同一产物还包含普通便携 ZIP、校验文件、`windows-build-report.json` 及截图和日志。云端 Mesa 驱动没有添加到交付包。

下一轮实机验收：在目标 Windows 电脑打开工程装配，选择同一零件的一个实例并平移、旋转，另存 `.stp` 后在查看器及目标 CAD 中重新打开，核对各实例位置、零件数量、名称与颜色；再检查撤销/重做和背景颜色重启后的保持。原有 Linux Workspace 两项截图采样断言仍保留为未解决项，后续应依据模型实际投影区域修正截图判据，并同时复测上游基线和本分支，不能简单降低阈值。PMI 重新导出不属于本版本已实现范围。
