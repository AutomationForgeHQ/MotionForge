// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "MotionForgeTypes.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class UMotionDef;

/**
 * Every take a definition has generated, and the one choice between them that matters.
 *
 * This is the human-in-the-loop moment the whole pipeline parks at. It holds no logic: every button
 * is a call to UMotionForgeSubsystem, which is the same thing an agent calls, so nothing is reachable
 * here that is not reachable from a script.
 *
 * It reads the provider's capabilities rather than assuming, because the correct workflow inverts
 * between the two billing models. On a metered provider a take is watched on the provider's own
 * viewer, free, and exactly one download is paid for. On a local runner there is no viewer and
 * nothing to save, so importing a take to look at it properly is the right move and the panel says
 * so.
 */
class SMotionDefTakes : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionDefTakes) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMotionDef>, Definition)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionDefTakes() override;

	/** Re-read the definition and rebuild. Safe to call often. */
	void Refresh();

	/**
	 * Whether generating would work, as of the last rebuild.
	 *
	 * Cached rather than asked on demand: the check reads the credential vault, and a toolbar's
	 * CanExecute runs every frame. The panel already recomputes it whenever anything it draws
	 * moves, which is exactly when this can change.
	 */
	const FMotionReadiness& Readiness() const { return LastReadiness; }

private:

	TSharedRef<SWidget> BuildHeader(const FMotionProviderCaps& Caps);
	TSharedRef<SWidget> BuildCostLine(const FMotionProviderCaps& Caps);
	TSharedRef<SWidget> BuildCard(const FMotionCandidate& Candidate);
	TSharedRef<SWidget> BuildNothingYet(const FMotionProviderCaps& Caps);

	/** The resolved provider for this definition, or an empty caps when there is no such provider. */
	FMotionProviderCaps ResolveCaps() const;

	/** Choosing is the only verb on this panel. Generating and importing are toolbar commands. */
	FReply OnChoose(FString MotionId);

	/** Somebody moved the provider elsewhere - redraw if it is the one this definition uses. */
	void OnProviderStateChanged(FName ProviderId);

	/** Ask the provider to re-read its readiness now, rather than waiting for the slow poll. */
	FReply OnRefreshProvider();

	/**
	 * Cheap fingerprint of everything this panel draws.
	 *
	 * Polling redraws a Slate tree every second for an editor sitting idle, which is both wasteful and
	 * visibly twitchy when a text box has focus. Comparing a fingerprint means the rebuild happens on
	 * the tick something actually changed and on no other.
	 */
	uint32 Fingerprint() const;

	EActiveTimerReturnType Poll(double InCurrentTime, float InDeltaTime);

	TWeakObjectPtr<UMotionDef> Definition;
	TSharedPtr<SVerticalBox> Body;

	uint32 LastFingerprint = 0;
	FMotionReadiness LastReadiness;

	/** Dropped on destruction, so a closed window stops being told about providers. */
	FDelegateHandle ProviderStateHandle;

	/** Seconds since the provider was last asked to re-read its own readiness. */
	float SinceProviderPoll = 0.f;
};
