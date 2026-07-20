// AnimLangAST.h - Abstract Syntax Tree for AnimLang DSL
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimationAsset.h"

/**
 * Pin type枚举 - 对应 UE 的引脚类型
 */
enum class EPinType : uint8
{
	Pose,
	Float,
	Int,
	Bool,
	Vector,
	Rotator,
	Transform,
	Name,
	Enum,
	Object,
	Struct,
	Unknown
};

/**
 * 表达式 AST（条件、参数引用等）
 */
struct ANIMBP2FP_API FExpressionAST
{
	virtual ~FExpressionAST() = default;
	virtual FString ToString() const = 0;
};

struct FLiteralExpr : public FExpressionAST
{
	enum class EType { Float, Int, Bool, String };
	EType Type;
	FString Value;
	
	virtual FString ToString() const override { return Value; }
};

struct FParamExpr : public FExpressionAST
{
	FString ParamName;
	virtual FString ToString() const override { return FString::Printf(TEXT(":%s"), *ParamName); }
};

struct FBinaryExpr : public FExpressionAST
{
	FString Operator;  // ">", "<", "+", "-", "*", "/"
	TSharedPtr<FExpressionAST> Left;
	TSharedPtr<FExpressionAST> Right;
	
	virtual FString ToString() const override
	{
		return FString::Printf(TEXT("(%s %s %s)"), *Operator, *Left->ToString(), *Right->ToString());
	}
};

struct FLogicalExpr : public FExpressionAST
{
	FString Operator;  // "and", "or", "not"
	TArray<TSharedPtr<FExpressionAST>> Operands;
	
	virtual FString ToString() const override;
};

/**
 * 命名子节点 - 保留 pin 名称以区分多个动画数据输入
 */
struct FNamedChild
{
	FString PinName;  // e.g. "base-pose", "blend-pose-0", "additive", "a", "b"
	TSharedPtr<struct FAnimNodeAST> Node;
};

enum class EAnimNodeCoverage : uint8
{
	Exact,
	Reflected,
	Lossy,
	Unsupported
};

/**
 * 动画节点 AST
 */
struct ANIMBP2FP_API FAnimNodeAST
{
	virtual ~FAnimNodeAST() = default;
	
	FString NodeType;  // "sequence-player", "blend", "state-machine", etc.
	FString NodeId;    // Stable ID for incremental update (maps to UE NodeGuid)
	FString NodeClassPath;  // Exact editor-node UClass path used by reflected fallback import
	EAnimNodeCoverage Coverage = EAnimNodeCoverage::Exact;
	TMap<FString, FString> Properties;  // Non-pose parameters (float, bool, int, enum, etc.)
	TArray<FNamedChild> Children;  // Pose inputs with pin names
	
	virtual FString ToString(int32 Indent = 0) const;
	
	// Helper to add a named child
	void AddChild(const FString& PinName, TSharedPtr<FAnimNodeAST> ChildNode);
	
	// Helper to add an unnamed child (auto-numbered)
	void AddChild(TSharedPtr<FAnimNodeAST> ChildNode);
	
	// Helper: Get property as float
	float GetFloatProperty(const FString& Key, float Default = 0.0f) const;
	bool GetBoolProperty(const FString& Key, bool Default = false) const;
	FString GetStringProperty(const FString& Key, const FString& Default = TEXT("")) const;
};

/**
 * 状态机 AST
 */
struct ANIMBP2FP_API FStateMachineAST
{
	FString Name;
	FString InitialState;
	
	struct FState
	{
		FString Name;
		TSharedPtr<FAnimNodeAST> Animation;
		
		// Optional: 完成后自动转换
		FString TransitionTo;
		bool bOnFinish = false;
	};
	
	struct FTransition
	{
		FString FromState;  // "any" for wildcard
		FString ToState;
		TSharedPtr<FExpressionAST> Condition;
		FString RuleGraph;          // BlueprintLisp DSL of the transition condition graph (for restore)
		float BlendDuration = 0.2f;
		bool bInterruptible = false;
		TArray<FString> FromStates;  // For "any" transitions
		int32 Priority = 0;
	};
	
	TArray<FState> States;
	TArray<FTransition> Transitions;
	
	FString ToString(int32 Indent = 0) const;
};

/**
 * 变量定义
 */
struct ANIMBP2FP_API FVariableDef
{
	FString Name;
	EPinType Type;
	FString DefaultValue;
	FString Description;
	FString TypeObjectPath;  // For enum/object-like variables that need a concrete asset/class path
	FString PinCategory;     // Exact UE FEdGraphPinType category (authoritative when non-empty)
	FString PinSubCategory;
	FString ContainerType;   // none, array, set, or map
	bool bIsReference = false;
	bool bIsConst = false;
	bool bIsWeakPointer = false;
	bool bIsUObjectWrapper = false;
	
	// For float/int: range
	float RangeMin = 0.0f;
	float RangeMax = 1.0f;
	
	FString ToString() const;
};

/**
 * Cached pose 定义 — 对应 Lisp 的 (define name body)
 * SaveCachedPose 提升为顶层绑定，UseCachedPose 退化为变量引用
 */
struct ANIMBP2FP_API FCachedPoseDef
{
	FString Name;  // 缓存名（作为变量名，空格替换为连字符）
	TSharedPtr<FAnimNodeAST> Body;  // 绑定的子树
	
	/** 将名称转为合法的 DSL 标识符 (kebab-case, 无空格) */
	FString GetIdentifier() const;
};

/**
 * Helper graph 定义 — 承载复杂值绑定的 BlueprintLisp 子图
 */
struct ANIMBP2FP_API FHelperGraphDef
{
	FString Id;            // Stable helper id referenced by (subgraph-ref ...)
	FString GraphName;     // Actual generated Blueprint graph/function name
	FString GeneratedVar;  // Generated member variable used as the PoseGraph bridge
	EPinType GeneratedType = EPinType::Float;
	FString UpdateGroup;   // Managed update entry/group name
	FString DSL;           // BlueprintLisp helper graph DSL body

	FString ToString(int32 Indent = 0) const;
};

/** Ordinary Blueprint logic graph represented by BlueprintLisp. */
struct ANIMBP2FP_API FLogicGraphDef
{
	FString Role;             // event or function
	FString Kind;             // ubergraph or function
	FString GraphName;
	FString SchemaClassPath;  // Optional graph schema class path
	FString DSL;

	FString ToString(int32 Indent = 0) const;
};

struct ANIMBP2FP_API FAnimNotifySnapshot
{
	FString ClassPath;
	FString Name;
	float Time = 0.0f;
	float Duration = 0.0f;
	bool bIsState = false;
};

struct ANIMBP2FP_API FAnimSyncMarkerSnapshot
{
	FString Name;
	float Time = 0.0f;
};

struct ANIMBP2FP_API FMontageSectionSnapshot
{
	FString Name;
	float StartTime = 0.0f;
	FString NextSectionName;
};

/** Read-only description of an externally-owned animation asset. */
struct ANIMBP2FP_API FAnimationAssetMetadataSnapshot
{
	bool bHasSnapshot = false;
	bool bHasRootMotion = false;
	bool bEnableRootMotion = false;
	bool bForceRootLock = false;
	FString RootMotionRootLock;
	TArray<FAnimNotifySnapshot> Notifies;
	TArray<FAnimSyncMarkerSnapshot> SyncMarkers;
	TArray<FMontageSectionSnapshot> MontageSections;
	TArray<FString> SlotTrackNames;
	TArray<FString> UnsupportedFields;
};

struct ANIMBP2FP_API FAnimDependency
{
	FString ObjectPath;
	FString ClassPath;
	FString Role;
	FString Mode = TEXT("external");
	FAnimationAssetMetadataSnapshot AssetMetadata;

	FString ToString(int32 Indent = 0) const;
};

struct ANIMBP2FP_API FAnimBlueprintMetadata
{
	FString RootMotionMode;
};

/**
 * 完整的动画蓝图 AST
 */
struct ANIMBP2FP_API FAnimGraphAST
{
	FString Name;
	FString SkeletonPath;       // Target skeleton asset path (e.g. "/Game/Mannequin/Skeleton")
	FAnimBlueprintMetadata Metadata;
	TArray<FAnimDependency> Dependencies;
	TArray<FString> ImplementedInterfaces;  // Asset paths of AnimLayerInterfaces implemented by the BP
	TArray<FVariableDef> Variables;
	TArray<FHelperGraphDef> HelperGraphs;  // (helpers ...) 块 — BlueprintLisp helper subgraphs for complex value bindings
	TArray<FLogicGraphDef> LogicGraphs;    // Ordinary EventGraph/function graphs exported as BlueprintLisp
	bool bHasLogicGraphsBlock = false;     // Distinguishes legacy DSL from an explicit empty replacement set
	TArray<FCachedPoseDef> Defines;  // (define ...) 块 — SaveCachedPose 节点
	TSharedPtr<FAnimNodeAST> RootNode;

	FString ToString() const;
	
	// S-expression output
	FString ToSExpression(bool bPrettyPrint = true, int32 IndentSize = 2) const;
};

/**
 * 类型检查器
 */
class ANIMBP2FP_API FTypeChecker
{
public:
	struct FTypeError
	{
		FString Message;
		FString NodeName;
		int32 Line = -1;
	};
	
	/**
	 * Type-check an AST
	 * @param AST The AST to check
	 * @param OutErrors Array to receive errors
	 * @return true if type-safe
	 */
	static bool Check(const TSharedPtr<FAnimGraphAST>& AST, TArray<FTypeError>& OutErrors);
	
private:
	static EPinType GetNodeOutputType(const FString& NodeType);
	static TArray<EPinType> GetNodeInputTypes(const FString& NodeType);
	static bool IsCompatible(EPinType From, EPinType To);
};
