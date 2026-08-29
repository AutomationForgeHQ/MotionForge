using UnrealBuildTool;

public class MotionForgeEditor : ModuleRules
{
	public MotionForgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
				"MotionForge",     // the track and section types this exists to display
				"AssetDefinition", // UAssetDefinition, which decides what double-click opens
				"AssetRegistry",   // the library hears about definitions added and removed elsewhere
				"AssetTools",      // creating a definition through the ordinary new-asset dialogue
				"LevelSequence",   // the only sequence kind a prompt track belongs on
				"MovieScene",
				"MovieSceneTools", // FSequencerSection's painting
				"MovieSceneTracks",
				"PropertyEditor",  // the details view inside the definition window
				"Sequencer",       // ISequencerModule, for registering a track editor at all
				"SequencerCore",   // MakeAddButton, the + on a track's row
				"Slate",
				"SlateCore",
				"InputCore",
				"ToolMenus",       // Generate lives on Sequencer's own toolbar, which is a UToolMenu
				"UnrealEd",
				"WorkspaceMenuStructure", // the tab category the definition window's tabs sit in
			}
			);
	}
}
