# ECABridge 插件研究报告

> **分析时间**: 2026-03-27
> **来源**: Unreal Engine 引擎 Git 历史 (commit eae6f611ff10 ~ e5ff0c95242f)
> **作者**: Jon Olick (Epic Games)
> **注意**: 插件名是 **ECABridge** (Epic Code Assistant Bridge)，不是 EACBridge

---

## 一、概述

ECABridge 是 Epic Games 官方的**实验性 MCP 插件**，目的是让 AI 助手（Claude Code 等）通过 MCP 协议操控 Unreal 编辑器。其中**最核心的功能之一**是 **BlueprintLisp** —— 一个 LISP 风格的 S-expression DSL，用于蓝图图（Blueprint Graph）的双向序列化。

### 定位差异

| 维度 | ECABridge/BlueprintLisp | AnimBP2FP/AnimLang |
|------|------------------------|-------------------|
| **目标蓝图类型** | 通用 Blueprint (EventGraph, 函数图) | 动画蓝图 (AnimGraph) |
| **DSL 风格** | LISP S-expression | 类 LISP S-expression |
| **运行方式** | MCP Server (HTTP)，AI 实时调用 | Commandlet 离线批处理 |
| **设计目标** | AI 辅助蓝图编辑 | 蓝图文本化 + 增量 Diff/Patch |
| **代码量** | ~55 个源文件, 229+ 命令 | ~31 个源文件 |
| **Export 粒度** | 语义级（event/branch/let/call） | 节点级（define/node/ref） |

---

## 二、架构总览

```
┌─────────────────────────────────────────────────┐
│  外部 AI (Claude Code / MCP Client)             │
│       ↕  MCP Protocol (HTTP JSON-RPC)           │
├─────────────────────────────────────────────────┤
│  FECAMCPServer (HTTP Server, port config)       │
│       ↕  JSON Command Routing                   │
├─────────────────────────────────────────────────┤
│  UECABridge (命令路由器)                          │
│       ↕  FECACommandRegistry                    │
├─────────┬─────────┬─────────┬───────────────────┤
│Blueprint│Blueprint│Material │ Actor/Component/  │
│  Lisp   │  Node   │  Node   │ DataTable/UMG/... │
│Commands │Commands │Commands │ 20+ 命令类别       │
├─────────┴─────────┴─────────┴───────────────────┤
│  BlueprintLisp (DSL Engine)                     │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐ │
│  │FLispParser│  │Blueprint │  │LispToBlueprint│ │
│  │(Tokenizer │  │ToLisp    │  │(DSL→BP Nodes) │ │
│  │ + Parser) │  │(BP→DSL)  │  │               │ │
│  └──────────┘  └──────────┘  └───────────────┘ │
├─────────────────────────────────────────────────┤
│  FECABridgeToolset (UE ToolsetRegistry 适配)     │
│  → 暴露给 AIAssistant / EDA 等其他消费者          │
└─────────────────────────────────────────────────┘
```

### 模块组成

插件只有**一个 Editor 模块** `ECABridge`，包含：
- **MCP Server**: HTTP JSON-RPC 服务端
- **Command Registry**: 229+ 命令的注册/路由中心
- **BlueprintLisp**: DSL 引擎（本报告核心）
- **Commands/**: 20+ 命令类别（Blueprint, Actor, Material, Niagara, UMG, Metasound...）
- **ToolsetRegistry 适配层**: 让引擎其他系统（AIAssistant 等）也能调用

---

## 三、BlueprintLisp DSL 语法设计

### 3.1 AST 节点类型

```cpp
enum class ELispNodeType : uint8 {
    Nil,      // 空/null
    Symbol,   // 标识符: foo, BeginPlay, GetHealth
    Keyword,  // 关键字: :true, :false, :pin-name
    Number,   // 数值: 42, 3.14
    String,   // 字符串: "hello"
    List,     // 列表: (a b c)
};
```

只有 **6 种 AST 节点类型**，极其简洁。对比 AnimLang 的 13 种 Token 类型，BlueprintLisp 用更少的类型覆盖更多场景。

### 3.2 核心语法 (Core Forms)

| 语法形式 | 语义 | 蓝图节点映射 |
|---------|------|------------|
| `(event Name ...)` | 事件节点 | K2Node_Event |
| `(func Name ...)` | 函数定义 | K2Node_FunctionEntry + K2Node_FunctionResult |
| `(let var expr)` | 局部变量 | K2Node_VariableGet (缓存结果) |
| `(set var expr)` | 设置变量 | K2Node_VariableSet |
| `(seq ...)` | 顺序执行 | K2Node_ExecutionSequence |
| `(branch cond :true :false)` | 条件分支 | K2Node_IfThenElse |
| `(foreach item coll ...)` | 循环 | ForEachLoop macro |
| `(call target func args...)` | 调用函数 | K2Node_CallFunction |
| `(delay seconds)` | 延迟 | Delay node |
| `(cast Type var body)` | 类型转换 | K2Node_DynamicCast |
| `(vec x y z)` | 向量字面值 | MakeVector |
| `(rot p y r)` | 旋转字面值 | MakeRotator |
| `(switch var :case1 ... :default ...)` | 分支选择 | K2Node_Switch* |
| `(on-component Comp Event ...)` | 组件事件 | K2Node_ComponentBoundEvent |
| `(input-action Name ...)` | 输入动作 | K2Node_InputAction |
| `(input-key Key ...)` | 按键输入 | K2Node_InputKey |
| `(asset "/Game/Path")` | 资产引用 | SoftObjectReference |
| `(print expr)` | 打印 | PrintString |
| `(valid? expr)` | 有效性检查 | IsValid |
| `(+ - * / > < >= <= == !=)` | 数学/比较 | Math nodes |

### 3.3 完整示例

```lisp
;; 事件驱动的游戏逻辑
(event BeginPlay
  (let player (GetPlayerCharacter 0))
  (branch (IsValid player)
    :true (seq
      (let health (call player GetHealth))
      (PrintString (format "Health: {}" health)))
    :false
      (PrintString "No player!")))

;; 组件碰撞处理
(on-component BoxComp BeginOverlap
  :params ((OtherActor Actor))
  (cast Character OtherActor
    (seq
      (call _cast_result AddHealth 25)
      (PlaySound2D (asset "/Game/Sounds/S_Pickup"))
      (destroy))))

;; 自定义函数
(func ResetStats
  (set Health 100)
  (set Armor 50)
  (set Speed 600))
```

---

## 四、蓝图 → DSL 导出管线 (Blueprint → Lisp)

### 4.1 整体流程

```
UBlueprint
  → EdGraph (EventGraph / FunctionGraph)
    → 遍历所有 UEdGraphNode
      → 构建 NodeMap (NodeId → JsonObject)
        → 找到所有入口节点 (Event/FunctionEntry)
          → ExecChainToLisp() 递归跟踪执行链
            → PureNodeToLisp() 处理纯节点 (数据流)
              → FLispNode AST
                → ToString() 序列化为 S-expression 文本
```

### 4.2 关键实现细节

**入口识别**: 找到图中所有 `K2Node_Event`、`K2Node_CustomEvent`、`K2Node_FunctionEntry` 作为导出起点。

**执行链跟踪** (`ExecChainToLisp`):
- 从入口节点开始，沿 exec pin (Then/Execute) 跟踪执行流
- 每个执行节点递归处理其数据输入
- 分支节点 (Branch, Switch) 生成嵌套的 `:true`/`:false` 关键字参数
- 顺序节点 (Sequence) 生成 `(seq ...)` 列表

**纯节点处理** (`PureNodeToLisp`):
- 无 exec pin 的节点（数学运算、变量获取等）
- 递归解析数据输入，生成嵌套的函数调用表达式
- 例如 `(+ Health (* Armor 0.5))` 对应 Add(Health, Multiply(Armor, 0.5))

**语义提升**: 不是简单地 1:1 映射节点，而是做了语义压缩：
- `K2Node_VariableGet` → 直接用变量名符号
- `K2Node_CallFunction` → `(call target func args...)`
- `K2Node_MacroInstance("ForEachLoop")` → `(foreach ...)`
- 多个连续 exec 节点 → `(seq ...)`

### 4.3 选项配置

```cpp
struct FBlueprintToLispOptions {
    bool bIncludeComments = false;      // 包含节点注释
    bool bIncludePositions = false;     // 包含节点坐标
    bool bCompactOutput = true;         // 紧凑输出
};
```

---

## 五、DSL → 蓝图 导入管线 (Lisp → Blueprint)

### 5.1 整体流程

```
S-expression 文本
  → FLispParser::Parse() 词法+语法分析
    → TArray<FLispNodePtr> AST
      → ProcessForm() 递归处理每个顶级 form
        → 识别 form 类型 (event/func/let/set/branch/...)
          → 创建 UEdGraphNode (K2Node_*)
            → 设置节点属性 + 创建 Pin 连接
              → FBlueprintEditorUtils::MarkBlueprintAsModified()
                → 自动布局 (BlueprintAutoLayout)
                  → 编译蓝图
```

### 5.2 关键实现

**词法分析** (FLispParser::Tokenize):
- 括号匹配、字符串转义、数值解析
- `:keyword` 作为关键字参数
- `;` 行注释
- 行号/列号追踪用于错误报告

**语法分析** (FLispParser::Parse):
- 递归下降，构建 FLispNode 树
- 6 种节点类型足够表示所有结构

**Form 处理** (ProcessForm):
核心分发逻辑，根据第一个 symbol 选择处理器：

```
event/custom-event  → 创建 K2Node_Event / K2Node_CustomEvent
func                → 创建 K2Node_FunctionEntry + K2Node_FunctionResult
let/set             → 创建 K2Node_VariableGet/Set
seq                 → 创建 K2Node_ExecutionSequence
branch              → 创建 K2Node_IfThenElse
foreach             → 创建 ForEachLoop macro
call                → 创建 K2Node_CallFunction
cast                → 创建 K2Node_DynamicCast
delay               → 创建 Delay 节点
input-action        → 创建 K2Node_InputAction
on-component        → 创建 K2Node_ComponentBoundEvent
switch              → 创建 K2Node_Switch*
vec/rot             → 创建 MakeVector/MakeRotator
print               → PrintString
(其他)              → 尝试作为函数调用
```

**表达式解析** (ResolveExpression):
- 将 DSL 表达式转换为节点 + pin 引用
- 变量名 → VariableGet 节点
- 字面值 → 对应常量节点
- 函数调用 → CallFunction 节点的返回 pin
- 数学表达式 → 对应 Math 节点

**自动布局** (BlueprintAutoLayout):
- 创建节点后自动排列，避免重叠
- 这是 JSON node command 方式不具备的优势

### 5.3 与 JSON Node Command 方式的对比

ECABridge 提供**两套**蓝图操作接口：

| 维度 | BlueprintLisp (DSL) | JSON Node Commands |
|------|--------------------|--------------------|
| **接口** | `lisp_to_blueprint` | `add_node`, `connect_pins`, `set_pin_value` |
| **粒度** | 语义级 (event/branch/call) | 操作级 (逐节点创建+逐 pin 连接) |
| **AI 友好度** | ★★★★★ 极高 | ★★☆ 中低 |
| **代码量** | 3-5 行 DSL = 完整逻辑 | 10-30 个 JSON 命令 |
| **自动布局** | ✅ 内置 | ❌ 需手动指定坐标 |
| **错误率** | 低 (语义验证) | 高 (pin 名拼写、类型不匹配) |
| **灵活性** | 覆盖常用模式 | 可操作任意节点 |
| **官方推荐** | "PREFERRED" | fallback |

官方在头文件注释中明确标注：
> **PREFERRED**: Use BlueprintLisp format for all Blueprint implementation tasks.

---

## 六、与 AnimBP2FP 的对比分析

### 6.1 共同点

| 方面 | 说明 |
|------|------|
| **DSL 风格** | 都采用 S-expression / LISP 风格 |
| **双向转换** | 都支持 BP → DSL 和 DSL → BP |
| **AST 中间层** | 都有显式 AST 结构 |
| **递归下降解析** | Parser 都用递归下降 |
| **关键字参数** | `:keyword value` 模式 |

### 6.2 关键差异

| 维度 | ECABridge/BlueprintLisp | AnimBP2FP/AnimLang |
|------|------------------------|-------------------|
| **蓝图类型** | 通用 BP (K2Node_*) | 动画 BP (AnimGraphNode_*, StateMachine) |
| **节点 ID** | 无稳定 ID，靠语义去重 | `$id` 或三阶段匹配 |
| **Export 策略** | **语义提升** (branch→branch, var→symbol) | **节点级忠实** (1:1 映射) |
| **Import 策略** | 创建 K2Node 实例 → 连接 Pin | 创建 AnimGraphNode → 连接 Pin |
| **Diff/Patch** | ❌ 无 (整体替换) | ✅ 16 种 DiffOp, 增量更新 |
| **Round-trip 保真** | 不保证 (语义等价即可) | 追求 100% 文本级保真 |
| **输出用途** | AI 实时编辑 | 版本控制/文本 Diff |
| **状态机** | 不涉及 | 核心功能 |
| **Transition** | 不涉及 | 条件图导出 |

### 6.3 设计哲学差异

**ECABridge**: "让 AI 能读懂并生成蓝图逻辑" → **语义压缩优先**
- 不需要 round-trip 保真，只要语义等价
- Export 做大量语义提升（VariableGet → 变量名符号）
- Import 从语义重建节点，自动处理 Pin 连接

**AnimBP2FP**: "让蓝图可以做文本化版本控制" → **保真度优先**
- 追求 Export→Parse→ToString 100% 一致
- Import→Export round-trip 需要高保真
- 需要增量 Diff/Patch 支持

---

## 七、对 AnimBP2FP 的启发

### 7.1 值得借鉴的设计

1. **`(call target func args...)` 模式**: ECABridge 的函数调用语法很清晰，AnimLang 的 ref 连接可以参考这种显式调用风格

2. **Keyword 参数 `:key value`**: 两者都用了这个模式，验证了这是 UE 蓝图 DSL 的好选择

3. **自动布局**: ECABridge 在 Import 后自动排列节点，AnimBP2FP 可以考虑类似功能

4. **MCP 集成**: 未来 AnimBP2FP 如果要做编辑器集成，可以参考 ECABridge 的 MCP Server + Command Registry 架构

5. **语义级 DSL 作为补充层**: 对于 AI 辅助编辑场景，在 AnimLang 节点级 DSL 之上增加一层语义级 DSL 是有价值的

### 7.2 AnimBP2FP 的优势

1. **Diff/Patch**: ECABridge 完全没有增量更新能力，只能全量替换图。AnimBP2FP 的 16 种 DiffOp + 三阶段匹配是核心竞争力

2. **保真度**: ECABridge 是 lossy 的（语义等价但不保证结构一致），AnimBP2FP 追求 lossless round-trip

3. **动画蓝图专业性**: 状态机、Transition、AnimGraph 节点类型的深度支持是 ECABridge 完全没有的

4. **离线批处理**: Commandlet 模式适合 CI/CD 流水线和批量处理

---

## 八、ECABridge 命令分类一览

| 类别 | 命令文件 | 主要功能 |
|------|---------|---------|
| **BlueprintLisp** | ECABlueprintLispCommands | parse, blueprint_to_lisp, lisp_to_blueprint, help |
| **Blueprint** | ECABlueprintCommands | create, add_component, compile, add_variable, get_info |
| **Blueprint Node** | ECABlueprintNodeCommands | add_node, connect_pins, set_pin_value (底层操作) |
| **Actor** | ECAActorCommands | spawn, transform, get_info |
| **Asset** | ECAAssetCommands | create, load, save, delete, references |
| **Component** | ECAComponentCommands | add, remove, get, set properties |
| **DataTable** | ECADataTableCommands | CRUD rows, import/export |
| **Editor** | ECAEditorCommands | console commands, save, get state |
| **Material Node** | ECAMaterialNodeCommands | material graph node operations |
| **Mesh** | ECAMeshCommands | geometry operations |
| **Metasound** | ECAMetasoundCommands | audio graph operations |
| **Niagara** | ECANiagaraCommands | particle system operations |
| **UMG** | ECAUMGCommands | widget operations |
| **Widget Tree** | ECAWidgetTreeCommands | UI tree operations |
| **View** | ECAViewCommands | viewport operations |
| **Event** | ECAEventCommands | event queue |
| **Project** | ECAProjectCommands | project settings |
| **MVVM** | ECAMVVMCommands | viewmodel debug |

---

## 九、Commit 历史

| Commit | 日期 | 描述 |
|--------|------|------|
| `d484da17ce54` | 初始 | ECABridge experimental MCP plugin |
| `93b5a8f0e4f6` | - | [Backout] |
| `eae6f611ff10` | 2026-03-09 | 重新提交：完整 MCP 插件 + BlueprintLisp + 55 个源文件 |
| `17696bcd8b41` | - | Fix static analysis (divide by zeros) |
| `e13598f3a72e` | - | Fixes for unity builds |
| `18eada1d265f` | - | Fixed docs for (rot), exposed batch blueprint node gen |
| `463f56872ec3` | - | More fixes for unity builds |
| `9cae6de916a9` | - | Fixed PVS warnings |
| `1938c4ec6560` | - | Expand add_blueprint_function_node class search scope |
| `8d895585f7d1` | - | Add save_asset, save_all_dirty, save_and_compile_blueprint |
| `cf07beb4fa57` | - | Use engine WriteRowAsJSON for DataTable |
| `170f707243fd` | - | Add get_viewmodel_values, list_live_viewmodels |
| `1a514ee6c05c` | - | Fix static analysis warnings in MVVM |
| `2fb8f67f9342` | - | Wire up run_console_command target_world |
| `c0506f68c548` | - | Add diff_widget_blueprint MCP tool |
| `88c4002bf8f9` | - | Add get_references and get_referencers |
| `68ccd0df0b8a` | - | Clear dirty flag on temp package in diff_widget_blueprint |
| `e5ff0c95242f` | 2026-03-18 | EDA/ECA Collaborative Interop (ToolsetRegistry 适配) |

---

## 十、总结

**ECABridge** 是 Epic 官方的 AI 辅助蓝图编辑框架，其 BlueprintLisp 采用 **语义级 LISP DSL** 实现蓝图↔文本互转。核心设计特点：

1. **极简 AST** (6 种节点类型) + **语义提升** (将底层蓝图节点映射为高级语义 form)
2. **AI-first 设计**: DSL 被设计为 AI 易读易写，而非人工精确编辑
3. **MCP 集成**: 通过 HTTP JSON-RPC 暴露 229+ 命令
4. **无 Diff/Patch**: 整体替换策略，不支持增量更新
5. **通用 BP 专注**: 不处理动画蓝图、状态机等专业图类型

对于 AnimBP2FP 而言，两者互补而非竞争：ECABridge 处理通用蓝图的 AI 编辑，AnimBP2FP 处理动画蓝图的文本化和增量更新。
