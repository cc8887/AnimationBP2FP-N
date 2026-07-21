// AnimBP2FPEditor.Build.cs - Editor Module Build Configuration
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

using UnrealBuildTool;

public class AnimBP2FPEditor : ModuleRules
{
	public AnimBP2FPEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});
		
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimBP2FP",           // Runtime module
			"BlueprintLisp",       // EventGraph DSL export
			"UnrealEd",            // Editor framework
			"AnimGraph",           // Animation graph nodes
			"BlueprintGraph",      // Blueprint graph base
			"AnimationCore",
			"ControlRig",
			"ControlRigDeveloper",
			"ControlRigEditor",
			"RigVM",
			"RigVMDeveloper",
			"Slate",               // UI framework
			"SlateCore",
			"EditorStyle",
			"ToolMenus",           // Editor menus
			"DeveloperSettings",   // Project settings
			"AssetRegistry"        // Asset discovery
		});
	}
}
