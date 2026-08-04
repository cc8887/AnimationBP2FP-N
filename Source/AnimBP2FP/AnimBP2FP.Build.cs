// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

using UnrealBuildTool;

public class AnimBP2FP : ModuleRules
{
	public AnimBP2FP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		string CppStandardName = Target.Version.MajorVersion > 5
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 5)
			? "Cpp20" : "Cpp17";
		CppStandard = (CppStandardVersion)System.Enum.Parse(typeof(CppStandardVersion), CppStandardName);

		PublicIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"RigVM",
			}
		);

		// 仅在编辑器构建中包含编辑器依赖
		if (Target.bBuildEditor)
		{
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"AnimGraph",
					"AnimGraphRuntime",
					"BlueprintGraph",
					// BlueprintLisp: EventGraph -> DSL export
					"BlueprintLisp",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"UnrealEd",
					"AssetTools",
					"AssetRegistry",
					"AnimationCore",
					"ControlRig",
					"ControlRigDeveloper",
					"ControlRigEditor",
					"RigVMDeveloper",
					"Slate",
					"SlateCore",
				}
			);
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
