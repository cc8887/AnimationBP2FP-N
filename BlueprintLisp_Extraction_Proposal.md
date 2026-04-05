# ECABridge BlueprintLisp DSL 解析组件拆分方案

## 1. 背景

ECABridge 插件 (`Engine/Plugins/Experimental/ECABridge/`) 内含一个完整的 LISP S-expression DSL 解析引擎（BlueprintLisp），但目前它与 UE 编辑器层（MCP Server、Blueprint 操作、Material、Niagara 等 30+ 命令类别）深度耦合在同一个 `ECABridge` 模块中（260KB 的 `ECABlueprintLispCommands.cpp`）。

**目标**：将 DSL 解析部分（AST + Parser + ToString + 工具函数）拆为独立的、可复用的 UE 模块，使其能被 AnimBP2FP、MatBP2FP 等其他插件直接依赖。

---

## 2. 现状分析

### 2.1 核心文件（DSL 解析相关）

| 文件 | 大小 | 内容 |
|------|------|------|
| `Public/BlueprintLisp.h` | 8.8 KB | AST 定义 + Parser 声明 + Converter 声明 + 工具函数 |
| `Private/BlueprintLisp.cpp` | 21.1 KB | AST 实现 + Parser 实现 + Converter 骨架 + 工具函数实现 |

**总计：~30 KB，约 937 行**。这是需要拆出的核心。

### 2.2 现有架构

```
BlueprintLisp.h
├── ELispNodeType (enum: Nil/Symbol/Keyword/Number/String/List)
├── FLispNode (AST 节点 + 工厂 + 查询 + ToString)
├── FLispParser (递归下降解析器，无独立 Lexer)
├── FLispParseResult (解析结果)
├── FBlueprintLispConverter (BP↔Lisp 双向转换 — 骨架/TODO)
└── BlueprintLisp:: namespace (PrettyPrint/Minify/ExtractSymbols/IsValidSymbol)
```

### 2.3 依赖关系

**DSL 解析本身的依赖极简**：
- `CoreMinimal.h` — FString, TArray, TSharedPtr, TSet, TMap
- `Dom/JsonObject.h` — 仅 `FBlueprintLispConverter` 使用（LispToBlueprint 的 JSON 中间格式）

**ECABridge 模块的重依赖**（不需要）：
- UnrealEd, BlueprintGraph, Kismet, Slate, UMG, Niagara, Metasound, MeshDescription 等 30+ 模块

### 2.4 与 AnimBP2FP / MatBP2FP 的 DSL 对比

| 维度 | ECABridge BlueprintLisp | AnimBP2FP AnimLang | MatBP2FP MatLang |
|------|----------------------|--------------------|-------------------|
| AST 节点类型 | 6 种 (Nil/Symbol/Keyword/Number/String/List) | ~15 种 (Program/Node/Property/...) | ~10 种 (Material/Expr/Output/...) |
| 解析方式 | 递归下降，无独立 Lexer | 两阶段：Tokenizer(13 types) → Parser | 两阶段：Tokenizer(13 types) → Parser |
| 目标领域 | 通用蓝图图 | 动画蓝图 | 材质蓝图 |
| 共享可能性 | 通用 S-expression 基础设施 | 可复用 Parse/ToString | 可复用 Parse/ToString |

---

## 3. 拆分方案

### 3.1 方案概述

创建独立 UE 插件 `BlueprintLisp`，仅包含 DSL 解析核心，零编辑器依赖。

```
Engine/Plugins/Experimental/BlueprintLisp/     (或放在项目级 Plugins/)
├── BlueprintLisp.uplugin
└── Source/BlueprintLisp/
    ├── BlueprintLisp.Build.cs        (~10 行，仅依赖 Core + Json)
    ├── Public/
    │   ├── BlueprintLisp.h           (原样，去掉 ECABRIDGE_API → BLUEPRINTLISP_API)
    │   └── BlueprintLispModule.h     (模块声明)
    └── Private/
        ├── BlueprintLisp.cpp          (拆分后：仅 AST + Parser + 工具函数)
        ├── BlueprintLispConverter.cpp (可选：FBlueprintLispConverter 实现)
        └── BlueprintLispModule.cpp
```

### 3.2 具体拆分策略

#### 阶段 1：提取纯解析模块（最小可用集）

从 `BlueprintLisp.cpp` (937 行) 中提取以下部分：

**保留到新模块** (~600 行)：
- `FLispNode` 全部实现（工厂方法、类型检查、`IsForm`、`GetFormName`、`GetKeywordArg`、`HasKeyword`）
- `FLispNode::ToString()` 完整实现（含 PrettyPrint 逻辑）
- `FLispParseResult` 实现
- `FLispParser` 全部实现（递归下降解析器）
- `BlueprintLisp` namespace 工具函数（`PrettyPrint`、`Minify`、`ExtractSymbols`、`IsValidSymbol`）

**留在 ECABridge** (~340 行)：
- `FBlueprintLispResult` — 蓝图转换结果
- `FBlueprintLispConverter` — BP↔Lisp 双向转换（依赖 UE 编辑器 API）
- `FBlueprintLispConverter::ProcessForm/ResolveExpression/CreateNodeJson` — 蓝图节点操作

#### 阶段 2：ECABridge 适配

ECABridge 改为依赖新模块：
```csharp
// ECABridge.Build.cs
PublicDependencyModuleNames.Add("BlueprintLisp");  // 新增
```

ECABridge 中的代码改动极小：
- `#include "BlueprintLisp.h"` → `#include "BlueprintLisp/BlueprintLisp.h"` (或调整 PublicIncludePaths)
- 所有 `FLispNode`、`FLispParser` 等类型通过模块依赖自动可见
- `ECABlueprintLispCommands.cpp` (260KB) 中的 Phase 3 diff 代码直接使用新模块的 AST

### 3.3 Build.cs 依赖

```csharp
// BlueprintLisp.Build.cs
public class BlueprintLisp : ModuleRules
{
    public BlueprintLisp(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp23;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",        // FString, TArray, TSharedPtr
            "CoreUObject", // UObject 基础（FLispNode 可能需要）
            "Json"         // FJsonObject（如保留 Converter）
        });
    }
}
```

### 3.4 .uplugin 配置

```json
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "1.0",
    "FriendlyName": "BlueprintLisp",
    "Description": "Standalone LISP S-expression DSL parser and AST for Unreal Engine",
    "Category": "Programming",
    "CreatedBy": "Extracted from ECABridge",
    "CanContainContent": false,
    "IsBetaVersion": true,
    "Installed": false,
    "Modules": [
        {
            "Name": "BlueprintLisp",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        }
    ]
}
```

---

## 4. 高级扩展（可选）

### 4.1 增强 Parser 为独立 Lexer + Parser

当前 BlueprintLisp 的 Parser 是单阶段递归下降（直接操作字符流），没有独立的词法分析层。拆分后可以考虑增加：

```
FLispToken (enum: LParen/RParen/String/Number/Symbol/Keyword/EOF)
FLispLexer (输入字符串 → FLispToken[])
FLispParser (FLispToken[] → FLispNode[])
```

**优点**：
- 更好的错误定位和恢复
- 支持 Syntax Highlighting（token 级别）
- 方便实现 LSP（Language Server Protocol）

**缺点**：
- 增加约 200 行代码
- 与现有 API 不完全兼容（需要 `Parse()` 内部包装）

**建议**：阶段 1 先保持现有架构，仅在需要 LSP/highlighting 时再拆 Lexer。

### 4.2 增加语义验证层

当前 `ValidateLisp()` 只做简单检查。拆分后可扩展：
- Schema 定义：声明每种 form 的参数结构（类似 JSON Schema）
- 类型推断：追踪 let/set 变量的类型
- 死代码检测：未使用的变量、不可达的分支

### 4.3 增加 Diff / Patch 原语

ECABridge Phase 3 的语义 diff (`ELispDiffOp` + `FLispDiffEntry`) 目前嵌入在 `ECABlueprintLispCommands.cpp` (~L6527)。如果拆到独立模块，可以让所有基于 S-expression DSL 的插件复用同一套 diff/patch 基础设施。

```
BlueprintLisp 模块扩展:
├── FLispDiff (AST 对比)
├── FLispPatch (AST 修改)
└── FLispSchema (可选: 形式验证)
```

---

## 5. 对 AnimBP2FP / MatBP2FP 的价值

拆分后的 `BlueprintLisp` 模块可以直接被 AnimBP2FP 和 MatBP2FP 依赖：

```csharp
// AnimBP2FP.Build.cs
PublicDependencyModuleNames.Add("BlueprintLisp");
```

**具体使用场景**：
1. **通用 S-expression 工具函数** — `PrettyPrint`、`Minify` 可直接用于格式化 AnimLang / MatLang 输出
2. **AST 操作基类** — `FLispNode` 的 `IsForm`、`GetKeywordArg`、`HasKeyword` 是通用的 S-expression 遍历原语
3. **统一的 Diff 基础设施** — 如果 AnimLang/MatLang 也用 S-expression 表示，可复用同一套 diff 算法

**注意**：AnimBP2FP 和 MatBP2FP 目前有自己的 AST 定义（`FAnimNodeAST`、`FMaterialGraphAST`），不一定需要直接替换为 `FLispNode`。更实际的是：
- 复用 `FLispParser` 做底层 S-expression 解析（如 MatLang 的 connect 表达式）
- 复用 `PrettyPrint` / `Minify` 做格式化
- 未来如果 DSL 统一为纯 S-expression，则可完全迁移到 `FLispNode` AST

---

## 6. 实施计划

| 步骤 | 内容 | 预估工作量 |
|------|------|-----------|
| 1 | 创建 `BlueprintLisp` 插件骨架（.uplugin + Build.cs + Module） | 30 min |
| 2 | 从 `BlueprintLisp.cpp` 提取纯解析代码到新模块 | 1 hr |
| 3 | 修改 `BlueprintLisp.h`：`ECABRIDGE_API` → `BLUEPRINTLISP_API`，去掉 Converter 声明 | 30 min |
| 4 | ECABridge 添加对 BlueprintLisp 模块的依赖 | 15 min |
| 5 | ECABridge 中的 Converter 代码保留在 ECABridge 模块内 | 15 min |
| 6 | 编译验证两个模块 | 30 min |
| 7 | （可选）AnimBP2FP 添加依赖并试用 | 1 hr |

**总预估**：~3-4 小时（不含可选步骤）

---

## 7. 风险与注意事项

1. **ECABRIDGE_API 宏**：需要确保新模块用 `BLUEPRINTLISP_API` 导出，ECABridge 通过模块依赖链接
2. **FBlueprintLispConverter 的归属**：它依赖 UE 编辑器 API（Blueprint 操作），所以必须留在 ECABridge 中。但它的部分方法（`ValidateLisp`）可以移到新模块
3. **include 路径**：拆分后其他模块引用时，include 路径从 `"BlueprintLisp.h"` 变为 `"BlueprintLisp/BlueprintLisp.h"`（或设置 PublicIncludePaths）
4. **原 ECABridge 的 include 保持兼容**：可以在 ECABridge 中做一个转发头文件
5. **源码版本**：ECABridge 在 ue5-main 分支，拆分出的插件需确保与当前引擎版本兼容
