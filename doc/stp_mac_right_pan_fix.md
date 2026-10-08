# Mac 右键拖拽平移修复

日期：2026-10-02。仅修改 Mac 产品行为；Windows 的事件逻辑、提示文本和现有运行包保持不变。

Mac 三维视口现以左键拖动旋转、右键拖拽平移；底部提示和“帮助 → 操作说明”同步更新。原平移把每次鼠标移动的增量传给 Pan(..., false)，而 OCCT 的 false 模式会恢复之前保存的相机起点，造成连续移动不累计及旧方向恢复。Mac 现使用增量位移和 Pan(..., true)，每帧基于当前相机平移。右键产生的 ContextMenu 事件在三维视口消费，鼠标释放或移动事件不再含按住按钮时停止拖动。中键平移仍保留；Mac 的 Shift + 左键不再作为平移方式。

产品代码和新增回归均在 Q_OS_MACOS 条件分支内。Windows 保留此前逻辑，此次未构建或测试 Windows，也未操作虚拟机。

## 验证

新增 Mac OpenGL 回归在修改前失败，提示“Mac right pan rotated camera”，证实原逻辑在旋转后恢复旧相机。修复后验证：

- 正交和透视模式，六段右键移动与同起终点单段移动结果一致，反向拖动恢复原相机中心。
- 相机 Eye 和 Center 同量移动；Direction、Up、Scale 和眼点至中心距离不变，位移位于视图平面。
- 单击右键不改变相机及选择数量；释放后移动不继续平移，漏掉释放事件但收到无按钮移动时同样停止。
- 视口消费右键菜单事件；左键拖动继续旋转，按住 Shift 时也保持旋转行为。
- 既有原生三维回归继续通过，包括源变换与几何不变、颜色/透明度、爆炸、剖切、显示精度和截图。
- 实际 MainWindow 布局回归仍通过，默认窗口显示完整控制区；较窄窗口、BOM 开关和控制滑条交互正常。

测试通过 Qt 鼠标事件在原生 Mac OpenGL 窗口执行，并检查真实 OCCT 相机状态，不以提示文本或“中心发生变化”作为充分证据。最终便携包另运行四组搬移自检并独立核验签名。

## 复现

```sh
.tools/venv/bin/cmake --build build --target STPViewer stp_viewer_tests stp_mac_layout_tests --parallel 6
QT_PLUGIN_PATH=/path/to/visible-qt-plugins build/stp_viewer_tests samples/colored-assembly.step
QT_PLUGIN_PATH=/path/to/visible-qt-plugins build/stp_mac_layout_tests output-dir samples/colored-assembly.step
bash scripts/package-macos.sh build/STPViewer.app --output dist/mac-right-pan-fix
```

平移参数语义依据固定依赖 OCCT 7.9.3 的 V3d_View.hxx 和 V3d_View.cxx；Qt 事件规则参考 [QMouseEvent](https://doc.qt.io/qt-6.8/qmouseevent.html)。此次范围限于鼠标导航，既有目标系统、实际大模型和 PMI 验收边界保持不变。
