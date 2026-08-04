// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimLangAST.h"
#include "AnimLangParser.h"
#include "AnimLispWorkspace.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRigModuleParsePrintRoundTrip,
	"AnimBP2FP.RigModule.ParsePrintRoundTrip",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRigModuleParsePrintRoundTrip::RunTest(const FString& Parameters)
{
	FAnimLispTypeRef SentinelType{TEXT("bool"), TEXT("None"), TEXT("None")};
	FAnimLispTypeRef EmptyType{TEXT("bool"), FString(), FString()};
	TestTrue(TEXT("None and empty exact-type sentinels canonicalize equivalently"), SentinelType == EmptyType);
	FAnimLispTypeRef ObjectType{TEXT("FVector"), TEXT("/Script/CoreUObject.Vector"), FString()};
	ObjectType.Canonicalize();
	TestEqual(TEXT("Non-empty object type paths remain exact after canonicalization"),
		ObjectType.CPPTypeObject, FString(TEXT("/Script/CoreUObject.Vector")));
	TestFalse(TEXT("Non-empty object type paths do not collapse to empty"), ObjectType == EmptyType);

	const FString Source = TEXT(
		"(anim-blueprint \"RigConsumer\"\n"
		"  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:test\")\n"
		"  :anim-graph\n"
		"  (control-rig\n"
		"    :library (rig-ref FootRig)\n"
		"    :entry (rig-entry FootRig/ForwardsSolve)\n"
		"    :inputs ((GroundNormal (bind-path \"CharacterProperties.GroundNormal\"))\n"
		"             (Enabled (pin-default true))\n"
		"             (Label (pin-default \"Foot IK\"))\n"
		"             (Offset (pin-default \"(X=1.0,Y=2.0,Z=3.0)\")))))");

	TArray<FAnimLangParseError> FirstErrors;
	const TSharedPtr<FAnimGraphAST> First = FAnimLangParser::Parse(Source, FirstErrors);
	TestTrue(TEXT("Typed Rig module source parses without diagnostics"), First.IsValid() && FirstErrors.Num() == 0);
	if (!First.IsValid() || FirstErrors.Num() != 0) return false;
	TestEqual(TEXT("One typed Rig import is retained"), First->RigImports.Num(), 1);
	if (First->RigImports.Num() == 1)
	{
		TestEqual(TEXT("Rig import asset identity is exact"), First->RigImports[0].Target.AssetPath,
			FString(TEXT("/Game/Test/FootRig")));
		TestEqual(TEXT("Rig import alias is exact"), First->RigImports[0].Alias, FString(TEXT("FootRig")));
		TestEqual(TEXT("Rig import expected hash is exact"), First->RigImports[0].ExpectedHash,
			FString(TEXT("sha256:test")));
	}
	TestTrue(TEXT("Control Rig node has a typed binding"), First->RootNode.IsValid() && First->RootNode->RigBinding.IsSet());
	if (!First->RootNode.IsValid() || !First->RootNode->RigBinding.IsSet()) return false;
	const FAnimRigNodeBinding& FirstBinding = First->RootNode->RigBinding.GetValue();
	TestEqual(TEXT("Typed Rig binding module is exact"), FirstBinding.RigModule.AssetPath,
		FString(TEXT("/Game/Test/FootRig")));
	TestEqual(TEXT("Typed Rig binding alias is exact"), FirstBinding.ImportAlias, FString(TEXT("FootRig")));
	TestEqual(TEXT("Typed Rig entry symbol is exact"), FirstBinding.EntryName, FString(TEXT("ForwardsSolve")));
	TestEqual(TEXT("All typed Rig input bindings are retained"), FirstBinding.Inputs.Num(), 4);
	if (FirstBinding.Inputs.Num() == 4)
	{
		auto FindValue = [&FirstBinding](const FString& Name) -> FString
		{
			const FAnimRigInputBinding* Input = FirstBinding.Inputs.FindByPredicate(
				[&Name](const FAnimRigInputBinding& Candidate) { return Candidate.RigInputName == Name; });
			return Input ? Input->ValueExpression : FString();
		};
		TestEqual(TEXT("Typed Rig binding expression is exact"), FindValue(TEXT("GroundNormal")),
			FString(TEXT("(bind-path \"CharacterProperties.GroundNormal\")")));
		TestEqual(TEXT("Bool pin default is typed"), FindValue(TEXT("Enabled")),
			FString(TEXT("(pin-default true)")));
		TestEqual(TEXT("String pin default is typed"), FindValue(TEXT("Label")),
			FString(TEXT("(pin-default \"Foot IK\")")));
		TestEqual(TEXT("Vector pin default is losslessly quoted"), FindValue(TEXT("Offset")),
			FString(TEXT("(pin-default \"(X=1.0,Y=2.0,Z=3.0)\")")));
	}

	const FString Canonical = First->ToString();
	TestTrue(TEXT("Rig import survives canonical printing"), Canonical.Contains(
		TEXT("(import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:test\")")));
	TestTrue(TEXT("Rig library and entry survive canonical printing"),
		Canonical.Contains(TEXT(":library (rig-ref FootRig)"))
		&& Canonical.Contains(TEXT(":entry (rig-entry FootRig/ForwardsSolve)")));
	TestTrue(TEXT("Typed Rig input bindings survive canonical printing"),
		Canonical.Contains(TEXT("(GroundNormal (bind-path \"CharacterProperties.GroundNormal\"))"))
		&& Canonical.Contains(TEXT("(Enabled (pin-default true))"))
		&& Canonical.Contains(TEXT("(Label (pin-default \"Foot IK\"))"))
		&& Canonical.Contains(TEXT("(Offset (pin-default \"(X=1.0,Y=2.0,Z=3.0)\"))")));

	TArray<FAnimLangParseError> SecondErrors;
	const TSharedPtr<FAnimGraphAST> Second = FAnimLangParser::Parse(Canonical, SecondErrors);
	TestTrue(TEXT("Canonical Rig module source reparses without diagnostics"),
		Second.IsValid() && SecondErrors.Num() == 0);
	if (!Second.IsValid() || SecondErrors.Num() != 0) return false;
	TestTrue(TEXT("Reparsed Control Rig node retains typed binding"),
		Second->RootNode.IsValid() && Second->RootNode->RigBinding.IsSet());
	TestEqual(TEXT("Typed Rig module source reaches a fixed point"), Second->ToString(), Canonical);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRigModuleGrammarValidation,
	"AnimBP2FP.RigModule.GrammarValidation",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRigModuleGrammarValidation::RunTest(const FString& Parameters)
{
	auto ExpectError = [this](const FString& Body, const FString& Fragment)
	{
		TArray<FAnimLangParseError> Errors;
		FAnimLangParser::Parse(TEXT("(anim-blueprint \"Invalid\"\n") + Body + TEXT("\n)"), Errors);
		return TestTrue(TEXT("Invalid Rig module grammar reports: ") + Fragment,
			Errors.ContainsByPredicate([&Fragment](const FAnimLangParseError& Error)
			{
				return Error.Message.Contains(Fragment);
			}));
	};

	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  (import-rig :asset \"/Game/Test/OtherRig\" :as FootRig :expected-hash \"sha256:b\")"),
		TEXT("Duplicate Rig import alias"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as Other :expected-hash \"sha256:b\")"),
		TEXT("Duplicate Rig import asset"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"not-a-hash\")"),
		TEXT("expected-hash"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry ForwardsSolve) :inputs ())"),
		TEXT("alias-qualified"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref Missing) :entry (rig-entry Missing/ForwardsSolve) :inputs ())"),
		TEXT("Unknown Rig import alias"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ())"),
		TEXT("Duplicate control-rig :library"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :entry broken :entry (rig-entry FootRig/ForwardsSolve) :library (rig-ref FootRig) :inputs ())"),
		TEXT("Duplicate control-rig :entry"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs () :inputs ())"),
		TEXT("Duplicate control-rig :inputs"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs () :control-rig-asset-reference \"/Game/Test/FootRig.FootRig_C\")"),
		TEXT("cannot combine typed Rig fields with legacy"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ((Enabled :cpp-type \"\" :cpp-type \"bool\" (pin-default true))))"),
		TEXT("Duplicate control-rig input :cpp-type"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ((Enabled :cpp-type :cpp-type \"bool\" (pin-default true))))"),
		TEXT("Duplicate control-rig input :cpp-type"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ((GroundNormal :cpp-type-object \"\" :cpp-type-object \"/Script/CoreUObject.Vector\" (pin-default \"(X=0,Y=0,Z=1)\"))))"),
		TEXT("Duplicate control-rig input :cpp-type-object"));
	ExpectError(
		TEXT("  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:a\")\n")
		TEXT("  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ((Enabled :container-type \"\" :container-type \"array\" (pin-default true))))"),
		TEXT("Duplicate control-rig input :container-type"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRigModuleLegacyReferenceMigration,
	"AnimBP2FP.RigModule.LegacyReferenceMigration",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRigModuleLegacyReferenceMigration::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(
		"(anim-blueprint \"LegacyRigConsumer\"\n"
		"  :anim-graph (control-rig\n"
		"    :control-rig-asset-reference \"/Game/Test/FootRig.FootRig_C\"\n"
		"    :exposed-input-pins (pin-names \"GroundNormal\" \"DebugDraw\")))");
	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(Source, Errors);
	TestTrue(TEXT("Legacy Control Rig reference remains parseable"), AST.IsValid()
		&& !Errors.ContainsByPredicate([](const FAnimLangParseError& Error) { return !Error.bWarning; }));
	TestEqual(TEXT("Legacy Control Rig reference reports one migration warning"), Errors.Num(), 1);
	if (Errors.Num() == 1)
	{
		TestTrue(TEXT("Legacy warning has exact code and node location"), Errors[0].bWarning
			&& Errors[0].Code == TEXT("legacy-unresolved-rig-entry")
			&& Errors[0].Line == 2);
	}
	if (!AST.IsValid()) return false;
	TestEqual(TEXT("Legacy reference synthesizes one Rig import"), AST->RigImports.Num(), 1);
	if (AST->RigImports.Num() == 1)
	{
		TestEqual(TEXT("Generated class canonicalizes to source blueprint asset"),
			AST->RigImports[0].Target.AssetPath, FString(TEXT("/Game/Test/FootRig")));
		TestEqual(TEXT("Legacy Rig alias is deterministic"), AST->RigImports[0].Alias, FString(TEXT("FootRig")));
	}
	TestTrue(TEXT("Legacy node carries unresolved typed Rig identity"),
		AST->RootNode.IsValid() && AST->RootNode->RigBinding.IsSet());
	if (!AST->RootNode.IsValid() || !AST->RootNode->RigBinding.IsSet()) return false;
	const FAnimRigNodeBinding& Binding = AST->RootNode->RigBinding.GetValue();
	TestFalse(TEXT("Legacy migration removes the raw asset authority from the AST"),
		AST->RootNode->Properties.Contains(TEXT("control-rig-asset-reference")));
	TestFalse(TEXT("Legacy migration removes the raw exposed pin manifest from the AST"),
		AST->RootNode->Properties.Contains(TEXT("exposed-input-pins")));
	TestTrue(TEXT("Legacy entry remains unresolved"), Binding.EntryName.IsEmpty());
	TestTrue(TEXT("Legacy exposed names do not fabricate value bindings"), Binding.Inputs.Num() == 0);
	TestEqual(TEXT("Legacy node cannot claim exact coverage"), AST->RootNode->Coverage, EAnimNodeCoverage::Lossy);
	const FString Canonical = AST->ToString();
	TestFalse(TEXT("Canonical legacy node does not invent a Rig entry"), Canonical.Contains(TEXT("rig-entry")));
	TestFalse(TEXT("Canonical typed node suppresses the legacy raw asset reference"),
		Canonical.Contains(TEXT(":control-rig-asset-reference")));
	TestTrue(TEXT("Canonical unresolved node retains typed Rig module identity"),
		Canonical.Contains(TEXT("(import-rig :asset \"/Game/Test/FootRig\" :as FootRig)"))
		&& Canonical.Contains(TEXT(":library (rig-ref FootRig)")));
	TArray<FAnimLangParseError> CanonicalErrors;
	const TSharedPtr<FAnimGraphAST> Reparsed = FAnimLangParser::Parse(Canonical, CanonicalErrors);
	TestTrue(TEXT("Canonical unresolved typed node reparses without legacy warnings"),
		Reparsed.IsValid() && CanonicalErrors.Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRigModuleWorkspaceCanonicalDetection,
	"AnimBP2FP.RigModule.WorkspaceCanonicalDetection",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRigModuleWorkspaceCanonicalDetection::RunTest(const FString& Parameters)
{
	const FString RigSource = TEXT(
		"(rig-module :asset \"/Game/Test/FootRig\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"sha256:test\")\n"
		"(rig-variables\n"
		"  (variable :id \"ground-normal\" :name \"GroundNormal\" :access public-input :cpp-type \"FVector\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"None\"))\n"
		"(define-rig-entry \"ForwardsSolve\" :id \"forwards-solve\" :event \"ForwardsSolve\")\n");
	const FString AnimSource = FString::Chr(0xFEFF) + TEXT(
		"; canonical AnimLang may begin with comments\n"
		"(anim-blueprint \"BOMConsumer\"\n"
		"  (import-rig :asset \"/Game/Test/FootRig\" :as FootRig :expected-hash \"sha256:test\")\n"
		"  :anim-graph (control-rig :library (rig-ref FootRig) :entry (rig-entry FootRig/ForwardsSolve) :inputs ()))");
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("FootRig.riglang"), RigSource);
	Workspace.AddSource(TEXT("BOMConsumer.animlang"), AnimSource);
	FAnimLangDiagnostics Diagnostics;
	TestTrue(TEXT("Workspace recognizes canonical anim-blueprint after BOM and comments"),
		Workspace.Build(Diagnostics));
	if (Diagnostics.HasErrors())
	{
		for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items) AddInfo(Diagnostic.ToString());
	}
	return true;
}

#endif
