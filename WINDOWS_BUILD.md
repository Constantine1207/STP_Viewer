# Windows 构建与验证

这里是源码构建入口。拿到便携 ZIP 的使用者只需解压并打开 `STPViewer.exe`，无需安装下列开发工具。2026-09-30 已在 16 GB Windows 11 ARM64 虚拟机完成 x64 构建、服务回归 2/2、真实 OpenGL 交互回归及四组便携包搬移自检，另完成三项标准精度性能与导出核对。当前拟发布版本为 1.0 正式版；项目作者已确认 Windows 10 x64 和 Windows 11 x64 中运行实测通过。具体最低版本与独立干净系统范围见验证说明。最新包状态以生成的 JSON、SHA-256 和实际截图/日志为准，详细实测表见 `VALIDATION.md`。

## 在现有电脑或 Windows 虚拟机上构建

1. 将源码 ZIP 解压到本地文件夹。建议路径较短，如 `C:\STPViewer`；空格和中文路径会按完整参数传递，不要求更改系统区域设置。首次构建会下载依赖并编译 OCCT，请为源码、缓存和编译产物预留足够空间，保持联网。
2. 构建者需要 **Visual Studio 2022 或 Build Tools 2022** 的“使用 C++ 的桌面开发”工作负载，包含 **MSVC v143 x64/x86** 与 **Windows 10/11 SDK**。还需 [Python 3.12+ x64](https://www.python.org/downloads/windows/)，推荐 Python 3.12 并安装 `py` 启动器。应用无需预装 Qt、OCCT、CMake、Ninja 或 Excel；脚本在当前项目 `.tools/` 和 `.deps/` 中取得固定版本。
3. 双击根目录的 **`build-windows.cmd`**，或在 Windows PowerShell 5.1 / PowerShell 7 中执行下面命令。保持 Windows 已登录且桌面可交互，虚拟机使用可用的三维显卡驱动；自动图形验证会打开窗口。无需手动打开 VS 开发者终端，脚本用 VS 自带 `vswhere` 查找工具并初始化 x64 环境。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Jobs 4
```

`-ExecutionPolicy Bypass` 仅用于这一次启动的进程；脚本不修改系统永久执行策略。构建入口不自动安装 Visual Studio 或系统 Python；缺少这些前提时会停止，并留下失败报告。普通用户可执行构建，开发工具安装由电脑管理者按原有流程完成。

默认先运行 CAD/BOM/PMI、测量几何与任务队列服务测试，再运行真实 OpenGL、标签会话、外观还原与界面布局回归，然后部署运行库、解压到临时目录并清除 Qt/OCCT 开发环境后检查版本和六组 STEP→截图→Excel→拆分流程。第一次编译较久，窗口显示每阶段及日志位置；可另开 PowerShell 实时看当前日志：

```powershell
Get-Content .\dist\validation\windows\bootstrap\dependencies.stdout.log -Wait -Tail 30
```

修复失败原因后重新执行同一入口，会复用已有下载和编译缓存。已有固定依赖可用 `-SkipBootstrap`；`-SkipGuiTests` 只跳过单独的交互回归，打包后的六组图形自检仍必需且不能默认为通过。

搬移自检同时核对随包 Qt 中文译文已从应用相对路径加载、标准 Cancel 文本为“取消”；结果记录为 `qtChineseTranslationLoaded` / `qtCancelText`，新资源是否可用须由最终打包自检确认。

## 回传结果

成功后 `dist/` 应包含：

| 文件 | 用途 |
|---|---|
| `STPViewer-1.0.0-windows-x64.zip` | 包含 EXE、运行库、Qt 插件、OCCT 资源、样例、使用说明和许可的便携交付包 |
| `STPViewer-1.0.0-windows-x64.zip.sha256` | ZIP SHA-256 |
| `windows-build-report.json` | 环境、原生 CPU/进程架构、MSVC/SDK 版本、依赖版本、阶段结果、最终状态和验收范围 |
| `windows-bundle-validation.json` | 每个部署 EXE/DLL 的 x64 PE 架构、SHA-256、导入 DLL 解析及搬移自检结果 |
| `validation/windows/` | 编译/测试日志、JUnit 服务测试结果、导入/导出自检 JSON、四组模型/爆炸/剖切/界面截图及 BOM/拆分文件 |

将这些文件回传即可继续核对。失败时至少回传 `windows-build-report.json` 和 `validation/windows/`；无需传出整个 `.deps/`、`.tools/` 或电脑环境变量。失败、取消或跳过会分别记录，旧 ZIP 不能代替本次成功报告。发布 EXE 未做 Microsoft Authenticode 签名；本次运行可能需按系统提示确认来源。

本次 VM 回传的证据位于 `dist/validation/windows-vm/`。`windows-performance.json` 记录实际解压 ZIP 和 EXE 校验值、版本、16 GB ARM64 / x64 目标环境、三样例加载与实际旋转帧率、100 ms 轮询读取的工作集及进程高水位；`windows-performance-evidence.zip` 包含完整截图、四表 BOM、拆分文件、manifest、内存时间序列和日志。`windows-validation-evidence.zip` 保留构建、服务、图形、PE/DLL 审计与四组搬移证据。OOXML 核对不等于已在 Microsoft Excel 或 Windows LibreOffice 中实际打开；性能样例不等于 200 MB 工程装配验收。原始报告的绝对路径属于测试时的 Windows 目录，回传后按归档内样例目录定位文件。

## 虚拟机和兼容性边界

Windows 11 ARM 虚拟机可以用于构建和运行 **x64 目标程序的仿真验证**，前提是已装可运行的 x64 Python/MSVC 工具及图形驱动。报告通过系统 CPU 架构和进程架构区分此情况，不将它记作原生 x64 实机或原生 ARM64 应用。虚拟 GPU 的 OpenGL 能力和性能与物理显卡不同；若图形测试失败，应保留截图与日志再判断，不能只依据服务测试把七项功能认定已验收。

本机构建后搬移运行只说明当前 Windows 主机上的运行库闭包可用。最终仍需在断网、普通用户、未安装开发依赖的目标 Windows 10 1809 / Windows 11 x64 电脑上打开 ZIP，并验证中文路径、移动目录、真实工程文件、200 MB 装配规模、Excel 应用兼容性及异常处理。最低系统版本、PMI 保真和性能的剩余边界见 `VALIDATION.md`。

## 常见失败

- **缺少 VS / Windows SDK**：在 Visual Studio Installer 中为 VS2022 添加上述工作负载及组件，再重跑。VS2026 或仅装 C# 工具不符合当前固定编译链。
- **找不到 Python**：Microsoft Store 的 `python.exe` 占位别名不算可用 Python。安装 x64 Python 并启用 `py` 启动器，或让真实 `python.exe` 位于 PATH；脚本检测版本和 64 位指针宽度。
- **下载失败**：查看 bootstrap 的 stdout/stderr。固定版本下载和源归档 SHA-256 会校验，失败不会继续构建。可重新执行以复用完整缓存，不关闭 HTTPS 校验。
- **黑窗、图形启动失败或截图失败**：保持桌面登录，检查 VM 的三维加速/显示驱动和具体日志；不要用服务测试替代图形验证。
- **缺 DLL / 插件 / 许可**：打包会在审计及搬移启动时停止。不要从系统或其他软件目录随意复制 DLL；修复对应的固定依赖或官方 VS release CRT 来源。

开发工具定位依据 [Microsoft vswhere 文档](https://github.com/microsoft/vswhere/wiki/Find-VC)，Windows 参数及编码处理参照 [PowerShell Start-Process](https://learn.microsoft.com/powershell/module/microsoft.powershell.management/start-process) 与 [PowerShell 文件编码说明](https://learn.microsoft.com/powershell/module/microsoft.powershell.core/about/about_character_encoding)。

## VM 开发工具准备入口

`scripts/prepare-windows-tools.ps1` 默认只从官方地址下载并校验安装器，**不安装软件**。它限定 VS2022 的 `vs/17/release` 通道，校验 Microsoft Authenticode 有效签名；Python 使用 3.12 系列最后提供完整 Windows 安装器的 3.12.10 x64，核对 Python.org Sigstore 公布的 SHA-256 与 Python Software Foundation 有效签名。签名或摘要不符会停止，绝不运行该文件。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\prepare-windows-tools.ps1
```

用户同意在构建 VM 中安装开发工具及对应许可后，才执行带安装开关的入口：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\prepare-windows-tools.ps1 -InstallBuildTools -InstallPython
```

VS 安装为系统开发工具，可能需要管理员确认，不自动重启。Python 安装限制为当前用户，目标在项目 `.tools/python312/`，启用用户级 `py` 启动器、保留系统 PATH，构建入口优先使用该项目 Python。安装器、签名、SHA-256、退出码及重启需求记录在 `dist/windows-tools-report.json` 和 `.tools/installers/` 日志中。VS 编译器会按实际目录选择 x64 或 ARM64 主机工具，目标始终为 x64。

原生 ARM64 主机→x64 编译器范围依据 [Microsoft 的 Visual Studio on ARM 文档](https://learn.microsoft.com/visualstudio/install/visual-studio-on-arm-devices?view=vs-2022)。Python 安装器版本及发布摘要来源为 [Python 3.12.10 发布页](https://www.python.org/downloads/release/python-31210/) 与其官方 Sigstore 包；Python 仅作为构建工具，最终便携应用不携带 Python。

## 2026-10-03 功能同步

Windows 使用与 Mac 共用的标签页、BOM 独立页、右侧提示、人工材料/密度、勾选导出范围、右键零件菜单和平移、外观还原以及作者签名。Windows 采用窗口内菜单与 Ctrl+W，完成导出后可打开资源管理器。EXE 内嵌 AEME 多尺寸 ICO，窗口/任务栏使用同源 PNG。平台差异见 `doc/stp_desktop_updates.md`，新一轮证据见发行目录 `WINDOWS_SYNC.md` 与 `validation/windows-sync/`。

若把确定性源码 ZIP 解压覆盖到旧构建目录，ZIP 的固定文件时间可能早于既有对象文件。更新受影响的源码/头文件时间或使用新的构建目录，避免 Ninja/AUTOMOC 复用旧接口。此次 VM 同步核对全部源码 SHA-256 并刷新相关输入后重建。最终用户应将新便携 ZIP 解压到新的文件夹，避免混用旧 DLL。

## 2026-10-05 测量与单文件同步

Mac 的测量、测量记录、圆心与无限参考轴、A/B 独立无限拓展及紧凑空间标注已开放到 Windows，零件抓手也使用共用实现。界面不再显示轴间距离及无限拓展辅助的 M 编号和无穷符号。交互及缓存说明见 `doc/stp_windows_measurement.md`。

构建入口增加测量服务、测量会话集成测试，并对半径/直径参考模型和无限拓展模型进行便携包实测。成功部署 ZIP 后，`scripts/package-windows-single.ps1` 从同一 ZIP 快照生成 `STPViewer-1.0.0-windows-x64-portable.exe` 与 SHA-256。单 EXE 使用 Windows .NET Framework 4.x，运行时释放到当前用户 LocalAppData 缓存，无需管理员或安装。它不是静态链接程序。

单文件打包必须通过仅 EXE 搬移启动、完整校验解压、热缓存启动、缓存修改恢复以及中文/空格/相对模型路径的七项测量自检；证据位于 `validation/windows/single-exe/`。Windows 10 x64／Windows 11 x64 的运行实测由项目作者确认；自动回归的 ARM 虚拟机记录继续单独保留，独立干净机器仍待验证。
