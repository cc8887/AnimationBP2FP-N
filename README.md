# AnimBP2FP - Animation Blueprint to Functional Programming

**将 UE 动画蓝图转换为函数式语言的研究项目**

## 项目概述

本项目研究如何将 Unreal Engine 5.6 的动画蓝图（Animation Blueprint）无损转换为函数式编程语言表示，并实现双向转换的 UE 插件。

## 研究目标

1. **分析动画蓝图的计算模型**
   - 数据流图结构
   - 状态机模型
   - 混合逻辑
   - 动画节点的语义

2. **选择合适的函数式语言范式**
   - 评估 Lisp 系列（Scheme/Clojure）
   - 评估 Haskell 系列（Haskell/PureScript）
   - 选择最接近的范式

3. **设计 DSL（领域特定语言）**
   - 表示动画图的语法
   - 状态机的函数式建模
   - 混合树的组合子

4. **实现 UE5.6 插件**
   - 解析动画蓝图资产
   - 生成 DSL 代码
   - 从 DSL 重建动画蓝图
   - 验证等价性

## 动画蓝图的核心特征

### 1. 数据流图（Data Flow Graph）
```
                    ┌──────────────┐
Input Pose ────────>│ Blend Nodes  │───────> Output Pose
                    │              │
Animation Asset ───>│ State Machine│
                    │              │
Parameters ────────>│ Calculations │
                    └──────────────┘
```

**特点**：
- 有向无环图（DAG）
- 纯函数式计算
- 不可变姿态（Pose）传递

### 2. 状态机（State Machine）
```lisp
; 状态机本质上是函数式的状态转换
(define-state-machine character-locomotion
  :states [idle walk run jump]
  :transitions
    [(idle -> walk   :when (> speed 0.1))
     (walk -> run    :when (> speed 5.0))
     (run  -> jump   :when jump-pressed)
     (jump -> idle   :when grounded)])
```

### 3. 混合节点（Blend Nodes）
```haskell
-- 混合本质上是加权组合
blend :: Float -> Pose -> Pose -> Pose
blend alpha p1 p2 = (1 - alpha) * p1 + alpha * p2

-- 混合空间是高维插值
blendSpace2D :: (Float, Float) -> [(Pose, (Float, Float))] -> Pose
```

## 语言选择分析

### Lisp 系列的优势
- ✅ 同像性（Homoiconicity）：代码即数据
- ✅ 宏系统：可以定义自定义语法
- ✅ S-表达式天然表示树结构
- ✅ 动态类型（可选）
- ❌ 类型安全较弱

### Haskell 系列的优势
- ✅ 强类型系统：编译时保证正确性
- ✅ 代数数据类型（ADT）：完美建模状态机
- ✅ 类型类（Type Classes）：抽象动画节点
- ✅ 惰性求值：延迟计算
- ✅ 函数组合：管道式思维
- ❌ 语法较复杂

### 初步结论

**动画蓝图更接近 Haskell**，理由：

1. **类型约束**：动画蓝图有严格的引脚类型（Pose, Float, Bool 等）
2. **纯函数性**：动画节点通常是纯函数（无副作用）
3. **代数数据类型**：状态机天然是 Sum Types（`State = Idle | Walk | Run`）
4. **组合子模式**：混合节点是典型的组合子（Combinator）

但考虑到：
- Lisp 的元编程能力
- S-表达式更易于机器生成/解析
- 与 UE 的互操作性

**最终选择：基于 Lisp 语法 + Haskell 类型系统的混合 DSL**

## DSL 设计（AnimLang）

### 语法示例

```lisp
;; 定义动画蓝图
(anim-blueprint "ThirdPersonCharacter"
  
  ;; 输入参数
  :inputs
    [(float :speed 0.0 :range [0.0 600.0])
     (bool :is-in-air false)
     (float :direction 0.0 :range [-180.0 180.0])]
  
  ;; 状态机
  :state-machine :locomotion
    :states
      [(state :idle
         :animation "Idle_Rifle"
         :loop true)
       
       (state :walk
         :blend-space-2d "WalkBlendSpace"
           :x-axis :speed
           :y-axis :direction)
       
       (state :jump
         :animation "Jump_Start"
         :then "Jump_Loop"
         :exit "Jump_Land")]
    
    :transitions
      [(idle -> walk
         :condition (> :speed 10.0)
         :blend-time 0.2)
       
       (walk -> idle
         :condition (< :speed 10.0)
         :blend-time 0.3)
       
       (* -> jump
         :condition :is-in-air
         :blend-time 0.1)]
  
  ;; 输出混合
  :output
    (blend-per-bone
      :base (state-machine :locomotion)
      :overlay (slot "UpperBody")
      :bone-filter "spine_01"))
```

### 类型系统

```haskell
-- 核心类型
data Pose = Pose BoneTransforms

data AnimNode a where
  Animation      :: String -> AnimNode Pose
  BlendSpace1D   :: Param Float -> [(Pose, Float)] -> AnimNode Pose
  BlendSpace2D   :: Param Float -> Param Float -> [(Pose, (Float, Float))] -> AnimNode Pose
  Blend          :: Param Float -> AnimNode Pose -> AnimNode Pose -> AnimNode Pose
  StateMachine   :: StateMachineDef -> AnimNode Pose
  LayeredBlend   :: [BoneFilter] -> [AnimNode Pose] -> AnimNode Pose
  
-- 状态机定义
data StateMachineDef = StateMachine
  { states      :: [State]
  , transitions :: [Transition]
  , initial     :: StateName
  }

data State = State
  { name      :: StateName
  , animation :: AnimNode Pose
  }

data Transition = Transition
  { from      :: StateName
  , to        :: StateName
  , condition :: Expr Bool
  , blendTime :: Float
  }

-- 表达式系统
data Expr a where
  Param    :: String -> Expr a
  Literal  :: a -> Expr a
  (:>)     :: Ord a => Expr a -> Expr a -> Expr Bool
  (:<)     :: Ord a => Expr a -> Expr a -> Expr Bool
  (:&&)    :: Expr Bool -> Expr Bool -> Expr Bool
  (:||)    :: Expr Bool -> Expr Bool -> Expr Bool
```

## 实现计划

### Phase 1: 研究与建模 (Week 1-2)
- [x] 分析动画蓝图的内部表示
- [ ] 提取核心节点类型
- [ ] 建立类型理论模型
- [ ] 设计 DSL 语法

### Phase 2: DSL 解析器 (Week 3)
- [ ] 实现 Lisp S-表达式解析器（C++）
- [ ] 类型检查器
- [ ] AST 构建

### Phase 3: 蓝图 → DSL (Week 4)
- [ ] 读取 UAnimBlueprint 资产
- [ ] 遍历动画图
- [ ] 生成 DSL 代码
- [ ] 优化输出格式

### Phase 4: DSL → 蓝图 (Week 5)
- [ ] 解析 DSL 代码
- [ ] 构建 UAnimGraphNode
- [ ] 连接引脚
- [ ] 创建 UAnimBlueprint 资产

### Phase 5: 测试与验证 (Week 6)
- [ ] 构建测试用例集
- [ ] 双向转换测试
- [ ] 等价性验证（姿态输出一致性）
- [ ] 性能测试

### Phase 6: 文档与发布 (Week 7)
- [ ] API 文档
- [ ] 使用教程
- [ ] 示例项目
- [ ] 性能优化

## 测试策略

### 测试用例设计

#### 1. 基础节点测试
```lisp
;; Test Case 1: Simple Animation
(test "simple-animation"
  :input '(anim "Idle")
  :verify-roundtrip)

;; Test Case 2: Blend Two Animations
(test "blend-two"
  :input '(blend 0.5
            (anim "Walk")
            (anim "Run"))
  :verify-roundtrip
  :verify-output-pose)
```

#### 2. 状态机测试
```lisp
;; Test Case 3: Simple State Machine
(test "simple-state-machine"
  :input '(state-machine
            :states [(idle (anim "Idle"))
                     (walk (anim "Walk"))]
            :transitions [(idle -> walk (> speed 0))])
  :verify-state-transitions
  :verify-roundtrip)
```

#### 3. 复杂混合测试
```lisp
;; Test Case 4: Layered Blend
(test "layered-blend"
  :input '(layered-blend
            :base (state-machine :locomotion)
            :layers [(slot "UpperBody" :filter "spine_01")
                     (slot "LeftArm" :filter "arm_l")])
  :verify-bone-masks
  :verify-output-pose)
```

#### 4. 完整角色测试
```lisp
;; Test Case 5: Third Person Character (Real World)
(test "third-person-character"
  :source "Content/Characters/Mannequin/Animations/ThirdPerson_AnimBP.uasset"
  :verify-full-roundtrip
  :verify-runtime-equivalence
  :compare-poses-over-time 1000) ;; 1000 frames
```

### 验证方法

#### 1. 结构等价性
- DSL → Blueprint → DSL 应该一致
- Blueprint → DSL → Blueprint 应该一致

#### 2. 语义等价性
```cpp
// 在相同输入下，输出姿态应该完全一致
bool VerifySemanticEquivalence(UAnimBlueprint* Original, UAnimBlueprint* Reconstructed)
{
    TArray<FTestInput> TestInputs = GenerateTestInputs();
    
    for (const FTestInput& Input : TestInputs)
    {
        FPoseContext OriginalPose = EvaluateAnimBP(Original, Input);
        FPoseContext ReconstructedPose = EvaluateAnimBP(Reconstructed, Input);
        
        if (!ComparePoses(OriginalPose, ReconstructedPose, EPSILON))
        {
            return false;
        }
    }
    
    return true;
}
```

#### 3. 性能对比
- 转换速度（Blueprint ↔ DSL）
- 运行时性能（是否有性能损失）
- 内存占用

## 项目结构

```
AnimBP2FP/
├── README.md                      # 本文档
├── Research/                      # 研究文档
│   ├── AnimBlueprintAnalysis.md  # 动画蓝图内部结构分析
│   ├── FPComparison.md           # Lisp vs Haskell 对比
│   └── TypeTheory.md             # 类型理论建模
├── DSL/                          # DSL 定义
│   ├── Grammar.ebnf              # EBNF 语法
│   ├── TypeSystem.hs             # Haskell 类型系统实现
│   ├── Examples/                 # DSL 示例
│   │   ├── simple_blend.al
│   │   ├── state_machine.al
│   │   └── third_person_char.al
│   └── Semantics.md              # 语义定义
├── Plugin/                       # UE5.6 插件
│   ├── AnimBP2FP.uplugin
│   ├── Source/
│   │   ├── AnimBP2FP/
│   │   │   ├── Private/
│   │   │   │   ├── AnimBPExporter.cpp    # Blueprint → DSL
│   │   │   │   ├── AnimBPImporter.cpp    # DSL → Blueprint
│   │   │   │   ├── DSLParser.cpp         # S-表达式解析器
│   │   │   │   ├── TypeChecker.cpp       # 类型检查
│   │   │   │   └── ASTBuilder.cpp        # AST 构建
│   │   │   └── Public/
│   │   │       ├── AnimBPExporter.h
│   │   │       ├── AnimBPImporter.h
│   │   │       ├── DSLParser.h
│   │   │       └── AnimLangAST.h         # AST 定义
│   │   └── AnimBP2FPEditor/              # 编辑器扩展
│   │       ├── AnimBPDSLEditor.cpp       # DSL 代码编辑器
│   │       └── AnimBPConverterWidget.cpp # 转换 UI
│   └── Content/
│       └── Icons/
├── Tests/                        # 测试用例
│   ├── UnitTests/
│   │   ├── DSLParserTests.cpp
│   │   ├── ExporterTests.cpp
│   │   └── ImporterTests.cpp
│   ├── IntegrationTests/
│   │   ├── RoundtripTests.cpp        # 双向转换测试
│   │   └── EquivalenceTests.cpp      # 等价性验证
│   └── TestAssets/                   # 测试用动画蓝图
│       ├── SimpleBlend.uasset
│       ├── StateMachine.uasset
│       └── ThirdPersonChar.uasset
├── Benchmarks/                   # 性能测试
│   ├── ConversionSpeed.cpp
│   └── RuntimePerformance.cpp
└── Documentation/                # 文档
    ├── API_Reference.md
    ├── DSL_Tutorial.md
    ├── Plugin_Usage.md
    └── Theory.md                 # 理论背景
```

## 技术栈

- **UE5.6 C++**: 插件实现
- **Haskell**: 类型系统原型（可选）
- **Python**: 测试脚本与数据分析
- **ANTLR** 或 **手写递归下降解析器**: DSL 解析

## 预期成果

1. **理论贡献**
   - 动画蓝图的形式化模型
   - 图形化编程与函数式编程的映射理论
   - 类型安全的动画组合系统

2. **实用工具**
   - 可用的 UE5.6 插件
   - 文本化的动画蓝图表示（便于版本控制）
   - 自动化测试工具

3. **性能优化潜力**
   - 编译期优化（通过类型推导）
   - 更好的代码复用（函数抽象）
   - 自动并行化（纯函数性）

## 挑战与风险

### 技术挑战
1. **不完全映射**：某些 UE 特性可能无法完美映射（如 Anim Notify）
2. **性能开销**：转换本身可能引入延迟
3. **UE 内部 API 变化**：依赖 UE 内部实现

### 缓解策略
1. 定义不支持特性的白名单/黑名单
2. 优化解析器与生成器
3. 使用公开 API，封装内部依赖

## 时间线

- **Week 1-2**: 研究与设计 ✅（本文档）
- **Week 3-4**: DSL 实现与解析器
- **Week 5-6**: UE 插件开发
- **Week 7-8**: 测试与验证
- **Week 9**: 文档与发布

## 参考资料

- [UE5 Animation System](https://docs.unrealengine.com/5.6/en-US/animation-system-overview/)
- [Functional Reactive Animation (FRP)](http://conal.net/papers/icfp97/)
- [The Algebra of Programming](https://www.cs.ox.ac.uk/richard.bird/books.html)
- [Haskell for Game Logic](https://wiki.haskell.org/Game_Development)
- [Lisp in Game Development](http://www.gamasutra.com/view/feature/131394/lisp_in_game_development.php)

---

**当前状态**: 🟢 Phase 1 - 研究与设计  
**预计完成**: 8-9 weeks  
**风险等级**: 🟡 中等（依赖 UE 内部 API）