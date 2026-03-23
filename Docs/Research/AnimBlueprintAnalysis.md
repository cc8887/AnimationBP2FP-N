# Animation Blueprint Internal Structure Analysis

## 核心类层次

```cpp
// UE5.6 动画蓝图核心类
UAnimBlueprint : UBlueprint
    ├─ UAnimBlueprintGeneratedClass
    ├─ FAnimBlueprintDebugData
    └─ UAnimGraphNode_* (动画图节点)

UAnimGraphNode_Base : UEdGraphNode
    ├─ UAnimGraphNode_StateMachine
    ├─ UAnimGraphNode_SequencePlayer
    ├─ UAnimGraphNode_BlendSpacePlayer
    ├─ UAnimGraphNode_LayeredBoneBlend
    ├─ UAnimGraphNode_Slot
    └─ UAnimGraphNode_StateResult
```

## 动画图的数据结构

### 1. 编辑时表示（Editor-Time）

```cpp
class UAnimGraphNode_Base : public UEdGraphNode
{
public:
    // 引脚连接
    TArray<UEdGraphPin*> Pins;
    
    // 运行时节点
    FAnimNode_Base* RuntimeNode;
    
    // 节点属性
    FStructProperty* NodeProperties;
};
```

**关键发现**：
- 动画图是 `UEdGraph` 的实例
- 节点是 `UEdGraphNode` 的子类
- 引脚通过 `UEdGraphPin` 连接

### 2. 运行时表示（Runtime）

```cpp
// 运行时动画节点基类
struct FAnimNode_Base
{
    // 执行逻辑
    virtual void Update_AnyThread(const FAnimationUpdateContext& Context);
    virtual void Evaluate_AnyThread(FPoseContext& Output);
    
    // 初始化
    virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context);
};

// 示例：混合节点
struct FAnimNode_BlendListByBool : public FAnimNode_Base
{
    FPoseLink TruePose;
    FPoseLink FalsePose;
    bool bActiveValue;
    
    virtual void Evaluate_AnyThread(FPoseContext& Output) override
    {
        if (bActiveValue)
            TruePose.Evaluate(Output);
        else
            FalsePose.Evaluate(Output);
    }
};
```

**关键发现**：
- 运行时节点是纯 C++ 结构体
- 使用 `FPoseLink` 连接节点（函数式的延迟求值）
- `Evaluate` 方法是纯函数（给定输入 → 确定输出）

### 3. 状态机表示

```cpp
class UAnimGraphNode_StateMachine : public UAnimGraphNode_Base
{
public:
    // 状态机资产
    class UAnimationStateMachineGraph* EditorStateMachineGraph;
    
    // 运行时节点
    FAnimNode_StateMachine Node;
};

struct FAnimNode_StateMachine : public FAnimNode_Base
{
    // 状态定义
    TArray<FAnimationState> States;
    
    // 转换规则
    TArray<FAnimationTransitionBetweenStates> Transitions;
    
    // 当前状态
    int32 CurrentState;
};

struct FAnimationState
{
    // 状态名
    FName StateName;
    
    // 状态动画图（递归！）
    TArray<FAnimNode_Base*> StateNodes;
    
    // 状态结果
    FAnimNode_StateResult* ResultNode;
};

struct FAnimationTransitionBetweenStates
{
    int32 PreviousState;
    int32 NextState;
    
    // 转换条件（蓝图表达式）
    FAnimNode_TransitionResult* TransitionLogic;
    
    float CrossfadeDuration;
};
```

**关键发现**：
- 状态机本质上是一个 **有限状态自动机（FSM）**
- 每个状态包含一个子动画图（递归结构）
- 转换条件是蓝图表达式（需要转换为 DSL 表达式）

## 引脚类型系统

```cpp
// 引脚类型
enum EPinType
{
    PT_Pose,           // FPoseLink
    PT_Float,          // float
    PT_Int,            // int32
    PT_Bool,           // bool
    PT_Vector,         // FVector
    PT_Rotator,        // FRotator
    PT_Transform,      // FTransform
    PT_Name,           // FName
    PT_Object,         // UObject*
};
```

**映射到 Haskell 类型**：
```haskell
data PinType
    = TPose
    | TFloat
    | TInt
    | TBool
    | TVector
    | TRotator
    | TTransform
    | TName
    | TObject TypeId

-- 引脚连接验证
canConnect :: PinType -> PinType -> Bool
canConnect TPose TPose = True
canConnect TFloat TFloat = True
canConnect _ _ = False
```

## 关键节点类型及其函数式等价

### 1. 序列播放器（Sequence Player）

```cpp
// UE 节点
struct FAnimNode_SequencePlayer : public FAnimNode_AssetPlayerBase
{
    UAnimSequence* Sequence;
    float PlayRate;
    bool bLoopAnimation;
    float StartPosition;
};
```

```haskell
-- 函数式等价
data SequencePlayer = SequencePlayer
    { sequence    :: AnimSequence
    , playRate    :: Float
    , loopAnim    :: Bool
    , startPos    :: Float
    } deriving (Show, Eq)

evalSequence :: SequencePlayer -> Float -> Pose
evalSequence player time = sampleAnimation (sequence player) adjustedTime
  where
    adjustedTime = (time * playRate player) `mod'` duration
    duration = if loopAnim player 
               then getAnimDuration (sequence player)
               else infinity
```

### 2. 混合节点（Blend）

```cpp
// UE 节点
struct FAnimNode_BlendListByBool : public FAnimNode_Base
{
    TArray<FPoseLink> BlendPose;
    TArray<float> BlendTime;
    bool bActiveValue;
};
```

```haskell
-- 函数式等价（组合子）
blend :: Float -> Pose -> Pose -> Pose
blend alpha p1 p2 = Pose $ zipWith interpolate (bones p1) (bones p2)
  where
    interpolate b1 b2 = lerp alpha b1 b2

blendList :: Int -> [Pose] -> Pose
blendList activeIndex poses = poses !! activeIndex

-- 高阶组合子
blendBy :: (a -> Bool) -> Pose -> Pose -> a -> Pose
blendBy predicate p1 p2 value = 
    if predicate value then p1 else p2
```

### 3. 混合空间（Blend Space）

```cpp
// UE 节点
struct FAnimNode_BlendSpacePlayer : public FAnimNode_AssetPlayerBase
{
    UBlendSpace* BlendSpace;
    float X;
    float Y;
};
```

```haskell
-- 函数式等价（多维插值）
data BlendSpace = BlendSpace
    { samples :: [(Pose, (Float, Float))]  -- (pose, (x, y))
    , xRange  :: (Float, Float)
    , yRange  :: (Float, Float)
    }

evalBlendSpace2D :: BlendSpace -> (Float, Float) -> Pose
evalBlendSpace2D bs (x, y) = 
    weightedBlend $ triangulate (samples bs) (x, y)
  where
    triangulate :: [(Pose, (Float, Float))] -> (Float, Float) -> [(Pose, Float)]
    triangulate samples point = delaunayInterpolation samples point
    
    weightedBlend :: [(Pose, Float)] -> Pose
    weightedBlend weighted = foldl1 blendPoses
      [ scalePose w p | (p, w) <- weighted ]
```

### 4. 分层混合（Layered Blend）

```cpp
// UE 节点
struct FAnimNode_LayeredBoneBlend : public FAnimNode_Base
{
    FPoseLink BasePose;
    TArray<FPoseLink> BlendPoses;
    TArray<FInputBlendPose> LayerSetup;  // Bone filters
    TArray<float> BlendWeights;
};
```

```haskell
-- 函数式等价（过滤 + 组合）
data BoneFilter = BoneFilter
    { rootBone :: BoneName
    , depth    :: Int       -- -1 = all children
    , blendMode :: BlendMode
    }

data BlendMode = Replace | Blend deriving (Eq)

layeredBlend :: Pose -> [(Pose, BoneFilter, Float)] -> Pose
layeredBlend base layers = foldl applyLayer base layers
  where
    applyLayer :: Pose -> (Pose, BoneFilter, Float) -> Pose
    applyLayer basePose (overlayPose, filter, weight) =
        blendPosesWithMask basePose overlayPose (createMask filter) weight
    
    createMask :: BoneFilter -> BoneMask
    createMask filter = buildBoneMask (rootBone filter) (depth filter)
```

### 5. 状态机（State Machine）

```cpp
// UE 节点
struct FAnimNode_StateMachine : public FAnimNode_Base
{
    TArray<FAnimationState> States;
    TArray<FAnimationTransitionBetweenStates> Transitions;
    int32 CurrentState;
    float ElapsedTime;
};
```

```haskell
-- 函数式等价（代数数据类型 + 状态 Monad）
data StateMachine state input output = StateMachine
    { states      :: Map state (AnimGraph input output)
    , transitions :: [Transition state input]
    , initial     :: state
    }

data Transition state input = Transition
    { fromState :: state
    , toState   :: state
    , condition :: input -> Bool
    , blendTime :: Float
    }

-- 状态机的求值（使用 State Monad）
evalStateMachine :: StateMachine s i o -> i -> State (SMState s) o
evalStateMachine sm input = do
    current <- gets currentState
    
    -- 检查转换
    case findTransition sm current input of
        Just (nextState, blendDuration) -> do
            modify $ \s -> s { currentState = nextState
                              , transitionProgress = 0.0
                              , isTransitioning = True }
        Nothing -> return ()
    
    -- 求值当前状态
    let currentGraph = states sm ! current
    evalAnimGraph currentGraph input

-- 纯函数版本（明确传递状态）
evalStateMachinePure :: StateMachine s i o -> i -> SMState s -> (o, SMState s)
evalStateMachinePure sm input state = 
    let (nextState, transitionTime) = updateState sm input state
        graph = states sm ! (currentState nextState)
        output = runAnimGraph graph input
    in (output, nextState)
```

## 蓝图表达式的函数式表示

### UE 蓝图节点

```cpp
// Greater Than
UK2Node_CallFunction \"float > float\" : UK2Node
    Inputs: [A (float), B (float)]
    Output: ReturnValue (bool)

// Logical AND
UK2Node_CallFunction \"bool && bool\" : UK2Node
    Inputs: [A (bool), B (bool)]
    Output: ReturnValue (bool)
```

### DSL 表达式 AST

```haskell
data Expr a where
    -- 字面量
    LitFloat  :: Float -> Expr Float
    LitBool   :: Bool -> Expr Bool
    LitInt    :: Int -> Expr Int
    
    -- 参数引用
    Param     :: String -> Expr a
    
    -- 算术运算
    Add       :: Expr Float -> Expr Float -> Expr Float
    Sub       :: Expr Float -> Expr Float -> Expr Float
    Mul       :: Expr Float -> Expr Float -> Expr Float
    Div       :: Expr Float -> Expr Float -> Expr Float
    
    -- 比较运算
    Greater   :: Ord a => Expr a -> Expr a -> Expr Bool
    Less      :: Ord a => Expr a -> Expr a -> Expr Bool
    Equal     :: Eq a => Expr a -> Expr a -> Expr Bool
    
    -- 逻辑运算
    And       :: Expr Bool -> Expr Bool -> Expr Bool
    Or        :: Expr Bool -> Expr Bool -> Expr Bool
    Not       :: Expr Bool -> Expr Bool
    
    -- 条件表达式
    If        :: Expr Bool -> Expr a -> Expr a -> Expr a

-- 求值器
eval :: Expr a -> Map String Dynamic -> a
eval (LitFloat f) _    = f
eval (Param name) env  = lookupParam name env
eval (Greater a b) env = eval a env > eval b env
eval (And a b) env     = eval a env && eval b env
```

## 完整示例：第三人称角色动画蓝图

### UE 资产结构

```
ThirdPersonCharacter_AnimBP
├─ AnimGraph (Root)
│   └─ Output Pose
│       └─ Layered Blend Per Bone
│           ├─ Base Pose: State Machine (Locomotion)
│           └─ Blend Pose 0: Slot (UpperBody)
│
├─ State Machine: Locomotion
│   ├─ States
│   │   ├─ Idle
│   │   │   └─ Sequence Player: Idle_Rifle
│   │   ├─ Walk/Run
│   │   │   └─ Blend Space 2D: WalkRunBlendSpace
│   │   │       ├─ X: Speed
│   │   │       └─ Y: Direction
│   │   └─ Jump
│   │       └─ Sequence Player: Jump_Loop
│   │
│   └─ Transitions
│       ├─ Idle → Walk/Run: Speed > 10
│       ├─ Walk/Run → Idle: Speed < 10
│       └─ Any → Jump: IsInAir == true
│
└─ EventGraph (Blueprint Logic)
    ├─ Event BlueprintUpdateAnimation
    │   ├─ Get Owning Pawn
    │   ├─ Get Velocity
    │   ├─ VectorLength → Speed
    │   ├─ CalculateDirection → Direction
    │   └─ IsFalling → IsInAir
```

### 对应的 DSL 代码

```lisp
(anim-blueprint \"ThirdPersonCharacter\"
  
  ;; 输入变量（从 EventGraph 计算）
  :variables
    [(float :speed 0.0)
     (float :direction 0.0)
     (bool :is-in-air false)]
  
  ;; 主动画图
  :anim-graph
    (layered-blend-per-bone
      :base
        (state-machine :locomotion
          :initial :idle
          
          :states
            [(state :idle
               (sequence-player \"Idle_Rifle\"
                 :loop true
                 :play-rate 1.0))
             
             (state :walk-run
               (blendspace-2d \"WalkRunBlendSpace\"
                 :x :speed      ;; 引用变量
                 :y :direction
                 :loop true))
             
             (state :jump
               (sequence-player \"Jump_Loop\"
                 :loop true))]
          
          :transitions
            [(transition :idle :walk-run
               :condition (> :speed 10.0)
               :duration 0.2
               :interrupt-on-condition true)
             
             (transition :walk-run :idle
               :condition (< :speed 10.0)
               :duration 0.3)
             
             (transition :any :jump
               :condition :is-in-air
               :duration 0.1)])
      
      :layers
        [(blend-layer (slot \"UpperBody\")
           :bone-filter \"spine_01\"
           :blend-depth -1)]))  ;; All children
```

## 转换算法伪代码

### Blueprint → DSL

```python
def export_anim_blueprint(anim_bp: UAnimBlueprint) -> str:
    # 1. 提取变量
    variables = extract_variables(anim_bp)
    
    # 2. 遍历 AnimGraph
    root_node = find_output_node(anim_bp.FunctionGraphs['AnimGraph'])
    anim_tree = traverse_graph(root_node)
    
    # 3. 转换为 DSL AST
    ast = AnimGraphAST(
        name=anim_bp.GetName(),
        variables=variables,
        graph=anim_tree
    )
    
    # 4. Pretty-print
    return ast.to_sexp()

def traverse_graph(node: UAnimGraphNode) -> ASTNode:
    if isinstance(node, UAnimGraphNode_StateMachine):
        return convert_state_machine(node)
    elif isinstance(node, UAnimGraphNode_SequencePlayer):
        return convert_sequence_player(node)
    elif isinstance(node, UAnimGraphNode_BlendListByBool):
        return convert_blend(node)
    # ... 其他节点类型
```

### DSL → Blueprint

```python
def import_anim_blueprint(dsl_code: str) -> UAnimBlueprint:
    # 1. 解析 DSL
    ast = parse_sexp(dsl_code)
    type_check(ast)
    
    # 2. 创建空白蓝图
    anim_bp = create_empty_anim_blueprint(ast.name)
    
    # 3. 创建变量
    for var in ast.variables:
        add_variable(anim_bp, var.name, var.type, var.default)
    
    # 4. 构建 AnimGraph
    graph = anim_bp.FunctionGraphs['AnimGraph']
    output_node = find_output_node(graph)
    
    root_anim_node = build_anim_nodes(ast.graph, graph)
    connect_pins(root_anim_node.get_pin('Pose'), output_node.get_pin('Result'))
    
    # 5. 编译
    compile_blueprint(anim_bp)
    
    return anim_bp

def build_anim_nodes(ast_node: ASTNode, graph: UAnimGraph) -> UAnimGraphNode:
    if ast_node.type == 'state-machine':
        return build_state_machine(ast_node, graph)
    elif ast_node.type == 'sequence-player':
        return build_sequence_player(ast_node, graph)
    # ... 递归构建
```

## 类型安全性保证

### 引脚类型检查

```haskell
-- 类型检查器
typeCheck :: AnimGraph -> Either TypeError AnimGraph
typeCheck graph = do
    let nodes = getAllNodes graph
    forM_ nodes $ \node -> do
        forM_ (inputPins node) $ \pin -> do
            case connectedPin pin of
                Just srcPin -> 
                    unless (pinType srcPin `isCompatible` pinType pin) $
                        Left $ TypeMismatch (pinType srcPin) (pinType pin)
                Nothing -> 
                    unless (hasDefaultValue pin || isOptional pin) $
                        Left $ MissingConnection pin
    return graph

isCompatible :: PinType -> PinType -> Bool
isCompatible TPose TPose = True
isCompatible TFloat TFloat = True
isCompatible TInt TFloat = True  -- 隐式转换
isCompatible _ _ = False
```

## 总结：关键映射

| UE 概念 | 函数式概念 | Haskell 类型 |
|---------|------------|--------------|
| AnimGraph | 数据流图 | `AnimNode Pose` |
| FPoseLink | 惰性求值 | `() -> Pose` |
| State Machine | 有限状态自动机 | `State s m` |
| Blend Node | 组合子 | `Pose -> Pose -> Pose` |
| Transition Condition | 谓词函数 | `Input -> Bool` |
| Animation Sequence | 时间索引函数 | `Float -> Pose` |
| Bone Filter | 集合谓词 | `BoneName -> Bool` |

**核心洞察**：
1. 动画蓝图本质上是 **纯函数式的数据流图**
2. 状态机可以用 **代数数据类型 + State Monad** 建模
3. 所有节点都是 **组合子**（可组合的函数）
4. 引脚类型系统可以用 **GADT** 实现完全类型安全

**下一步**：设计具体的 DSL 语法和解析器