// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimLangDiagnostics.h"
#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"
#include "AnimLispModule.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispModuleIdentityTest,
	"AnimBP2FP.AnimLisp.Module.Identity",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimLispModuleIdentityTest::RunTest(const FString& Parameters)
{
	const FAnimLispModuleId Id = FAnimLispModuleId::FromAssetPath(
		TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement"),
		EAnimLispModuleKind::Rig);

	TestEqual(
		TEXT("Canonical identity"),
		Id.ToString(),
		FString(TEXT("rig:/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement")));

	const FAnimLispModuleId NormalizedId = FAnimLispModuleId::FromAssetPath(
		TEXT("  Game\\Blueprints\\ControlRigs\\CR_Biped_FootPlacement///  "),
		EAnimLispModuleKind::Rig);
	TestTrue(TEXT("Equivalent paths normalize to equal identities"), Id == NormalizedId);
	TestEqual(TEXT("Equivalent identities have equal hashes"), GetTypeHash(Id), GetTypeHash(NormalizedId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispTypeIdentityTest,
	"AnimBP2FP.AnimLisp.Module.TypeIdentity",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimLispTypeIdentityTest::RunTest(const FString& Parameters)
{
	FAnimLispTypeRef Type;
	Type.CPPType = TEXT("FVector");
	Type.CPPTypeObject = TEXT("/Script/CoreUObject.Vector");
	Type.ContainerType = TEXT("none");

	const FAnimLispTypeRef EqualType = Type;
	TestTrue(TEXT("All exact type fields equal"), Type == EqualType);
	TestFalse(TEXT("Equal exact types are not unequal"), Type != EqualType);

	FAnimLispTypeRef DifferentCPPType = Type;
	DifferentCPPType.CPPType = TEXT("FVector3f");
	TestFalse(TEXT("CPP type participates in identity"), Type == DifferentCPPType);
	TestTrue(TEXT("Different CPP types are unequal"), Type != DifferentCPPType);

	FAnimLispTypeRef DifferentCPPTypeObject = Type;
	DifferentCPPTypeObject.CPPTypeObject = TEXT("/Script/Engine.Vector_NetQuantize");
	TestFalse(TEXT("CPP type object participates in identity"), Type == DifferentCPPTypeObject);
	TestTrue(TEXT("Different CPP type objects are unequal"), Type != DifferentCPPTypeObject);

	FAnimLispTypeRef DifferentContainerType = Type;
	DifferentContainerType.ContainerType = TEXT("array");
	TestFalse(TEXT("Container type participates in identity"), Type == DifferentContainerType);
	TestTrue(TEXT("Different container types are unequal"), Type != DifferentContainerType);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispTokenizerSourceSpanTest,
	"AnimBP2FP.AnimLisp.Module.TokenizerSourceSpan",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimLispTokenizerSourceSpanTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT("(rig-module Test)");
	TArray<FAnimLangParseError> Errors;
	const TArray<FAnimLangToken> Tokens = FAnimLangTokenizer::Tokenize(
		Source,
		TEXT("Test.riglang"),
		&Errors);

	TestTrue(TEXT("Tokenization succeeds"), Errors.Num() == 0);
	TestEqual(TEXT("Tokenizer emits the expected token sequence"), Tokens.Num(), 5);
	if (Tokens.Num() != 0)
	{
		TestEqual(TEXT("Source filename"), Tokens[0].Span.SourceFile, FString(TEXT("Test.riglang")));
		TestEqual(TEXT("Source line"), Tokens[0].Span.Line, 1);
		TestEqual(TEXT("Source column"), Tokens[0].Span.Column, 1);
		TestEqual(TEXT("Source span length"), Tokens[0].Span.Length, 1);
	}
	if (Tokens.Num() >= 5)
	{
		TestEqual(TEXT("Multi-character identifier span offset"), Tokens[1].Span.Offset, 1);
		TestEqual(TEXT("Multi-character identifier span length"), Tokens[1].Span.Length, 10);
		TestEqual(TEXT("Second identifier span offset"), Tokens[2].Span.Offset, 12);
		TestEqual(TEXT("Second identifier span length"), Tokens[2].Span.Length, 4);
		TestEqual(TEXT("EOF span offset"), Tokens.Last().Span.Offset, Source.Len());
		TestEqual(TEXT("EOF span length"), Tokens.Last().Span.Length, 0);
	}

	TArray<FAnimLangToken> LegacyTokens;
	TArray<FAnimLangLexError> LegacyErrors;
	TestTrue(TEXT("Legacy tokenizer succeeds"), FAnimLangTokenizer::Tokenize(Source, LegacyTokens, LegacyErrors));
	TestTrue(TEXT("Legacy tokenizer reports no errors"), LegacyErrors.Num() == 0);
	TestEqual(TEXT("Legacy token count is unchanged"), LegacyTokens.Num(), Tokens.Num());
	if (LegacyTokens.Num() == Tokens.Num())
	{
		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			TestEqual(TEXT("Legacy token type is unchanged"), LegacyTokens[Index].Type, Tokens[Index].Type);
			TestEqual(TEXT("Legacy token value is unchanged"), LegacyTokens[Index].Value, Tokens[Index].Value);
			TestEqual(TEXT("Legacy token line is unchanged"), LegacyTokens[Index].Line, Tokens[Index].Line);
			TestEqual(TEXT("Legacy token column is unchanged"), LegacyTokens[Index].Column, Tokens[Index].Column);
			TestEqual(TEXT("Legacy token offset is unchanged"), LegacyTokens[Index].Offset, Tokens[Index].Offset);
			TestTrue(TEXT("Legacy token source filename remains empty"), LegacyTokens[Index].Span.SourceFile.IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispCrossFileDiagnosticTest,
	"AnimBP2FP.AnimLisp.Module.CrossFileDiagnostic",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimLispCrossFileDiagnosticTest::RunTest(const FString& Parameters)
{
	FAnimLangSourceLoc UseLocation;
	UseLocation.SourceFile = TEXT("Mover.animlang");
	UseLocation.Line = 12;
	UseLocation.Column = 7;

	FAnimLangSourceLoc DefinitionLocation;
	DefinitionLocation.SourceFile = TEXT("FootRig.riglang");
	DefinitionLocation.Line = 28;
	DefinitionLocation.Column = 3;

	FAnimLangDiagnostic Diagnostic(
		EAnimLangDiagSeverity::Error,
		EAnimLangDiagCategory::Capability,
		TEXT("Rig function cannot be called from an Anim module"),
		UseLocation);
	Diagnostic.AddRelatedLocation(DefinitionLocation, TEXT("Rig function defined here"));

	const FString Rendered = Diagnostic.ToString();
	TestTrue(TEXT("Primary file location is rendered"), Rendered.Contains(TEXT("Mover.animlang:12:7")));
	TestTrue(TEXT("Related file location is rendered"), Rendered.Contains(TEXT("FootRig.riglang:28:3")));
	TestTrue(TEXT("Related message is rendered"), Rendered.Contains(TEXT("Rig function defined here")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispLegacyRigObjectPathMigrationTest,
	"AnimBP2FP.AnimLisp.Module.LegacyRigObjectPathMigration",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimLispLegacyRigObjectPathMigrationTest::RunTest(const FString& Parameters)
{
	FAnimGraphAST SourceAST;
	SourceAST.Name = TEXT("ABP_LegacyRigReference");
	SourceAST.RootNode = MakeShared<FAnimNodeAST>();
	SourceAST.RootNode->NodeType = TEXT("control-rig");
	SourceAST.RootNode->NodeClassPath = TEXT("/Script/ControlRigDeveloper.AnimGraphNode_ControlRig");
	SourceAST.RootNode->Coverage = EAnimNodeCoverage::Reflected;
	SourceAST.RootNode->Properties.Add(
		TEXT("control-rig-asset-reference"),
		TEXT("\"(BlueprintRigClass=\\\"/Script/ControlRig.ControlRigBlueprintGeneratedClass'/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement_C'\\\")\""));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(SourceAST.ToString(), Errors);
	const bool bHasParseError = Errors.ContainsByPredicate(
		[](const FAnimLangParseError& Error) { return !Error.bWarning; });
	TestTrue(TEXT("Archived Control Rig reference parses without errors"), Parsed.IsValid() && !bHasParseError);
	TestTrue(TEXT("Migration reports unresolved public Rig entry"), Errors.ContainsByPredicate(
		[](const FAnimLangParseError& Error)
		{
			return Error.bWarning && Error.Code == TEXT("legacy-unresolved-rig-entry");
		}));
	if (!Parsed.IsValid() || Parsed->RigImports.Num() != 1) return false;

	const FAnimLispImport& Import = Parsed->RigImports[0];
	TestEqual(TEXT("Generated class reference normalizes to asset path"), Import.Target.AssetPath,
		FString(TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement")));
	TestTrue(TEXT("Parser marks only synthesized legacy imports"), Import.bLegacyExternal);
	TestTrue(TEXT("Migrated node has a typed binding"), Parsed->RootNode->RigBinding.IsSet());
	return true;
}

#endif
