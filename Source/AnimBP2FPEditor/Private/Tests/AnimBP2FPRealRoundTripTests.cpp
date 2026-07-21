// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangAST.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimBP2FPRealRoundTripTest
{
	static FString DescribeFirstDifference(const FString& Expected, const FString& Actual)
	{
		const int32 CommonLength = FMath::Min(Expected.Len(), Actual.Len());
		int32 Index = 0;
		while (Index < CommonLength && Expected[Index] == Actual[Index]) ++Index;
		const int32 ContextStart = FMath::Max(0, Index - 120);
		const int32 ContextLength = 300;
		return FString::Printf(TEXT("first difference at char %d; expected-len=%d actual-len=%d\nexpected: %s\nactual:   %s"),
			Index, Expected.Len(), Actual.Len(),
			*Expected.Mid(ContextStart, ContextLength).Replace(TEXT("\n"), TEXT("\\n")),
			*Actual.Mid(ContextStart, ContextLength).Replace(TEXT("\n"), TEXT("\\n")));
	}

	static bool RunAssetRoundTrip(FAutomationTestBase& Test, const TCHAR* SourcePath, const FName DestinationName)
	{
		UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr, SourcePath);
		Test.TestNotNull(TEXT("source AnimBlueprint loads"), Source);
		if (!Source) return false;

		const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
		Test.TestTrue(TEXT("source exports to AST"), SourceAST.IsValid());
		if (!SourceAST.IsValid()) return false;
		const FString SourceDSL = SourceAST->ToString();
		Test.TestFalse(TEXT("strict source DSL has no unsupported coverage"), SourceDSL.Contains(TEXT(":coverage unsupported")));

		UClass* ParentClass = Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass();
		UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass, GetTransientPackage(), DestinationName, BPTYPE_Normal,
			UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("AnimBP2FPRealRoundTripTest"))));
		Test.TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
		if (!Destination) return false;
		Destination->TargetSkeleton = Source->TargetSkeleton;

		const FAnimBPImporter::FUpdateResult ImportResult = FAnimBPImporter::UpdateBlueprintDetailed(Destination, SourceDSL);
		if (!ImportResult.bSuccess)
		{
			for (const FString& Warning : ImportResult.Warnings)
			{
				Test.AddError(TEXT("import: ") + Warning);
			}
		}
		Test.TestTrue(TEXT("transient DSL import and compile succeeds"), ImportResult.bSuccess);
		Test.TestEqual(TEXT("compiled blueprint status"), Destination->Status, BS_UpToDate);
		if (!ImportResult.bSuccess) return false;

		TSharedPtr<FAnimGraphAST> ReExportedAST = FAnimBPExporter::ExportToAST(Destination);
		Test.TestTrue(TEXT("transient destination re-exports"), ReExportedAST.IsValid());
		if (!ReExportedAST.IsValid()) return false;

		// External dependency snapshots are read-only source manifests and AssetRegistry has no
		// package dependency record for a transient blueprint. Normalize only those two identities.
		ReExportedAST->Name = SourceAST->Name;
		ReExportedAST->Dependencies = SourceAST->Dependencies;
		const FString ReExportedDSL = ReExportedAST->ToString();
		const bool bCanonicalMatch = SourceDSL == ReExportedDSL;
		if (!bCanonicalMatch)
		{
			const FString DumpStem = FPaths::Combine(FPaths::ProjectLogDir(), DestinationName.ToString());
			FFileHelper::SaveStringToFile(SourceDSL, *(DumpStem + TEXT("-source.dsl")));
			FFileHelper::SaveStringToFile(ReExportedDSL, *(DumpStem + TEXT("-actual.dsl")));
			Test.AddError(DescribeFirstDifference(SourceDSL, ReExportedDSL));
		}
		Test.TestTrue(TEXT("re-exported canonical AST matches source"), bCanonicalMatch);
		return bCanonicalMatch;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRealCMCAndMoverTransientRoundTrip,
	"AnimBP2FP.RealAssets.CMCAndMoverTransientRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRealCMCAndMoverTransientRoundTrip::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPRealRoundTripTest;
	const bool bCMC = RunAssetRoundTrip(*this,
		TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"),
		TEXT("ABP_CMC_TransientRoundTrip"));
	const bool bMover = RunAssetRoundTrip(*this,
		TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"),
		TEXT("ABP_Mover_TransientRoundTrip"));
	return bCMC && bMover;
}

#endif
