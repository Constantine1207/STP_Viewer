# Mac 控制区布局修复

日期：2026-10-01。此次仅调整 Mac；Windows 构建、运行包和布局逻辑保持原样。

## 问题和修复

三维窗口下方的装配爆炸、显示与透明度、剖切控制区被裁切。修复前，在 1440×812 和 1440×831 逻辑像素的实际 Mac 窗口中，滚动区虽然有 96 像素高度，但只有 4 像素位于中央面板的可见区域内；主要控件全部位于其父面板边界之外。

Mac 分支将滚动区和紧凑标签页的纵向大小策略从 Ignored 改为 Minimum。应用样式并完成控件 polish 后，依据实际内容最小高度及横向滚动条空间预留完整控制区。在当前 Mac 上，完整控制区高度为 115 逻辑像素。三维区占用剩余空间；狭窄窗口可以横向滚动访问全部控件。所有产品代码变更均由 Q_OS_MACOS 包围；新增测试目标仅在 APPLE 构建中启用。

Qt 6.8 文档说明，Ignored 会使布局忽略 minimumSizeHint；滚动内容的尺寸约束不能代替外层可见区域的空间分配。参考：[QWidget](https://doc.qt.io/qt-6.8/qwidget.html)、[QScrollArea](https://doc.qt.io/qt-6.8/qscrollarea.html)。

## 实际验证

- 原生 MainWindow 在 1440×812、1440×831、1280×760 下，11 个交互控件均完整位于全部父面板可见区域内。1440×831 在 DPR=2 下对应用户截图的 2880×1662 像素。
- 在 1000×700 窗口逐个滚动到 11 个控件，全部可见；隐藏、恢复 BOM 面板时三个滑条均完整可见。
- 加载彩色装配样例后，以鼠标事件从实际手柄位置拖动爆炸至 78%、点击复位至 0%、拖动透明度至 73%，选择 X 剖切并拖动剖切位置至 77%。BOM 数量在交互前后均为 5 和 2。
- 新 Mac ZIP 在搬移后的路径、清除开发依赖路径的环境中，对 cube、colored-assembly、pmi-basic 和 NIST AP242 四组样例完成打开、截图、BOM 和拆分导出。四个 BOM 工作表和各拆分清单的 XLSX ZIP 结构均核验通过。
- 从最终 ZIP 独立解压，codesign --verify --deep --strict 通过；47 个 Mach-O 及包内依赖核验通过。

## 复现测试

```sh
.tools/venv/bin/cmake --build build --target STPViewer stp_mac_layout_tests --parallel 6
QT_PLUGIN_PATH=/path/to/visible-qt-plugins build/stp_mac_layout_tests output-dir samples/colored-assembly.step
bash scripts/package-macos.sh build/STPViewer.app --output dist/mac-controls-fix
```

测试输出布局 JSON、各窗口截图和鼠标交互后的模型截图。该回归检查实际可见区域，避免仅依靠程序设置滑条值而漏掉裁切。

此次布局验证使用公开随包样例；用户截图中的 carrier.stp 未作为输入提供。小屏紧凑启动分支的高度计算已同步修正，但上述实测是在当前 1512×876 逻辑像素可用桌面上完成。既有大模型、最低系统版本及任意 STEP 的完整 PMI 验收边界继续适用。
