// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "RigLangAST.h"
#include "RigLangParser.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangParserTests
{
const EAutomationTestFlags TestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

FString MakeModuleHeader()
{
	return TEXT("(rig-module :asset \"/Game/Test/CR_Test\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"test-hash\")");
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

	const FRigLangParseError& Error = Errors[0];
	Test.TestTrue(TEXT("Error message identifies the rejected construct"), Error.Message.Contains(MessageFragment));
	Test.TestEqual(TEXT("Error source file"), Error.Location.SourceFile, SourceFile);
	Test.TestEqual(TEXT("Error source line"), Error.Location.Line, ExpectedLine);
	Test.TestEqual(TEXT("Error source column"), Error.Location.Column, ExpectedColumn);
	return true;
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
	TestTrue(TEXT("Fixture parses without errors"), FirstErrors.IsEmpty());
	if (!TestNotNull(TEXT("Fixture produces a module"), First.Get()))
	{
		return false;
	}

	const FString FirstCanonical = First->ToCanonicalString();
	TestFalse(TEXT("Canonical output is not empty"), FirstCanonical.IsEmpty());
	TestEqual(TEXT("Canonical printing is deterministic"), First->ToCanonicalString(), FirstCanonical);

	TArray<FRigLangParseError> SecondErrors;
	const TSharedPtr<FRigModuleAST> Second = FRigLangParser::Parse(
		FirstCanonical,
		TEXT("MinimalFootRig.canonical.riglang"),
		SecondErrors);
	TestTrue(TEXT("Canonical output reparses without errors"), SecondErrors.IsEmpty());
	if (!TestNotNull(TEXT("Canonical output produces a module"), Second.Get()))
	{
		return false;
	}

	TestTrue(TEXT("Parse-print-parse preserves authoritative semantics"), First->SemanticEquals(*Second));
	TestEqual(TEXT("Canonical output reaches a fixed point"), Second->ToCanonicalString(), FirstCanonical);
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
		+ TEXT("  (control :id \"element-a\" :name \"RootControl\" :parent \"root\"))");
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
	TestTrue(TEXT("Structured link parses without errors"), Errors.IsEmpty());
	if (!TestNotNull(TEXT("Structured link produces a module"), Module.Get()))
	{
		return false;
	}

	const FString Canonical = Module->ToCanonicalString();
	TestTrue(TEXT("Canonical link uses :from"), Canonical.Contains(TEXT(":from \"node-a.Result.X\"")));
	TestTrue(TEXT("Canonical link uses :to"), Canonical.Contains(TEXT(":to \"node-b.Input.Y\"")));
	TArray<FRigLangParseError> ReparseErrors;
	const TSharedPtr<FRigModuleAST> Reparsed = FRigLangParser::Parse(Canonical, TEXT("StructuredLink.canonical.riglang"), ReparseErrors);
	TestTrue(TEXT("Canonical structured link reparses"), ReparseErrors.IsEmpty());
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
		+ TEXT("  (control :id \"control-root\" :name \"SharedName\" :parent \"root\"))");
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
