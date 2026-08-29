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
				"LevelSequence",   // the only sequence kind a prompt track belongs on
				"MovieScene",
				"MovieSceneTools", // FSequencerSection's painting
				"MovieSceneTracks",
				"Sequencer",       // ISequencerModule, for registering a track editor at all
				"SequencerCore",   // MakeAddButton, the + on a track's row
				"Slate",
				"SlateCore",
				"ToolMenus",       // Generate lives on Sequencer's own toolbar, which is a UToolMenu
				"UnrealEd",
			}
			);
	}
}
