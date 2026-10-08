# Third-party notices / 第三方软件声明

STP Viewer 1.0.0 基于 Mayo v0.10.0 定制。它使用 Open CASCADE Technology 的几何、STEP/XDE 和可视化功能。应用源码随源码包提供；第三方源码的固定版本和取得方式位于 `scripts/dependencies.lock.json`、`scripts/bootstrap_deps.py`。下列上游库未作源代码修改，编译选项由构建脚本记录。

便携包内的完整许可文本与 Qt 官方 SPDX 版权/组件清单位于 macOS `.app/Contents/Resources/licenses` 或 Windows `licenses`。SPDX 文件是上游 Qt 安装包的完整清单，可能包含本应用未部署的 Qt 工具/模块；实际运行库清单见随包验证文件。Mayo 的完整 BSD 许可同时保存在 `LICENSE.txt`。

| Component | Version / source | License used |
|---|---|---|
| Mayo | [v0.10.0](https://github.com/fougue/mayo/tree/v0.10.0) | BSD-2-Clause, Copyright 2016 Fougue SAS |
| Qt Core, Gui, Widgets, OpenGL, OpenGLWidgets, Svg and runtime plugins | [6.8.3](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/) | LGPL-3.0; additional bundled component licenses in Qt-SBOM |
| Qt 中文标准界面译文 | [qttranslations v6.8.3](https://github.com/qt/qttranslations/tree/v6.8.3) | LGPL-3.0-only option under the upstream module license rule |
| Open CASCADE Technology | [V7_9_3](https://github.com/Open-Cascade-SAS/OCCT/tree/V7_9_3) | LGPL-2.1 with OCCT exception 1.0 |
| FreeType | [2.13.3](https://github.com/freetype/freetype/tree/VER-2-13-3) | FreeType License (FTL) |
| libxlsxwriter | [1.2.4](https://github.com/jmcnamara/libxlsxwriter/tree/v1.2.4) | BSD-2-Clause; includes additional FreeBSD/minizip notices in its License.txt |
| zlib | [1.3.1](https://zlib.net/fossils/zlib-1.3.1.tar.gz), Windows; macOS uses the operating system zlib | zlib license |
| fmt, Microsoft GSL, KDBindings, magic_enum, miniply, fast_float | Mayo-vendored versions identified by `src/3rdparty/commit_*.txt` | MIT (fmt has an additional object-code exception; fast_float MIT option used) |

This software is based in part on the work of the FreeType Team. Portions of this software are copyright © 1996–2024 The FreeType Project (https://freetype.org/). All rights reserved.

Qt 和 OCCT 以共享库/框架形式分发。允许为调试用户修改的库而进行反向工程；应用不附加禁止这些库修改、替换或重新链接的条款。可使用随附应用源码、固定版本的依赖源码以及构建脚本重新生成应用。macOS 包采用本地 ad-hoc 签名；替换库后须对修改后的包重新签名。这里的来源链接与构建配置帮助定位对应版本，不能代替再分发者根据其分发方式履行完整许可义务。

2026-10-03 Mac 的应用源码增加 `src/stp/mac_accessibility.mm`，参考 Qt 官方 [QTBUG-134784 引用计数修复](https://github.com/qt/qtbase/commit/b1ed5f656f064e553b33752f8e87d2f5b9553e38)，为固定 Qt 6.8.3 的 Cocoa 表格无障碍缓存补足对象持有关系。兼容处理随应用源码提供；Qt 上游共享库和平台插件仍为未修改的 6.8.3 二进制，无障碍功能保持启用。Qt 的来源与许可仍按上表及随包 Qt 许可文本声明。Windows 不编译或部署这一兼容处理。

Qt 中文译文使用官方 v6.8.3 的原始 TS，并由同版本 lrelease 编译为 QM。原始 TS、QM、上游许可规则、来源摘要及完整许可文本随源码放在 `resources/translations`，随便携包放在 macOS `Contents/Resources/translations` 或 Windows `translations`。翻译文件采用上游默认模块许可中的 LGPL-3.0-only 选项；附带的 BSD 文本仅对应上游构建文件的许可规则，不能作为翻译文件的许可声明。

Windows 便携包还复制 Visual Studio 官方可再分发目录中的 Microsoft Visual C++ release runtime。它是平台运行时，而非本项目开发依赖；分发受 Visual Studio 对应的 Microsoft 许可约束。不得把任意开发工具或 Debug runtime 当作可再分发文件。macOS 的系统框架不复制进包。

## Mayo-vendored MIT components

- fmt: Copyright (c) 2012–present, Victor Zverovich.
- Microsoft GSL: Copyright (c) 2015 Microsoft Corporation. All rights reserved.
- KDBindings: Copyright 2021–2022 Klarälvdalens Datakonsult AB, a KDAB Group company.
- magic_enum: Copyright (c) 2019–2024 Daniil Goncharov.
- miniply: Copyright (c) 2019 Vilya Harvey.
- fast_float: Copyright (c) 2021 The fast_float authors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
