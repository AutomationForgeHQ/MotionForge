# MotionForge

Describe a motion, generate it with an AI motion provider, and get a game-ready animation sequence
imported onto the skeleton you chose.

```
pair a character once  →  submit  →  poll  →  review  →  download  →  import
```

**Status: 0.2 — verified end to end against a live Uthana account on 2026-08-04.** A written prompt
produced a correct 4-second `UAnimSequence` on this project's own skeleton, checked by eye against
Uthana's web viewer.

---

## What this is not

MotionForge produces a `UAnimSequence` and stops there. It deliberately does **not** create montages,
stamp animation curves, place notifies, set up IK, or know anything about any gameplay framework.

Those are conventions belonging to whatever consumes the animation, and baking them in would tie this
plugin to one project's way of working.

**[MontageForge](../MontageForge/README.md) is the next link in the chain** — it turns these sequences
into montages with notifies, layering curves and blends. Written as a sibling rather than folded in,
for the same reason: it needs GameplayAbilities, and MotionForge needs nothing but the engine. See
[EXTENSION_PLAN.md](EXTENSION_PLAN.md) for how that split was decided.

---

## Providers are a quality and cost tier

One pipeline, several generators, chosen **per motion definition** rather than per project.

| Provider | | |
|---|---|---|
| **[Kimodo](../MotionForgeKimodo/README.md)** | free, local, seeded, seconds per take | blocking out a library, iterating on wording |
| **Uthana** | paid, your own rig, fingers included | the clips that ship |

Block out thirty motions on Kimodo for nothing, then flip the ten that survive to Uthana and
regenerate. That is one field on the asset, not a different workflow.

Nothing above the provider line assumes anything about which one it is talking to. It asks
`GetProviderCaps` — as can you, from Python, or an agent through `Get Provider Capabilities`:

```python
unreal.get_editor_subsystem(unreal.MotionForgeSubsystem).get_provider_caps("Kimodo")
```

| Capability | Why the pipeline needs it |
|---|---|
| `NativeFrameRate` | 60 on Uthana, 30 on Kimodo. **Not a global setting** — see below |
| `bIsMetered` | a free provider must estimate zero, not the lesser wrong number |
| `bSupportsSeed` | decides whether a discarded take is recoverable or gone forever |
| `bNeedsCredential` | a local provider has nothing to sign in to |
| `bSupportsConstraints`, `bSupportsPromptRewrite` | ignored rather than refused where absent |
| `SetupHint` | what a human has to do before this provider works at all |

Adding a provider is a new plugin that calls `FMotionForgeModule::RegisterProvider` at module
startup. MotionForge never learns it exists, and deleting it changes nothing.

---

## The one thing to understand

Applies to providers that accept a character. [Kimodo](../MotionForgeKimodo/README.md) does not —
it generates on its own fixed rig and the clip is mapped onto your skeleton during import, so a
Motion Character there needs only a `TargetSkeleton`.

**Upload your own character.** Everything else follows from it.

The provider generates motion against whatever character you pair with, so if that character is your
own mesh on your own skeleton, the animation comes back on bone names you already have. It then
imports one to one, with no retargeting and no conversion step.

The rig does not come back *identical* — Uthana returns 162 of this project's 361 bones, dropping
`root1`, the virtual bones and the finer correctives. That is harmless: every bone it does return
exists in the target skeleton, so it is a strict subset and nothing is animated that shouldn't be.

Retargeting is only needed for a character you did **not** upload — one of the provider's stock
characters, say — where the rigs genuinely differ. See [Retargeting](#retargeting-for-rigs-you-did-not-upload).

---

## Setup

### 1. API key

**Project Settings → Plugins → MotionForge → Credentials.**

| Field | |
|---|---|
| **Credential Provider Id** | Which provider the fields below act on. Empty means whichever provider is the default. |
| **API Key** | Paste and commit. Stored immediately, then blanked. |
| **Status** | Whether a key is available and where it is read from. |

Acting on the key happens from the **console**, not the settings page:

```
MotionForge.TestConnection      one cheap authenticated call; result under LogMotionForge
MotionForge.CredentialStatus    re-read Status, e.g. after setting an environment variable
MotionForge.ClearKey            forget the stored key
```

Each takes an optional provider id, defaulting to the one in settings.

> **Why not buttons?** `UFUNCTION(CallInEditor)` does not render on a `UDeveloperSettings` page.
> `FObjectDetails::AddCallInEditorMethods` discards objects flagged `RF_ArchetypeObject` before
> deciding whether to draw them, and a settings panel edits the CDO, which is one. Real buttons need
> an `IDetailCustomization`; until that exists the console is the honest surface rather than a
> control that silently does nothing.

The key goes into the **OS credential vault** (Windows Credential Manager, under
`MotionForge/Uthana`), never into a project file. The settings property holding it is `Transient` and
carries no `config` specifier, so it cannot reach an ini; it is cleared the moment it is handed to the
vault and cannot be read back out through the panel. Copying the project, committing it, or zipping it
cannot carry the key, because it was never inside the project directory.

**`MOTIONFORGE_UTHANA_KEY` overrides the vault when set**, for CI and headless runs. Note the
precedence: clearing the vault while that variable is set changes nothing, and the log says so rather
than letting Status contradict the action.

**Do not write the vault entry with `cmdkey` or the Credential Manager GUI.** This plugin stores the
blob as UTF-8; Windows' own tools write UTF-16LE, so a key set that way reads back garbled and fails
authentication while looking correct.

### 2. Billing model — set this before generating anything

The two plans invert the correct workflow, and getting it backwards costs real money.

| | Bills on | So you should |
|---|---|---|
| **Pay as you go** | **generated** seconds, kept or discarded | ask for few variants; downloads are free, so fetch them all |
| **Subscription** | **downloaded** seconds; generation unlimited | generate generously, review, download only the keeper |

Set `Billing Model` to match the account, and `Rate Per Billed Second` to turn estimates into money
(Uthana's `text-to-motion-3.0` is $0.10 per generated second on PAYG). Nothing can detect this; only
the account knows.

On PAYG, **reviewing before downloading saves nothing** — the money was spent at submission. Review
to pick the best take, not to control cost.

### 3. Frame rate — you no longer set this

**`Target Frame Rate` in settings is now only a fallback.** The rate comes from the provider's
capabilities: 60 for Uthana, 30 for Kimodo.

It had to move. A global setting is correct for exactly one provider and silently wrong for every
other, and a wrong frame rate is the worst kind of bug here — Uthana, asked for less than its native
rate, **re-times rather than resamples**, so a 4-second clip requested at `fps=30` arrives as an
8.3-second file. It imports without a single warning, logs cleanly, and is simply wrong: long,
mushy, and easily mistaken for a padded prompt or a billing error.

### 4. Blender — leave it empty

**Leave `Blender Executable` unset.** Importing the provider's file untouched is what produces a
correct animation.

A Blender FBX round trip reorients the rig: `automatic_bone_orientation` rebuilds bone orientations
from child positions instead of honouring the file, and the exporter applies an axis conversion. The
clip arrives rotated ninety degrees and twisted while every log, duration and bone-count check still
passes — which is exactly why it is worth stating here rather than leaving to be rediscovered.

What that costs is **trimming**. For stripping root translation, use the provider's own `in_place`
download option instead: free, server-side, lossless.

`normalize_motion.py` remains in the plugin and is wired up; it is off, not gone. Re-enabling it
means solving the axis handling and verifying a clip visually against the provider's viewer.

### 5. A character

Create a **Motion Character** asset:

| Field | Meaning |
|---|---|
| `ProviderId` | Which provider to pair with. Ids are not portable between them. |
| `TargetSkeleton` | The skeleton finished animations end up on. |
| `PreviewMesh` | The mesh to upload, on that skeleton. This defines the rig the provider generates against. |

Then pair it — `ProviderCharacterId` is filled in for you:

```
MotionForge.UploadCharacter /Game/_Generated/Motion/MC_MyCharacter
```

That exports `PreviewMesh` to FBX, uploads it, writes the returned id into the asset and saves.
`SourceFbxPath` records what was sent, so "which mesh is this id actually of?" stays answerable a
year later.

**Pair once.** Every take ever generated is tied to the id it was generated against, and on a
provider without seeds a take cannot be reproduced. Re-uploading creates a *second* character and
orphans the first one's takes, so a character that already has an id is refused unless forced.

`MotionForge.ListCharacters` shows what the account already holds, including any stock characters the
provider ships. Those can be used by pasting the id in directly — but prefer uploading your own,
because that is what makes the one-to-one import work.

---

## Use

Create a **Motion Definition** asset per motion, fill in the prompt, and generate.

### Prompt authoring

The single most common cause of unusable output is an underspecified prompt.

Models with a minimum clip length spend the whole duration whether or not you tell them how, so a
vague prompt comes back padded and lifeless. It is not choosing to be calm; it is filling time.

**And that rule applies per beat, not per clip** — which is the thing everybody gets wrong, because a
beat is a much smaller thing to underspecify. On a segmenting provider:

> **A beat inherits the body, not the words.**

Each beat begins in the pose the beat before it ended in — continuity is not the problem — but its text
is evaluated **without knowing what the previous beat said**. So every beat has to name the pose it is
acting on, in full, as though it were the only sentence the model will ever read.

```
[SETUP]   what is held, in which hand, starting posture
[BEAT 1]  action + tempo adverb + exact contact point
[BEAT 2]  hold: name the pose being held, not just "still"
[BEAT 3]  action + tempo adverb + direction
[SETTLE]  return to a relaxed standing idle
```

That second line used to read `explicit hold: "holds for two seconds"`, and that is exactly the
sentence that fails:

> ❌ The person holds the arm perfectly still

> ✅ The person holds the right arm motionless in the previous pose, raised out in front at shoulder
> height

Nothing in the first says *which* pose. Standing with the arms down is perfectly still, and the model
is not wrong to read it that way — the sentence that raised the arm belongs to a beat this one cannot
see. Measured on this project's own pipeline, same seed and same durations, over a five-second hold:

| | frame 30 | frame 120 | frame 180 |
|---|---|---|---|
| ❌ *"perfectly still"* | arm raised | **arms fully down** | nothing left for the next beat to lower |
| ✅ pose named | 86.8cm above pelvis | 58.7cm | 58.9cm, then lowers cleanly to 0.6cm |

**Note what naming the pose does and does not buy.** It removes the failure — the arm stays up and the
following beat still has work to do. It does not produce true stillness: the hand drifts down about
28cm over the five seconds, most of it in the first three. Words hold a pose *approximately*. For
exactly, use a [constraint](#authoring-the-prompt-on-a-timeline).

Three further rules behind that shape:

- **Tempo attaches per beat, not per clip.** A global "make it energetic" lands on whichever beat the
  model treats as dominant and leaves the rest slow.
- **Weight lives in the torso.** Bracing, planted feet, leaning back under a load, and the
  exhale-and-drop afterwards are what sell mass. Calling an object "heavy" does nothing on its own.
- **End in neutral.** A clip that ends mid-gesture pops when it blends back to locomotion.

**A beat that goes wrong robs the beat after it.** That is why diagnosis means reading the timeline
beat by beat rather than watching the clip — the symptom shows up downstream of the cause.

Bad, then good:

> ❌ a person takes a stimpack, injects it into their leg, then tosses it away

> ✅ a person holds a syringe in their right hand, sharply stabs it into their right thigh just above
> the knee, holds it pressed hard against the leg for two seconds, then rips it out and flicks it away
> quickly to the right, and drops both arms into a relaxed standing idle

### Authoring the prompt on a timeline

A prompt with three sentences and a total length **is already a timeline**. A segmenting provider cuts
it at every full stop and gives each piece a duration, whether or not anybody chose them. Written in a
text box, that division is invisible until the clip comes back the wrong length.

So put it in Sequencer, where a beat you can see is a beat you cannot be surprised by:

```
MotionForge: Create Prompt Sequence   /Game/_Generated/Motion/Definitions/MD_Foo
```

That builds `/Game/_Generated/Motion/Sequences/LS_MD_Foo` with a **Motion Prompt** track, one section
per beat, each section's *length* being that beat's duration — so dragging the boundary between two
sections is exactly editing `BeatSeconds = [2, 7, 3]`. It binds a character on the definition's
skeleton at the same time, and points the definition's `Control → Constraint Sequence` at it.

**One sequence, both jobs.** It is deliberately the same field and the same asset that carries the
[constraint poses](../MotionForgeKimodo/README.md#authoring-a-whole-clip-from-a-level-sequence).
Constraint keys index the whole timeline and prompt beats divide it, so the two sit alongside each
other and need no translation between them — a lucky alignment rather than a design, and worth
knowing because it means posing the character and timing the words happen in one window.

#### The rules the track keeps

- **The sequence wins while it is present.** With a prompt track on it, the track is the truth for the
  prompt *and* its beats; the definition's own `Prompt` and `BeatSeconds` are ignored rather than
  merged, and left untouched underneath. Same rule as `ConstraintSequence` has always had. A track
  that will not read **refuses the generation** rather than quietly falling back to wording somebody
  stopped maintaining the day they laid it out in time.
- **Beats are defined by their start times.** Each start marks the end of the beat before it, so the
  beats tile by construction and a clip cannot have a hole in it. Dragging a section's *right* edge on
  its own therefore does nothing except leave a visible gap — drag the **next** beat's left edge.
- **Boundaries snap to the generator's frame rate on read** — 30fps on Kimodo, not the timeline's
  display rate. The generator computes `int(duration * fps)` and *truncates*, so a boundary a
  thousandth of a second short of a frame loses that frame silently.
- **Nothing is normalised behind your back.** Gaps, overlaps, a first beat that does not start at
  zero: all reported, none repaired. A tool that closes a gap for you produces a clip that does not
  match the timeline you are looking at, and "why is this beat shorter than I drew it" then has no
  answer anywhere.

#### Reading it back, and the check that matters

```
MotionForge: Read Prompt Beats   /Game/_Generated/Motion/Definitions/MD_Foo
```

Answers for a definition with a prompt sequence and one without — `FromSequence` says which — so it is
the reliable way to ask what a definition currently requests. **Read `Problems` before generating.**
Nothing in it blocks a run; every entry changes what comes back in a way the timeline does not show.

The one worth the whole feature:

> Beat 2's text contains a full stop ('The person holds the arm perfectly still for **2.5** seconds').
> A provider that segments divides on full stops and nothing else — a decimal point included — so this
> beat becomes two at the far end while the timeline still shows one.
>
> These 3 section(s) join into a prompt that a segmenting provider reads as 4 beat(s), so the
> durations would pair with the wrong text.

That is the exact defect that cost a session on 2026-08-15, caught before submitting instead of after
importing.

#### Starting from the sequence instead

Create Prompt Sequence needs a definition's path before you have anything open, which is the wrong way
round when you are already in Sequencer. The other direction:

> new Level Sequence → **+ Add → Motion Prompt Track** → set **Definition** on it → **Pull**

and the beats appear with a character on the definition's skeleton spawned in, ready to pose.

**Setting `Definition` writes through.** It points that definition's `Constraint Sequence` at this
sequence, so the two cannot disagree and there is nothing to reconcile. If the definition was already
reading a *different* sequence it is repointed, and the log names the one left behind — "use this
sequence instead" is an ordinary thing to want, so it is allowed rather than refused.

#### In Sequencer

Once `MotionForgeEditor` is loaded, everything is on the track's own row or the beat's right-click:

| | |
|---|---|
| **+ Add → Motion Prompt Track** | add one to a sequence you already have |
| **sync status** on the row | see below. Fixed width, so it can never crowd out the buttons beside it |
| **Generate** on the row | run these beats; the take lands on the animation row below. Disabled **with the reason** when it cannot run, never hidden |
| **+ Beat** on the row | append a two-second beat after the last, growing the playback range to fit. Only appears on hover — the same entry is in the right-click menu |
| **right-click the track** | the state spelled out, **Add Beat**, **Pull Prompt From Definition**, **Bake Beats Into Definition** |
| **right-click a beat** | **Edit Beat Prompt…**, **Split at Playhead**, **Delete and Close the Gap** |
| **double-click a beat** | the same edit window |

#### Which way is the sequence and the asset out of step?

Two directions, and they are not symmetrical:

| | |
|---|---|
| **Pull** | asset → sequence. Replaces the beats here with what the definition says, and puts a character in to pose. This **discards edits made on the timeline** — it is how you go back |
| **Bake** | sequence → asset. Writes these beats onto the definition as its prompt, durations and length. The sequence keeps winning and stays assigned; this is what survives deleting it |

So after dragging a boundary, the status reads **ahead** and *the sequence is already what generates* —
nothing is required. Bake when you want the asset to say the same thing.

**Editing a beat opens a window**, not a field: a beat is a whole sentence, and the window warns while
you type if the text contains a full stop — which on a segmenting provider divides the beat in two at
the far end while the timeline still shows one.

Beats append rather than insert, and delete by giving their time to the beat *before* them, because
they tile — dropping one at the playhead would have to overlap or split what is already there, and
leaving a hole behind on delete is a problem the reader can only report, never fix.

The status says how the track stands against its definition. **None of it is an error:**

| | |
|---|---|
| **no definition** | the track names none, so its beats generate nothing |
| **not linked back** | it names a definition that reads some *other* sequence. Two sequences can name the same definition and only one can be its Constraint Sequence, so these beats generate nothing. Pull to claim it |
| **in sync** | the beats match the definition's own prompt and durations. Usually just after a pull |
| **sequence wins** | it has been edited since. **This is what generates** — the working state, not a warning. Bake to write it back |

#### The rig, and why it arrives switched off

A sequence built this way also gets a **Control Rig track — disabled, and with no keys on it.** Which
rig comes from the Motion Character's `Control Rig` field; leave it empty and no track is added.

That shape is the whole design:

- **The animation row is a preview.** It is what came back, it is replaced every generation, and
  nothing reads it to decide anything.
- **The rig is where you say what you want.** Key a pose on it and that moment becomes a constraint.
  Leave it alone and it costs nothing.
- **It is muted so the take plays underneath**, which is what makes it a preview rather than a fight.

**Never bake a take onto that rig wholesale.** Baking puts a key on every frame, and every keyed time
becomes a constraint — measured at **119** against a practical ceiling of about twenty. That is not
generation, it is the clip played back to itself. Take the one pose you want and key that:

| on the track's right-click | |
|---|---|
| **Copy Take's Pose to Rig at Playhead** | scrub to where the clip looks right, copy, then drag the rig key to where it goes wrong |
| **Put Definition's Constraints on the Rig** | poses already authored on the asset, keyed at their own frames — so old and new sit on one timeline and go out together |
| **Bake Sequence Into Definition** | everything back onto the asset: prompt, durations, length **and** the constraint poses |

**A Motion Constraint track** says what a stretch of timeline pins. Add one from **+ Add**, drag a span
over the moments where only a hand matters, and rig keys inside it become hand constraints while
everything else stays whatever the prompt track's **Constraint Type** says. That fallback is the point —
most clips want one answer and should not have to draw a section to say so.

Both directions of the pose bridge go through the engine's own solve, so nothing maps bones to controls
by naming convention — and a key driven by a pose asset arrives the same way for free.

**The rig is muted, and the harvest wakes it for its own bake.** Do not read that as optional: the flag
is consumed when the sequence *compiles*, so anything that un-mutes after a player exists changes
nothing. Generating with the rig switched off is the normal case and works.

#### After it is generated

The imported clip is placed on an animation track in the same sequence, under the beats that asked for
it — so what you asked for and what you got are on adjacent rows. It is replaced on each import rather
than stacked.

When a prompt has settled:

```
MotionForge: Bake Prompt Beats Into Definition   /Game/_Generated/Motion/Definitions/MD_Foo
```

Insurance, not a step in the workflow. It copies the beats back onto the definition as `Prompt`,
`BeatSeconds` and `Length`, and **leaves the reference in place** — the sequence keeps winning, and the
same sequence usually carries the constraint poses, so clearing it to bake a prompt would be a poor
trade. What it buys is that deleting the sequence later costs nothing.

### The two modes

**Human in the loop** (default) — generate, then stop so someone can look at the takes:

```
Generate → poll → AwaitingReview → SelectCandidate → DownloadSelected → import → Ready
```

Every candidate carries a `ViewerUrl`, so takes can be watched in the provider's own player for free
before choosing.

**Automatic** — `RunFullPipeline`, unattended. Generates, takes the first usable variant, downloads
and imports without stopping.

Which is cheaper depends entirely on the billing model above. On a subscription, review saves
downloads and therefore money. On pay-as-you-go it saves nothing, because generation already billed —
so Automatic is the efficient choice there, not the reckless one.

Only the chosen take is downloaded. The others are not thrown away: their motion ids are on the asset
and the provider still holds them, so any can be fetched later.

---

## Retargeting, for rigs you did not upload

Unnecessary in the normal case, and off by default. It exists for characters whose rig genuinely
differs from the project's — a provider's stock character, or a rig that was auto-rigged on ingest.

Two extra fields on the Motion Character switch it on:

| Field | |
|---|---|
| `ProviderMesh` | The character as the provider stores it. Fill via `ImportProviderCharacterRig`. |
| `Retargeter` | An IK Retargeter from `ProviderMesh` to `PreviewMesh`, authored once by hand. |

With `ProviderMesh` set, clips import onto **its** skeleton as `AS_<Name>_Source` and are then
retargeted onto `TargetSkeleton` as `AS_<Name>`. With it unset — the normal case — clips import
straight onto `TargetSkeleton` and there is no intermediate asset.

Worth knowing if you go down this road: Uthana is not self-consistent between endpoints. The
character bundle comes back with 162 bones **including** `root`; motion files come back with 161 and
**no** `root`, leaving six bones parentless. That difference is what the retarget path has to absorb.

### Which way is forward

`FMotionRetargetMap::ForwardYawDegrees` decides the heading every built clip is squared up to.

Squaring up is not optional — without it a take generated walking north-east arrives rotated in the
asset, and every montage built on it inherits the offset. But *which* heading to square up to belongs
to the target skeleton, not to the generator. Unreal calls +X forward; the stock UE mannequin **mesh**
faces +Y and is turned back by its Character blueprint. Leave this at zero for a skeleton that rests
facing +X, and set it for one that does not.

Get it wrong and every joint sits at exactly the right height while the whole character points a
quarter turn away from every other animation on that skeleton — see
[MotionForgeKimodo](../MotionForgeKimodo/README.md#what-is-verified-and-what-is-not) for how long
that can hide. `FKimodoRigFactory::MeasureFacingYaw` measures it from a rest pose's heel-to-toe
vectors rather than assuming.

### If you are authoring the retargeter yourself

Align the **source** rig to the target, not the other way round.

The IK Retargeter's *Auto Align* rotates each bone until its chain tangent points along its
counterpart's. Matching one direction to another constrains two degrees of freedom and leaves the
twist about that axis to whatever the minimal rotation happens to be. On arms that is harmless,
because the A-pose-to-T-pose swing dominates. On spine and neck both chains already point nearly
straight up, the swing is almost nothing, and what survives is pure unconstrained twist — arms
correct, torso and head turned.

Aligning the source puts those approximations on the rig you can regenerate and leaves the project's
own skeleton resting exactly as it was authored.

---

## Scripting

Every operation is on the editor subsystem, so it is reachable from C++, Blueprint, Python — and
therefore from an agent through the companion
[MotionForgeToolset](../MotionForgeToolset/README.md) plugin.

```python
import unreal, json
mf = unreal.get_editor_subsystem(unreal.MotionForgeSubsystem)

# Price it BEFORE submitting - on pay-as-you-go this is where all the money goes
payload = {
    "mode": "HumanInTheLoop",
    "definitions": [
        {
            "assetName": "MD_StimpackSelf",
            "prompt": "a person holds a syringe in their right hand, sharply stabs it into ...",
            "length": 4,
            "variants": 2,
        },
    ],
}
print(mf.submit_batch_from_json(json.dumps(payload)))

# Watch it
print(mf.get_status_json([]))
print(mf.get_batch_status_json("batch_204512_001"))

# Review and fetch
mf.select_candidate("/Game/_Generated/Motion/MD_StimpackSelf.MD_StimpackSelf", "<motionId>")
mf.download_selected(["/Game/_Generated/Motion/MD_StimpackSelf.MD_StimpackSelf"])
```

Four guarantees the API holds to:

- **Idempotent.** Re-running an operation already underway does nothing. An agent that retries after a
  timeout cannot pay twice for the same generation.
- **Non-blocking.** Long operations return a batch id immediately; progress is polled. The editor
  never stalls.
- **Structured.** Status and errors come back as data, not log lines.
- **Costed first.** `EstimateGenerationCost` prices a batch before the assets exist, which is the only
  point at which a pay-as-you-go decision can still be made.

### Typed or JSON, your choice

Every observation call comes in two forms. `GetStatus`, `GetBatchStatus`, `EstimateCost`,
`EstimateGenerationCost`, `GetCredentialInfo`, `FindMotionDefs` and `SubmitBatch` return `USTRUCT`s;
`GetStatusJson`, `GetBatchStatusJson`, `EstimateCostJson`, `DescribeCredential`, `ListMotionDefs` and
`SubmitBatchFromJson` are thin wrappers over them.

The JSON forms exist because Python and the review widget have a payload to hand and no convenient
way to build an `FMotionDefSpec` array. Everything else — C++, Blueprint, and the MCP toolset in
particular — should use the typed calls, so a schema change happens in one place.

Filter the Output Log on **`LogMotionForge`** to follow a run.

---

## Every clip says what made it

**An imported animation carries a `Motion Take Provenance` record** — open the asset, look under
*Asset User Data* in the Details panel. It holds the provider and model, the runner and whether it was
local, the prompt and its beats, the seed, whether the clip was normalised or retargeted, the motion
id, and when it was made.

It exists because the definition is not a reliable answer. Definitions get renamed, edited,
regenerated and deleted; the animation outlives all of it, and two takes can share a name in different
folders.

**The three fields that matter most are `NativeFrameRate`, `FrameCount` and `DurationSeconds`,
because together they are a test:**

```
frames ÷ duration  must equal  the provider's native rate
```

240 keys in four seconds is a 60fps clip. That is *correct* from Uthana and *wrong* from Kimodo, whose
model runs at 30 — and a Kimodo clip like that holds twice the motion its length allows and plays at
double speed. It passes every other check: bone counts, curve counts, a clean log, and a duration
exactly as requested. Three clips shipped that way for a week before anyone divided one number by the
other.

So the import warns when the numbers disagree, and one call sweeps the library:

```
MotionForge: Report Motion Provenance
```

Read **`bLooksMisRated`** first. Worth running after changing a provider, a runner image, or **the
machine doing the importing** — that last one is not paranoia, it is how the Blender round trip
silently switched itself back on. Clips made before this existed report an empty provider rather than
a guess, which itself dates them.

**Nothing reads provenance to decide anything.** It is a record. The moment the pipeline branches on
it, a clip somebody hand-copied becomes a bug report.

---

## Notes

**Candidates are never pruned.** `text-to-motion-3.0` exposes no seed, so a generation cannot be
reproduced. The motion lives on the provider's server and the `MotionId` is the only route back to
it. A discarded id is a permanently lost take.

**Raw downloads are kept**, in `Saved/MotionForge` by default, for the same reason.

**Mind the length floor.** `text-to-motion-3.0` will not generate below four seconds. Interaction
verbs usually want two to three, so generate at the floor and trim later. The plugin logs a warning
when it clamps a requested length. Trimming currently means doing it by hand, because the Blender
pass is disabled.

**The provider cannot give prompts back.** A motion exposes only `id`, `name`, `created`, `updated`,
`tags` and its asset list — no prompt, model, length or settings. Existing motions therefore cannot
be reverse-engineered into definitions. Writing meaning in at creation time, by renaming each motion
to its definition's name, would fix that; it is not implemented, and is worth doing **before**
generating a large library rather than after.

**Verify pipeline changes by looking at the clip**, side by side with the provider's viewer. Bone
counts, durations, key counts and a clean log all pass while the result is visibly wrong — that
combination accounted for most of the time spent getting this working.

---

## What is verified, and what is not

Verified against a live account on 2026-08-04:

- basic auth (key as username, empty password), connection test
- character upload (GraphQL multipart `create_character`) and listing
- generation with `text-to-motion-3.0`, job polling, motion id retrieval
- download at `fps=60&no_mesh=true`
- import onto the project skeleton — 3.98s / 240 keys, matching the provider's viewer
- the result driving a montage, an ability and a gameplay effect in game, via
  [MontageForge](../MontageForge/README.md)

Verified on 2026-08-15, against Kimodo:

- **the prompt track drives generation**, measured on the clip rather than on the call. The same
  definition, generated twice: with `Length 6` and no beat durations it produced **180 frames / 5.97s**;
  with a prompt track saying 1s + 5s + 2s — and the definition still reading `Length 6`, `BeatSeconds`
  empty — it produced **240 frames / 7.97s**. The timeline won, and the clip is the evidence
- round trip: a three-sentence prompt laid out as three sections and read back as three beats, with the
  joined prompt identical to the original
- the full-stop check: a beat reading "...for 2.5 seconds" is reported as becoming two beats at the far
  end, before anything is submitted
- both refusals, each naming which: an empty prompt, and a definition with no skeleton
- the take landing on an animation track in the same sequence, under the beats that asked for it
- baking back: `Length 8`, `BeatSeconds [1, 5, 2]` written onto the definition
- provenance now records the beats **with their text**, taken from the track rather than guessed

Verified on 2026-08-16:

- **the constraint harvest reads a sequence Create Prompt Sequence built.** Three marked frames at 0s,
  2s and 6s came back as constraint keys at clip frames 0, 60 and 180, each carrying 30 joint rotations
  and a hip position — and the three hip heights differ (1.027 / 0.946 / 1.023 m), which is what proves
  the sequence is genuinely driving the character rather than sampling however it happens to stand
- so **the whole loop's foundation holds**: the spawned character poses, the animation track drives it,
  and the moments you mark become the next generation's constraints
- **the beat rule above, on our own pipeline.** Naming the pose in a hold beat took the right hand from
  fully down by frame 120 to 58.7cm above the pelvis, with the following beat still having something to
  lower. Same seed, same durations, one sentence changed
- **the definition link writes through both ways**, including repointing a definition that was reading
  another sequence, with the orphaned one named in the log
- **Pull reads the asset, not the sequence** — pulling onto a sequence whose beats had been reworded
  brought back the definition's own wording, which is the whole point of it
- **a regeneration imports the take it just made.** `Automatic` used to select the first usable
  candidate *on the asset*, so every rerun silently re-imported the oldest clip — a definition with
  four takes now correctly imports the fourth

Not exercised: the row's buttons and the beat right-click menu are Slate and cannot be driven over MCP,
so they need a human to click them. Everything they call is verified above.

**Not exercised, and worth doing before relying on it: a Motion Constraint span changing what reaches
the provider.** The harvest groups keys by type and the providers read a list of typed constraints, but
no span has been drawn over a real rig key and checked. Draw one over a key, leave a second key outside
it, and `Kimodo.PreviewConstraints` should report two entries with different types. Baking several
types back onto a definition is untested for the same reason.

Verified on 2026-08-07:

- **the retarget path**, end to end, driven by the Kimodo provider: import onto a provider rig as
  `AS_<Name>_Source`, then retarget onto the project skeleton. The retargeted clip matches its source
  on every measure taken — shoulder axis to 0.1°, heading to 0.1°, hand height to 0.2cm, foot spread
  to 0.1cm
- **`FMotionAnimBuilder`**, the direct rotation retarget, including `ForwardYawDegrees`
- **the Kimodo provider** — see
  [MotionForgeKimodo](../MotionForgeKimodo/README.md#what-is-verified-and-what-is-not)

Not exercised:

- **Blender normalisation** — disabled; trimming and root-translation stripping go with it
- **`rewrite_prompt` true vs false** — one clip generated each way, not yet judged
- **whether re-downloading bills twice on a subscription** — unknown. On pay-as-you-go it does not:
  the same two clips were re-fetched roughly eight times over one session with no change to the
  balance, which matches the documented "downloads are unlimited". A subscription meters downloads,
  so the question is real there and unanswered
- **the retarget path against more than one target skeleton.** Everything above was measured against
  the Narrative mannequin

---

## Layout

```
MotionForge.uplugin               editor-only; depends on IKRig for the retarget path
Resources/normalize_motion.py     trim, root bone, zero translation - currently disabled
Source/MotionForge/
  MotionForgeSubsystem.*          the public API - everything goes through here
  MotionDef.*                     one motion: prompt, candidates, result
  MotionCharacter.*               provider character id ↔ target skeleton ↔ optional retarget
  MotionForgeSettings.*           Project Settings page, billing model, fallback frame rate
  MotionCredentialStore.*         OS credential vault
  IMotionProvider.h               provider contract, and the capabilities every one declares
  MotionControl.h                 seed, sampler settings, kinematic constraints
  MovieSceneMotionPromptTrack.*   the track whose sections are beats, and the section
  MotionPromptSequence.*          building one from a definition, and reading it back
  MotionTakeProvenance.*          what made this clip, written onto the clip
  Providers/UthanaProvider.*      first implementation
  MotionNormalizeTask.*           Blender invocation
  MotionImporter.*                FBX → UAnimSequence, and → USkeletalMesh for provider rigs
  MotionAnimBuilder.*             rotations on a foreign rig → UAnimSequence on ours, no file format
Source/MotionForgeEditor/         Sequencer's affordance for the prompt track - nothing else
  MotionPromptTrackEditor.*       so a beat can be seen, dragged and added
```

**Why two modules.** A custom `UMovieSceneTrack` is invisible in Sequencer until a track editor is
registered for it, and a track editor needs the `Sequencer` module. Everything the prompt track *does*
lives in `MotionForge` and works with `MotionForgeEditor` absent: building a sequence, reading the
beats, generating from them and baking them back are plain `UMovieScene` data, driven over MCP with no
UI in the picture. The editor module is the dragging.

**No menus, no panels, no toolbar buttons** — only what the track itself needs to exist in Sequencer.
Human UI for these plugins starts from UX flows rather than from controls appearing next to features;
see [AUTOMATION_FORGE_SHELL_PLAN.md](../../AUTOMATION_FORGE_SHELL_PLAN.md), which is on hold for that
discussion.

`FMotionForgeModule` holds the provider registry, so an add-on plugin registers itself at module
startup and this plugin never learns its name.

---

## The editor surface

Added 2026-08-29. Everything here is a thin layer over `UMotionForgeSubsystem`,
so an agent reaches all of it too — that is rule 5, and it has no exceptions.

**A Motion Definition opens into a window.** The prompt first and large, because
the prompt is the work; the provider a list of what is actually installed;
length, variants and character as pickers; the whole `Control` tree behind
*Advanced*. On the right, the takes: status in a sentence, what a generation
would cost before it is spent, and a card per take carrying the motion id — the
only route back to a take on a provider that cannot reproduce one — and a single
*Choose*. Generate, Import, Prompt Timeline and Show Animation are on the
toolbar.

**Generate is offered only when it would work.** `CheckReadiness` asks the same
six questions submission asks — provider, credential, provider ready, character,
character usable, prompt — so the button greys out with the reason in its
tooltip instead of failing after the click. A missing key gets a button to the
Keys page.

**A provider that runs on hardware can offer its own setup.** Implement
`GetSetupSurfaceLabel` and `OpenSetupSurface` on `IMotionProvider` and the
definition window grows an *Open …* button, leading when the provider says it is
not ready. MotionForge never learns what the provider is managing; Kimodo's
answer happens to be a Docker container or a rented GPU.

**Readiness stays current.** `OnProviderStateChanged` covers anything done
inside the editor, a ten-second `RefreshState` poll covers what it cannot see —
a container stopped from a terminal, a pod released elsewhere — and there is a
refresh button because both will still miss something.

**A Motion Character is a checklist, not ten fields.** Pairing is five steps of
which four must happen in order, and which of them exist depends on the
provider: a service that retargets on its own hardware wants a character
uploaded and an id back; one that generates on a fixed rig wants neither.

The one thing worth knowing before reading that page: **`Provider Mesh` is not a
required field.** Its presence chooses the pipeline — empty imports straight onto
the target skeleton, set imports onto the provider's rig and retargets. Both are
finished configurations.

**The library.** *Tools ▸ Automation Forge ▸ Motion Library*, or
`MotionForge.Library`. Every definition in the project on one list: its state,
which provider it will actually use, how many takes it has, the animation it
produced, and its prompt. Filters carry their own counts. Sort by name, state or
provider; search names and prompts; double-click to open.

Two things it does that a folder of icons cannot:

- **A selection is priced before it is spent.** Select five definitions and the
  footer says what generating them would cost, per the providers they each
  resolve to, before the button is pressed. Generate is offered only for the
  ones that would actually work, and says so when none would.
- **It catches a result that is gone.** A definition's status records what the
  pipeline did and stays true after somebody deletes the clip, so a library can
  be full of definitions claiming *Ready* with nothing to show. The library asks
  the asset registry instead of believing the status, and says so in red — as
  does `GetMotionStatus`, through `bImportedSequenceMissing`.

**Keys reach two surfaces.** `Config/ForgeMachine.json` declares the Uthana key —
what it is for, where to get one, the Credential Manager entry, the environment
variable. The editor's Keys page reads it, and so does the Automation Forge hub,
which is a separate application and can therefore set a key before an editor is
open. Same vault entry either way.

**Creating either asset.** Right-click in the Content Browser ▸ *Automation
Forge ▸ MotionForge ▸ Motion Definition* (or *Motion Character*), or **New
Definition** in the library. Before 0.2.0 there was no factory, so the only
route was *Miscellaneous ▸ Data Asset* and finding the class in a list of every
data asset class in the project. Both routes now apply the same project defaults
through `UMotionDef::ApplyProjectDefaults`, which is what `CreateMotionDef` uses
— a definition made by hand is not subtly different from one an agent authored.
