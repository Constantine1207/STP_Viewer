# Mac BOM 页面与右侧提示

日期：2026-10-02。仅调整 Mac；Windows 产品逻辑、布局及运行包保持原样。

默认页面显示三维视口及爆炸、显示、剖切控制。原底部 BOM 停靠模块已移除，点击顶部“BOM 与导入诊断”才打开独立页面。页面包含“零件汇总”“诊断与日志”，并提供“返回三维”。页签和数据继续使用原控件，切页不重新读取文件、不重建三维视口、不触发额外选中。查看新文件时返回三维页；诊断消息不自动跳转页面。

“左键旋转、右键拖拽平移、滚轮缩放、双击适配”放在右侧属性与标注栏的“三维操作”区域，位于工程信息/PMI 标签页之外。小屏 Mac 启动时保留右侧栏，装配树可由视图菜单展开。用户选择隐藏侧栏后，进入 BOM 页再返回会恢复此前的显隐状态。

## 验证

- 默认三维页不显示 BOM 或诊断，没有底部 bomDock；操作提示属于右侧 detailsDock。
- 1440×812、1440×831、1280×760 窗口中 11 个控制区交互控件及右侧提示完整可见；1000×700 中全部控件可滚动访问，已检查右侧四条提示截图。
- 鼠标点击顶部入口、切换诊断标签及点击返回有效；手动隐藏右侧栏后，切页往返保留隐藏状态。
- 单实例选择在三次往返后仍为单实例；在 BOM 中主动选择汇总行会联动选择该定义的全部实例。
- 相机 Eye/Center/Direction/Scale、原 QOpenGLWidget 身份、透明度和剖切控件状态保持；返回后图像正常，已目视检查渲染截图。
- 爆炸、透明度及剖切滑条交互仍通过，BOM 数量保持 5、2。

测试使用原生 MainWindow 和公开彩色装配样例。构造时的小屏紧凑分支依据当前屏幕选择；上述窄窗口实测使用窗口缩放，不等于换一台小屏 Mac 的启动验收。既有系统版本、真实大模型和 PMI 验收边界继续适用。

## 构建和回归

```sh
.tools/venv/bin/cmake --build build --target STPViewer stp_mac_layout_tests --parallel 6
QT_PLUGIN_PATH=/path/to/visible-qt-plugins build/stp_mac_layout_tests output-dir samples/colored-assembly.step
bash scripts/package-macos.sh build/STPViewer.app --output dist/mac-bom-page
```

最终包自检另记录 bomHiddenOnModelPage、bomPageScreenshot 和 diagnosticsPageScreenshot。四组搬移样例应全部为 true，且 Excel 和拆分输出正确。页面容器依照 [Qt 6.8 QStackedWidget](https://doc.qt.io/qt-6.8/qstackedwidget.html) 保留页面对象。
