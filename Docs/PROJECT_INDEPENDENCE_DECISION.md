# 项目独立性决策文档

**决策时间**: 2026-03-24 03:00 GMT+8  
**决策人**: yuchencui  
**相关项目**: AnimBP2FP, MaterialBP2FP

---

## 🎯 核心决策

**AnimBP2FP 和 MaterialBP2FP 保持为独立项目，不合并。**

---

## 📊 关键差异对比

| 维度 | AnimBP2FP | MaterialBP2FP | 影响 |
|------|-----------|---------------|------|
| **函数式纯度** | ~70% | **~95%** | 架构设计根本差异 |
| **计算模型** | 时序状态机 | 纯数学计算图（DAG） | 不同的验证逻辑 |
| **副作用** | 有（状态转换） | 几乎无（除 Custom Node） | 不同的测试策略 |
| **类型系统** | UE 自定义 | HLSL 标准 | 类型检查器不兼容 |
| **测试方法** | 姿态对比（慢） | 像素对比（快） | 测试框架不同 |

---

## 🔬 技术原因

### 1. 函数式纯度差异（核心）

#### AnimBP: 70% 纯函数
```lisp
;; ✅ 纯函数节点（70%）
(blend 0.5
  (sequence-player "Walk")
  (sequence-player "Run"))

;; ❌ 状态机有内部状态（30%）
(state-machine :locomotion
  :states [...]
  :transitions [...])
;; 相同输入可能产生不同输出（取决于当前状态）
```

#### MaterialBP: 95% 纯函数
```lisp
;; ✅ 完全纯函数（95%）
(material "PBR"
  :base-color (multiply
                (texture-sample "T_Albedo")
                (param :tint)))
;; 相同输入 → 相同输出（确定性）

;; ❌ Custom Node 可能有副作用（5%）
(custom-node "
  float3 result = WorldTime.xyz; // 访问全局状态
  return result;
")
```

**结论**: 合并会损失 MaterialBP 的纯函数式优势。

---

### 2. 类型系统不兼容

#### AnimBP 类型（UE 自定义）
```haskell
data AnimType
  = Pose              -- 骨骼姿态（复杂结构体）
  | Float
  | Bool
  | Vector            -- UE 的 FVector
  | Rotator           -- UE 的 FRotator
  | AnimSequence      -- 资产引用
```

#### MaterialBP 类型（HLSL 标准）
```haskell
data MaterialType
  = Float             -- float
  | Float2            -- float2
  | Float3            -- float3
  | Float4            -- float4
  | Texture2D         -- Texture2D
  | TextureCube       -- TextureCube
```

**结论**: 类型检查器需要完全不同的实现。

---

### 3. 测试策略不同

#### AnimBP 测试（复杂）
```cpp
// 需要运行时验证
bool TestAnimBP(UAnimBlueprint* Original, UAnimBlueprint* Reconstructed)
{
    for (int i = 0; i < 1000; ++i)  // 运行 1000 帧
    {
        FPose OriginalPose = Evaluate(Original);
        FPose ReconstructedPose = Evaluate(Reconstructed);
        
        // 比较每根骨骼的变换
        if (!ComparePoses(OriginalPose, ReconstructedPose))
            return false;
    }
    return true;
}
```

#### MaterialBP 测试（简单）
```cpp
// 静态渲染对比
bool TestMaterialBP(UMaterial* Original, UMaterial* Reconstructed)
{
    UTextureRenderTarget2D* RT1 = Render(Original);
    UTextureRenderTarget2D* RT2 = Render(Reconstructed);
    
    return ComparePixels(RT1, RT2, Threshold=0.01);
}
```

**结论**: 测试框架完全不同，合并会导致混乱。

---

### 4. Linter 规则冲突

#### AnimBP Linter 规则
```racket
;; 验证状态机完整性
(define (check-state-machine sm)
  (check-initial-state-exists sm)
  (check-all-states-reachable sm)
  (check-no-dead-states sm)
  (check-transition-conditions sm))

;; 验证时序逻辑
(define (check-temporal-logic node)
  (check-blend-timing node)
  (check-animation-sync node))
```

#### MaterialBP Linter 规则
```racket
;; 验证 HLSL 类型安全
(define (check-material-types node)
  (check-pin-types node)
  (check-auto-conversion node)
  (check-texture-channels node))

;; 验证像素着色正确性
(define (check-shader-correctness node)
  (check-normal-map-format node)
  (check-roughness-range node))
```

**结论**: 规则集完全不同，合并会导致 Linter 臃肿且难以维护。

---

## ✅ 共享基础设施（最小化耦合）

### 可以共享的部分
```
Common/
├── sexpp/                  # S-expression 解析器（C++）
├── Racket/
│   ├── parser-base.rkt     # 基础解析器框架
│   └── formatter-base.rkt  # 基础格式化工具
└── UE-Editor/
    ├── EditorModuleBase/   # 编辑器模块基类
    └── AutoExportPattern/  # 自动导出模式（UE-style）
```

### 必须独立的部分
```
AnimBP2FP/
├── animlang-types.rkt      # 动画类型定义
├── animlang-nodes.rkt      # 动画节点库
├── animlang-lint.rkt       # 动画专用 Linter
├── AnimBPExporter.cpp      # 动画蓝图导出器
└── StateChecker.cpp        # 状态机验证器

MaterialBP2FP/
├── materiallang-types.rkt  # 材质类型定义（HLSL）
├── materiallang-nodes.rkt  # 材质节点库
├── materiallang-lint.rkt   # 材质专用 Linter
├── MaterialExporter.cpp    # 材质蓝图导出器
└── ShaderValidator.cpp     # 着色器验证器
```

---

## 🎯 统一 CLI 工具（保持接口一致）

虽然项目独立,但可以提供统一的 CLI 接口：

```bash
# 统一的 CLI 工具
ue-functional export --type animation --input MyAnimBP.uasset
ue-functional export --type material --input MyMaterial.uasset

ue-functional import --type animation --input MyAnimBP.animlang
ue-functional import --type material --input MyMaterial.matl

# 背后调用不同的实现
animation: AnimBP2FP/bin/animlang
material:  MaterialBP2FP/bin/materiallang
```

---

## 📅 开发计划

### Phase 1: AnimBP2FP（当前）
- **状态**: 进行中（~30% 完成）
- **优先级**: 高
- **原因**: 验证架构，为 MaterialBP2FP 提供经验

### Phase 2: MaterialBP2FP（等待）
- **状态**: 设计阶段（~5% 完成）
- **优先级**: 中
- **原因**: 等待 AnimBP2FP 验证后再启动实现

### 理由
- 先完成 AnimBP2FP 可以验证：
  - S-expression 解析器的有效性
  - Racket 工具链的可行性
  - UE 编辑器集成模式
  - 自动导出系统（UE-style）
  
- MaterialBP2FP 可以复用验证过的架构，减少返工

---

## 🤝 协作模式

### 共享文档
```
Docs/
├── S-Expression-Guide.md   # S-expression 语法指南（共享）
├── UE-Editor-Integration.md # UE 编辑器集成指南（共享）
└── Best-Practices.md       # 最佳实践（共享）
```

### 独立文档
```
AnimBP2FP/Docs/
├── State-Machine-Modeling.md
├── Pose-Comparison.md
└── AnimLang-Spec.md

MaterialBP2FP/Docs/
├── HLSL-Type-System.md
├── Shader-Validation.md
└── MaterialLang-Spec.md
```

---

## 📝 总结

### 为什么独立？
1. **函数式纯度**: 95% vs 70%（根本差异）
2. **类型系统**: HLSL vs UE（不兼容）
3. **测试策略**: 像素对比 vs 姿态对比（完全不同）
4. **Linter 规则**: 着色器验证 vs 状态机验证（规则集冲突）

### 如何协作？
1. **共享基础设施**: 解析器、格式化工具、编辑器模式
2. **统一 CLI 接口**: 保持用户体验一致
3. **共享文档**: 通用指南和最佳实践
4. **经验传递**: AnimBP2FP → MaterialBP2FP

### 预期收益
1. **代码清晰**: 各自专注于自己的领域
2. **维护简单**: 避免耦合导致的复杂性
3. **并行开发**: 可以由不同团队独立开发
4. **灵活演进**: 可以独立更新迭代

---

**最后更新**: 2026-03-24 03:00 GMT+8  
**决策状态**: ✅ 已确认  
**相关提交**: 
- AnimBP2FP: commit 97822b3
- MaterialBP2FP: commit ace85da