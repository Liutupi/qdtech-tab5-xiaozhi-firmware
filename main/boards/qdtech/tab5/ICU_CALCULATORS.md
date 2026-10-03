# Tab5 ICU 数值工具

应用页的“ICU 数值工具”提供五类计算。点左栏选项目，点输入行打开数字键盘；键盘顶部显示当前字段与单位，避免表单被遮挡时填错。点“计算”查看结果。Nabo 可通过 `self.icu.open` 打开页面，并通过独立的 `self.icu.egfr`、`self.icu.uacr`、`self.icu.oxygen`、`self.icu.blood_gas`、`self.icu.pump` 工具计算及展示结果。语音输入须说清数值和单位。页面数据只在内存里，返回应用时清空，不写入 NVS 或 SD 卡。

手动计算 eGFR 时须点击性别栏明确选择；更改任何输入、性别或药物后，旧结果立即失效。语音计算结果单独显示，输入值以右侧结果文字为准；点击“手动输入”后才进入本地表单。错误结果用警示色显示。接近 KDIGO 分层界限时，数值自动增加显示精度，以免四舍五入后的数字与类别相矛盾。血气三项与 Henderson–Hasselbalch 关系明显不符时暂停酸碱和代偿解读，提示核对标本及单位。

## 输入约定

| 项目 | 输入 | 输出 / 方法 |
| --- | --- | --- |
| eGFR | 成人年龄、性别、血肌酐 μmol/L | 2021 CKD-EPI 肌酐公式，mL/min/1.73m²、KDIGO G 分层 |
| uACR | 同一次尿样的白蛋白 mg/L、肌酐 mmol/L | mg/mmol 和 mg/g、KDIGO A 分层；mg/g = (mg/mmol)/0.11312 |
| 氧合 | FiO₂ **百分数**、PaO₂ mmHg、可选平均气道压 cmH₂O | P/F = PaO₂/(FiO₂/100)；正式 OI = FiO₂(%)×平均气道压/PaO₂ |
| 血气 | pH、PaCO₂ mmHg、HCO₃⁻ mmol/L；可选 Na⁺、Cl⁻ mmol/L、白蛋白 g/L | 酸碱方向、AG、低白蛋白校正 AG、Winter 预计 PaCO₂ 与简要代偿提示 |
| 静脉泵 | 药物总量、**配好后最终总液量** mL、泵速 mL/h；按需体重 kg | 浓度、实际输注量与药物惯用单位 |

静脉泵默认显示 50 mL，含义为抽吸、加入药物并配制完成后的**最终总液量**。如果实际操作是“50 mL 盐水再加药液”，应填最终读到的总液量，不能直接沿用 50。药物原液剂量按照包装标注的活性成分/单位输入，不自动假设支数或规格。该工具不预置推荐剂量、给药方案、稀释液或配伍结论；胺碘酮等药物的稀释液须以所用制剂的说明书和科室规范为准。

## 药物与输出单位

| 药物 | 输入药量 | 输出 |
| --- | --- | --- |
| 去甲肾上腺素、肾上腺素、阿拉明（间羟胺）、多巴胺、多巴酚丁胺 | mg | μg/kg/min |
| 胺碘酮 | mg | mg/min |
| 奥美拉唑、呋塞米 | mg | mg/h |
| 血管加压素 | U | U/min |
| 生长抑素、奥曲肽 | mg | μg/h |
| 胰岛素 | U | U/h |
| 右美托咪定 | mg | μg/kg/h |

计算链：浓度 = 总药量 ÷ 最终总液量；每小时输注量 = 浓度 × mL/h；再按 1000 μg/mg、60 min/h、kg 换算。体重仅在带 `/kg` 的项目必填。

## 临床边界与依据

- eGFR 针对 18 岁以上且血肌酐相对稳定者；急性肾损伤、快速变化的血肌酐或危重症情况下可能不准确。单次 G/A 分层不等于 CKD 诊断。公式与单位见 [NIDDK 成人 eGFR 公式](https://www.niddk.nih.gov/research-funding/research-programs/kidney-clinical-research-epidemiology/laboratory/glomerular-filtration-rate-equations/adults)；分层见 [KDIGO 2024 CKD 指南](https://kdigo.org/wp-content/uploads/2024/03/KDIGO-2024-CKD-Guideline.pdf)。
- P/F 和正式 OI 是两个量；单凭 P/F 不诊断 ARDS。OI 定义见 [PALICC 共识](https://pmc.ncbi.nlm.nih.gov/articles/PMC5253180/)。
- AG = Na⁺ − (Cl⁻ + HCO₃⁻)；白蛋白校正 = AG + 2.5×(4 − 白蛋白 g/dL)；Winter = 1.5×HCO₃⁻+8±2。代偿公式及混合性紊乱说明见 [MSD Manual 专业版](https://www.msdmanuals.com/professional/nephrology/acid-base-regulation-and-disorders/acid-base-disorders)。
- [胺碘酮注射液说明书](https://dailymed.nlm.nih.gov/dailymed/fda/fdaDrugXsl.cfm?setid=d8d04647-8e25-4127-8ecf-360ce1991c2f)列示 D5W 中的配伍/稳定性资料，因此不能从“科室常用盐水 50 mL”推断其可用盐水稀释。

结果为算术复核，不替代药师配伍核验、双人核对、医嘱或完整临床判断。进行床旁应用前需由科室用实际制剂与典型病例核对显示单位、有效数字和操作流程。
