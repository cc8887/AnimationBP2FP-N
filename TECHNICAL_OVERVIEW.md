# AnimBP2FP 技术概览

> UE5 编辑器插件：动画蓝图（AnimBlueprint）⇄ S-expression DSL（AnimLang）双向转换工具

---

## 一、项目定位

AnimBP2FP 实现动画蓝图与自定义文本 DSL 之间的**双向无损转换**，使动画蓝图可以：

- 以纯文本形式阅读、编辑、比较
- 纳入版本管理（Git diff/merge）
- 支持增量更新（只 patch 变化部分）
- 通过语义分析发现错误（重复定义、循环依赖、未解析引用）

---

## 二、DSL 设计（AnimLang）

### 2.1 为什么选 S-expression

| 考量 | S-expression 的优势 |
|------|-------------------|
| 结构映射 | 树形语法天然映射动画图的节点树 |
| 解析简单 | 无歧义、递归下降即可完成 |
| 可扩展 | 新节点类型无需改语法规则 |
| 可读性 | Keyword 参数 `:key value` 提供良好的自文档化 |

### 2.2 语法结构

```lisp
(anim-blueprint "ALS_AnimBP"
  :skeleton "/Game/AdvancedLocomotionSystemV/CharacterAssets/MannequinSkeleton"
  :variables [(var :name "Speed" :type Float :default 0.0)
              (var :name "IsMoving" :type Bool :default false)]

  ;; SaveCachedPose → 函数式 define 绑定
  (define PostLayering
    (save-cached-pose
      :source-pose (linked-anim-layer :layer "OverlayLayer")))

  :anim-graph
    (output-pose
      (blend-poses-by-bool
        :active-value (ref "Get IsMoving")         ;; EventGraph 变量引用
        :true-pose (sequence-player
                     :name "Run_F"
                     :sequence (asset "/Game/Animations/Run_F")
                     :loop true)
        :false-pose (sequence-player
                     :name "Idle"
                     :loop true))))
```

### 2.3 语法元素表

| 语法 | 语义 | 示例 |
|------|------|------|
| `(node-type ...)` | 动画图节点 | `(sequence-player ...)` |
| `:key value` | 关键字属性 (pin/property) | `:loop true` |
| `:pin-name (child)` | Pose pin 子节点连接 | `:true-pose (sequence-player ...)` |
| `(ref "Title")` | 节点间引用（变量/缓存 pose） | `(ref "Get Speed")` |
| `(asset "/Path")` | UE 资产全路径引用 | `(asset "/Game/Anims/Run")` |
| `(define Name body)` | SaveCachedPose 绑定 | `(define Locomotion (...))` |
| `[item1 item2 ...]` | 列表/数组 | `:variables [...]` |
| `(From -> To ...)` | 状态转换定义 | `(Idle -> Run :duration 0.2)` |
| `; comment` | 注释 | `;; blend tree` |

### 2.4 命名约定

- UE 节点类型 `UAnimGraphNode_SequencePlayer` → DSL `sequence-player`（CamelCase → kebab-case）
- Pin 名称 `TruePose` → `:true-pose`
- 反向还原时 kebab-case → CamelCase

---

## 三、模块架构

```
AnimBP2FP/                          (Runtime 模块, LoadingPhase: PreDefault)
├── Public/
│   ├── AnimLangAST.h               — AST 数据结构定义
│   ├── AnimLangTokenizer.h         — 词法分析器
│   ├── AnimLangParser.h            — 语法分析器
│   ├── AnimLangDiffer.h            — AST 差异比较
│   └── AnimLangDiagnostics.h       — 诊断系统
├── Private/
│   ├── AnimLangTokenizer.cpp       — 13 种 Token 类型
│   ├── AnimLangParser.cpp          — ~720 行递归下降解析
│   ├── AnimLangDiffer.cpp          — 16 种 DiffOp, 三阶段匹配
│   ├── AnimLangRoundTrip.cpp       — 往返验证
│   ├── AnimBPExporter.cpp          — ~1075 行, Blueprint → DSL (#if WITH_EDITOR)
│   ├── AnimBPImporter.cpp          — ~850 行, DSL → Blueprint (#if WITH_EDITOR)
│   ├── AnimBPPatcher.cpp           — 增量更新 (#if WITH_EDITOR)
│   └── AnimLangDiagnostics.cpp     — 语义分析
│
AnimBP2FPEditor/                    (Editor 模块, LoadingPhase: Default)
├── Private/
│   ├── AnimBP2FPEditorModule.cpp   — 编辑器菜单 UI
│   ├── AnimBP2FPExportCommandlet.cpp   — Export 命令行
│   ├── AnimBP2FPImportCommandlet.cpp   — Import 命令行
│   ├── AnimBP2FPRoundTripCommandlet.cpp — 往返测试命令行
│   └── AnimBP2FPSettings.cpp       — 插件设置
```

### 关键设计决策

- **编辑器专用代码**（Exporter/Importer/Patcher）放在 Runtime 模块但用 `#if WITH_EDITOR` 隔离，避免 Shipping 包体积
- Runtime 模块 `PreDefault` 加载确保 AST/Parser 等基础设施先于 Editor 模块可用
- Commandlet 放在 Editor 模块，通过 `-run=` 参数调用

---

## 四、核心组件详解

### 4.1 Tokenizer（词法分析）

**13 种 Token 类型**：

| Token | 匹配 | 示例 |
|-------|------|------|
| `LParen` | `(` | |
| `RParen` | `)` | |
| `LBracket` | `[` | |
| `RBracket` | `]` | |
| `String` | `"..."` (支持 `\"` `\\` 转义) | `"Run_F"` |
| `Integer` | 整数 | `42` |
| `Float` | 浮点数 | `0.5` |
| `Bool` | `true` / `false` | `true` |
| `Keyword` | `:identifier` | `:loop` |
| `Identifier` | 标识符 | `sequence-player` |
| `Arrow` | `->` | 状态转换 |
| `Comment` | `;...` | `;; note` |
| `EOF` | 结束 | |

- 手写逐字符状态机，无第三方依赖
- 字符串中 `\"` → `"`, `\\` → `\`（转义还原）

### 4.2 Parser（语法分析）

- **递归下降**，约 720 行
- 输入：Token 流 → 输出：`FAnimGraphAST`

**AST 核心结构**：

```cpp
struct FAnimNodeAST {
    FString Type;                           // 节点类型 (kebab-case)
    FString NodeId;                         // 唯一标识
    TMap<FString, FString> Properties;      // :key → value
    TArray<FAnimNodeChild> Children;        // Pose pin 子节点
    TArray<FAnimNodeAST> Defines;           // define 绑定列表
};
```

**特殊解析逻辑**：
- `(ref "...")` → Properties 中存 `(ref "Node Title")` 原始字符串
- `(asset "...")` → Properties 中存 `(asset "/Game/Path")` 原始字符串
- `(define Name body)` → 提取到 Defines 列表
- `[...]` → 透传为字符串列表
- **错误恢复**：语法错误时跳到匹配括号继续解析

### 4.3 Exporter（Blueprint → DSL）

约 1075 行，核心流程：

```
AnimBlueprint
  └→ AnimGraph (Root Node)
       └→ 递归遍历 Pose Pin 连接
            ├→ 每个节点:
            │   ├→ 类型映射 (CamelCase → kebab-case)
            │   ├→ CollectNonPoseParams() — 遍历 Pin, 提取非默认值属性
            │   ├→ CollectInternalProperties() — FProperty 反射, CDO 比较
            │   └→ 递归处理子节点
            ├→ SaveCachedPose → (define) 绑定
            │   └→ Kahn 算法拓扑排序
            └→ StateMachine → 完整展开
                 ├→ State 子图递归
                 └→ Transition 列表
```

**12+ 专门处理的节点类型**：

| 节点类型 | 特殊处理 |
|---------|---------|
| `SequencePlayer` | `:name`, `:sequence (asset)`, `:loop` |
| `SequenceEvaluator` | `:sequence (asset)`, `:explicit-time` |
| `BlendSpacePlayer` | `:name`, `:blend-space (asset)`, `:loop` |
| `SaveCachedPose` | → `(define Name body)` |
| `UseCachedPose` | → 变量引用 |
| `StateMachine` | 递归展开 State/Transition |
| `LinkedAnimLayer` | `:layer`, `:interface` |
| `BlendListByEnum` | `:active-enum-value`, 多 pose pin |
| `AlphaBoolBlend` | 结构体属性 |
| `ModifyCurve` | 动态数组 pin |
| `LayeredBoneBlend` | 动态数组 pin + 骨骼设置 |
| `TwoBoneIK` 等 | 骨骼/效果器通过反射导出 |

**CollectInternalProperties 反射机制**：

```
FAnimNode_Base 子结构体
  └→ FProperty 遍历
       ├→ 与 CDO 比较，跳过默认值
       ├→ 跳过 PoseLink / 已有 Pin 的属性
       └→ 类型分发:
            Bool/Int/Float → 裸值
            Enum/Name/String → 引号包裹
            Object → (asset "path")
            Struct/Array → ExportText 引号包裹
```

### 4.4 Importer（DSL → Blueprint）

约 850 行，完整反向管线：

```
DSL Text
  └→ Parse → AST
       └→ BuildAnimGraph
            ├→ FindAnimNodeClass(kebab → CamelCase → TObjectIterator 查 UClass)
            ├→ BuildAnimNode (13+ 节点类型)
            │   ├→ 创建 UAnimGraphNode_* 实例
            │   ├→ AllocateDefaultPins + ReconstructNode
            │   ├→ 特殊属性处理 (Sequence/BlendSpace/Layer...)
            │   ├→ 通用属性循环:
            │   │   ├→ (ref "...") → Skip + [DEGRADATION:RefConnection]
            │   │   ├→ (asset "...") → 提取路径 + SetNodeProperty
            │   │   └→ 普通值 → SetNodeProperty
            │   └→ 递归处理子节点
            ├→ BuildStateMachine
            │   ├→ 创建 State 节点 + 子图
            │   ├→ 创建 Transition 节点
            │   └→ CreateConnections(From, To)
            └→ ConnectPins (fuzzy matching)
                 └→ UpdateBlueprint (增量 / 全重建)
```

**SetNodeProperty 三级匹配**：

1. **Pin 精确匹配** → `Pin->DefaultValue = CleanValue`
2. **Pin 模糊匹配**（忽略大小写/下划线/kebab）→ 同上
3. **FProperty 反射** → `ImportText_Direct` 设置内部属性

**资产查找三级策略**：

1. `LoadObject<>` 全路径加载
2. `TObjectIterator<>` 短名搜索（已加载对象）
3. `AssetRegistry` 搜索（含 `ScanPathsSynchronous` 强制索引引擎路径）

### 4.5 Differ（差异比较）

**16 种 DiffOp 类型**，覆盖节点的增删改：

- 类型变更、属性增删改、子节点增删改移、Define 变更等

**三阶段子节点匹配算法**：

| 阶段 | 策略 | 匹配质量 |
|------|------|---------|
| 1 | Pin 名称精确匹配 | 最佳 |
| 2 | NodeId 匹配 | 高 |
| 3 | 类型 + 位置启发式 | 兜底 |

### 4.6 Patcher（增量更新）

```
当前 Blueprint → Export → AST_old
用户编辑的 DSL → Parse → AST_new
               Diff(AST_old, AST_new) → DiffOps
               Apply(DiffOps, Blueprint) → 修改后的 Blueprint
               Compile Blueprint
```

- 只修改变化的节点，保留未修改部分的内部状态
- 避免全量重建带来的 GUID 变化和编辑器状态丢失

### 4.7 Diagnostics（诊断系统）

**Severity × Category 二维模型**：

| Severity | 含义 |
|----------|------|
| Error | 致命错误，无法继续 |
| Warning | 退化/精度损失 |
| Info | 信息性提示 |

**语义分析**：
- 重复 `define` 检测
- 循环依赖检测（define 间的引用环）
- 未解析引用检测（`(ref "...")` 目标不存在）

**Import 退化标签系统**：

| 标签 | 含义 |
|------|------|
| `[DEGRADATION:RefConnection]` | EventGraph 变量连接无法还原 |
| `[DEGRADATION:CachedPoseLink]` | UseCachedPose 无法链接 SaveCachedPose |
| `[DEGRADATION:AssetLoad]` | 资产加载失败 |
| `[DEGRADATION:AssetRef]` | (asset) 属性反射设置失败 |
| `[DEGRADATION:PropertySet]` | Pin/Property 匹配失败 |

### 4.8 RoundTrip 验证

```
Blueprint → Export → DSL_text
                      └→ Parse → AST → ToString → DSL_text'
                                                    逐行比较(DSL_text, DSL_text')
```

验证 Exporter ↔ Parser 的**无损往返**，确保导出的 DSL 可以被完美解析回来。

---

## 五、关键技术难点与解决方案

| # | 难点 | 解决方案 |
|---|------|---------|
| 1 | **UE 动画节点类型多样（50+ 种）** | 12+ 节点专门分支 + CollectInternalProperties 反射兜底 |
| 2 | **节点属性未暴露为 EdGraph Pin** | FProperty 反射遍历 FAnimNode_Base，CDO 比较过滤默认值 |
| 3 | **资产引用查找** | 三级策略：LoadObject → TObjectIterator → AssetRegistry |
| 4 | **BlendSpace1D 在 /Engine/ 路径下** | `ScanPathsSynchronous({"/Engine/"})` 强制索引 |
| 5 | **MinimalAPI 类方法不可调用** | 直接访问 public UPROPERTY + 基类 virtual 绕过 |
| 6 | **SaveCachedPose 依赖顺序** | Kahn 算法拓扑排序 define 绑定 |
| 7 | **状态机多层嵌套** | 递归展开：StateMachine → State 子图 → Transition 列表 |
| 8 | **引号转义一致性** | 单一输出点：AST ToString 统一转义，Parser/Importer 反向还原 |
| 9 | **Pin 名称不精确匹配** | Fuzzy matching（大小写/下划线/kebab 容错） |
| 10 | **EventGraph 变量连接** | 当前记录为退化（需 K2Node_VariableGet 创建，待实现） |
| 11 | **动态数组 Pin 数量不确定** | 预处理 AST 计算子节点数量，创建足够 pin 后再 ReconstructNode |

---

## 六、UE API 关键用法

### 动画节点操作

```cpp
// Pin 访问 (直接字段, 不是方法)
Node->Pins

// SequencePlayer
SeqPlayer->Node.SetSequence(AnimSeq);
SeqPlayer->Node.SetLoopAnimation(bLoop);
SeqPlayer->Node.GetSequence();

// BlendSpacePlayer
BSPlayer->Node.SetBlendSpace(BlendSpace);
BSPlayer->Node.SetLoop(bLoop);

// StateMachine
SMNode->EditorStateMachineGraph;  // 不是 GetStateMachineGraph()
SMNode->OnRenameNode(Name);

// Transition
TransNode->CreateConnections(FromState, ToState);
TransNode->CrossfadeDuration = 0.2f;
TransNode->PriorityOrder = 1;

// LinkedAnimLayer (MinimalAPI 绕过)
LayerNode->Node.Layer = FName("LayerName");  // 直接访问 public 成员
LayerNode->Node.Interface = InterfaceClass;
```

### Blueprint 操作

```cpp
// 创建动画蓝图
UAnimBlueprintFactory::FactoryCreateNew(...)

// 添加变量
FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type)

// 骨架
AnimBlueprint->TargetSkeleton  // UE5: GetTargetSkeleton()

// Pin 类型常量
UEdGraphSchema_K2::PC_Struct / PC_Float / PC_Real / PC_Int / PC_Boolean / PC_Name

// 手动创建 Pin
Node->CreatePin(EGPD_Input, PinType, PinName)

// UClass 动态查找 (UE5 中 ANY_PACKAGE 已弃用)
for (TObjectIterator<UClass> It; It; ++It) { ... }
```

### AssetRegistry

```cpp
// 强制扫描路径
IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
AR.ScanPathsSynchronous({"/Engine/"}, true);

// 按类搜索
TArray<FAssetData> Assets;
AR.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), Assets, false);
```

---

## 七、工具链与使用方式

### 编辑器菜单

`Tools → AnimBP2FP →`

| 菜单项 | 功能 |
|--------|------|
| Export AnimLang Nodes | 导出当前打开的动画蓝图 |
| Export AnimBP to DSL | 批量导出所有动画蓝图 |
| Run Round-Trip Validation | 运行往返验证测试 |

### Commandlet（无头模式）

```bash
# 导出所有动画蓝图到 DSL
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPExport -stdout -nullrhi

# 往返验证
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPRoundTrip -stdout -nullrhi

# Import 测试 (不修改原蓝图, 创建 _Imported 副本)
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPImport -test -stdout -nullrhi

# Import 更新 (直接修改原蓝图)
UnrealEditor-Cmd.exe "Project.uproject" -run=AnimBP2FPImport -update -stdout -nullrhi
```

### 输出目录

```
<ProjectDir>/AnimLang/Exported/
├── ALS_AnimBP.animlang             — DSL 文本文件
├── Editor.animlang
├── round_trip_report.txt           — Export 往返报告
└── import_report.txt               — Import 测试报告
```

---

## 八、验证状态

### Export 往返测试

| 蓝图 | 结果 |
|------|------|
| ALS_AnimBP | ✅ PASS (100%) |
| ALS_PlayerCameraBehavior | ✅ PASS (100%) |
| Bow_AnimBP | ✅ PASS (100%) |
| Editor | ✅ PASS (100%) |
| TutorialAnimationBlueprint | ✅ PASS (100%) |
| TutorialTPP_AnimBlueprint | ✅ PASS (100%) |

### Import 往返测试

| 蓝图 | 保真度 | 剩余 diff 原因 |
|------|--------|---------------|
| TutorialAnimBP | **100%** ✅ | — |
| Editor | **94.7%** | alpha-bool-blend 空结构体 |
| ALS_AnimBP | **92.6%** | ref 连接 + alpha-bool-blend |
| Bow_AnimBP | **87.5%** | ref 连接 (explicit-time) |
| TutorialTPP | **87.5%** | ref 连接 (x=Get Speed) |
| CameraBehavior | **74.9%** | ref 连接 + 节点标题本地化 |

### 剩余限制

1. **EventGraph 变量连接**（77 处）— 需创建 K2Node_VariableGet
2. **UseCachedPose 链接**（12 处）— 需设置 LinkToCacheName
3. **alpha-bool-blend 空结构体** — CDO 默认值过滤
4. **节点标题本地化** — Import 后标题从自定义变为本地化默认

---

*最后更新：2026-03-25*
