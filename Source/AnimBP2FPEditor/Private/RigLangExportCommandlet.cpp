#include "RigLangExportCommandlet.h"

#include "FBP2FPMappingRegistry.h"
#include "RigLangExporter.h"
#include "RigLangParser.h"
#if ENGINE_MAJOR_VERSION < 5
#include "ControlRigBlueprint.h"
#else
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#endif
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigLangExportCommandlet, Log, All);

namespace
{
	bool IsGameAssetPath(const FString& AssetPath)
	{
		return AssetPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			&& FPackageName::IsValidLongPackageName(AssetPath, true);
	}

	FString NormalizeFullPath(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		return Result;
	}

	bool ValidateModuleSourceHash(
		const FString& Output,
		const FString& Source,
		const FString& Expected,
		FString& OutError)
	{
		FString Actual;
		if (FPaths::GetExtension(Output).Equals(TEXT("riglang"), ESearchCase::IgnoreCase))
		{
			TArray<FRigLangParseError> Errors;
			const TSharedPtr<FRigModuleAST> ModuleAST = FRigLangParser::Parse(Source, Output, Errors);
			if (!ModuleAST.IsValid() || Errors.ContainsByPredicate(
				[](const FRigLangParseError& Error) { return !Error.bWarning; }))
			{
				OutError = FString::Printf(TEXT("Rig output has parse errors: %s"), *Output);
				return false;
			}
			Actual = FRigLangExporter::ComputeContentHash(ModuleAST->ToCanonicalHashInput());
			if (Actual != ModuleAST->Header.ContentHash)
			{
				OutError = FString::Printf(TEXT("Rig header semantic hash mismatch: %s"), *Output);
				return false;
			}
		}
		else
		{
			Actual = FRigLangExporter::ComputeContentHash(Source);
		}
		if (Actual != Expected)
		{
			OutError = FString::Printf(TEXT("Bundle output hash mismatch: %s"), *Output);
			return false;
		}
		return true;
	}

	bool ValidateBundleContents(
		const FString& Json,
		const TArray<AnimBP2FPCommandlets::FBundleOutput>& Outputs,
		FString& OutError)
	{
		if (!AnimBP2FPCommandlets::ValidateBundleManifestJson(Json, OutError)) return false;
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		const TArray<TSharedPtr<FJsonValue>>& Modules = Root->GetArrayField(TEXT("modules"));
		TMap<FString, const AnimBP2FPCommandlets::FBundleOutput*> OutputsByPath;
		for (const AnimBP2FPCommandlets::FBundleOutput& Output : Outputs)
		{
			OutputsByPath.Add(NormalizeFullPath(Output.TargetPath).ToLower(), &Output);
		}
		int32 SuccessCount = 0;
		for (const TSharedPtr<FJsonValue>& Value : Modules)
		{
			const TSharedPtr<FJsonObject> Module = Value->AsObject();
			if (Module->GetStringField(TEXT("status")) != TEXT("success")) continue;
			++SuccessCount;
			const FString OutputPath = Module->GetStringField(TEXT("output_path"));
			const AnimBP2FPCommandlets::FBundleOutput* const* Planned =
				OutputsByPath.Find(NormalizeFullPath(OutputPath).ToLower());
			if (!Planned)
			{
				OutError = FString::Printf(TEXT("Bundle success output is not staged: %s"), *OutputPath);
				return false;
			}
			if (!ValidateModuleSourceHash(
				OutputPath, (*Planned)->Content,
				Module->GetStringField(TEXT("canonical_hash")), OutError))
			{
				return false;
			}
		}
		if (SuccessCount != Outputs.Num())
		{
			OutError = TEXT("Bundle contains unmanifested staged outputs");
			return false;
		}
		return true;
	}
}

bool AnimBP2FPCommandlets::ParseAssetPath(
	const FString& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath.Reset();
	if (!FParse::Value(*Params, TEXT("AssetPath="), OutAssetPath) || !IsGameAssetPath(OutAssetPath))
	{
		OutError = TEXT("-AssetPath must name one /Game package");
		return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::ParseAnimExportParams(
	const FString& Params, FString& OutAssetPath, bool& bOutIncludeRigModules, FString& OutError)
{
	bOutIncludeRigModules = FParse::Param(*Params, TEXT("IncludeRigModules"));
	return ParseAssetPath(Params, OutAssetPath, OutError);
}

bool AnimBP2FPCommandlets::ValidateAssetRoot(const FString& AssetRoot, FString& OutError)
{
	if ((AssetRoot != TEXT("/Game")
		&& !AssetRoot.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		|| !FPackageName::IsValidLongPackageName(AssetRoot, true))
	{
		OutError = FString::Printf(
			TEXT("Invalid -AssetRoot='%s'; expected /Game or a valid /Game package path"),
			*AssetRoot);
		return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::ParseWorkspace(
	const FString& Params, FString& OutWorkspace, FString& OutError)
{
	if (!FParse::Value(*Params, TEXT("Workspace="), OutWorkspace))
	{
		OutError = TEXT("-Workspace is required");
		return false;
	}
	OutWorkspace = NormalizeFullPath(OutWorkspace);
	const FString AllowedRoot = NormalizeFullPath(FPaths::ProjectSavedDir() / TEXT("BP2DSL"));
	if (!OutWorkspace.Equals(AllowedRoot, ESearchCase::IgnoreCase)
		&& !OutWorkspace.StartsWith(AllowedRoot + TEXT("/"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("-Workspace must be Saved/BP2DSL or one of its descendants");
		return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::PrepareOutputForExport(const FString& OutputPath, FString& OutError)
{
	if (OutputPath.IsEmpty())
	{
		OutError = TEXT("Output path is empty");
		return false;
	}
	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*FPaths::GetPath(OutputPath), true);
	if (Files.FileExists(*OutputPath) && !Files.Delete(*OutputPath, false, true, true))
	{
		OutError = FString::Printf(TEXT("Failed to revoke old output: %s"), *OutputPath);
		return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::WriteFileAtomically(
	const FString& OutputPath, const FString& Content, FString& OutError)
{
	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*FPaths::GetPath(OutputPath), true);
	const FString StagingPath = OutputPath + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
		Content, *StagingPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Files.Delete(*StagingPath, false, true, true);
		OutError = FString::Printf(TEXT("Failed to write staging output: %s"), *StagingPath);
		return false;
	}
	if (!Files.Move(*OutputPath, *StagingPath, true, true, false, true))
	{
		Files.Delete(*StagingPath, false, true, true);
		OutError = FString::Printf(TEXT("Failed to atomically commit output: %s"), *OutputPath);
		return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::ValidateMappedOutputPath(const FString& OutputPath, FString& OutError)
{
	const FString Output = NormalizeFullPath(OutputPath);
	const FString Root = NormalizeFullPath(FPaths::ProjectSavedDir() / TEXT("BP2DSL"));
	if (Output.IsEmpty() || (!Output.Equals(Root, ESearchCase::IgnoreCase)
		&& !Output.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)))
	{
		OutError = FString::Printf(TEXT("Mapped output escapes Saved/BP2DSL: %s"), *OutputPath);
		return false;
	}
	return true;
}

void AnimBP2FPCommandlets::CollectSafeBundleRevokePaths(
	const FString& ManifestPath,
	const FString& CurrentAnimTarget,
	TSet<FString>& OutPaths)
{
	auto AddSafe = [&OutPaths](const FString& Path)
	{
		FString Error;
		if (!Path.IsEmpty() && !FPaths::IsRelative(Path)
			&& ValidateMappedOutputPath(Path, Error))
		{
			OutPaths.Add(NormalizeFullPath(Path));
		}
	};
	AddSafe(ManifestPath);
	AddSafe(CurrentAnimTarget);

	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *ManifestPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)
		|| !Root.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!Root->TryGetArrayField(TEXT("modules"), Modules) || !Modules) return;
	for (const TSharedPtr<FJsonValue>& Value : *Modules)
	{
		const TSharedPtr<FJsonObject> Module = Value.IsValid() ? Value->AsObject() : nullptr;
		FString OutputPath;
		if (Module.IsValid() && Module->TryGetStringField(TEXT("output_path"), OutputPath))
		{
			AddSafe(OutputPath);
		}
	}
}

void AnimBP2FPCommandlets::CleanupBundleTargets(const TSet<FString>& Paths)
{
	for (const FString& Path : Paths)
	{
		FString Error;
		if (!FPaths::IsRelative(Path) && ValidateMappedOutputPath(Path, Error))
		{
			PrepareOutputForExport(Path, Error);
		}
	}
}

bool AnimBP2FPCommandlets::ValidateUniqueOutputPaths(
	const TArray<FString>& OutputPaths, FString& OutError)
{
	TSet<FString> Seen;
	for (const FString& OutputPath : OutputPaths)
	{
		const FString Key = NormalizeFullPath(OutputPath).ToLower();
		if (Seen.Contains(Key))
		{
			OutError = FString::Printf(TEXT("Duplicate output path: %s"), *OutputPath);
			return false;
		}
		Seen.Add(Key);
	}
	return true;
}

bool AnimBP2FPCommandlets::ValidateBundleManifestJson(const FString& Json, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("Bundle manifest is not valid JSON");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!Root->TryGetArrayField(TEXT("modules"), Modules) || !Modules)
	{
		OutError = TEXT("Bundle manifest requires modules");
		return false;
	}
	TArray<FString> Outputs;
	for (const TSharedPtr<FJsonValue>& Value : *Modules)
	{
		const TSharedPtr<FJsonObject> Module = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Identity, Source, Output, Hash, Status;
		const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
		const TSharedPtr<FJsonObject>* Coverage = nullptr;
		if (!Module.IsValid()
			|| !Module->TryGetStringField(TEXT("module_identity"), Identity) || Identity.IsEmpty()
			|| !Module->TryGetStringField(TEXT("source_asset"), Source) || Source.IsEmpty()
			|| !Module->TryGetStringField(TEXT("output_path"), Output) || Output.IsEmpty()
			|| !Module->TryGetStringField(TEXT("canonical_hash"), Hash) || Hash.IsEmpty()
			|| !Module->TryGetStringField(TEXT("status"), Status)
			|| (Status != TEXT("success") && Status != TEXT("failed"))
			|| !Module->TryGetArrayField(TEXT("dependencies"), Dependencies)
			|| !Module->TryGetObjectField(TEXT("coverage"), Coverage) || !Coverage || !Coverage->IsValid()
			|| !(*Coverage)->HasTypedField<EJson::Number>(TEXT("exact"))
			|| !(*Coverage)->HasTypedField<EJson::Number>(TEXT("reflected"))
			|| !(*Coverage)->HasTypedField<EJson::Number>(TEXT("lossy"))
			|| !(*Coverage)->HasTypedField<EJson::Number>(TEXT("unsupported")))
		{
			OutError = TEXT("Bundle module is missing identity/source/output/hash/dependencies/status/coverage");
			return false;
		}
		Outputs.Add(Output);
	}
	return ValidateUniqueOutputPaths(Outputs, OutError);
}

bool AnimBP2FPCommandlets::ValidateBundleOutputHashes(const FString& Json, FString& OutError)
{
	if (!ValidateBundleManifestJson(Json, OutError)) return false;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("Bundle manifest is not valid JSON");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!Root->TryGetArrayField(TEXT("modules"), Modules) || !Modules)
	{
		OutError = TEXT("Bundle manifest requires modules");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Modules)
	{
		const TSharedPtr<FJsonObject> Module = Value->AsObject();
		const FString Output = Module->GetStringField(TEXT("output_path"));
		const FString Expected = Module->GetStringField(TEXT("canonical_hash"));
		if (Module->GetStringField(TEXT("status")) != TEXT("success")) continue;
		FString Source;
		if (!FFileHelper::LoadFileToString(Source, *Output))
		{
			OutError = FString::Printf(TEXT("Bundle output is unreadable: %s"), *Output);
			return false;
		}
		if (!ValidateModuleSourceHash(Output, Source, Expected, OutError)) return false;
	}
	return true;
}

bool AnimBP2FPCommandlets::CommitBundleAtomically(
	const TArray<FBundleOutput>& Outputs,
	const FString& ManifestPath,
	const FString& ManifestContent,
	FString& OutError)

{
	return CommitBundleAtomically(Outputs, ManifestPath, ManifestContent, {}, OutError);
}

bool AnimBP2FPCommandlets::CommitBundleAtomically(
	const TArray<FBundleOutput>& Outputs,
	const FString& ManifestPath,
	const FString& ManifestContent,
	const TSet<FString>& PriorBundleTargets,
	FString& OutError)
{
	IFileManager& Files = IFileManager::Get();
	TArray<FString> CommitTargets;
	TSet<FString> CommitTargetKeys;
	for (const FBundleOutput& Output : Outputs)
	{
		CommitTargets.Add(Output.TargetPath);
		CommitTargetKeys.Add(NormalizeFullPath(Output.TargetPath).ToLower());
	}
	CommitTargets.Add(ManifestPath);
	CommitTargetKeys.Add(NormalizeFullPath(ManifestPath).ToLower());

	TArray<FString> RevokeTargets = CommitTargets;
	TSet<FString> RevokeTargetKeys = CommitTargetKeys;
	for (const FString& PriorTarget : PriorBundleTargets)
	{
		FString SafetyError;
		const FString Key = NormalizeFullPath(PriorTarget).ToLower();
		if (!FPaths::IsRelative(PriorTarget)
			&& !CommitTargetKeys.Contains(Key)
			&& ValidateMappedOutputPath(PriorTarget, SafetyError)
			&& !RevokeTargetKeys.Contains(Key))
		{
			RevokeTargets.Add(NormalizeFullPath(PriorTarget));
			RevokeTargetKeys.Add(Key);
		}
	}

	bool bRevokeFailed = false;
	for (const FString& Target : RevokeTargets)
	{
		if (Files.FileExists(*Target) && !Files.Delete(*Target, false, true, true))
		{
			if (OutError.IsEmpty()) OutError = TEXT("Failed to revoke bundle target: ") + Target;
			bRevokeFailed = true;
		}
	}
	if (bRevokeFailed) return false;
	TArray<FString> Staging;
	auto Cleanup = [&]()
	{
		for (const FString& Stage : Staging) Files.Delete(*Stage, false, true, true);
		for (const FString& Target : RevokeTargets) Files.Delete(*Target, false, true, true);
	};
	auto Stage = [&](const FString& Target, const FString& Content) -> bool
	{
		Files.MakeDirectory(*FPaths::GetPath(Target), true);
		const FString Path = Target + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		Staging.Add(Path);
		FString Readback;
		return FFileHelper::SaveStringToFile(Content, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& FFileHelper::LoadFileToString(Readback, *Path) && Readback == Content;
	};
	for (const FBundleOutput& Output : Outputs)
	{
		if (!Stage(Output.TargetPath, Output.Content)) { OutError = TEXT("Bundle staging failed"); Cleanup(); return false; }
	}
	if (!Stage(ManifestPath, ManifestContent)) { OutError = TEXT("Manifest staging failed"); Cleanup(); return false; }
	TArray<FBundleOutput> StagedOutputs;
	for (int32 Index = 0; Index < Outputs.Num(); ++Index)
	{
		FString StagedContent;
		if (!FFileHelper::LoadFileToString(StagedContent, *Staging[Index]))
		{
			OutError = TEXT("Bundle staged output became unreadable");
			Cleanup();
			return false;
		}
		StagedOutputs.Add({Outputs[Index].TargetPath, MoveTemp(StagedContent)});
	}
	FString StagedManifest;
	if (!FFileHelper::LoadFileToString(StagedManifest, *Staging.Last())
		|| !ValidateBundleContents(StagedManifest, StagedOutputs, OutError))
	{
		if (OutError.IsEmpty()) OutError = TEXT("Bundle staged manifest became unreadable");
		Cleanup();
		return false;
	}
	for (int32 Index = 0; Index < CommitTargets.Num(); ++Index)
	{
		if (!Files.Move(*CommitTargets[Index], *Staging[Index], true, true, false, true))
		{
			OutError = TEXT("Bundle commit failed");
			Cleanup();
			return false;
		}
	}
	return true;
}

URigLangExportCommandlet::URigLangExportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 URigLangExportCommandlet::Main(const FString& Params)
{
	FString AssetPath;
	FString Error;
	if (!AnimBP2FPCommandlets::ParseAssetPath(Params, AssetPath, Error))
	{
		UE_LOG(LogRigLangExportCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	const FString OutputPath = FBP2FPMappingRegistry::BlueprintToDSLPath(
		AssetPath, TEXT("Rig"), TEXT(".riglang"));
	if (!AnimBP2FPCommandlets::ValidateMappedOutputPath(OutputPath, Error)
		|| !AnimBP2FPCommandlets::PrepareOutputForExport(OutputPath, Error))
	{
		UE_LOG(LogRigLangExportCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	const FString ObjectPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
	UControlRigBlueprint* Blueprint = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	if (!Blueprint)
	{
		UE_LOG(LogRigLangExportCommandlet, Error, TEXT("Missing Control Rig asset: %s"), *AssetPath);
		return 1;
	}
	const FRigLangExportResult Export = FRigLangExporter::Export(Blueprint);
	if (!Export.bSuccess || !Export.Module.IsValid()
		|| !AnimBP2FPCommandlets::WriteFileAtomically(
			OutputPath, Export.Module->ToCanonicalString(), Error))
	{
		UE_LOG(LogRigLangExportCommandlet, Error, TEXT("Rig export failed for %s: %s"), *AssetPath, *Error);
		return 1;
	}
	UE_LOG(LogRigLangExportCommandlet, Display, TEXT("RigLang export: %s"), *OutputPath);
	return 0;
}
