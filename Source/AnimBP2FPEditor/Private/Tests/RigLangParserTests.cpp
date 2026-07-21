// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "RigLangAST.h"
#include "RigLangParser.h"
#include "AnimLispWorkspace.h"
#include "Rigs/RigHierarchyElements.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangParserTests
{
const EAutomationTestFlags TestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

FString MakeModuleHeader()
{
	return TEXT("(rig-module :asset \"/Game/Test/CR_Test\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"test-hash\")");
}

bool IsExpectedLegacyGraphWarning(const FRigLangParseError& Error)
{
	return Error.bWarning && (Error.Message.Contains(TEXT("legacy-rig-call-identity"))
		|| Error.Message.Contains(TEXT("legacy-inline-graph-migrated"))
		|| Error.Message.Contains(TEXT("legacy-editor-guid-normalized"))
		|| Error.Message.Contains(TEXT("legacy-control-settings-lossy-migrated")));
}

bool ExpectParseError(
	FAutomationTestBase& Test,
	const FString& Source,
	const FString& SourceFile,
	const FString& MessageFragment,
	const int32 ExpectedLine,
	const int32 ExpectedColumn)
{
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(Source, SourceFile, Errors);

	Test.TestFalse(TEXT("Invalid source does not produce a module"), Module.IsValid());
	Test.TestFalse(TEXT("Invalid source reports at least one error"), Errors.IsEmpty());
	if (Errors.IsEmpty())
	{
		return false;
	}

	const FRigLangParseError* Error = Errors.FindByPredicate([](const FRigLangParseError& Candidate)
	{
		return !Candidate.bWarning;
	});
	if (!Test.TestNotNull(TEXT("Invalid source reports a fatal error"), Error)) return false;
	Test.TestTrue(TEXT("Error message identifies the rejected construct"), Error->Message.Contains(MessageFragment));
	Test.TestEqual(TEXT("Error source file"), Error->Location.SourceFile, SourceFile);
	Test.TestEqual(TEXT("Error source line"), Error->Location.Line, ExpectedLine);
	Test.TestEqual(TEXT("Error source column"), Error->Location.Column, ExpectedColumn);
	return true;
}

FString QuoteRigLangString(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return TEXT("\"") + Escaped + TEXT("\"");
}

FString ExportControlSettings(const ERigControlType ControlType)
{
	FRigControlSettings Settings;
	Settings.ControlType = ControlType;
	FString Serialized;
	FRigControlSettings::StaticStruct()->ExportText(
		Serialized, &Settings, &Settings, nullptr, PPF_None, nullptr);
	return Serialized;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserRoundTripTest,
	"AnimBP2FP.RigLang.Parser.RoundTrip",
	RigLangParserTests::TestFlags)

bool FRigLangParserRoundTripTest::RunTest(const FString& Parameters)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectPluginsDir() / TEXT("AnimBP2FP/Content/Tests/RigLang/MinimalFootRig.riglang"));
	FString Source;
	if (!TestTrue(TEXT("Fixture exists"), IFileManager::Get().FileExists(*FixturePath))
		|| !TestTrue(TEXT("Fixture can be read"), FFileHelper::LoadFileToString(Source, *FixturePath)))
	{
		return false;
	}
	TArray<FRigLangParseError> FirstErrors;
	const TSharedPtr<FRigModuleAST> First = FRigLangParser::Parse(Source, FixturePath, FirstErrors);
	TestFalse(TEXT("Fixture parses without hard errors"),
		FirstErrors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; }));
	TestEqual(TEXT("Fixture reports the exact migration warning count"), FirstErrors.Num(), 6);
	auto CountWarnings = [&FirstErrors](const TCHAR* Fragment)
	{
		return FirstErrors.FilterByPredicate([Fragment](const FRigLangParseError& Error)
		{
			return Error.bWarning && Error.Message.Contains(Fragment);
		}).Num();
	};
	TestEqual(TEXT("Fixture reports one control migration warning"), CountWarnings(TEXT("legacy-control-settings-lossy-migrated")), 1);
	TestEqual(TEXT("Fixture reports one inline graph migration warning"), CountWarnings(TEXT("legacy-inline-graph-migrated")), 1);
	TestEqual(TEXT("Fixture reports three editor GUID normalization warnings"), CountWarnings(TEXT("legacy-editor-guid-normalized")), 3);
	TestEqual(TEXT("Fixture reports one short-call identity warning"), CountWarnings(TEXT("legacy-rig-call-identity")), 1);
	TestTrue(TEXT("Fixture reports one explicitly lossy legacy control-settings migration"),
		FirstErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return Error.bWarning
				&& Error.Message.Contains(TEXT("legacy-control-settings-lossy-migrated"))
				&& Error.Message.Contains(TEXT("not exact"));
		}) && !FirstErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return !RigLangParserTests::IsExpectedLegacyGraphWarning(Error);
		}));
	if (!TestNotNull(TEXT("Fixture produces a module"), First.Get()))
	{
		return false;
	}
	const FRigHierarchyElementAST& LegacyControl = First->Hierarchy[1];
	TestEqual(TEXT("Legacy control promotes exactly one typed state"), LegacyControl.States.Num(), 1);
	TestEqual(TEXT("Legacy control type is authoritative"), LegacyControl.States[0].Type, FString(TEXT("Transform")));
	TestFalse(TEXT("Legacy :control-type is removed after promotion"), LegacyControl.Properties.Contains(TEXT("control-type")));
	TestFalse(TEXT("Legacy :settings is removed after promotion"), LegacyControl.Properties.Contains(TEXT("settings")));
	FRigControlSettings PromotedSettings;
	const TCHAR* SettingsRemainder = FRigControlSettings::StaticStruct()->ImportText(
		*LegacyControl.States[0].SerializedValue, &PromotedSettings, nullptr, PPF_None, nullptr, TEXT("FRigControlSettings"));
	while (SettingsRemainder && FChar::IsWhitespace(*SettingsRemainder)) ++SettingsRemainder;
	TestTrue(TEXT("Promoted settings payload is complete"), SettingsRemainder && *SettingsRemainder == TEXT('\0'));
	TestEqual(TEXT("Legacy shape is mapped"), PromotedSettings.ShapeName, FName(TEXT("Box")));
	TestEqual(TEXT("Legacy limits are mapped"), PromotedSettings.LimitEnabled.Num(), 2);
	if (PromotedSettings.LimitEnabled.Num() == 2)
	{
		TestTrue(TEXT("First legacy limit remains enabled"),
			PromotedSettings.LimitEnabled[0].bMinimum && PromotedSettings.LimitEnabled[0].bMaximum);
		TestFalse(TEXT("Second legacy limit remains disabled"),
			PromotedSettings.LimitEnabled[1].bMinimum || PromotedSettings.LimitEnabled[1].bMaximum);
	}

	const FString FirstCanonical = First->ToCanonicalString();
	TestFalse(TEXT("Canonical output is not empty"), FirstCanonical.IsEmpty());
	TestFalse(TEXT("Canonical output omits legacy :control-type"), FirstCanonical.Contains(TEXT(":control-type")));
	TestFalse(TEXT("Canonical output omits legacy :settings form"), FirstCanonical.Contains(TEXT(":settings (control-settings")));
	TestEqual(TEXT("Canonical printing is deterministic"), First->ToCanonicalString(), FirstCanonical);

	TArray<FRigLangParseError> SecondErrors;
	const TSharedPtr<FRigModuleAST> Second = FRigLangParser::Parse(
		FirstCanonical,
		TEXT("MinimalFootRig.canonical.riglang"),
		SecondErrors);
	TestFalse(TEXT("Canonical output reparses without hard errors"),
		SecondErrors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; }));
	TestEqual(TEXT("Canonical reparse retains exactly one unavoidable short-call warning"), SecondErrors.Num(), 1);
	TestTrue(TEXT("Canonical reparse has only legacy-rig-call-identity"), SecondErrors.Num() == 1
		&& SecondErrors[0].bWarning
		&& SecondErrors[0].Message.Contains(TEXT("legacy-rig-call-identity")));
	if (!TestNotNull(TEXT("Canonical output produces a module"), Second.Get()))
	{
		return false;
	}

	TestTrue(TEXT("Parse-print-parse preserves authoritative semantics"), First->SemanticEquals(*Second));
	TestEqual(TEXT("Canonical output reaches a fixed point"), Second->ToCanonicalString(), FirstCanonical);
	TestEqual(TEXT("Canonical hash input reaches a fixed point"), Second->ToCanonicalHashInput(), First->ToCanonicalHashInput());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserTypedDeclarationsRoundTripTest,
	"AnimBP2FP.RigLang.Parser.TypedDeclarationsRoundTrip",
	RigLangParserTests::TestFlags)

bool FRigLangParserTypedDeclarationsRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-graph :id \"library\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"function-library\" :parent-id \"\"\n")
		+ TEXT("  (rig-collapse :id \"function-node\" :guid \"function-node-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph-id \"graph\"))\n")
		+ TEXT("(define-rig-graph :id \"graph\" :editor-guid \"22222222-2222-2222-2222-222222222222\" :role \"function\" :parent-id \"library\"\n")
		+ TEXT("  (rig-local-variable :guid \"11111111-1111-1111-1111-111111111111\" :name \"Unused\" :cpp-type \"TArray<FVector>\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"array\" :default \"()\")\n")
		+ TEXT("  (rig-local-variable :guid \"22222222-2222-2222-2222-222222222222\" :name \"Counter\" :cpp-type \"int32\" :cpp-type-object \"\" :container-type \"\" :default \"7\"))\n")
		+ TEXT("(define-rig-function \"Interleaved\" :id \"fn\" :visibility public :return-cpp-type \"void\" :graph-id \"graph\"\n")
		+ TEXT("  (rig-argument :name \"InputA\" :direction input :cpp-type \"float\" :cpp-type-object \"\" :container-type \"\" :default \"1.0\" :execute-context false :constant true :input-variable true)\n")
		+ TEXT("  (rig-argument :name \"InOut\" :direction io :cpp-type \"FVector\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"\" :default \"(X=0,Y=0,Z=0)\" :execute-context false :constant false :input-variable false)\n")
		+ TEXT("  (rig-argument :name \"OutputB\" :direction output :cpp-type \"bool\" :cpp-type-object \"\" :container-type \"\" :default \"False\" :execute-context false :constant false :input-variable false)\n")
		+ TEXT("  (rig-external-variable :guid \"33333333-3333-3333-3333-333333333333\" :name \"Speed\" :cpp-type \"float\" :cpp-type-object \"\" :container-type \"\" :public true :read-only true)\n")
		+ TEXT("  (rig-dependency :host \"/Game/Rigs/CR_Dep.CR_Dep\" :library-node-path \"RigVMLibrary.Dep\" :hash 4294967295))");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> First = FRigLangParser::Parse(Source, TEXT("TypedDeclarations.riglang"), Errors);
	TestTrue(TEXT("Typed declaration source parses"), Errors.IsEmpty());
	if (!TestNotNull(TEXT("Typed declaration source produces a module"), First.Get())) return false;
	if (!TestEqual(TEXT("Two graphs parsed"), First->Graphs.Num(), 2)
		|| !TestEqual(TEXT("One function parsed"), First->Functions.Num(), 1)) return false;

	const FRigGraphAST& Graph = First->Graphs[1];
	TestTrue(TEXT("Canonical printer emits typed contained graph identity"),
		First->ToCanonicalString().Contains(TEXT(":contained-graph-id \"graph\"")));
	TestEqual(TEXT("Unused locals are preserved"), Graph.LocalVariables.Num(), 2);
	if (Graph.LocalVariables.Num() == 2)
	{
		TestEqual(TEXT("Local order is authoritative"), Graph.LocalVariables[0].Name, FString(TEXT("Unused")));
		TestEqual(TEXT("Local GUID is typed"), Graph.LocalVariables[0].Guid, FString(TEXT("11111111-1111-1111-1111-111111111111")));
		TestEqual(TEXT("Local exact type"), Graph.LocalVariables[0].Type.CPPType, FString(TEXT("TArray<FVector>")));
		TestEqual(TEXT("Local object type"), Graph.LocalVariables[0].Type.CPPTypeObject, FString(TEXT("/Script/CoreUObject.Vector")));
		TestEqual(TEXT("Local container"), Graph.LocalVariables[0].Type.ContainerType, FString(TEXT("array")));
		TestEqual(TEXT("Local default"), Graph.LocalVariables[1].DefaultValue, FString(TEXT("7")));
	}

	const FRigFunctionAST& Function = First->Functions[0];
	TestEqual(TEXT("Argument interleaving is preserved"), Function.Arguments.Num(), 3);
	if (Function.Arguments.Num() == 3)
	{
		TestEqual(TEXT("Argument zero is input"), Function.Arguments[0].Direction, ERigPinDirection::Input);
		TestEqual(TEXT("Argument one remains IO"), Function.Arguments[1].Direction, ERigPinDirection::IO);
		TestEqual(TEXT("Argument two is output"), Function.Arguments[2].Direction, ERigPinDirection::Output);
		TestTrue(TEXT("Argument const is typed"), Function.Arguments[0].bConstant);
		TestTrue(TEXT("Argument input-variable is typed"), Function.Arguments[0].bInputVariable);
	}
	TestEqual(TEXT("External variables are typed"), Function.ExternalVariables.Num(), 1);
	TestEqual(TEXT("Dependencies are typed"), Function.Dependencies.Num(), 1);
	if (Function.Dependencies.Num() == 1)
	{
		TestEqual(TEXT("Dependency uint32 hash is exact"), Function.Dependencies[0].Hash, MAX_uint32);
	}

	const FString Canonical = First->ToCanonicalString();
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Second = FRigLangParser::Parse(Canonical, TEXT("TypedDeclarations.canonical.riglang"), ReparseErrors);
	TestTrue(TEXT("Typed declaration canonical text reparses"), ReparseErrors.IsEmpty());
	if (!TestNotNull(TEXT("Typed declaration canonical text produces a module"), Second.Get())) return false;
	TestEqual(TEXT("Typed declarations reach a byte-stable fixed point"), Second->ToCanonicalString(), Canonical);
	TestEqual(TEXT("Typed declarations reach a hash-input fixed point"), Second->ToCanonicalHashInput(), First->ToCanonicalHashInput());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserTypedCallIdentityTest,
	"AnimBP2FP.RigLang.Parser.TypedCallIdentity",
	RigLangParserTests::TestFlags)

bool FRigLangParserTypedCallIdentityTest::RunTest(const FString& Parameters)
{
	const FString Host = TEXT("/Game/Rigs/Other.Other_C");
	const FString LibraryPath = TEXT("/Game/Rigs/Other.Other:RigVMFunctionLibrary.Solve");
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(import-rig :asset \"/Game/Rigs/Other\" :alias Other :content-hash \"\")\n")
		+ TEXT("(define-rig-function \"Caller\" :id \"caller\" :visibility public :return-cpp-type \"void\"\n")
		+ TEXT("  (rig-call :id \"call\" :guid \"11111111-1111-1111-1111-111111111111\" :function \"WrongShort\" :class \"\"")
		+ TEXT(" :function-identifier-host \"") + Host + TEXT("\"")
		+ TEXT(" :function-library-node-path \"") + LibraryPath + TEXT("\"))");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(
		Source, TEXT("TypedCallIdentity.riglang"), Errors);
	TestTrue(TEXT("Typed call identity source parses"), Module.IsValid()
		&& !Errors.ContainsByPredicate([](const FRigLangParseError& Error)
		{ return !RigLangParserTests::IsExpectedLegacyGraphWarning(Error); }));
	if (!Module.IsValid() || Module->Functions.IsEmpty()) return false;
	const FRigGraphAST* FunctionGraph = Module->Graphs.FindByPredicate([&Module](const FRigGraphAST& Graph)
		{ return Graph.StableId == Module->Functions[0].GraphStableId; });
	if (!TestNotNull(TEXT("Typed call resolves through authoritative graph inventory"), FunctionGraph)
		|| FunctionGraph->Nodes.IsEmpty()) return false;
	const FRigNodeAST& Call = FunctionGraph->Nodes[0];
	TestFalse(TEXT("Function identifier host is promoted out of raw properties"),
		Call.Properties.Contains(TEXT("function-identifier-host")));
	TestFalse(TEXT("Function library node path is promoted out of raw properties"),
		Call.Properties.Contains(TEXT("function-library-node-path")));
	const FString Canonical = Module->ToCanonicalString();
	TestTrue(TEXT("Typed external call prints its semantic alias-qualified symbol"),
		Canonical.Contains(TEXT(":function \"Other/Solve\"")));
	TestTrue(TEXT("Typed external call prints the exact host identity"),
		Canonical.Contains(TEXT(":function-identifier-host \"") + Host + TEXT("\"")));
	TestTrue(TEXT("Typed external call prints the exact library-node path"),
		Canonical.Contains(TEXT(":function-library-node-path \"") + LibraryPath + TEXT("\"")));
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(
		Canonical, TEXT("TypedCallIdentity.canonical.riglang"), ReparseErrors);
	TestTrue(TEXT("Typed call canonical form reparses without diagnostics"),
		Reparsed.IsValid() && ReparseErrors.IsEmpty());
	if (Reparsed.IsValid())
	{
		TestEqual(TEXT("Typed call canonical text reaches a fixed point"),
			Reparsed->ToCanonicalString(), Canonical);
		TestEqual(TEXT("Typed call hash input reaches a fixed point"),
			Reparsed->ToCanonicalHashInput(), Module->ToCanonicalHashInput());
	}

	const FString LegacySource = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"Caller\" :id \"caller\" :visibility public :return-cpp-type \"void\"\n")
		+ TEXT("  (rig-call :id \"legacy\" :guid \"22222222-2222-2222-2222-222222222222\" :function \"Solve\" :class \"\"))");
	TArray<FRigLangParseError> LegacyErrors;
	const TSharedPtr<FRigModuleAST> LegacyModule = FRigLangParser::Parse(
		LegacySource, TEXT("LegacyCallIdentity.riglang"), LegacyErrors);
	TestTrue(TEXT("Legacy short call remains parseable"), LegacyModule.IsValid());
	TestTrue(TEXT("Legacy short call emits a source-located migration warning"),
		LegacyErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return Error.bWarning && Error.Message.Contains(TEXT("legacy-rig-call-identity"))
				&& Error.Location.SourceFile == TEXT("LegacyCallIdentity.riglang")
				&& Error.Location.Line > 1;
		}));

	const FString DuplicateIdentitySource = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"First\" :id \"first\" :visibility public :return-cpp-type \"void\"")
		+ TEXT(" :identifier-host \"") + Host + TEXT("\" :library-node-path \"") + LibraryPath + TEXT("\")")
		+ TEXT("\n(define-rig-function \"Second\" :id \"second\" :visibility public :return-cpp-type \"void\"")
		+ TEXT(" :identifier-host \"") + Host + TEXT("\" :library-node-path \"") + LibraryPath + TEXT("\")");
	TArray<FRigLangParseError> DuplicateIdentityErrors;
	const TSharedPtr<FRigModuleAST> DuplicateIdentityModule = FRigLangParser::Parse(
		DuplicateIdentitySource, TEXT("DuplicateTypedIdentity.riglang"), DuplicateIdentityErrors);
	TestFalse(TEXT("Two declarations cannot share one typed function identity"),
		DuplicateIdentityModule.IsValid());
	TestTrue(TEXT("Duplicate typed identity reports the duplicate and first declaration locations"),
		DuplicateIdentityErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return Error.Message.Contains(TEXT("Duplicate typed function identifier"))
				&& Error.Message.Contains(TEXT("DuplicateTypedIdentity.riglang:2:"))
				&& Error.Location.SourceFile == TEXT("DuplicateTypedIdentity.riglang")
				&& Error.Location.Line == 3;
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateModuleHeaderTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateModuleHeader",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateModuleHeaderTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-module :asset \"/Game/Test/CR_Other\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"other-hash\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateModule.riglang"), TEXT("Duplicate rig-module"), 2, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserMissingAssetTest,
	"AnimBP2FP.RigLang.Parser.RejectsMissingAsset",
	RigLangParserTests::TestFlags)

bool FRigLangParserMissingAssetTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT("(rig-module :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"test-hash\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("MissingAsset.riglang"), TEXT("asset"), 1, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserMalformedHierarchyTest,
	"AnimBP2FP.RigLang.Parser.RejectsMalformedHierarchy",
	RigLangParserTests::TestFlags)

bool FRigLangParserMalformedHierarchyTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n  (bone :id \"bone-root\" :parent \"\")\n)");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("MalformedHierarchy.riglang"), TEXT("name"), 3, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserTypedHierarchySchemaTest,
	"AnimBP2FP.RigLang.Parser.TypedHierarchySchema",
	RigLangParserTests::TestFlags)

bool FRigLangParserTypedHierarchySchemaTest::RunTest(const FString& Parameters)
{
	const FString FloatSettings = RigLangParserTests::QuoteRigLangString(
		RigLangParserTests::ExportControlSettings(ERigControlType::Float));
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n")
		+ TEXT("  (control :id \"Control:hand\" :name \"hand\" :parent \"root\"\n")
		+ TEXT("    (rig-parent :id \"Bone:root\" :label \"Root\" :current-location 0.25 :current-rotation 0.5 :current-scale 0.75 :initial-location 0.125 :initial-rotation 0.375 :initial-scale 0.625)\n")
		+ TEXT("    (rig-transform :role initial-local :translation (1 2 3) :rotation (0 0 0 1) :scale (1 1 1))\n")
		+ TEXT("    (rig-transform :role current-local :translation (4 5 6) :rotation (0 0 0 1) :scale (1 1 1))\n")
		+ TEXT("    (rig-state :kind control-settings :role initial :type Float :serialized ") + FloatSettings + TEXT(")\n")
		+ TEXT("    (rig-state :kind control-value :role initial :type Float :number 2.5)\n")
		+ TEXT("    (rig-metadata :name \"Speed\" :kind float :numbers (3.5))))\n");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(Source, TEXT("TypedHierarchy.riglang"), Errors);
	TestTrue(TEXT("Typed hierarchy parses"), Errors.IsEmpty());
	if (!TestNotNull(TEXT("Typed hierarchy produces a module"), Module.Get())) return false;
	const FRigHierarchyElementAST& Element = Module->Hierarchy[0];
	TestEqual(TEXT("Typed parent count"), Element.Parents.Num(), 1);
	TestEqual(TEXT("Typed parent location weight"), Element.Parents[0].CurrentWeight.Location, 0.25);
	TestEqual(TEXT("Typed parent rotation weight"), Element.Parents[0].CurrentWeight.Rotation, 0.5);
	TestEqual(TEXT("Typed parent scale weight"), Element.Parents[0].CurrentWeight.Scale, 0.75);
	TestEqual(TEXT("Typed transform count"), Element.Transforms.Num(), 2);
	TestEqual(TEXT("Typed state count"), Element.States.Num(), 2);
	TestEqual(TEXT("Typed metadata count"), Element.Metadata.Num(), 1);
	TestEqual(TEXT("Typed vector arity and values"), Element.Transforms[0].Value.GetTranslation(), FVector(1, 2, 3));
	TestTrue(TEXT("Canonical printer emits typed parent form"), Module->ToCanonicalString().Contains(TEXT("(rig-parent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserRejectsMalformedTypedHierarchyTest,
	"AnimBP2FP.RigLang.Parser.RejectsMalformedTypedHierarchy",
	RigLangParserTests::TestFlags)

bool FRigLangParserRejectsMalformedTypedHierarchyTest::RunTest(const FString& Parameters)
{
	auto ErrorsFor = [](const FString& Child)
	{
		TArray<FRigLangParseError> Errors;
		FRigLangParser::Parse(RigLangParserTests::MakeModuleHeader()
			+ TEXT("\n(rig-hierarchy (control :id \"c\" :name \"c\" :parent \"\" ") + Child + TEXT("))"),
			TEXT("MalformedTypedHierarchy.riglang"), Errors);
		return Errors;
	};
	auto Contains = [](const TArray<FRigLangParseError>& Errors, const FString& Fragment)
	{
		return Errors.ContainsByPredicate([&](const FRigLangParseError& Error) { return Error.Message.Contains(Fragment); });
	};
	TestTrue(TEXT("Required transform field"), Contains(ErrorsFor(TEXT("(rig-transform :role initial-local :rotation (0 0 0 1) :scale (1 1 1))")), TEXT("requires :translation")));
	TestTrue(TEXT("Duplicate typed field"), Contains(ErrorsFor(TEXT("(rig-transform :role initial-local :translation (0 0 0) :translation (1 1 1) :rotation (0 0 0 1) :scale (1 1 1))")), TEXT("Duplicate property :translation")));
	TestTrue(TEXT("Unknown role enum"), Contains(ErrorsFor(TEXT("(rig-transform :role mystery :translation (0 0 0) :rotation (0 0 0 1) :scale (1 1 1))")), TEXT("invalid transform role")));
	TestTrue(TEXT("Quaternion arity"), Contains(ErrorsFor(TEXT("(rig-transform :role initial-local :translation (0 0 0) :rotation (0 0 1) :scale (1 1 1))")), TEXT("requires 4 numeric components")));
	TestTrue(TEXT("Invalid control settings payload"), Contains(ErrorsFor(TEXT("(rig-state :kind control-settings :role initial :type Float :serialized \"not-a-control-settings-struct\")")), TEXT("invalid FRigControlSettings payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserGraphOwnershipTest,
	"AnimBP2FP.RigLang.Parser.RejectsInvalidGraphOwnership",
	RigLangParserTests::TestFlags)

bool FRigLangParserGraphOwnershipTest::RunTest(const FString& Parameters)
{
	auto Parse = [](const FString& Body, TArray<FRigLangParseError>& Errors)
	{
		return FRigLangParser::Parse(RigLangParserTests::MakeModuleHeader() + TEXT("\n") + Body,
			TEXT("GraphOwnership.riglang"), Errors);
	};
	auto HasExact = [](const TArray<FRigLangParseError>& Errors, const FString& Message)
	{
		return Errors.ContainsByPredicate([&Message](const FRigLangParseError& Error) { return Error.Message == Message; });
	};
	auto HasFatal = [](const TArray<FRigLangParseError>& Errors)
	{
		return Errors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; });
	};
	auto Rejects = [this, &Parse, &HasExact](const TCHAR* Label, const FString& Body, const FString& Message)
	{
		TArray<FRigLangParseError> Errors;
		const TSharedPtr<FRigModuleAST> Module = Parse(Body, Errors);
		TestFalse(Label, Module.IsValid());
		TestTrue(*FString::Printf(TEXT("%s exact diagnostic"), Label), HasExact(Errors, Message));
		const FRigLangParseError* Exact = Errors.FindByPredicate([&Message](const FRigLangParseError& Error)
		{
			return Error.Message == Message;
		});
		if (Exact)
		{
			TestEqual(*FString::Printf(TEXT("%s diagnostic source"), Label),
				Exact->Location.SourceFile, FString(TEXT("GraphOwnership.riglang")));
			TestTrue(*FString::Printf(TEXT("%s diagnostic line"), Label), Exact->Location.Line > 1);
		}
	};

	TArray<FRigLangParseError> ValidErrors;
	const TSharedPtr<FRigModuleAST> ValidMultiEvent = Parse(
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-entry \"Construction\" :id \"construction\" :event \"Construction\" :graph-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"forward\" :event \"Forward Solve\" :graph-id \"root\")"), ValidErrors);
	TestTrue(TEXT("Multiple event entries may share one root graph"), ValidMultiEvent.IsValid() && !HasFatal(ValidErrors));
	TArray<FRigLangParseError> ValidNodeContainedErrors;
	const TSharedPtr<FRigModuleAST> ValidNodeContained = Parse(
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\"\n")
		TEXT("  (rig-collapse :id \"collapse\" :guid \"collapse-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph-id \"child\"))\n")
		TEXT("(define-rig-graph :id \"child\" :editor-guid \"child-guid\" :role \"node-contained\" :parent-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")"), ValidNodeContainedErrors);
	TestTrue(TEXT("Node-contained graph resolves to its typed parent node owner"),
		ValidNodeContained.IsValid() && !HasFatal(ValidNodeContainedErrors));
	TArray<FRigLangParseError> LegacyErrors;
	const TSharedPtr<FRigModuleAST> LegacyContained = Parse(
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\"\n")
		TEXT("  (rig-collapse :id \"collapse\" :guid \"collapse-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph \"child\"))\n")
		TEXT("(define-rig-graph :id \"child\" :editor-guid \"child-guid\" :role \"node-contained\" :parent-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")"), LegacyErrors);
	TestTrue(TEXT("Legacy contained graph property remains readable"),
		LegacyContained.IsValid() && !HasFatal(LegacyErrors));
	if (LegacyContained.IsValid())
	{
		const FString Canonical = LegacyContained->ToCanonicalString();
		TestEqual(TEXT("Legacy contained graph migrates to the typed field"),
			LegacyContained->Graphs[0].Nodes[0].ContainedGraphStableId, FString(TEXT("child")));
		TestFalse(TEXT("Legacy contained graph is not retained as a raw property"),
			LegacyContained->Graphs[0].Nodes[0].Properties.Contains(TEXT("contained-graph")));
		TestTrue(TEXT("Legacy graph editor identity emits a source-located migration warning"),
			LegacyErrors.ContainsByPredicate([](const FRigLangParseError& Error)
			{
				return Error.bWarning
					&& Error.Message.Contains(TEXT("legacy-editor-guid-normalized"))
					&& Error.Location.SourceFile == TEXT("GraphOwnership.riglang")
					&& Error.Location.Line > 1;
			}));
		TestFalse(TEXT("Legacy graph editor migration metadata does not enter the semantic AST"),
			LegacyContained->Graphs[0].Properties.Contains(TEXT("legacy-editor-guid-migrated")));
		FGuid MigratedEditorGuid;
		TestTrue(TEXT("Legacy graph editor identity migrates to a valid nonzero GUID"),
			FGuid::ParseExact(LegacyContained->Graphs[0].EditorGuid,
				EGuidFormats::DigitsWithHyphens, MigratedEditorGuid)
				&& MigratedEditorGuid.IsValid());
		TestTrue(TEXT("Legacy migration prints the typed contained graph field"),
			Canonical.Contains(TEXT(":contained-graph-id \"child\"")));
		TestFalse(TEXT("Legacy migration does not print the legacy contained graph field"),
			Canonical.Contains(TEXT(":contained-graph \"child\"")));
		TArray<FRigLangParseError> CanonicalErrors;
		const TSharedPtr<FRigModuleAST> CanonicalModule = FRigLangParser::Parse(
			Canonical, TEXT("GraphOwnership.canonical.riglang"), CanonicalErrors);
		TestTrue(TEXT("Migrated contained graph canonical form reparses"),
			CanonicalModule.IsValid() && CanonicalErrors.IsEmpty());
		if (CanonicalModule.IsValid())
			TestEqual(TEXT("Legacy migration reaches a hash-input fixed point"),
				CanonicalModule->ToCanonicalHashInput(), LegacyContained->ToCanonicalHashInput());
	}

	Rejects(TEXT("Duplicate graph editor GUID"),
		TEXT("(define-rig-graph :id \"root-a\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"root-b\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-entry \"EventA\" :id \"entry-a\" :event \"Event A\" :graph-id \"root-a\")\n")
		TEXT("(define-rig-entry \"EventB\" :id \"entry-b\" :event \"Event B\" :graph-id \"root-b\")"),
		TEXT("Duplicate graph editor GUID '11111111-1111-1111-1111-111111111111'"));
	Rejects(TEXT("Empty function graph ID"),
		TEXT("(define-rig-graph :id \"library\" :editor-guid \"library-guid\" :role \"function-library\" :parent-id \"\")\n")
		TEXT("(define-rig-function \"Solve\" :id \"fn\" :visibility public :graph-id \"\")"),
		TEXT("Rig function 'Solve' requires :graph-id"));
	Rejects(TEXT("Empty entry graph ID"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"\")"),
		TEXT("Rig entry 'ForwardSolve' requires :graph-id"));
	Rejects(TEXT("Missing parent graph"),
		TEXT("(define-rig-graph :id \"body\" :editor-guid \"body-guid\" :role \"function\" :parent-id \"missing\")\n")
		TEXT("(define-rig-function \"Solve\" :id \"fn\" :visibility public :graph-id \"body\")"),
		TEXT("Graph 'body' has missing parent graph 'missing'"));
	Rejects(TEXT("Missing function graph"),
		TEXT("(define-rig-graph :id \"library\" :editor-guid \"library-guid\" :role \"function-library\" :parent-id \"\")\n")
		TEXT("(define-rig-function \"Solve\" :id \"fn\" :visibility public :graph-id \"stale\")"),
		TEXT("Rig function 'Solve' references missing graph 'stale'"));
	Rejects(TEXT("Missing entry graph"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"stale\")"),
		TEXT("Rig entry 'ForwardSolve' references missing graph 'stale'"));
	Rejects(TEXT("Illegal entry graph role"),
		TEXT("(define-rig-graph :id \"library\" :editor-guid \"library-guid\" :role \"function-library\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"body\" :editor-guid \"body-guid\" :role \"function\" :parent-id \"library\")\n")
		TEXT("(define-rig-function \"Solve\" :id \"fn\" :visibility public :graph-id \"body\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"body\")"),
		TEXT("Rig entry 'ForwardSolve' must reference a root graph"));
	Rejects(TEXT("Duplicate function owners"),
		TEXT("(define-rig-graph :id \"library\" :editor-guid \"library-guid\" :role \"function-library\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"body\" :editor-guid \"body-guid\" :role \"function\" :parent-id \"library\")\n")
		TEXT("(define-rig-function \"SolveA\" :id \"fn-a\" :visibility public :graph-id \"body\")\n")
		TEXT("(define-rig-function \"SolveB\" :id \"fn-b\" :visibility public :graph-id \"body\")"),
		TEXT("Function graph 'body' must have exactly one function owner"));
	Rejects(TEXT("Illegal function parent role"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"body\" :editor-guid \"body-guid\" :role \"function\" :parent-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")\n")
		TEXT("(define-rig-function \"Solve\" :id \"fn\" :visibility public :graph-id \"body\")"),
		TEXT("Function graph 'body' must have a function-library parent"));
	Rejects(TEXT("Unowned node-contained graph"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"child\" :editor-guid \"child-guid\" :role \"node-contained\" :parent-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")"),
		TEXT("Node-contained graph 'child' must have exactly one owning node in parent graph 'root'"));
	Rejects(TEXT("Stale node-contained graph reference"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\"\n")
		TEXT("  (rig-collapse :id \"collapse\" :guid \"collapse-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph-id \"missing\"))\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")"),
		TEXT("Node 'collapse' references missing contained graph 'missing'"));
	Rejects(TEXT("Duplicate node-contained owners"),
		TEXT("(define-rig-graph :id \"root\" :editor-guid \"root-guid\" :role \"root\" :parent-id \"\"\n")
		TEXT("  (rig-collapse :id \"collapse\" :guid \"collapse-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph-id \"child\")\n")
		TEXT("  (rig-aggregate :id \"aggregate\" :guid \"aggregate-guid\" :class \"/Script/RigVMDeveloper.RigVMAggregateNode\" :contained-graph-id \"child\"))\n")
		TEXT("(define-rig-graph :id \"child\" :editor-guid \"child-guid\" :role \"node-contained\" :parent-id \"root\")\n")
		TEXT("(define-rig-entry \"ForwardSolve\" :id \"entry\" :event \"Forward Solve\" :graph-id \"root\")"),
		TEXT("Node-contained graph 'child' must have exactly one owning node in parent graph 'root'"));
	Rejects(TEXT("Contained graph parent back-reference"),
		TEXT("(define-rig-graph :id \"root-a\" :editor-guid \"root-a-guid\" :role \"root\" :parent-id \"\"\n")
		TEXT("  (rig-collapse :id \"collapse\" :guid \"collapse-guid\" :class \"/Script/RigVMDeveloper.RigVMCollapseNode\" :contained-graph-id \"child\"))\n")
		TEXT("(define-rig-graph :id \"root-b\" :editor-guid \"root-b-guid\" :role \"root\" :parent-id \"\")\n")
		TEXT("(define-rig-graph :id \"child\" :editor-guid \"child-guid\" :role \"node-contained\" :parent-id \"root-b\")\n")
		TEXT("(define-rig-entry \"EventA\" :id \"entry-a\" :event \"Event A\" :graph-id \"root-a\")\n")
		TEXT("(define-rig-entry \"EventB\" :id \"entry-b\" :event \"Event B\" :graph-id \"root-b\")"),
		TEXT("Node 'collapse' contained graph 'child' must name 'root-a' as its parent"));
	Rejects(TEXT("Graph parent cycle"),
		TEXT("(define-rig-graph :id \"a\" :editor-guid \"a-guid\" :role \"node-contained\" :parent-id \"b\")\n")
		TEXT("(define-rig-graph :id \"b\" :editor-guid \"b-guid\" :role \"node-contained\" :parent-id \"a\")"),
		TEXT("Graph parent cycle: a -> b -> a"));
	TArray<FRigLangParseError> DisjointCycleErrors;
	Parse(
		TEXT("(define-rig-graph :id \"a\" :editor-guid \"a-guid\" :role \"node-contained\" :parent-id \"b\")\n")
		TEXT("(define-rig-graph :id \"b\" :editor-guid \"b-guid\" :role \"node-contained\" :parent-id \"a\")\n")
		TEXT("(define-rig-graph :id \"c\" :editor-guid \"c-guid\" :role \"node-contained\" :parent-id \"d\")\n")
		TEXT("(define-rig-graph :id \"d\" :editor-guid \"d-guid\" :role \"node-contained\" :parent-id \"c\")"),
		DisjointCycleErrors);
	TestTrue(TEXT("First disjoint graph parent cycle is diagnosed"),
		HasExact(DisjointCycleErrors, TEXT("Graph parent cycle: a -> b -> a")));
	TestTrue(TEXT("Second disjoint graph parent cycle is diagnosed"),
		HasExact(DisjointCycleErrors, TEXT("Graph parent cycle: c -> d -> c")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicatePinPathTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicatePinPath",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicatePinPathTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"DuplicatePins\" :id \"function-duplicate-pins\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"node-a\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\" :method \"Execute\" :event \"\" :injected false :coverage exact\n")
		+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"float\" :cpp-type-object \"\" :container-type \"None\" :default \"0.0\" :execute-context false)\n")
		+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"float\" :cpp-type-object \"\" :container-type \"None\" :default \"1.0\" :execute-context false)))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicatePin.riglang"), TEXT("Duplicate pin path"), 5, 6);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserUnbalancedFormTest,
	"AnimBP2FP.RigLang.Parser.RejectsUnbalancedForm",
	RigLangParserTests::TestFlags)

bool FRigLangParserUnbalancedFormTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n  (bone :id \"bone-root\" :name \"root\" :parent \"\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("Unbalanced.riglang"), TEXT("Unbalanced"), 2, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserUnsupportedTopLevelTest,
	"AnimBP2FP.RigLang.Parser.RejectsUnsupportedTopLevel",
	RigLangParserTests::TestFlags)

bool FRigLangParserUnsupportedTopLevelTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-magic :connected-to \"node-a.Result\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("UnsupportedTopLevel.riglang"), TEXT("Unsupported top-level form"), 2, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateNodeStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateNodeStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateNodeStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"DuplicateNodes\" :id \"function-duplicate-nodes\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"node-a\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\")\n")
		+ TEXT("  (rig-unit :id \"node-a\" :guid \"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\" :class \"/Script/ControlRig.RigUnit_Test\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateNodeId.riglang"), TEXT("Duplicate node stable ID"), 4, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateHierarchyStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateHierarchyStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateHierarchyStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n")
		+ TEXT("  (bone :id \"element-a\" :name \"root\" :parent \"\")\n")
		+ TEXT("  (null :id \"element-a\" :name \"RootNull\" :parent \"root\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateHierarchyId.riglang"), TEXT("Duplicate hierarchy stable ID"), 4, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateVariableStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateVariableStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateVariableStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-variables\n")
		+ TEXT("  (variable :id \"variable-a\" :name \"First\" :access internal :cpp-type \"float\")\n")
		+ TEXT("  (variable :id \"variable-a\" :name \"Second\" :access internal :cpp-type \"float\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateVariableId.riglang"), TEXT("Duplicate variable stable ID"), 4, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateFunctionStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateFunctionStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateFunctionStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"First\" :id \"function-a\" :visibility internal)\n")
		+ TEXT("(define-rig-function \"Second\" :id \"function-a\" :visibility internal)");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateFunctionId.riglang"), TEXT("Duplicate function stable ID"), 3, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateEntryStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateEntryStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateEntryStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-entry \"First\" :id \"entry-a\" :event \"First\")\n")
		+ TEXT("(define-rig-entry \"Second\" :id \"entry-a\" :event \"Second\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateEntryId.riglang"), TEXT("Duplicate entry stable ID"), 3, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserAmbiguousNodeStableIdTest,
	"AnimBP2FP.RigLang.Parser.RejectsAmbiguousNodeStableId",
	RigLangParserTests::TestFlags)

bool FRigLangParserAmbiguousNodeStableIdTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"AmbiguousNode\" :id \"function-ambiguous-node\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"a.b\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("AmbiguousNodeId.riglang"), TEXT("must not contain '.'"), 3, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserStructuredLinkRoundTripTest,
	"AnimBP2FP.RigLang.Parser.StructuredLinkRoundTrip",
	RigLangParserTests::TestFlags)

bool FRigLangParserStructuredLinkRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"LinkGraph\" :id \"function-link-graph\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"node-a\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\")\n")
		+ TEXT("  (rig-call :id \"node-b\" :guid \"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\" :function \"CallTarget\" :class \"\")\n")
		+ TEXT("  (rig-link :from \"node-a.Result.X\" :to \"node-b.Input.Y\"))");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(Source, TEXT("StructuredLink.riglang"), Errors);
	TestFalse(TEXT("Structured link parses without hard errors"),
		Errors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; }));
	TestTrue(TEXT("Structured link fixture reports only expected legacy graph warnings"),
		!Errors.IsEmpty() && !Errors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return !RigLangParserTests::IsExpectedLegacyGraphWarning(Error);
		}));
	if (!TestNotNull(TEXT("Structured link produces a module"), Module.Get()))
	{
		return false;
	}

	const FString Canonical = Module->ToCanonicalString();
	TestTrue(TEXT("Canonical link uses :from"), Canonical.Contains(TEXT(":from \"node-a.Result.X\"")));
	TestTrue(TEXT("Canonical link uses :to"), Canonical.Contains(TEXT(":to \"node-b.Input.Y\"")));
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(Canonical, TEXT("StructuredLink.canonical.riglang"), ReparseErrors);
	TestFalse(TEXT("Canonical structured link reparses without hard errors"),
		ReparseErrors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; }));
	TestTrue(TEXT("Canonical structured link retains only the expected migration warning"),
		!ReparseErrors.IsEmpty() && !ReparseErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return !RigLangParserTests::IsExpectedLegacyGraphWarning(Error);
		}));
	return TestNotNull(TEXT("Canonical structured link produces a module"), Reparsed.Get())
		&& TestTrue(TEXT("Structured link semantics survive roundtrip"), Module->SemanticEquals(*Reparsed));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserImportUnknownPropertyRoundTripTest,
	"AnimBP2FP.RigLang.Parser.ImportUnknownPropertyRoundTrip",
	RigLangParserTests::TestFlags)

bool FRigLangParserImportUnknownPropertyRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(import-rig :asset \"/Game/Test/CR_Future\" :alias Future :content-hash \"future-hash\" :capability (future (nested true)))");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(Source, TEXT("ImportFuture.riglang"), Errors);
	TestTrue(TEXT("Import with balanced future property parses"), Errors.IsEmpty());
	if (!TestNotNull(TEXT("Import with balanced future property produces a module"), Module.Get()))
	{
		return false;
	}

	const FString Canonical = Module->ToCanonicalString();
	TestTrue(
		TEXT("Canonical import preserves balanced future property"),
		Canonical.Contains(TEXT(":capability (future (nested true))")));
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(
		Canonical,
		TEXT("ImportFuture.canonical.riglang"),
		ReparseErrors);
	TestTrue(TEXT("Canonical future import reparses"), ReparseErrors.IsEmpty());
	return TestNotNull(TEXT("Canonical future import produces a module"), Reparsed.Get())
		&& TestTrue(TEXT("Future import semantics survive roundtrip"), Module->SemanticEquals(*Reparsed));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewGraphTruthSourceTest,
	"AnimBP2FP.RigLang.Review.A.GraphTruthSource",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewGraphTruthSourceTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-graph :id \"root\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\")\n")
		+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"ForwardsSolve\" :graph-id \"root\"\n")
		+ TEXT("  (rig-unit :id \"legacy-inline\" :guid \"22222222-2222-2222-2222-222222222222\" :class \"/Script/ControlRig.RigUnit_Test\"))");
	const bool bRejectedMixedTruth = RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("ReviewGraphTruthSource.riglang"), TEXT("legacy inline graph"), 3, 2);

	const FString LegacySource = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"Legacy\" :id \"legacy\" :visibility internal :return-cpp-type \"void\"\n")
		+ TEXT("  (rig-unit :id \"legacy-inline\" :guid \"22222222-2222-2222-2222-222222222222\" :class \"/Script/ControlRig.RigUnit_Test\"))");
	TArray<FRigLangParseError> LegacyErrors;
	const TSharedPtr<FRigModuleAST> Legacy = FRigLangParser::Parse(
		LegacySource, TEXT("ReviewLegacyMigration.riglang"), LegacyErrors);
	TestTrue(TEXT("Legacy-only inline graph migrates without hard errors"), Legacy.IsValid()
		&& !LegacyErrors.ContainsByPredicate([](const FRigLangParseError& Error) { return !Error.bWarning; }));
	if (!Legacy.IsValid()) return false;
	TestEqual(TEXT("Legacy function migration creates library and function inventory"), Legacy->Graphs.Num(), 2);
	TestTrue(TEXT("Legacy function retains only graph reference"), Legacy->Functions[0].Graph.Nodes.IsEmpty()
		&& !Legacy->Functions[0].GraphStableId.IsEmpty());
	const FString Canonical = Legacy->ToCanonicalString();
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(
		Canonical, TEXT("ReviewLegacyMigration.canonical.riglang"), ReparseErrors);
	TestTrue(TEXT("Migrated canonical inventory reparses without diagnostics"), Reparsed.IsValid() && ReparseErrors.IsEmpty());
	return bRejectedMixedTruth && Reparsed.IsValid()
		&& TestEqual(TEXT("Migrated canonical inventory reaches a fixed point"), Reparsed->ToCanonicalString(), Canonical);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewMetadataKindPrinterTest,
	"AnimBP2FP.RigLang.Review.B.MetadataKindPrinter",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewMetadataKindPrinterTest::RunTest(const FString& Parameters)
{
	FRigModuleAST Module;
	Module.Header.ModuleId = FAnimLispModuleId(EAnimLispModuleKind::Rig, TEXT("/Game/Test/MetadataKinds"));
	Module.Header.AssetClassPath = TEXT("/Script/ControlRig.ControlRigBlueprint");
	Module.Header.Version = 1;
	Module.Header.ContentHash = TEXT("metadata-kinds");
	FRigHierarchyElementAST& Element = Module.Hierarchy.AddDefaulted_GetRef();
	Element.Kind = ERigHierarchyElementKind::Null;
	Element.StableId = TEXT("Null:Metadata");
	Element.Name = TEXT("Metadata");

	for (int32 KindIndex = 0; KindIndex < 20; ++KindIndex)
	{
		FRigHierarchyMetadataAST& Metadata = Element.Metadata.AddDefaulted_GetRef();
		Metadata.Name = FString::Printf(TEXT("Kind%02d"), KindIndex);
		Metadata.Kind = static_cast<ERigHierarchyMetadataValueKind>(KindIndex);
		const bool bArrayKind = KindIndex >= static_cast<int32>(ERigHierarchyMetadataValueKind::BoolArray);
		if (!bArrayKind)
		{
			switch (Metadata.Kind)
			{
			case ERigHierarchyMetadataValueKind::Bool: Metadata.BoolValues.Add(true); break;
			case ERigHierarchyMetadataValueKind::Integer: Metadata.IntegerValues.Add(7); break;
			case ERigHierarchyMetadataValueKind::Float: Metadata.NumberValues.Add(1.25); break;
			case ERigHierarchyMetadataValueKind::Name:
			case ERigHierarchyMetadataValueKind::ElementKey: Metadata.StringValues.Add(TEXT("Value")); break;
			case ERigHierarchyMetadataValueKind::Vector: Metadata.VectorValues.Add(FVector(1, 2, 3)); break;
			case ERigHierarchyMetadataValueKind::Rotator: Metadata.RotatorValues.Add(FRotator(1, 2, 3)); break;
			case ERigHierarchyMetadataValueKind::Quat: Metadata.QuatValues.Add(FQuat::Identity); break;
			case ERigHierarchyMetadataValueKind::Transform: Metadata.TransformValues.Add(FTransform::Identity); break;
			case ERigHierarchyMetadataValueKind::LinearColor: Metadata.ColorValues.Add(FLinearColor::White); break;
			default: break;
			}
		}
	}

	const FString Canonical = Module.ToCanonicalString();
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(
		Canonical, TEXT("ReviewMetadataKinds.riglang"), Errors);
	for (const FRigLangParseError& Error : Errors) AddError(TEXT("Metadata kind parse diagnostic: ") + Error.ToString());
	TestTrue(TEXT("All metadata kinds, including empty arrays, reparse"), Reparsed.IsValid() && Errors.IsEmpty());
	return Reparsed.IsValid()
		&& TestEqual(TEXT("All metadata kinds survive roundtrip"), Reparsed->Hierarchy[0].Metadata.Num(), 20)
		&& TestEqual(TEXT("Metadata canonical text reaches a fixed point"), Reparsed->ToCanonicalString(), Canonical);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewHierarchyStateSchemaTest,
	"AnimBP2FP.RigLang.Review.C.HierarchyStateSchema",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewHierarchyStateSchemaTest::RunTest(const FString& Parameters)
{
	auto ParseState = [](const FString& State, TArray<FRigLangParseError>& Errors)
	{
		const FString Source = RigLangParserTests::MakeModuleHeader()
			+ TEXT("\n(rig-hierarchy\n  (control :id \"Control:C\" :name \"C\" :parent \"\"\n    ")
			+ State + TEXT("))\n");
		return FRigLangParser::Parse(Source, TEXT("ReviewHierarchyState.riglang"), Errors);
	};
	TArray<FRigLangParseError> RoleErrors;
	TestFalse(TEXT("Bone type rejects current role"),
		ParseState(TEXT("(rig-state :kind bone-type :role current :type Imported)"), RoleErrors).IsValid());
	TArray<FRigLangParseError> PayloadErrors;
	TestFalse(TEXT("Control value rejects mutually exclusive payloads"),
		ParseState(TEXT("(rig-state :kind control-value :role initial :type Float :number 1 :integer 1)"), PayloadErrors).IsValid());
	TArray<FRigLangParseError> ArityErrors;
	TestFalse(TEXT("Transform control value rejects wrong component arity"),
		ParseState(TEXT("(rig-state :kind control-value :role initial :type Transform :components (1 2 3))"), ArityErrors).IsValid());
	TestTrue(TEXT("Transform control value reports exact required arity"), ArityErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{ return Error.Message.Contains(TEXT("requires 10 components")); }));

	const FString DuplicateSource = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n  (curve :id \"Curve:C\" :name \"C\" :parent \"\"\n")
		+ TEXT("    (rig-state :kind curve :role initial :type Float :number 0 :bool false)\n")
		+ TEXT("    (rig-state :kind curve :role initial :type Float :number 1 :bool true)))\n");
	TArray<FRigLangParseError> DuplicateErrors;
	TestFalse(TEXT("Duplicate state kind-role pair is rejected"),
		FRigLangParser::Parse(DuplicateSource, TEXT("ReviewDuplicateState.riglang"), DuplicateErrors).IsValid());

	FRigModuleAST HashModule;
	HashModule.Header.ModuleId = FAnimLispModuleId(EAnimLispModuleKind::Rig, TEXT("/Game/Test/StateHash"));
	HashModule.Header.AssetClassPath = TEXT("/Script/ControlRig.ControlRigBlueprint");
	HashModule.Header.Version = 1;
	FRigHierarchyElementAST& Element = HashModule.Hierarchy.AddDefaulted_GetRef();
	Element.Kind = ERigHierarchyElementKind::Control;
	Element.StableId = TEXT("Control:C");
	Element.Name = TEXT("C");
	FRigHierarchyStateAST& State = Element.States.AddDefaulted_GetRef();
	State.Kind = ERigHierarchyStateKind::PreferredEuler;
	State.Role = TEXT("concurrent");
	State.Type = TEXT("XYZ");
	State.Components = { 1, 2, 3 };
	TestTrue(TEXT("Non-current role spelling remains in hash input"),
		HashModule.ToCanonicalHashInput().Contains(TEXT(":role concurrent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewVisibilityEnumTest,
	"AnimBP2FP.RigLang.Review.D.VisibilityEnum",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewVisibilityEnumTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"Typo\" :id \"typo\" :visibility publci :return-cpp-type \"void\")");
	const bool bParserRejected = RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("ReviewVisibility.riglang"), TEXT("invalid visibility"), 2, 2);
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("ReviewVisibility.riglang"), Source);
	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Workspace fails closed for invalid Rig visibility"), Workspace.Build(Diagnostics));
	return bParserRejected;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewLocalVariableDescriptionTest,
	"AnimBP2FP.RigLang.Review.E.LocalVariableDescription",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewLocalVariableDescriptionTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-graph :id \"root\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\"\n")
		+ TEXT("  (rig-local-variable :guid \"22222222-2222-2222-2222-222222222222\" :name \"Local\" :cpp-type \"FVector\"")
		+ TEXT(" :cpp-type-object \"/Script/CoreUObject.Vector\" :cpp-type-object-path \"/Script/CoreUObject.Vector\"")
		+ TEXT(" :container-type \"\" :default \"(X=1,Y=2,Z=3)\" :category \"LOCTEXT(\\\"CategoryKey\\\", \\\"Category\\\")\"")
		+ TEXT(" :tooltip \"LOCTEXT(\\\"TooltipKey\\\", \\\"Tooltip\\\")\" :exposed-on-spawn true :expose-to-cinematics true :public true :private false))\n")
		+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"ForwardsSolve\" :graph-id \"root\")");
	TArray<FRigLangParseError> Errors;
	const TSharedPtr<FRigModuleAST> Module = FRigLangParser::Parse(Source, TEXT("ReviewLocalVariable.riglang"), Errors);
	for (const FRigLangParseError& Error : Errors) AddError(TEXT("Local variable parse diagnostic: ") + Error.ToString());
	TestTrue(TEXT("Complete local variable description parses"), Module.IsValid() && Errors.IsEmpty());
	if (!Module.IsValid()) return false;
	const FRigGraphVariableAST& Local = Module->Graphs[0].LocalVariables[0];
	TestEqual(TEXT("Local CPP type object path is exact"), Local.CPPTypeObjectPath, FString(TEXT("/Script/CoreUObject.Vector")));
	TestEqual(TEXT("Local category serialization is exact"), Local.Category, FString(TEXT("LOCTEXT(\"CategoryKey\", \"Category\")")));
	TestEqual(TEXT("Local tooltip serialization is exact"), Local.Tooltip, FString(TEXT("LOCTEXT(\"TooltipKey\", \"Tooltip\")")));
	TestTrue(TEXT("Local flags are exact"), Local.bExposedOnSpawn && Local.bExposeToCinematics && Local.bPublic && !Local.bPrivate);
	const FString Canonical = Module->ToCanonicalString();
	TestTrue(TEXT("Local category is canonical"), Canonical.Contains(TEXT(":category \"LOCTEXT(")));
	TestTrue(TEXT("Local tooltip is canonical"), Canonical.Contains(TEXT(":tooltip \"LOCTEXT(")));
	TestTrue(TEXT("Local reconstruction flags are canonical"),
		Canonical.Contains(TEXT(":exposed-on-spawn true :expose-to-cinematics true :public true :private false")));
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(
		Canonical, TEXT("ReviewLocalVariable.canonical.riglang"), ReparseErrors);
	TestTrue(TEXT("Complete local variable canonical text reparses"), Reparsed.IsValid() && ReparseErrors.IsEmpty());
	if (Reparsed.IsValid())
		TestEqual(TEXT("Complete local variable canonical text reaches a fixed point"), Reparsed->ToCanonicalString(), Canonical);

	const FString DuplicateSource = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-graph :id \"root\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\"\n")
		+ TEXT("  (rig-local-variable :guid \"22222222-2222-2222-2222-222222222222\" :name \"Local\" :cpp-type \"float\")\n")
		+ TEXT("  (rig-local-variable :guid \"22222222-2222-2222-2222-222222222222\" :name \"Other\" :cpp-type \"float\")\n")
		+ TEXT("  (rig-local-variable :guid \"33333333-3333-3333-3333-333333333333\" :name \"Local\" :cpp-type \"float\")))");
	TArray<FRigLangParseError> DuplicateErrors;
	TestFalse(TEXT("Duplicate local GUID and name are rejected"),
		FRigLangParser::Parse(DuplicateSource, TEXT("ReviewDuplicateLocals.riglang"), DuplicateErrors).IsValid());
	TestTrue(TEXT("Duplicate local GUID is diagnosed"), DuplicateErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{ return Error.Message.Contains(TEXT("Duplicate local variable GUID")); }));
	TestTrue(TEXT("Duplicate local name is diagnosed"), DuplicateErrors.ContainsByPredicate([](const FRigLangParseError& Error)
		{ return Error.Message.Contains(TEXT("Duplicate local variable name")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewControlStateConsistencyTest,
	"AnimBP2FP.RigLang.Review.F.ControlStateConsistency",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewControlStateConsistencyTest::RunTest(const FString& Parameters)
{
	auto ParseControl = [](const FString& States, TArray<FRigLangParseError>& Errors)
	{
		const FString Source = RigLangParserTests::MakeModuleHeader()
			+ TEXT("\n(rig-hierarchy\n  (control :id \"Control:C\" :name \"C\" :parent \"\"\n")
			+ States + TEXT("))\n");
		return FRigLangParser::Parse(Source, TEXT("ReviewControlConsistency.riglang"), Errors);
	};
	const FString BoolSettings = RigLangParserTests::QuoteRigLangString(
		RigLangParserTests::ExportControlSettings(ERigControlType::Bool));

	TArray<FRigLangParseError> SettingsTypeErrors;
	TestFalse(TEXT("Serialized ControlType must match control-settings state type"), ParseControl(
		TEXT("    (rig-state :kind control-settings :role initial :type Float :serialized ")
		+ BoolSettings + TEXT(")\n"), SettingsTypeErrors).IsValid());

	TArray<FRigLangParseError> ValueTypeErrors;
	TestFalse(TEXT("All control-value roles must use one control type"), ParseControl(
		TEXT("    (rig-state :kind control-settings :role initial :type Bool :serialized ") + BoolSettings + TEXT(")\n")
		+ TEXT("    (rig-state :kind control-value :role current :type Bool :bool false)\n")
		+ TEXT("    (rig-state :kind control-value :role initial :type Transform :components (0 0 0 0 0 0 1 1 1 1))\n"),
		ValueTypeErrors).IsValid());

	TArray<FRigLangParseError> EulerErrors;
	TestFalse(TEXT("Preferred Euler initial and current must use one rotation order"), ParseControl(
		TEXT("    (rig-state :kind control-settings :role initial :type Bool :serialized ") + BoolSettings + TEXT(")\n")
		+ TEXT("    (rig-state :kind preferred-euler :role current :type XYZ :components (0 0 0))\n")
		+ TEXT("    (rig-state :kind preferred-euler :role initial :type ZYX :components (0 0 0))\n"),
		EulerErrors).IsValid());

	TArray<FRigLangParseError> MissingSettingsErrors;
	TestFalse(TEXT("Control element requires exactly one control-settings state"), ParseControl(
		TEXT("    (rig-state :kind control-value :role current :type Bool :bool false)\n"),
		MissingSettingsErrors).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewMetadataFieldExclusivityTest,
	"AnimBP2FP.RigLang.Review.G.MetadataFieldExclusivity",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewMetadataFieldExclusivityTest::RunTest(const FString& Parameters)
{
	auto ParseMetadata = [](const FString& Metadata, TArray<FRigLangParseError>& Errors)
	{
		const FString Source = RigLangParserTests::MakeModuleHeader()
			+ TEXT("\n(rig-hierarchy\n  (null :id \"Null:N\" :name \"N\" :parent \"\"\n    ")
			+ Metadata + TEXT("))\n");
		return FRigLangParser::Parse(Source, TEXT("ReviewMetadataFields.riglang"), Errors);
	};
	TArray<FRigLangParseError> ScalarErrors;
	TestFalse(TEXT("Scalar metadata rejects an extra empty value field"), ParseMetadata(
		TEXT("(rig-metadata :name \"Scalar\" :kind bool :bools (true) :integers ())"), ScalarErrors).IsValid());
	TArray<FRigLangParseError> ArrayErrors;
	TestFalse(TEXT("Array metadata rejects an extra empty value field"), ParseMetadata(
		TEXT("(rig-metadata :name \"Array\" :kind bool-array :bools () :numbers ())"), ArrayErrors).IsValid());
	TArray<FRigLangParseError> ValidErrors;
	TestTrue(TEXT("Matching metadata array field may be empty"), ParseMetadata(
		TEXT("(rig-metadata :name \"Array\" :kind bool-array :bools ())"), ValidErrors).IsValid()
		&& ValidErrors.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserReviewLocalIdentityNormalizationTest,
	"AnimBP2FP.RigLang.Review.H.LocalIdentityNormalization",
	RigLangParserTests::TestFlags)

bool FRigLangParserReviewLocalIdentityNormalizationTest::RunTest(const FString& Parameters)
{
	auto ParseLocals = [](const FString& Locals, TArray<FRigLangParseError>& Errors)
	{
		const FString Source = RigLangParserTests::MakeModuleHeader()
			+ TEXT("\n(define-rig-graph :id \"root\" :editor-guid \"11111111-1111-1111-1111-111111111111\" :role \"root\" :parent-id \"\"\n")
			+ Locals + TEXT(")\n")
			+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"ForwardsSolve\" :graph-id \"root\")");
		return FRigLangParser::Parse(Source, TEXT("ReviewLocalIdentity.riglang"), Errors);
	};
	TArray<FRigLangParseError> CanonicalErrors;
	const TSharedPtr<FRigModuleAST> Canonical = ParseLocals(
		TEXT("  (rig-local-variable :guid \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\" :name \"Local\" :cpp-type \"float\")\n"),
		CanonicalErrors);
	TestTrue(TEXT("Valid uppercase local GUID parses"), Canonical.IsValid());
	if (Canonical.IsValid()) TestTrue(TEXT("Local GUID canonicalizes to lower hyphen form"),
		Canonical->ToCanonicalString().Contains(TEXT(":guid \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"")));

	for (const FString& InvalidGuid : { FString(TEXT("not-a-guid")), FString(TEXT("00000000-0000-0000-0000-000000000000")) })
	{
		TArray<FRigLangParseError> Errors;
		TestFalse(TEXT("Invalid or zero local GUID is rejected"), ParseLocals(
			TEXT("  (rig-local-variable :guid \"") + InvalidGuid + TEXT("\" :name \"Local\" :cpp-type \"float\")\n"), Errors).IsValid());
	}

	TArray<FRigLangParseError> DuplicateGuidErrors;
	TestFalse(TEXT("Parsed GUID identity rejects differently-cased duplicate text"), ParseLocals(
		FString(TEXT("  (rig-local-variable :guid \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\" :name \"Local\" :cpp-type \"float\")\n"))
		+ TEXT("  (rig-local-variable :guid \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\" :name \"Other\" :cpp-type \"float\")\n"),
		DuplicateGuidErrors).IsValid());

	TArray<FRigLangParseError> DuplicateNameErrors;
	TestFalse(TEXT("FName identity rejects differently-cased duplicate local names"), ParseLocals(
		FString(TEXT("  (rig-local-variable :guid \"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\" :name \"Local\" :cpp-type \"float\")\n"))
		+ TEXT("  (rig-local-variable :guid \"11111111-2222-3333-4444-555555555555\" :name \"local\" :cpp-type \"float\")\n"),
		DuplicateNameErrors).IsValid());

	for (const FString& InvalidName : { FString(), FString(TEXT("None")) })
	{
		TArray<FRigLangParseError> Errors;
		TestFalse(TEXT("Empty or None local FName is rejected"), ParseLocals(
			TEXT("  (rig-local-variable :guid \"11111111-2222-3333-4444-555555555555\" :name \"")
			+ InvalidName + TEXT("\" :cpp-type \"float\")\n"), Errors).IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateModuleKeywordTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateModuleKeyword",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateModuleKeywordTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT("(rig-module :asset \"/Game/A\" :asset \"/Game/B\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"hash\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateModuleKeyword.riglang"), TEXT("Duplicate property :asset"), 1, 30);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateNodeUnknownKeywordTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateNodeUnknownKeyword",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateNodeUnknownKeywordTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"DuplicateNodeProperty\" :id \"function-duplicate-node-property\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"node-a\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\"\n")
		+ TEXT("    :future (first true)\n")
		+ TEXT("    :future (second true))\n")
		+ TEXT(")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateNodeKeyword.riglang"), TEXT("Duplicate property :future"), 5, 5);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserRawValueNestingLimitTest,
	"AnimBP2FP.RigLang.Parser.RejectsExcessiveRawValueNesting",
	RigLangParserTests::TestFlags)

bool FRigLangParserRawValueNestingLimitTest::RunTest(const FString& Parameters)
{
	FString NestedValue;
	for (int32 Depth = 0; Depth < 300; ++Depth) NestedValue += TEXT("(");
	NestedValue += TEXT("true");
	for (int32 Depth = 0; Depth < 300; ++Depth) NestedValue += TEXT(")");

	const FString Source = TEXT("(rig-module\n  :future ") + NestedValue
		+ TEXT("\n  :asset \"/Game/Test/CR_DeepRaw\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"deep-raw\")");
	return RigLangParserTests::ExpectParseError(
		*this,
		Source,
		TEXT("DeepRaw.riglang"),
		TEXT("Maximum RigLang nesting depth"),
		2,
		267);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserPinNestingLimitTest,
	"AnimBP2FP.RigLang.Parser.RejectsExcessivePinNesting",
	RigLangParserTests::TestFlags)

bool FRigLangParserPinNestingLimitTest::RunTest(const FString& Parameters)
{
	FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"DeepPins\" :id \"function-deep-pins\" :visibility internal\n")
		+ TEXT("  (rig-unit :id \"node-deep-pins\" :guid \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" :class \"/Script/ControlRig.RigUnit_Test\"");
	for (int32 Depth = 0; Depth < 300; ++Depth)
	{
		Source += FString::Printf(
			TEXT("\n    (pin :path \"P%d\" :direction input :cpp-type \"float\""),
			Depth);
	}
	for (int32 Depth = 0; Depth < 300; ++Depth) Source += TEXT(")");
	Source += TEXT("))");

	return RigLangParserTests::ExpectParseError(
		*this,
		Source,
		TEXT("DeepPins.riglang"),
		TEXT("Maximum RigLang nesting depth"),
		260,
		6);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateHierarchyNameTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateHierarchyName",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateHierarchyNameTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-hierarchy\n")
		+ TEXT("  (bone :id \"bone-root\" :name \"SharedName\" :parent \"\")\n")
		+ TEXT("  (null :id \"null-root\" :name \"SharedName\" :parent \"root\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateHierarchyName.riglang"), TEXT("Duplicate hierarchy name"), 4, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateImportAliasTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateImportAlias",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateImportAliasTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(import-rig :asset \"/Game/Test/CR_First\" :alias Shared :content-hash \"first\")\n")
		+ TEXT("(import-rig :asset \"/Game/Test/CR_Second\" :alias Shared :content-hash \"second\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateImportAlias.riglang"), TEXT("Duplicate import alias"), 3, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateVariableNameTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateVariableName",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateVariableNameTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(rig-variables\n")
		+ TEXT("  (variable :id \"variable-first\" :name \"SharedName\" :access internal :cpp-type \"float\")\n")
		+ TEXT("  (variable :id \"variable-second\" :name \"SharedName\" :access internal :cpp-type \"float\"))");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateVariableName.riglang"), TEXT("Duplicate variable name"), 4, 4);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateFunctionNameTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateFunctionName",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateFunctionNameTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"SharedName\" :id \"function-first\" :visibility internal)\n")
		+ TEXT("(define-rig-function \"SharedName\" :id \"function-second\" :visibility internal)");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateFunctionName.riglang"), TEXT("Duplicate function name"), 3, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserDuplicateEntryNameTest,
	"AnimBP2FP.RigLang.Parser.RejectsDuplicateEntryName",
	RigLangParserTests::TestFlags)

bool FRigLangParserDuplicateEntryNameTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-entry \"SharedName\" :id \"entry-first\" :event \"First\")\n")
		+ TEXT("(define-rig-entry \"SharedName\" :id \"entry-second\" :event \"Second\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("DuplicateEntryName.riglang"), TEXT("Duplicate entry name"), 3, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangParserFunctionEntryNameCollisionTest,
	"AnimBP2FP.RigLang.Parser.RejectsFunctionEntryNameCollision",
	RigLangParserTests::TestFlags)

bool FRigLangParserFunctionEntryNameCollisionTest::RunTest(const FString& Parameters)
{
	const FString Source = RigLangParserTests::MakeModuleHeader()
		+ TEXT("\n(define-rig-function \"SharedSymbol\" :id \"function-shared\" :visibility internal)\n")
		+ TEXT("(define-rig-entry \"SharedSymbol\" :id \"entry-shared\" :event \"Forwards Solve\")");
	return RigLangParserTests::ExpectParseError(
		*this, Source, TEXT("FunctionEntryCollision.riglang"), TEXT("Duplicate rig symbol name"), 3, 2);
}

#endif
