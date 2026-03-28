// AnimLangDiagnostics.h - Unified Error Diagnostics System
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Diagnostic severity levels
 */
enum class EAnimLangDiagSeverity : uint8
{
	Error,     // Fatal — prevents compilation/import
	Warning,   // Non-fatal — may produce incorrect results
	Info,      // Informational — performance or style hints
	Hint       // Suggestion — optional improvement
};

/**
 * Diagnostic category
 */
enum class EAnimLangDiagCategory : uint8
{
	Lex,       // Tokenizer error
	Parse,     // Parser error
	Type,      // Type mismatch
	Semantic,  // Semantic error (undefined ref, duplicate define, etc.)
	Import,    // Import/reconstruction error
	RoundTrip  // Round-trip validation issue
};

/**
 * Source location
 */
struct FAnimLangSourceLoc
{
	int32 Line = 0;
	int32 Column = 0;
	int32 Offset = 0;       // Character offset in source
	int32 Length = 0;        // Length of the problematic span (0 if unknown)
	FString SourceFile;      // Optional: which .animlang file
	
	FString ToString() const
	{
		if (SourceFile.IsEmpty())
		{
			return FString::Printf(TEXT("%d:%d"), Line, Column);
		}
		return FString::Printf(TEXT("%s:%d:%d"), *SourceFile, Line, Column);
	}
};

/**
 * A single diagnostic message
 */
struct ANIMBP2FP_API FAnimLangDiagnostic
{
	EAnimLangDiagSeverity Severity;
	EAnimLangDiagCategory Category;
	FString Message;
	FAnimLangSourceLoc Location;
	
	// Optional: related locations (e.g., "first defined here")
	TArray<FAnimLangSourceLoc> RelatedLocations;
	TArray<FString> RelatedMessages;
	
	// Optional: fix suggestion
	FString FixSuggestion;
	
	FString ToString() const;
	FString ToCompactString() const;  // "file:line:col: error: message"
	
	// Convenience constructors
	static FAnimLangDiagnostic LexError(const FString& Msg, int32 Line, int32 Col);
	static FAnimLangDiagnostic ParseError(const FString& Msg, int32 Line, int32 Col);
	static FAnimLangDiagnostic TypeError(const FString& Msg, int32 Line, int32 Col);
	static FAnimLangDiagnostic SemanticError(const FString& Msg, int32 Line, int32 Col);
	static FAnimLangDiagnostic SemanticWarning(const FString& Msg, int32 Line, int32 Col);
};

/**
 * Diagnostics collection
 */
struct ANIMBP2FP_API FAnimLangDiagnostics
{
	TArray<FAnimLangDiagnostic> Items;
	
	void Add(const FAnimLangDiagnostic& Diag) { Items.Add(Diag); }
	void Add(EAnimLangDiagSeverity Sev, EAnimLangDiagCategory Cat, const FString& Msg, int32 Line = 0, int32 Col = 0);
	
	bool HasErrors() const;
	bool HasWarnings() const;
	int32 ErrorCount() const;
	int32 WarningCount() const;
	
	FString ToReport() const;
	
	// Filter
	TArray<FAnimLangDiagnostic> GetErrors() const;
	TArray<FAnimLangDiagnostic> GetByCategory(EAnimLangDiagCategory Cat) const;
};

/**
 * AnimLang Semantic Analyzer
 * 
 * Post-parse validation that checks:
 *   1. All (ref "...") targets are resolvable
 *   2. All UseCachedPose references have matching (define ...) bindings
 *   3. No duplicate define names
 *   4. No circular define dependencies
 *   5. State machine consistency (initial state exists, transitions reference valid states)
 *   6. Variable usage consistency
 */
class ANIMBP2FP_API FAnimLangSemanticAnalyzer
{
public:
	/**
	 * Run semantic analysis on a parsed AST
	 * @param AST  The parsed AST to validate
	 * @param OutDiag  Receives diagnostic messages
	 */
	static void Analyze(const TSharedPtr<struct FAnimGraphAST>& AST, FAnimLangDiagnostics& OutDiag);

private:
	// Collect all defined names (from (define ...) blocks)
	static TSet<FString> CollectDefineNames(const TSharedPtr<struct FAnimGraphAST>& AST);
	
	// Collect all variable names
	static TSet<FString> CollectVariableNames(const TSharedPtr<struct FAnimGraphAST>& AST);
	
	// Check a node subtree for unresolved references
	static void CheckNodeRefs(
		const TSharedPtr<struct FAnimNodeAST>& Node,
		const TSet<FString>& DefineNames,
		const TSet<FString>& VariableNames,
		const FString& Path,
		FAnimLangDiagnostics& OutDiag);
	
	// Check state machine consistency
	static void CheckStateMachine(
		const TSharedPtr<struct FAnimNodeAST>& Node,
		const FString& Path,
		FAnimLangDiagnostics& OutDiag);
	
	// Check for circular define dependencies
	static void CheckCircularDefines(
		const TSharedPtr<struct FAnimGraphAST>& AST,
		FAnimLangDiagnostics& OutDiag);
};
