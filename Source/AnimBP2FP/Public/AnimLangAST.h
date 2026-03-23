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
	Object
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
 * 动画节点 AST
 */
struct ANIMBP2FP_API FAnimNodeAST
{
	virtual ~FAnimNodeAST() = default;
	
	FString NodeType;  // "sequence-player", "blend", "state-machine", etc.
	TMap<FString, FString> Properties;
	TArray<TSharedPtr<FAnimNodeAST>> Children;
	
	virtual FString ToString(int32 Indent = 0) const;
	
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
	
	// For float/int: range
	float RangeMin = 0.0f;
	float RangeMax = 1.0f;
	
	FString ToString() const;
};

/**
 * 完整的动画蓝图 AST
 */
struct ANIMBP2FP_API FAnimGraphAST
{
	FString Name;
	TArray<FVariableDef> Variables;
	TSharedPtr<FAnimNodeAST> RootNode;
	
	// Optional: Anim Notifies
	struct FAnimNotify
	{
		FString Name;
		FString CallbackName;
	};
	TArray<FAnimNotify> AnimNotifies;
	
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