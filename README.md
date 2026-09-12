# AnimBP2FP

AnimBP2FP 是一个 Unreal Editor 插件，用 Lisp 风格的领域语言在 Animation Blueprint 与文本之间进行双向转换。动画 Pose、状态机和动画层使用 AnimLang 表达，普通 K2 逻辑复用 BlueprintLisp，Control Rig 则使用独立的 RigLang 模块。

UE5.8 及以上可额外启用 `MCP/AnimBP2FPMCP` companion plugin 提供原生 MCP Toolset；低版本继续使用现有 Python Bridge、命令行和编辑器 API。

文本结果适合交给 AI 读取和修改，也便于 Git diff、代码评审、批量处理和自动化验证。

## 当前支持的功能

### Animation Blueprint 与 AnimLang

- 将现有 `UAnimBlueprint` 导出为 `.animlang`，也可以从 AnimLang 创建或更新动画蓝图。
- 保存 Skeleton、Root Motion、变量及其容器类型、AnimGraph、Cached Pose、状态机、Animation Layer 和外部动画资产依赖。
- 为常用 Pose 节点提供紧凑的领域语法；其他动画节点可通过类路径和反射属性保存，并标记 `exact`、`reflected`、`lossy` 或 `unsupported` 覆盖等级。
- 将属性访问、变量读取和纯值表达式分别保存为绑定或 BlueprintLisp 表达式，避免把非 Pose 逻辑压成普通字符串。

### BlueprintLisp 与 RigLang

- 使用内嵌 BlueprintLisp 保存 EventGraph、FunctionGraph、Transition Rule 和 Conduit Rule 等 K2 子图。
- 将 Control Rig 的 hierarchy、变量、RigVM 图、函数和公开入口导出为独立 `.riglang` 模块。
- 通过模块 hash、类型签名和依赖顺序把 `.animlang` 与 `.riglang` 组成可检查的 workspace bundle。
- 严格 bundle 模式会先在临时资产中解析、编译和重新导出；全部验证通过后才创建并保存持久资产。

### 局部更新与验证

- 更新前比较现有资产 AST 与新 AnimLang AST，并生成结构化差异。
- 对 root AnimGraph AST 中能够映射到非 Pose 输入 Pin 默认值的属性变化，可尝试在原节点上应用增量 Patch，保留节点位置和未变化的图结构。
- 已被更新器识别的节点增删、变量、Cached Pose 定义、辅助图、逻辑图或 Root Motion 元数据等结构变化，以及增量操作失败时，会自动回退到完整重建。
- 提供 Parser、workspace lint、往返验证、Blueprint 编译检查和 canonical re-export 检查。
- 提供编辑器菜单、Commandlet 和 Python Bridge，适合交互使用或无头批处理。

局部 Patch 当前只覆盖上述 root AnimGraph 的部分 Pin 默认值变化，并不等于任意节点 UPROPERTY、绑定、连线或顶层 AnimLang 字段都能原位更新。K2 子图和 RigLang 模块拥有各自的导入与验证流程，也不应视为已经支持任意逐引脚原位 Patch。对于标记为 `lossy` 或 `unsupported` 的节点，严格导入会拒绝继续，而不会静默创建不可靠资产。

## 支持的引擎版本

| Unreal Engine | 状态 |
| --- | --- |
| UE 5.6 | 当前公开版本的支持与验证基线 |
| UE 4.27、UE 5.0-5.5 | 当前公开版本未声明支持 |
| UE 5.7 及以上 | 尚未完成公开的逐版本兼容验证，不作支持承诺 |

Animation Blueprint、Control Rig 和 RigVM API 会随 UE5 小版本变化。若要自行尝试高于 UE 5.6 的版本，应在目标工程中重新编译插件，并至少执行一次导出、临时导入、Blueprint 编译和重新导出验证；这不代表该版本已经进入正式支持范围。

## 安装

### 1. 准备依赖

AnimBP2FP 依赖：

- [BlueprintLisp](https://github.com/cc8887/Blueprint2DSL)：处理普通 K2 图。
- `Animation Data`、`Control Rig` 和 `RigVM`：Unreal Engine 自带插件，插件描述符会请求启用它们。
- `Python Editor Script Plugin`：仅在需要调用 Python Bridge 时启用。

推荐安装到项目级 `Plugins` 目录：

```text
YourProject/
└── Plugins/
    ├── AnimBP2FP/
    │   ├── AnimBP2FP.uplugin
    │   └── Source/
    └── BlueprintLisp/
        ├── BlueprintLisp.uplugin
        └── Source/
```

可以直接克隆两个公开仓库：

```powershell
cd <YourProject>\Plugins
git clone https://github.com/cc8887/AnimationBP2FP-N.git AnimBP2FP
git clone https://github.com/cc8887/Blueprint2DSL.git BlueprintLisp
```

也可以把仓库内容分别复制到上述目录。不要复制其他工作区中的 `Binaries`、`Intermediate` 或日志文件。

### 2. 启用并编译

1. 在 Unreal Editor 的 Plugins 页面确认 `Animation Data`、`Control Rig` 和 `RigVM` 已启用；需要 Python API 时再启用 `Python Editor Script Plugin`。
2. 关闭编辑器，为 C++ 项目重新生成工程文件。
3. 编译项目的 Editor target。
4. 打开项目并确认 `AnimBP2FP`、`BlueprintLisp` 和 `Control Rig` 均已加载。

若项目原本只有 Blueprint，可先通过 Unreal Editor 添加一个空 C++ 类，以生成项目构建文件。

### 3. 验证安装

安装成功后，编辑器主菜单 `Tools` 的 `AnimBP2FP` 分区会提供节点 Stub 导出、Animation Blueprint 导出和往返验证。资产导入与更新通过 Commandlet 或 Python Bridge 完成。无头导出示例：

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=AnimBP2FPExport `
  -AssetPath=/Game/Path/ABP_Example `
  -stdout -nullrhi -unattended -nop4
```

导出包含 Control Rig 依赖的 bundle：

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=AnimBP2FPExport `
  -AssetPath=/Game/Path/ABP_Example `
  -IncludeRigModules -stdout -nullrhi -unattended -nop4
```

严格导入 workspace。`-Bundle` 目录必须只包含需要导入的独立 `.animlang` / `.riglang` 模块；不要直接指向同时包含 `_all_animbp.animlang` 聚合文件的默认批量导出目录：

可以新建一个空目录，把目标资产对应的单个 `.animlang` 和它引用的 `.riglang` 文件放入其中，再将该目录传给 `-Bundle`。

```powershell
UnrealEditor-Cmd.exe "<YourProject>.uproject" `
  -run=AnimBP2FPImport `
  -Bundle="<CleanWorkspace>" `
  -OutDir=/Game/Imported `
  -stdout -nullrhi -unattended -nop4
```

## Python 快速示例

```python
import unreal

result = unreal.AnimBP2FPPythonBridge.export_anim_blueprint_to_text(
    "/Game/Path/ABP_Example.ABP_Example"
)
if not result.success:
    raise RuntimeError(result.message)

edited = result.dsl_text
update = unreal.AnimBP2FPPythonBridge.update_anim_blueprint_from_text(
    "/Game/Path/ABP_Example.ABP_Example",
    edited,
    True,
)
if not update.success:
    raise RuntimeError(update.message)
if not update.saved_package:
    raise RuntimeError("Animation Blueprint updated but package save failed")
```

推荐工作流是“导出、保留稳定 ID、局部编辑、lint、导入测试资产、编译、重新导出比较”。这种方式既能让文本 diff 聚焦于实际语义变化，也更容易命中增量更新路径。

## UE5.8 原生 MCP Toolset

UE5.8 Editor 中将 `MCP/AnimBP2FPMCP` 复制到项目的 `Plugins/AnimBP2FPMCP/`，启用该 companion plugin 以及引擎插件 **Toolset Registry** 和 **Model Context Protocol / Unreal MCP**，重新生成项目文件并编译。它会在编辑器启动时注册原生 Toolset；它只接收 Unreal 资产路径和内存中的 DSL，不开放任意本地文件读写。核心插件本身不依赖 UE5.8 MCP。

当前暴露 7 个工具：

Toolset Registry 中的完整 toolset 标识为 `AnimBP2FPMCP.AnimBP2FPToolset`，下表列出函数名后缀。若 UE5.8 的 MCP 保持默认 tool-search 模式，代理通过 `list_toolsets`、`describe_toolset`、`call_tool` 发现和调用它们；关闭 tool-search 时，完整工具名为 `AnimBP2FPMCP.AnimBP2FPToolset.<FunctionName>`。

| Tool | 权限 | 用途 |
|------|------|------|
| `ExportAnimBlueprint` | Read | 导出 AnimBlueprint 为 AnimLang |
| `ExportEventGraph` | Read | 导出 EventGraph 为 BlueprintLisp |
| `GetAnimBlueprintSyncState` | Read | 查询单个资产的 BP/DSL 映射状态 |
| `ApplyAnimBundle` | Write | 严格预检并应用内存中的 Anim/Rig bundle |
| `UpdateAnimBlueprint` | Write | 更新已有 AnimBlueprint |
| `ReplaceEventGraph` | Write | 替换 BlueprintLisp 图 |
| `MergeEventGraph` | Write | 以 MergeAppend 语义更新 BlueprintLisp 图 |

`list_anim_blueprints`、`validate_anim_blueprint_roundtrip` 和 `lint_anim_bundle` 明确不属于该 MCP toolset，不会注册到 Toolset Registry。往返验证和 lint 仍可通过现有 Commandlet/测试链路执行。

`ApplyAnimBundle` 默认严格模式，并支持 `bCommitPersistent=false` 的瞬态预检；需要持久化时再显式提交。写工具的返回值包含 `bMutationStarted`、`bSavedPackage`/`bPersisted`、变更计数和结构化诊断，调用方应据此判断成功，不应只看 RPC 是否返回。

## 配套 AI Skill

AnimBP2FP 的配套 AI Skill 收录在公开仓库 [UE-Editor-MCPServer-Skills](https://github.com/cc8887/UE-Editor-MCPServer-Skills) 中：

- Git：`https://github.com/cc8887/UE-Editor-MCPServer-Skills.git`
- Skill 直达：[plugins/animbp2fp-mcp](https://github.com/cc8887/UE-Editor-MCPServer-Skills/tree/main/plugins/animbp2fp-mcp)

该 Skill 是通过 MCP 和 `unreal.AnimBP2FPPythonBridge` 调用本插件的可选自动化层，不是 AnimBP2FP 的编译或运行依赖。插件当前支持范围和能力边界以本 README 与当前代码为准。

## 相关项目

- [BlueprintLisp](https://github.com/cc8887/Blueprint2DSL)：Blueprint K2 图与 Lisp DSL 的双向转换。
- [MatBP2FP](https://github.com/cc8887/MaterialBP2DSL)：Material 图与 MatLang 的双向转换。
