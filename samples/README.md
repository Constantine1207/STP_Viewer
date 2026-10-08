# 样例与来源

- `colored-assembly.step`：本项目生成的 AP242 回归样例。2 种同名定义、7 个叶级实例、含总装/子装配共 10 个 occurrence。含嵌套旋转、红色定义、绿色面、蓝色定义、青色实例覆盖以及 Aluminium 材料。生成器：`stp_core_tests --generate-sample 文件.step`。
- `repeated-2000.step`：本项目生成的重复实例规模样例，1 个盒体定义、2,000 叶级实例、共 2,001 occurrence。仅用于重复实例性能，不代表 200 MB 复杂工程装配。生成器：`stp_core_tests --generate-benchmark 2000 文件.step`。
- `pmi-basic.step`：本项目生成的 AP242 PMI 回归样例，含源图形和语义尺寸，用于读取/拆分/重新读取核对。生成器：`stp_pmi_tests --generate-sample 文件.step`。
- `cube.step`：打包时从 Mayo 上游 `tests/inputs/cube.step` 复制，几何启动自检。
- `nist_ctc_01_asme1_ap242-e1.stp`：来自 **NIST MBE PMI Validation and Conformance Testing Project**，CTC-01 AP242 样例。原文件未修改，396,445 bytes；SHA-256 `85a5752da05f53c456ca3a9e038c90358e1d5a3141d1f0d6e5f0970f2356e821`。

NIST [模型说明与使用声明](https://www.nist.gov/ctl/smart-connected-systems-division/smart-connected-manufacturing-systems-group/mbe-pmi-0)允许不受限制使用这些模型/STEP 文件，并希望注明来源。本项目使用该样例不构成 NIST 对本应用的推荐或背书。[原始 STEP 下载入口](https://www.nist.gov/document/nist-pmi-step-files)。本包没有使用 NIST 标志进行宣传。

NIST 示例用于测试 PMI 实现，不应当作具体产品的 GD&T 最佳实践。当前仅核对本样例中读取到的条目和渲染情况，未完成全部 NIST 样例及全部 CAD 输出组合的符合性认证。

## 测量演示

- `measurement-reference.step`：本项目合成的圆柱测量模型，半径 10 mm、高 30 mm，用于半径、直径、圆心、轴线与中心回归。
- `measurement-extension.step`：本项目合成的圆柱、远处盒体与球体，用于有界／无限线、无限平面距离和不支持拓展的几何提示。
