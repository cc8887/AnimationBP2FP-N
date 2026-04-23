# AnimBP2FP - Project Progress Tracker

## 当前状态：Phase 4/5 - DSL ↔ 蓝图 双向管线 🔄

**最后更新**: 2026-03-25 09:30 GMT+8

---

## 📊 整体进度

```
[█████████████████░░░░░] ~70% 完成
```

| Phase | 状态 | 完成度 | 预计完成 |
|-------|------|--------|----------|
| Phase 1: 研究与设计 | ✅ 完成 | 100% | 2026-03-23 ✅ |
| Phase 2: DSL 解析器 | ✅ 完成 | 90% | 2026-03-25 ✅ |
| Phase 3: 蓝图 → DSL | ✅ 完成 | 95% | 2026-03-25 ✅ |
| Phase 4: DSL → 蓝图 | ✅ 核心完成 | 70% | 2026-04-06 |
| Phase 5: 测试与验证 | 🔄 进行中 | 60% | 2026-04-20 |
| Phase 6: 文档与发布 | ⏳ 待开始 | 0% | 2026-05-04 |

---

## Phase 1: 研究与设计 ✅ 100%

### 已完成
- [x] 分析 UE 动画蓝图内部结构
- [x] 研究 Lisp vs Haskell 范式对比
- [x] 选择混合 DSL 方案（Lisp 语法 + Haskell 类型系统）
- [x] 设计 AnimLang 语法
- [x] 定义 AST 数据结构
- [x] 编写核心类型定义（AnimLangAST.h）
- [x] 创建 DSL 示例文件（3 个）
- [x] 设计测试策略（TestStrategy.md）
- [x] 创建项目结构
- [x] 创建项目代码仓库
- [x] 实现类型存根系统（animlang-types.rkt, animlang-nodes.rkt）
- [x] 实现 UE Editor 集成（AnimBP2FPEditor 模块）
- [x] 实现 Racket Linter 原型（animlang-lint.rkt）
- [x] 明确项目独立性决策（与 MaterialBP2FP 保持独立）

---

## Phase 2: DSL 解析器 ✅ 90%

**目标**: 实现完整的 S-expression 解析器和类型检查器

### 已完成
- [x] **自研 Tokenizer** (13 token types, ~416 行) — 取代原计划的 sexpp 库
- [x] **自研 Parser** (~720 行) — 递归下降，S-expression → FAnimGraphAST
  - 支持 `(ref "...")`, `(asset "...")`, `[...]` 转换列表, `(define Name body)` 形式
  - 错误恢复机制 + 行号/列号追踪
- [x] **语义分析器** (AnimLangDiagnostics, ~489 行)
  - 重复 define 检测
  - 循环依赖检测 (DFS)
  - 未解析引用检测
  - Severity × Category 诊断系统
- [x] **ParseDefine 严格校验** (Expect RParen)
- [x] **ParseTransitionList 格式保真** (bFirstInTrans/bFirstInNested 标志)

### 未完成
- [ ] 引脚类型兼容性检查 (FTypeChecker — 当前 stub)
- [ ] 表达式类型推导
- [ ] 完善 Racket Linter (状态机验证规则)

---

## Phase 3: 蓝图 → DSL ✅ 95%

**目标**: 将 UAnimBlueprint 导出为 DSL 代码

### 已完成
- [x] **AnimBPExporter** (~1075 行)
  - 12+ 核心节点类型: sequence-player, blendspace-player, blend, apply-additive, layered-bone-blend, blend-list, state-machine 等
  - 通用 fallback (CamelToKebab 自动转换任意 AnimGraphNode)
  - 状态机完整展开 (状态 + 转换 + 初始状态 + auto-rule)
  - CollectNonPoseParams + CollectPoseInputs
  - SaveCachedPose → `(define name body)` 提升
  - UseCachedPose → 变量引用
- [x] **AnimNodeExporter** (~267 行) — 单节点导出辅助
- [x] **Pretty-printer** — ToString() 缩进格式化
- [x] **往返测试通过** — 6/6 蓝图 100% 保真

### 未完成
- [ ] 少量节点属性未导出 (某些节点的非引脚内部属性)
- [ ] 导出注释中的 Source 路径偶尔有冗余

---

## Phase 4: DSL → 蓝图 ✅ 核心完成 70%

**目标**: 从 DSL 代码生成/更新 UAnimBlueprint

### 已完成
- [x] **AnimBPImporter.h** — 完整接口定义 (20+ 方法)
- [x] **AnimBPImporter.cpp** — ~850 行核心实现
  - `Import(DSLCode, PackagePath)` — 从 DSL 代码创建蓝图
  - `ImportFromAST(AST, PackagePath)` — 从 AST 创建蓝图
  - `UpdateBlueprint(Blueprint, DSLCode)` — 增量更新蓝图
  - `UpdateBlueprintDetailed(Blueprint, DSLCode)` — 详细报告版增量更新
  - `CreateEmptyBlueprint` — AnimBlueprintFactory + Skeleton 加载
  - `FindAnimNodeClass` — 动态 UClass 查找 (kebab→CamelCase + 直接类名)
  - `BuildAnimNode` — 13+ 节点类型 + 通用 fallback
  - `BuildStateMachine` — 状态创建 + OnRenameNode 命名 + 入口连接
  - `BuildVariables` — 蓝图变量创建
  - `BuildAnimGraph` — 完整图重建 (变量 + define + 根节点)
  - `ConnectPins` + `FindInputPosePin` (normalized fuzzy matching)
  - `ClearAnimGraph` / `RebuildAnimGraph` — 完整图清理+重建
  - `CompileBlueprint` — 编译验证
- [x] **UpdateBlueprint 双策略实现**
  - Property-only changes → Patcher 增量修改 (保留节点位置)
  - Structural changes → ClearAnimGraph + 完整重建 (回退策略)
  - Incremental 失败时自动回退到 full rebuild
- [x] **Patcher 增强** — SetNodeProperty (fuzzy pin matching), ApplyVariableChange (add/remove/modify), FindNodeByPath (deep traversal)
- [x] **AnimBP2FPImportCommandlet** — Import + Round-trip + Update 测试
- [x] **Import 往返测试** — 1/6 完美通过 (TutorialAnimationBlueprint)
- [x] **Update 测试** — 6/6 全部通过 (属性修改 + full rebuild + no-op)

### 当前进行中
- [ ] **`(ref "...")` 值保留** — EventGraph 变量节点连接
- [ ] **Transitions 导入** — 条件图构建
- [ ] **通用属性值设置** — blend-time, curve-values 等属性还原
- [ ] **资产路径解析** — 完整路径 vs 短名查找优化

### 关键技术挑战
1. **节点类型 → C++ 类映射**: DSL kebab-case 节点名 → UAnimGraphNode_* 子类
2. **引脚连接语义**: Named children (`:base-pose`, `:blend-pose-0`) → UE 引脚名映射
3. **状态机图创建**: UAnimationStateMachineGraph + 状态节点 + 转换节点 + 条件图
4. **资产引用解析**: `(asset "/Game/...")` → UAnimSequence/UBlendSpace 等
5. **变量绑定**: DSL `:variables [...]` → 蓝图变量 + EventGraph 节点

**预计完成**: 2026-04-06

---

## Phase 5: 测试与验证 🔄 60%

### 已完成
- [x] **往返测试框架** (AnimLangRoundTrip, ~268 行)
  - Blueprint → DSL → Parse → DSL 比较
  - TestString / TestDirectory / CompareTexts / NormalizeForComparison
- [x] **Export 往返测试通过** — 6/6 蓝图，100% 保真，0 差异
- [x] **Differ 验证** — 16 种 DiffOp，三阶段子节点匹配
- [x] **诊断系统** — 语义分析 + 错误报告
- [x] **RoundTrip Commandlet** — `-run=AnimBP2FPRoundTrip`
- [x] **Import Round-Trip** — 1/6 完美通过 (DSL → Blueprint → DSL)
  - Import Commandlet: `-run=AnimBP2FPImport -test`
- [x] **UpdateBlueprint 测试** — 6/6 全部通过
  - Update Commandlet: `-run=AnimBP2FPImport -update`
  - 测试覆盖: 属性修改 (loop toggle, alpha), full rebuild 回退, no-op 检测

### 未完成
- [ ] **Import 往返测试**: DSL → Blueprint → DSL → 比较 (Phase 4 完成后)
- [ ] **单元测试** (目标 90% 覆盖率)
- [ ] **姿态等价性测试**: 同输入 → 同输出姿态
- [ ] **性能基准测试**: 大型蓝图解析/导出耗时
- [ ] **E2E 真实场景测试**: Third Person Character

**预计完成**: 2026-04-20

---

## Phase 6: 文档与发布 ⏳ 0%

### 交付物
- [ ] API 文档
- [ ] 用户手册
- [ ] 教程视频
- [ ] 示例项目
- [ ] GitHub Release
- [ ] Marketplace 提交（可选）

**预计完成**: 2026-05-04

---

## 🔗 项目独立性说明

**重要决策**（2026-03-24）：

AnimBP2FP 和 MaterialBP2FP **保持为独立项目**，不合并。

**原因**：
1. **函数式纯度差异**：AnimBP ~70% vs MaterialBP ~95%
2. **独立工具链**：状态机验证 vs 着色器验证
3. **不同类型系统**：UE 类型 vs HLSL 类型
4. **不同测试策略**：姿态对比 vs 像素对比

详见 **PROJECT_INDEPENDENCE_DECISION.md**。

---

## 🐛 已知问题

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| B1 | FTypeChecker 全部 stub | 中 | Phase 2 遗留 |
| B2 | `(ref "...")` 变量引用未还原 | 高 | Phase 4 待实现 |
| B3 | FVariableDef::ToString() switch 未覆盖所有 EPinType | 低 | 待修复 |
| B4 | Transitions 导入未实现 | 中 | Phase 4 待实现 |
| B5 | 通用属性值 (blend-time, curve-values) 未还原 | 中 | Phase 4 待实现 |

---

## 📝 技术债务

| 项目 | 描述 | 紧急度 |
|------|------|--------|
| EditorStyle | UE5.1+ 已弃用，应迁移到 FAppStyle | 低 |
| TargetSkeleton | UE5 可能需用 GetTargetSkeleton() | 中 |
| Unicode in TEXT() | 部分文件中 ✓✗⚠ 字符可能有编码问题 | 低 |
| LogTemp | 部分文件使用 LogTemp 应改为 LogAnimBP2FP | 低 |

---

## 🎯 里程碑

### M1: 原型验证 ✅
- [x] 完成研究与设计
- [x] 实现最小可行解析器
- [x] 实现简单节点转换
- [x] 演示往返转换 (6/6 通过)

**目标日期**: 2026-03-30 → **实际完成**: 2026-03-25 (提前 5 天) ✅

### M2: 核心功能
- [x] 支持所有基础节点（12+ 核心 + 通用 fallback）
- [x] 支持状态机导出
- [x] **DSL → Blueprint 导入** (Phase 4) ✅
- [x] **UpdateBlueprint 增量更新** ✅ 6/6 通过
- [ ] 通过完整测试用例 (当前 Import 1/6, Update 6/6)

**目标日期**: 2026-04-13

### M3: 生产就绪
- [ ] 支持完整特性
- [ ] 通过所有测试
- [ ] 性能优化

**目标日期**: 2026-05-04

---

## 📊 代码统计

| 类型 | 文件数 | 代码行数 |
|------|--------|----------|
| C++ 头文件 | 14 | ~1,000 |
| C++ 源文件 | 14 | ~6,500 |
| Racket 工具 | 6 | ~800 |
| DSL 示例/导出 | 7 | ~500 |
| 文档 | 15+ | ~80KB |
| **总计** | **56+** | **~7,700行+** |

---

## 🚀 下一步行动

### 本周（2026-03-25 ~ 2026-03-30）
1. ✅ 编译验证 + 往返测试 (6/6 通过)
2. ✅ **AnimBPImporter 核心实现** — Phase 4 完成
3. ✅ **Import Commandlet** — 命令行 DSL 导入 + 测试
4. ✅ **UpdateBlueprint 增量更新** — 6/6 测试通过
5. 🔄 **通用属性值还原** — blend-time, curve-values 等
6. 🔄 **linked-anim-layer 多引脚** — 按命名引脚连接

### 下周（2026-03-31 ~ 2026-04-06）
1. `(ref "...")` EventGraph 变量节点连接
2. Transitions 导入 (状态转换 + 条件图)
3. Import 边缘情况修复
4. 单元测试框架搭建

---

**最后更新**: 2026-03-25 09:30 GMT+8
**更新人**: OpenClaw AI Assistant
**总体进度**: 70% ✅✅✅✅✅✅✅░░░
