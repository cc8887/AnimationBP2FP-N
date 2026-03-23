# Testing Strategy for AnimBP2FP

## 测试目标

1. **正确性**：转换不丢失信息
2. **完整性**：覆盖所有节点类型
3. **等价性**：输出姿态一致
4. **性能**：转换速度和运行时开销

## 测试金字塔

```
               ┌─────────────┐
               │  Manual E2E │  (5%)
               │   Testing   │
               └─────────────┘
              ┌───────────────┐
              │  Integration  │  (20%)
              │     Tests     │
              └───────────────┘
            ┌───────────────────┐
            │    Unit Tests     │  (75%)
            └───────────────────┘
```

## 1. 单元测试（Unit Tests）

### 1.1 DSL 解析器测试

```cpp
// Tests/UnitTests/DSLParserTests.cpp

TEST_CASE("Parse Simple Animation", "[DSLParser]")
{
    std::string input = R"(
        (anim-blueprint "Test"
          :anim-graph
            (sequence-player "Idle" :loop true))
    )";
    
    auto ast = DSLParser::Parse(input);
    
    REQUIRE(ast.Name == "Test");
    REQUIRE(ast.Graph.Type == NodeType::SequencePlayer);
    REQUIRE(ast.Graph.GetProperty("animation") == "Idle");
    REQUIRE(ast.Graph.GetProperty("loop") == true);
}

TEST_CASE("Parse Blend Node", "[DSLParser]")
{
    std::string input = R"(
        (blend 0.5
          (sequence-player "A")
          (sequence-player "B"))
    )";
    
    auto ast = DSLParser::Parse(input);
    
    REQUIRE(ast.Type == NodeType::Blend);
    REQUIRE(ast.Children.size() == 2);
    REQUIRE(ast.GetProperty("alpha") == 0.5f);
}

TEST_CASE("Parse State Machine", "[DSLParser]")
{
    std::string input = R"(
        (state-machine :locomotion
          :initial :idle
          :states [(state :idle (anim "Idle"))]
          :transitions [(transition :idle :walk (> :speed 10))])
    )";
    
    auto ast = DSLParser::Parse(input);
    
    REQUIRE(ast.Type == NodeType::StateMachine);
    REQUIRE(ast.StateMachine.States.size() == 1);
    REQUIRE(ast.StateMachine.Transitions.size() == 1);
}
```

### 1.2 类型检查器测试

```cpp
// Tests/UnitTests/TypeCheckerTests.cpp

TEST_CASE("Valid Pose Connection", "[TypeChecker]")
{
    AnimGraphAST graph;
    // 构建: SequencePlayer -> Blend
    auto seq = CreateSequencePlayerNode("Idle");
    auto blend = CreateBlendNode(0.5f);
    ConnectNodes(seq, blend, 0);
    
    REQUIRE_NOTHROW(TypeChecker::Check(graph));
}

TEST_CASE("Invalid Type Connection", "[TypeChecker]")
{
    AnimGraphAST graph;
    // 尝试连接 Float -> Pose (应该失败)
    auto floatParam = CreateParameterNode("speed", PinType::Float);
    auto blend = CreateBlendNode(0.5f);
    ConnectNodes(floatParam, blend, 0);  // Blend 期望 Pose 输入
    
    REQUIRE_THROWS_AS(TypeChecker::Check(graph), TypeMismatchError);
}
```

### 1.3 表达式求值测试

```cpp
// Tests/UnitTests/ExpressionTests.cpp

TEST_CASE("Evaluate Comparison", "[Expression]")
{
    // (> :speed 10.0)
    auto expr = std::make_shared<GreaterExpr>(
        std::make_shared<ParamExpr>("speed"),
        std::make_shared<LiteralExpr>(10.0f)
    );
    
    std::map<std::string, float> params = {{"speed", 50.0f}};
    
    REQUIRE(expr->Evaluate(params) == true);
}

TEST_CASE("Evaluate Logical AND", "[Expression]")
{
    // (and (> :speed 10) (< :speed 100))
    auto expr = std::make_shared<AndExpr>(
        std::make_shared<GreaterExpr>(
            std::make_shared<ParamExpr>("speed"),
            std::make_shared<LiteralExpr>(10.0f)),
        std::make_shared<LessExpr>(
            std::make_shared<ParamExpr>("speed"),
            std::make_shared<LiteralExpr>(100.0f))
    );
    
    REQUIRE(expr->Evaluate({{"speed", 50.0f}}) == true);
    REQUIRE(expr->Evaluate({{"speed", 5.0f}}) == false);
}
```

## 2. 集成测试（Integration Tests）

### 2.1 往返测试（Roundtrip Tests）

```cpp
// Tests/IntegrationTests/RoundtripTests.cpp

TEST_CASE("Simple Blend Roundtrip", "[Roundtrip]")
{
    // 1. 加载测试蓝图
    UAnimBlueprint* OriginalBP = LoadObject<UAnimBlueprint>(
        nullptr, TEXT("/Game/Tests/SimpleBlend_BP.SimpleBlend_BP"));
    
    // 2. 导出为 DSL
    std::string DSLCode = AnimBPExporter::Export(OriginalBP);
    
    // 3. 从 DSL 重建
    UAnimBlueprint* ReconstructedBP = AnimBPImporter::Import(DSLCode, "Test_Reconstructed");
    
    // 4. 再次导出
    std::string DSLCode2 = AnimBPExporter::Export(ReconstructedBP);
    
    // 5. 验证 DSL 代码一致
    REQUIRE(DSLCode == DSLCode2);
}

TEST_CASE("State Machine Roundtrip", "[Roundtrip]")
{
    UAnimBlueprint* OriginalBP = LoadTestBlueprint("StateMachine_BP");
    
    std::string DSL1 = AnimBPExporter::Export(OriginalBP);
    UAnimBlueprint* ReconstructedBP = AnimBPImporter::Import(DSL1);
    std::string DSL2 = AnimBPExporter::Export(ReconstructedBP);
    
    // 验证状态数量
    REQUIRE(CountStates(OriginalBP) == CountStates(ReconstructedBP));
    
    // 验证转换数量
    REQUIRE(CountTransitions(OriginalBP) == CountTransitions(ReconstructedBP));
    
    // 验证 DSL 等价
    REQUIRE(DSL1 == DSL2);
}
```

### 2.2 姿态等价性测试

```cpp
// Tests/IntegrationTests/EquivalenceTests.cpp

TEST_CASE("Pose Output Equivalence - Blend", "[Equivalence]")
{
    UAnimBlueprint* OriginalBP = LoadTestBlueprint("SimpleBlend_BP");
    
    std::string DSLCode = AnimBPExporter::Export(OriginalBP);
    UAnimBlueprint* ReconstructedBP = AnimBPImporter::Import(DSLCode);
    
    // 生成测试输入
    TArray<FTestInput> TestInputs = GenerateTestInputs(100);
    
    for (const FTestInput& Input : TestInputs)
    {
        FPoseContext OriginalPose = EvaluateAnimBP(OriginalBP, Input);
        FPoseContext ReconstructedPose = EvaluateAnimBP(ReconstructedBP, Input);
        
        // 比较每根骨骼的变换
        for (int32 BoneIndex = 0; BoneIndex < OriginalPose.Pose.GetNumBones(); ++BoneIndex)
        {
            FTransform OriginalTransform = OriginalPose.Pose[BoneIndex];
            FTransform ReconstructedTransform = ReconstructedPose.Pose[BoneIndex];
            
            REQUIRE(OriginalTransform.Equals(ReconstructedTransform, KINDA_SMALL_NUMBER));
        }
    }
}

TEST_CASE("State Transition Equivalence", "[Equivalence]")
{
    UAnimBlueprint* OriginalBP = LoadTestBlueprint("StateMachine_BP");
    UAnimBlueprint* ReconstructedBP = ImportExport(OriginalBP);
    
    // 模拟状态转换
    SimulateInput input;
    input.speed = 0.0f;
    
    for (int frame = 0; frame < 1000; ++frame)
    {
        // 逐渐增加速度
        input.speed += 0.5f;
        
        int32 OriginalState = GetCurrentState(OriginalBP, input);
        int32 ReconstructedState = GetCurrentState(ReconstructedBP, input);
        
        REQUIRE(OriginalState == ReconstructedState);
    }
}
```

## 3. 端到端测试（E2E Tests）

### 3.1 真实角色测试

```cpp
// Tests/E2E/RealWorldTests.cpp

TEST_CASE("Third Person Character - Full Gameplay", "[E2E][Manual]")
{
    // 1. 加载第三人称角色蓝图
    UAnimBlueprint* ThirdPersonBP = LoadObject<UAnimBlueprint>(
        nullptr, TEXT("/Game/ThirdPersonCharacter_AnimBP"));
    
    // 2. 导出并重建
    std::string DSL = AnimBPExporter::Export(ThirdPersonBP);
    UAnimBlueprint* ReconstructedBP = AnimBPImporter::Import(DSL);
    
    // 3. 在实际游戏中测试
    UWorld* World = CreateTestWorld();
    ACharacter* OriginalChar = SpawnCharacterWithAnimBP(World, ThirdPersonBP);
    ACharacter* ReconstructedChar = SpawnCharacterWithAnimBP(World, ReconstructedBP);
    
    // 4. 模拟完整游戏循环（10秒）
    TArray<FPoseSnapshot> OriginalPoses;
    TArray<FPoseSnapshot> ReconstructedPoses;
    
    for (float time = 0.0f; time < 10.0f; time += GetWorld()->GetDeltaSeconds())
    {
        // 相同输入
        FVector2D Input = GetSimulatedInput(time);
        OriginalChar->AddMovementInput(FVector(Input.X, Input.Y, 0), 1.0f);
        ReconstructedChar->AddMovementInput(FVector(Input.X, Input.Y, 0), 1.0f);
        
        // Tick
        World->Tick(LEVELTICK_All, GetWorld()->GetDeltaSeconds());
        
        // 捕获姿态
        OriginalPoses.Add(CapturePose(OriginalChar));
        ReconstructedPoses.Add(CapturePose(ReconstructedChar));
    }
    
    // 5. 比较姿态序列
    float TotalError = 0.0f;
    for (int i = 0; i < OriginalPoses.Num(); ++i)
    {
        float Error = ComparePoses(OriginalPoses[i], ReconstructedPoses[i]);
        TotalError += Error;
        
        // 允许微小误差（浮点精度）
        REQUIRE(Error < 0.01f);
    }
    
    float AvgError = TotalError / OriginalPoses.Num();
    UE_LOG(LogTemp, Log, TEXT("Average pose error: %f"), AvgError);
    REQUIRE(AvgError < 0.001f);
}
```

### 3.2 性能回归测试

```cpp
// Tests/E2E/PerformanceTests.cpp

TEST_CASE("Conversion Performance", "[Performance]")
{
    UAnimBlueprint* ComplexBP = LoadTestBlueprint("ComplexCharacter_BP");
    
    // 导出性能
    auto start = std::chrono::high_resolution_clock::now();
    std::string DSL = AnimBPExporter::Export(ComplexBP);
    auto end = std::chrono::high_resolution_clock::now();
    auto exportDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    REQUIRE(exportDuration.count() < 100);  // 应在 100ms 内完成
    
    // 导入性能
    start = std::chrono::high_resolution_clock::now();
    UAnimBlueprint* ReconstructedBP = AnimBPImporter::Import(DSL);
    end = std::chrono::high_resolution_clock::now();
    auto importDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    REQUIRE(importDuration.count() < 500);  // 应在 500ms 内完成
}

TEST_CASE("Runtime Performance - No Overhead", "[Performance]")
{
    UAnimBlueprint* OriginalBP = LoadTestBlueprint("ThirdPersonCharacter_BP");
    UAnimBlueprint* ReconstructedBP = ImportExport(OriginalBP);
    
    // 预热
    for (int i = 0; i < 100; ++i)
    {
        EvaluateAnimBP(OriginalBP, GenerateRandomInput());
        EvaluateAnimBP(ReconstructedBP, GenerateRandomInput());
    }
    
    // 测量原始蓝图性能
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i)
    {
        EvaluateAnimBP(OriginalBP, GenerateRandomInput());
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto originalDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // 测量重建蓝图性能
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i)
    {
        EvaluateAnimBP(ReconstructedBP, GenerateRandomInput());
    }
    end = std::chrono::high_resolution_clock::now();
    auto reconstructedDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // 性能差异应在 5% 以内
    float performanceRatio = static_cast<float>(reconstructedDuration.count()) / originalDuration.count();
    REQUIRE(performanceRatio < 1.05f);
    
    UE_LOG(LogTemp, Log, TEXT("Original: %lld µs"), originalDuration.count());
    UE_LOG(LogTemp, Log, TEXT("Reconstructed: %lld µs"), reconstructedDuration.count());
}
```

## 4. 测试数据生成

### 4.1 测试输入生成器

```cpp
// Tests/Utilities/TestInputGenerator.h

class FTestInputGenerator
{
public:
    // 生成随机测试输入
    static TArray<FTestInput> GenerateRandom(int32 Count);
    
    // 生成边界测试用例
    static TArray<FTestInput> GenerateBoundary();
    
    // 生成真实游戏场景
    static TArray<FTestInput> GenerateGameplayScenario(const FString& ScenarioName);
};

// 实现
TArray<FTestInput> FTestInputGenerator::GenerateBoundary()
{
    return {
        // 最小值
        {.speed = 0.0f, .direction = -180.0f, .isInAir = false},
        
        // 最大值
        {.speed = 600.0f, .direction = 180.0f, .isInAir = true},
        
        // 中间值
        {.speed = 300.0f, .direction = 0.0f, .isInAir = false},
        
        // 转换边界
        {.speed = 10.0f, .direction = 0.0f, .isInAir = false},  // Idle/Walk threshold
        {.speed = 300.0f, .direction = 0.0f, .isInAir = false}, // Walk/Run threshold
    };
}
```

## 5. 持续集成（CI）

### GitHub Actions 配置

```yaml
# .github/workflows/test.yml

name: AnimBP2FP Tests

on: [push, pull_request]

jobs:
  unit-tests:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Setup UE5.6
        run: # ...
      - name: Build Plugin
        run: # ...
      - name: Run Unit Tests
        run: |
          cd Tests
          ./RunTests.bat --filter="[DSLParser][TypeChecker][Expression]"
  
  integration-tests:
    runs-on: windows-latest
    needs: unit-tests
    steps:
      - name: Run Integration Tests
        run: |
          ./RunTests.bat --filter="[Roundtrip][Equivalence]"
  
  performance-regression:
    runs-on: windows-latest
    needs: integration-tests
    steps:
      - name: Run Performance Tests
        run: |
          ./RunTests.bat --filter="[Performance]"
      - name: Upload Benchmark Results
        uses: actions/upload-artifact@v3
        with:
          name: performance-report
          path: benchmark_results.json
```

## 6. 测试覆盖率目标

| 类型 | 目标覆盖率 |
|------|-----------|
| 单元测试 | 90% |
| 集成测试 | 80% |
| E2E 测试 | 关键路径 100% |

## 总结

测试策略遵循以下原则：

1. **快速反馈**：单元测试秒级完成
2. **高置信度**：集成测试验证往返一致性
3. **真实场景**：E2E 测试模拟实际游戏
4. **性能保证**：无运行时开销
5. **自动化**：CI/CD 全流程覆盖