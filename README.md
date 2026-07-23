# AnimBP2FP - Animation Blueprint to Functional Programming

**UE5.6 插件：动画蓝图（AnimBlueprint）⇄ S-expression DSL（AnimLang）双向转换**

---

## 功能

- **导出**：将动画蓝图导出为文本 DSL（AnimLang），支持版本管理（Git diff/merge）
- **导入**：从 DSL 文本重建 / 更新动画蓝图
- **往返验证**：导出 → 解析 → 导出，验证 Exporter ↔ Parser 的无损往返
- **EventGraph 导出**：通过 BlueprintLisp 将 EventGraph 导出为 BlueprintLisp DSL
- **Commandlet 无头模式**：支持 `-run=AnimBP2FPExport/Import/RoundTrip` 批量处理
- **RigVM / Control Rig 模块**：将 Control Rig 导出为 `.riglang`，与 `.animlang` 组成可解析、可 lint、依赖有序的共享模块 bundle

---

## 安装

将插件复制到 UE5.6 项目的 `Plugins/` 目录或引擎的 `Engine/Plugins/` 目录：

```
YourProject/
└── Plugins/
    └── AnimBP2FP/
```

然后重新生成项目文件并编译。

---

## 使用方式

### 方式一：编辑器菜单

`Tools → AnimBP2FP →` 下有导出/导入/往返验证等菜单项。

### 方式二：Commandlet（无头模式）

```bash
# 导出所有动画蓝图
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPExport -stdout -nullrhi

# 导出一个 AnimBP 及其引用的 Rig 模块
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPExport \
  -AssetPath=/Game/Path/ABP_Name -IncludeRigModules -stdout -nullrhi

# 严格 lint、瞬态往返验证、持久化导入一个 Anim/Rig workspace
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimLispLint \
  -Workspace="Project/Saved/BP2DSL/Workspace" -stdout -nullrhi
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPRoundTrip \
  -Bundle="Project/Saved/BP2DSL/Workspace" -stdout -nullrhi
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPImport \
  -Bundle="Project/Saved/BP2DSL/Workspace" -OutDir=/Game/Imported -stdout -nullrhi

# 旧版单 .animlang 导入必须显式声明 Legacy；它不提供精确 bundle 保证
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPImport \
  -Legacy -File="Legacy.animlang" -OutDir=/Game/Imported -stdout -nullrhi
```

### 方式三：Python Bridge（编辑器进程内直调）

通过 MCP 连接 UE 编辑器，在编辑器 Python 环境中调用：

```python
import unreal

# 导出
result = unreal.AnimBP2FPPythonBridge.export_anim_blueprint_to_text(
    "/Game/Path/To/Your_AnimBP.Your_AnimBP"
)
print(result.dsl_text)

# 导入/更新
result = unreal.AnimBP2FPPythonBridge.update_anim_blueprint_from_text(
    "/Game/Path/To/Your_AnimBP.Your_AnimBP",
    dsl_text
)

# EventGraph 导出
result = unreal.AnimBP2FPPythonBridge.export_event_graph_to_text(
    "/Game/Path/To/Your_AnimBP.Your_AnimBP"
)
```

---

## AI Skill 库

AnimBP2FP 提供以下 AI Skill，位于 `UE-Editor-MCPServer-Skills` 仓库的 `plugins/` 目录下：

| Skill | 用途 |
|-------|------|
| **animbp2fp-mcp** | 通过 MCP 触发 AnimBP2FP 全套转换（导出/导入/更新/往返验证/EventGraph DSL），适合批量任务和 Commandlet 链路 |
| **alsv-blueprint-rw** | 在 ALSV 编辑器中交互式读写 AnimBlueprint（单资产），适合 AI 打开编辑器时直接读/改蓝图 |
| **blueprint-lisp** | BlueprintLisp 通用转换（EventGraph 导入/导出/更新），适用于任何项目的任意蓝图 |

安装后，AI 代理可通过这些 Skill 自动调用 AnimBP2FP 和 BlueprintLisp 的功能。

---

## DSL 示例

```lisp
(anim-blueprint "ALS_AnimBP"
  :skeleton "/Game/AdvancedLocomotionSystemV/CharacterAssets/MannequinSkeleton"
  :variables [(var :name "Speed" :type Float :default 0.0)]

  :anim-graph
    (output-pose
      (blend-poses-by-bool
        :active-value (ref "Get IsMoving")
        :true-pose (sequence-player :name "Run_F" :loop true)
        :false-pose (sequence-player :name "Idle" :loop true))))
```

## RigLang 共享模块

`.riglang` 使用 canonical S-expression 表示 Control Rig 的 hierarchy、变量、RigVM 图、函数和公开入口。模块头携带稳定资产身份与语义 hash：

```lisp
(rig-module :asset "/Game/Rigs/CR_FootPlacement"
  :class "/Script/ControlRigDeveloper.ControlRigBlueprint"
  :version 1 :content-hash "sha256:...")
(define-rig-entry "ForwardsSolve" :id "..." :event "Forwards Solve")
```

AnimLang 通过带 hash 的 `import-rig` 引入模块，再由 typed Control Rig 节点引用公开入口：

```lisp
(import-rig :asset "/Game/Rigs/CR_FootPlacement"
  :as FootPlacement :expected-hash "sha256:...")
(control-rig :library (rig-ref FootPlacement)
  :entry (rig-entry FootPlacement/ForwardsSolve)
  :inputs ((DebugDraw :cpp-type "bool" (pin-default false))))
```

严格模式会在创建持久化资产前完成模块 hash、依赖 DAG、公开入口和输入类型检查；先编译并语义复核全部 Rig，再编译 Anim，任何失败都会阻止或回滚整组导入。旧格式只有在显式 `-Legacy` 下才能引用既有 Control Rig，并报告 non-exact warning。

Anim 模块只能通过 `rig-entry` 绑定 Rig 的公开执行入口。Anim 中的 `rig-call` 不得调用 Rig 内部函数；内部 `define-rig-function` 只属于 RigVM 模块实现。

---

## 项目结构

```
AnimBP2FP/
├── AnimBP2FP.uplugin
├── Source/
│   ├── AnimBP2FP/               # Runtime 模块（Parser/Exporter/Importer/Differ/Patcher）
│   └── AnimBP2FPEditor/         # Editor 模块（菜单/Commandlet/Python Bridge）
├── Content/
├── Resources/
└── Extras/                      # 非 UE 插件部分（DSL 示例/测试）
    ├── DSL/Examples/
    └── Tests/
```

---

## 输出目录

- AnimGraph DSL：`<ProjectDir>/AnimLang/Exported/`
- EventGraph DSL：`<ProjectDir>/AnimLang/EventGraph/`
- Anim/Rig workspace：`<ProjectDir>/Saved/BP2DSL/AnimBP/`、`<ProjectDir>/Saved/BP2DSL/Rig/`
- Bundle manifest：`<ProjectDir>/Saved/BP2DSL/animlisp-bundle.json`
- Lint report：`<ProjectDir>/Saved/BP2DSL/Diagnostics/animlisp-lint.json`

---

## 验证状态

**Export 往返测试**：ALS_AnimBP / CameraBehavior / Bow_AnimBP / Editor / TutorialAnimBP / TutorialTPP 全部 **100% PASS**。

**Import 往返测试**：保真度 74.9%~100%，剩余 diff 主要集中在 EventGraph 变量连接（ref 连接，需 K2Node_VariableGet 支持）。

**RigVM 共享模块**：GASP Mover AnimBP + Foot Placement Control Rig 已通过 strict export、workspace lint、瞬态 round-trip、持久化 bundle import、失败 rollback/retry 与双冷启动确定性 hash 验证。

---

## 相关项目

- **MaterialBP2FP**：材质蓝图 ⇄ DSL 转换
- **BlueprintLisp**：EventGraph ⇄ BlueprintLisp DSL 转换
- **BlueprintAutoLayout**：图节点自动排版。AnimBP2FP 通过导入生命周期钩子（`PostNodeChanges` 阶段）与之集成——当导入上下文请求 `AutoLayout` 行为时，DSL 导入完成后会自动整理新增/变更节点的布局。集成为可选：BlueprintAutoLayout 未启用时不影响导入，只是不自动排版。

---

**版本**: 0.1.0-alpha  
**UE 版本**: 5.6+
