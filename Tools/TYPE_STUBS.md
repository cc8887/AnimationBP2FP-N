# AnimLang 类型存根（Stub）系统

## 📚 概述

AnimLang 支持类型存根文件，类似于：
- TypeScript 的 `.d.ts`
- Python 的 `.pyi`
- C/C++ 的头文件

这些存根文件提供：
- ✅ 类型定义
- ✅ IDE 自动补全
- ✅ 实时类型检查
- ✅ 文档生成

---

## 🎯 现有存根文件

### 1. `animlang-types.rkt` - 基础类型
- **路径**: `Tools/animlang-types.rkt`
- **内容**: 核心数据类型（PinType、Expr、State 等）
- **状态**: ✅ 已创建

### 2. `animlang-nodes.rkt` - 节点库
- **路径**: `Tools/animlang-nodes.rkt`
- **内容**: 23 个动画节点 + `(define Name body)` 语法
- **状态**: ✅ 已更新 (2026-03-25)

**新格式说明**：
- `SaveCachedPose` → `(define Name body)` 顶层绑定 (Lisp define 语义)
- `UseCachedPose` → `(Name)` 裸变量引用
- 所有参数使用 `:key value` 关键字风格
- 动画数据输入使用 `:pin-name (child ...)` 命名子节点
- 外部引用使用 `(ref "Node Title")` 表示
- 节点名称使用 kebab-case

**包含的节点**：
- **基础播放**（3 个）：sequence-player、sequence-evaluator、blendspace-player
- **混合**（5 个）：blend、blend-list、apply-additive、apply-mesh-space-additive、layered-bone-blend
- **状态机**（1 个）：state-machine（完整展开状态 + 转换）
- **定义绑定**：`(define Name body)` ← SaveCachedPose，`(Name)` ← UseCachedPose
- **骨骼修改**（4 个）：two-bone-i-k、modify-bone、modify-curve、constraint
- **空间转换**（2 个）：component-to-local-space、local-to-component-space
- **层/链接**（2 个）：linked-anim-layer、linked-input-pose
- **Slot**（1 个）：slot
- **其他**（3 个）：look-at、aim-offset、identity-pose
- **通用回退**：未列出的节点自动提取 pin 参数和 pose 输入

---

## 🤖 自动导出：从 UE 引擎生成

### 方案 A：使用 C++ Commandlet（推荐）

#### 1. 编译插件
```bash
cd AnimBP2FP/Plugin
ue5 build -target AnimBP2FP -platform Win64
```

#### 2. 运行导出命令
```bash
# 从 UE 编辑器内
UE Editor -> Cmd -> AnimNodeExporter.Export

# 或作为 Commandlet 运行
UnrealEditor-Cmd.exe YourProject \
  -run=AnimNodeExporter \
  -output=AnimLangNodesGenerated.rkt
```

#### 3. 导出内容
- 扫描所有 `UAnimGraphNode_Base` 子类
- 提取引脚信息（输入/输出类型）
- 提取属性定义
- 生成 Typed Racket 类型签名

**输出示例**：
```racket
;; define 绑定 (SaveCachedPose → 顶层 define)
(define Post-Layering
  (linked-anim-layer
    :base-layer-input (linked-anim-layer)
    :overlay-layer-input (linked-anim-layer)))

;; 变量引用 (UseCachedPose → 裸变量名)
(apply-mesh-space-additive :alpha (ref "Get Enable_AimOffset")
  :base (Post-Layering)           ;; ← 引用 define 绑定
  :additive (linked-anim-layer))

;; Blend List (多个命名 Pose 输入)
(blend-list :class "AnimGraphNode_BlendListByEnum"
            :blend-time-0 0.400000 :blend-time-1 0.500000
            :active-enum-value (ref "Get MovementState")
  :blend-pose-0
    (linked-anim-layer :in-pose (Post-Layering))
  :blend-pose-1
    (state-machine :name "Ragdoll States" :initial "In Ragdoll"
        :transitions [(In Ragdoll -> Blend Out Pose :duration 0.0 :priority 1 :rule (ref "不相等（枚举）"))]
      :in-ragdoll
        (sequence-player :name "ALS_Flail" :loop true :play-rate (ref "Get FlailRate"))
      :blend-out-pose
        (pose-snapshot :snapshot-name "RagdollPose")))

;; 多个 define 形成依赖链 (拓扑排序: 依赖在前)
(define Main-Camera-States
  (state-machine :name "Main Camera States" :initial "Velocity Direction"
      :transitions [(Velocity Direction -> Looking Direction ...) ...]
    :velocity-direction (blendspace-player ...)
    :looking-direction (blendspace-player ...)
    :aiming (blendspace-player ...)))
(define ShoulderSwap
  (blend-list ... :blend-pose-0 (Main-Camera-States) ...))
```

---

### 方案 B：使用 Blueprint 反射（轻量级）

如果不想编译 C++ 插件，可以用蓝图脚本：

#### 1. 创建编辑器工具蓝图
- 新建 Editor Utility Widget
- 使用 `Get All Classes Of Class` 获取所有 `AnimGraphNode` 子类
- 遍历并提取信息

#### 2. 导出为 JSON
```json
{
  "nodes": [
    {
      "name": "sequence-player",
      "class": "UAnimGraphNode_SequencePlayer",
      "inputs": [
        {"name": "Sequence", "type": "AnimSequence", "optional": false}
      ],
      "properties": [
        {"name": "bLoopAnimation", "type": "bool"},
        {"name": "PlayRate", "type": "float"}
      ]
    }
  ]
}
```

#### 3. 用 Python 脚本转换为 Racket
```python
# json_to_racket.py
import json

with open('nodes.json') as f:
    nodes = json.load(f)['nodes']

for node in nodes:
    print(f"(: {node['name']} (->* (...) AnimNode))")
```

---

## 🔄 类型检查集成

### 1. 在 Linter 中使用类型定义

更新 `animlang-lint.rkt`：

```racket
#lang typed/racket

(require "animlang-types.rkt")
(require "animlang-nodes.rkt")

;; 使用类型签名验证节点调用
(define (check-node-call node-name args)
  (match node-name
    ['sequence-player
     (check-args args sequence-player-signature)]
    ['blend
     (check-args args blend-signature)]
    [_ (error "Unknown node type")]))
```

### 2. IDE 集成（VSCode/Cursor）

创建 `.vscode/settings.json`：
```json
{
  "racket.executable": "racket",
  "racket.linter.enabled": true,
  "racket.linter.command": "./Tools/animlang-lint.rkt"
}
```

### 3. 自动补全

使用 Racket Mode（Emacs）或 Magic Racket（VSCode）：
- 输入 `(sequence-` 自动提示 `sequence-player`
- 提供参数类型提示（`:loop Boolean`）

---

## 📖 使用示例

### 示例 1：类型安全的 DSL

```racket
#lang typed/racket

(require "animlang-types.rkt")
(require "animlang-nodes.rkt")

;; 正确：类型匹配
(define walk : AnimNode
  (sequence-player "Walk_Fwd"
                   #:loop #t
                   #:play-rate 1.0))

;; 错误：类型不匹配（编译时报错）
(define bad : AnimNode
  (sequence-player "Walk_Fwd"
                   #:loop "yes"))  ;; 应为 Boolean
```

### 示例 2：自定义节点定义

如果你有自定义节点，可以手动添加：

```racket
;; 在 animlang-nodes.rkt 中添加

;; 自定义：腿部 IK
(: leg-ik (->* ()
               (#:left-foot-target Expr
                #:right-foot-target Expr
                #:pelvis-adjustment Float)
               AnimNode))

(provide leg-ik)
```

---

## 🛠️ 维护工作流

### 何时需要重新生成？

1. **UE 引擎升级**（新节点或 API 变化）
2. **添加自定义节点**
3. **修改现有节点属性**

### 自动化方案

#### Git Hook（推荐）

创建 `.git/hooks/pre-commit`：
```bash
#!/bin/bash

# 检测 UE 引擎版本变化
if git diff --cached Engine.version; then
    echo "UE version changed, regenerating node definitions..."
    ./Tools/export_nodes.sh
    git add Tools/animlang-nodes-generated.rkt
fi
```

#### CI/CD 集成

```yaml
# .github/workflows/update-types.yml
name: Update Type Definitions

on:
  schedule:
    - cron: '0 0 * * 0'  # 每周日

jobs:
  update-types:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Export UE Nodes
        run: |
          UnrealEditor-Cmd.exe AnimBP2FP -run=AnimNodeExporter
      - name: Commit Changes
        run: |
          git add Tools/animlang-nodes-generated.rkt
          git commit -m "Update node definitions"
          git push
```

---

## 📊 对比：手动 vs 自动

| 方面 | 手动维护 | 自动导出 |
|------|----------|----------|
| **初始成本** | 低（直接写） | 中（需写 Exporter） |
| **维护成本** | 高（手动同步） | 低（自动生成） |
| **完整性** | 可能遗漏 | 保证完整 |
| **自定义** | 灵活 | 需额外标注 |
| **文档质量** | 手写更好 | 自动生成较简 |

**建议**：
- **Phase 1-2**: 手动维护（快速迭代）✅ 当前方案
- **Phase 3+**: 切换到自动导出（生产环境）

---

## 🔗 相关文件

| 文件 | 作用 | 状态 |
|------|------|------|
| `animlang-types.rkt` | 基础类型定义 (含 RefExpr/AssetRef/State/Transition) | ✅ 已更新 (2026-03-25) |
| `animlang-nodes.rkt` | 节点库 (23 个 + define 语法，含状态机展开) | ✅ 已更新 (2026-03-25) |
| `AnimNodeExporter.h/cpp` | C++ 自动导出器 | ✅ 已创建 |
| `animlang-nodes-generated.rkt` | 自动生成存根 | ⏳ 待运行 |

---

## 📝 下一步

1. ✅ 创建基础类型定义（已完成）
2. ✅ 手动整理 18 个核心节点（已完成）
3. ⏳ 在 Linter 中集成类型检查（Phase 2.3）
4. ⏳ 实现 C++ Exporter（Phase 3）
5. ⏳ 验证自动导出（Phase 4）

---

**最后更新**: 2026-03-25 06:45 GMT+8  
**作者**: OpenClaw AI Assistant