# MotionForge

Every released version of MotionForge, newest first. A release publishes **one** section of this
file — the one whose heading matches its tag — as its release notes; for an `open` plugin those
notes are posted to Discord `#releases` automatically. Write for someone who installs the plugin,
not for the commit log.

Headings are `## <x.y.z> — <date>`. Use `Added` / `Changed` / `Fixed` / `Compatibility` /
`Known issues`, only the ones that apply.

## 0.4.0 — 2026-09-22

### Added
- **Get started.** *Tools ▸ Automation Forge ▸ MotionForge* walks a new user from nothing to a first motion: where motion is made (each provider's setup, measured, with the button that moves each step on, and what it costs), who it is for (a character prepared for that provider, or one made from any skeletal mesh), and a first prompt with examples. Finished steps fold to one line. The library is the other page of the same tab.
- **Watch a take before choosing it.** The definition window opens on a stage that plays a take on the character it is for - fetched where that is free, built into a clip that is never saved. *Show as B* puts a second take beside it on the same clock; floor, in place and orbit are switches. It plays exactly as its import will: on a character with a provider rig, the preview is built on that rig and retargeted with the character's own retargeter.
- Takes are numbered and listed with when, provider and model, seed, length, cost and state - in the game, ready on disk, or made from an older recipe. *Record* shows everything a take was sent.
- **Each provider brings its own settings**, under its own names: model, seed, steps and guidance for Kimodo; prompt rewriting for Uthana. A definition keeps one set per provider, so switching and switching back restores both.
- The Generate button says what it will do and what it costs, and asks before spending money. A stopped local runner is started rather than refused.
- *Choose and import* names the montages and sequences that use the clip before replacing it.
- The Motion Character window shows, per provider, whether the character suits it, the route its clips take, and that provider's own actions - build a Kimodo rig, upload to Uthana - with *Import directly* for a provider rig that has no retargeter. The route says when clips stop on the provider rig for want of a retargeter.
- Get started and the definition window are built from boxed sections that open and close like a Details category. Numbered steps carry a badge - their number, a tick when done, orange when something needs you - and a one-line summary in the header, readable while closed.

### Changed
- **Every Generate a person presses stops for review** - the definition window, the library and the prompt timeline. Unattended generation stays available to pipelines and agents through Run Full Pipeline.
- Billing belongs to each provider. Uthana's plan and rates moved to its own settings page, carried over automatically. A pay-as-you-go rate left at zero, which meant seconds only, now starts at Uthana's published price, and every price says it is that until you set your own.
- The model and sampler settings moved from the definition into the provider's settings. Older definitions migrate when loaded; *Migrate Motion Definitions* resaves them.
- The frame rate comes from the provider; the Target Frame Rate setting is gone.
- The library prices a selection per definition, on each one's own provider, and asks before generating or importing anything that bills.
- The prompt timeline's Generate gives its real price and asks before spending; Pull asks before replacing edited beats; pose actions are disabled, with the reason, on providers that take no poses.

### Fixed
- A take whose recorded file path is stale - a duplicated definition, a moved project - is found by name in the staging folder, so it previews and imports without fetching again.
- The download estimate priced fetching a pay-as-you-go take, which is free, at the generation rate.
- Cancel leaves its definitions in review if any take finished, or failed with the reason, instead of generating forever. A job past its timeout is kept and polled less often rather than dropped.
- Poses keyed only on the prompt timeline are sent, read once per generation rather than once per take.
- Generating new takes no longer takes the clip already in the game out of the record.
- Adding or moving a pose marks earlier takes as an older recipe.
- Setting one sampler setting through an update - Update Motion Definition, or any spec carrying the old `Control` fields - no longer resets the others (seed, postprocess, beat splitting, pose type) to their defaults. Only the settings the caller set are written.
- A clip's untracked bones - fingers, in-between spine bones - are posed from the hand-pose clip only when that clip is on the same skeleton. Copied onto another skeleton, a Narrative idle stretched a CC5 character's fingers into claws and bent its spine.

### Compatibility
- Removed settings: Default Model Id, Billing Model, Rate Per Billed Second, Currency, Default Mode and Target Frame Rate. Their values were carried to where they now live.
- A definition resaved by this version keeps its model and sampler settings in the provider's settings; an older MotionForge opens it with its own defaults for those.
- Update the MotionForge add-ons with it: MotionForgeUthana 0.2.0, MotionForgeKimodo 0.5.0,
  MotionForgeQuality 0.1.2 and the toolsets. Their earlier builds were compiled against the previous
  MotionForge and are not compatible with this one.

### Known issues
- Fetched takes are still kept per project in `Saved/MotionForge`; the shared take vault is not built yet.

## 0.3.1 — 2026-09-08
- Packaging fix: the release now carries everything the register allows. `BuildPlugin`'s filter excludes `Config/` and every `public_extra` path, so earlier zips shipped without them.
- `Config/ForgeMachine.json` reaches an installed copy for the first time, so the hub's Keys and Runners pages are no longer empty for it.

## 0.3.0 — 2026-09-07
- Extract ElevenLabs and Uthana; the cores now name no vendor
- The Tools menu shows one Automation Forge section, not eight loose tools
- Every plugin now points at kovati.dev

## 0.2.2 — 2026-08-30
- Keys that belong to a person, not to a plugin
- Settings-page copy, a measured pull size, and a pod disk that is not Kimodo's
- Versions for the release

## 0.2.1 — 2026-08-29
- ForgeKeys is not a plugin any more; the hub carries the keys
- Kimodo declares where it can be rented
- The READMEs catch up with a long day

## 0.2.0 — 2026-08-29
- Apache-2.0, and a release publishes its source
- ForgeKeys: one page for every key, and no plugin needs it
- Keys move to Editor Preferences, where a key belongs
- A window for a definition, and one take by default
- The prompt timeline opens with its motion on it
- Reach the provider's own setup, without knowing what it is
- Wire provider state, so a runner started elsewhere shows up here
- Ask before offering, not after failing
- A Motion Character as a sequence, not ten equal fields
- A provider is picked, and a character without one is universal
- README: the editor surface
- A definition says which provider it will use, and whether its clip still exists
- Both asset types can be created from the Content Browser
- The library
- Every plugin descriptor agrees with its release tag, and says who made it
- A shared plugin has to be optional at build time, not only at runtime

## 0.1.0 — 2026-08-28
- Initial commit: Colony prototype, plugins and plans
- Kimodo: free local motion generation, proven end to end; rent a GPU from the editor
- Kimodo: prove the pose converter, and fix the four things that broke it
- Kimodo: send authored poses, capture constraint keys, and prove the payload against the binary
- Direct retarget rest-alignment: find and fix why the arms crossed
- Choose the retargeter per clip instead of per rebuild
- Pose asset keys work, and constraint times are harvested from a Level Sequence
- Beats are addressable, and divisible only when you want them to be
- Normalisation is off by decision, not by a missing Blender
- Provenance: a clip can say what made it
- A prompt is a timeline, so author it on one; the harvest reads a sequence we built
- A rig that arrives switched off; copy the take's pose onto the rig and let the engine solve
- Constraint types along the clip, and no generation spent filling a row
- Output roots move; the things they read from stay where they are
- Quality is its own add-on, beside the providers rather than inside core
- The hands Kimodo never generated, and a gate that can test a number
- Cost before commit, for real this time
