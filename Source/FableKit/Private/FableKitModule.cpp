#include "Modules/ModuleManager.h"

#include "HAL/IConsoleManager.h"
#include "Internationalization/Text.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/MessageDialog.h"

DEFINE_LOG_CATEGORY_STATIC(LogFableDialogs, Log, All);

/**
 * ================================================================================================
 *  AUTO-ANSWERED MODAL DIALOGS  —  the editor must never be able to wedge a job again
 * ================================================================================================
 *
 * THE FAILURE THIS EXISTS FOR. A modal dialog in the Unreal editor blocks the GAME THREAD. That
 * kills the FableKit bridge for every lane at once, makes every UBT build fail with "Unable to
 * build while Live Coding is active", and — this is the part that costs the time — it is INVISIBLE
 * from outside: the process still reports Responding=True with a normal window title, and burns CPU
 * as though it were working. The only tell is that Saved/Logs/Deliria.log stops advancing.
 *
 * 2026-08-12 it cost most of a session. A PIE start raised a 580x187 window titled "Message" and
 * everything downstream — the bridge, the build, the screenshots — was stuck behind it for 45
 * minutes while the diagnosis went to "texture DDC thrashing", which the log immediately above the
 * freeze made look convincing. Nothing could clear it: the bridge needs the game thread the dialog
 * is holding, and driving the window with synthetic keystrokes is blocked by policy.
 *
 * THE FIX IS TO NEVER LET THE WINDOW EXIST. FMessageDialog::Open checks
 * `GIsEditor && FCoreDelegates::ModalMessageDialog.IsBound()` and, when it is bound, calls the
 * delegate INSTEAD of creating a window (MessageDialog.cpp:174). So binding it answers the prompt
 * in-process, on the calling thread, with no window and no block.
 *
 * ⚠ ORDER MATTERS. UnrealEd binds this same delegate to show the real Slate dialog, and it is a
 * single-cast TDelegate, so whoever binds LAST wins. FableKit is an Editor module loaded at Default
 * phase, i.e. BEFORE UnrealEd finishes its own init — binding in StartupModule would be silently
 * overwritten. Hence OnPostEngineInit, and hence keeping the previous binding so it can be restored
 * or forwarded to.
 *
 * ⚠ AND IT DOES NOT BLINDLY SAY YES. The standing authorization is to pick SAVE on a save prompt
 * (2026-08-07: "always remember to just press save or whatever, do it for me whenever, for
 * fablekit") — it is NOT authorization to confirm a deletion. A prompt whose text reads destructive
 * gets the SAFE answer instead, and every single decision is logged with the prompt's full text, so
 * nothing is ever silently agreed to.
 *
 * Turn it off with `fable.AutoAnswerDialogs 0` to get the real dialogs back.
 */
static TAutoConsoleVariable<int32> CVarFableAutoAnswerDialogs(
	TEXT("fable.AutoAnswerDialogs"),
	1,
	TEXT("1 = FableKit answers editor modal dialogs in-process so they can never block the game\n")
	TEXT("thread (and with it the bridge and every build). 0 = restore the real Slate dialogs.\n")
	TEXT("Every auto-answer is logged to LogFableDialogs with the prompt's full text."),
	ECVF_Default);

namespace FableDialogs
{
	/** The binding that was in place before ours — UnrealEd's real dialog, normally. */
	static TDelegate<EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)> PreviousBinding;
	static bool bInstalled = false;

	/**
	 * Does this prompt propose to destroy something?
	 *
	 * Deliberately generous — a false positive costs one un-answered convenience (the job sees
	 * "No" and carries on), a false negative could confirm deleting an asset. Matched on whole
	 * lowercased substrings of BOTH the title and the body.
	 */
	static bool LooksDestructive(const FText& Title, const FText& Message)
	{
		const FString Text = (Title.ToString() + TEXT(" ") + Message.ToString()).ToLower();
		static const TCHAR* const Danger[] = {
			TEXT("delete"), TEXT("destroy"), TEXT("erase"), TEXT("permanently"),
			TEXT("discard"), TEXT("revert"), TEXT("lose"), TEXT("overwrite"),
			TEXT("force delete"), TEXT("cannot be undone"), TEXT("are you sure"),
			TEXT("remove"), TEXT("reset to default"), TEXT("replace existing"),
		};
		for (const TCHAR* Word : Danger)
		{
			if (Text.Contains(Word)) { return true; }
		}
		return false;
	}

	/** The affirmative answer for a message type — "yes, proceed", "save", "ok". */
	static EAppReturnType::Type Affirmative(EAppMsgType::Type Type)
	{
		switch (Type)
		{
		case EAppMsgType::Ok:                       return EAppReturnType::Ok;
		case EAppMsgType::YesNo:                    return EAppReturnType::Yes;
		case EAppMsgType::OkCancel:                 return EAppReturnType::Ok;
		case EAppMsgType::YesNoCancel:              return EAppReturnType::Yes;
		case EAppMsgType::CancelRetryContinue:      return EAppReturnType::Continue;
		case EAppMsgType::YesNoYesAllNoAll:         return EAppReturnType::Yes;
		case EAppMsgType::YesNoYesAllNoAllCancel:   return EAppReturnType::Yes;
		case EAppMsgType::YesNoYesAll:              return EAppReturnType::Yes;
		default:                                    return EAppReturnType::Ok;
		}
	}

	/** The answer that changes nothing. Ok is the fallback because a plain Ok box has no refusal —
	 *  it is an acknowledgement, and leaving it unanswered is the one thing we must not do. */
	static EAppReturnType::Type Safe(EAppMsgType::Type Type)
	{
		switch (Type)
		{
		case EAppMsgType::Ok:                       return EAppReturnType::Ok;
		case EAppMsgType::YesNo:                    return EAppReturnType::No;
		case EAppMsgType::OkCancel:                 return EAppReturnType::Cancel;
		case EAppMsgType::YesNoCancel:              return EAppReturnType::No;
		case EAppMsgType::CancelRetryContinue:      return EAppReturnType::Cancel;
		case EAppMsgType::YesNoYesAllNoAll:         return EAppReturnType::No;
		case EAppMsgType::YesNoYesAllNoAllCancel:   return EAppReturnType::No;
		case EAppMsgType::YesNoYesAll:              return EAppReturnType::No;
		default:                                    return EAppReturnType::Ok;
		}
	}

	static EAppReturnType::Type Handle(EAppMsgCategory Category, EAppMsgType::Type Type,
	                                   const FText& Message, const FText& Title)
	{
		// Switched off at runtime: hand straight back to whatever was bound before us, so the real
		// dialog appears exactly as it used to.
		if (CVarFableAutoAnswerDialogs.GetValueOnAnyThread() == 0)
		{
			if (PreviousBinding.IsBound())
			{
				return PreviousBinding.Execute(Category, Type, Message, Title);
			}
			return Safe(Type);
		}

		const bool bDangerous = LooksDestructive(Title, Message);
		const EAppReturnType::Type Answer = bDangerous ? Safe(Type) : Affirmative(Type);

		/* WARNING, not Log, and it prints the WHOLE prompt. A dialog that was answered without a
		 * human is exactly the thing that must be greppable afterwards — if a job did something
		 * surprising, this line is the record of what it was asked and what it said. */
		UE_LOG(LogFableDialogs, Warning,
			TEXT("[FableKit] auto-answered a modal dialog (%s) -> %d\n         title: %s\n       message: %s"),
			bDangerous ? TEXT("DESTRUCTIVE, answered SAFE") : TEXT("routine, answered AFFIRMATIVE"),
			static_cast<int32>(Answer), *Title.ToString(), *Message.ToString());

		return Answer;
	}

	static void Install()
	{
		if (bInstalled)
		{
			return;
		}
		bInstalled = true;
		// Keep UnrealEd's binding so `fable.AutoAnswerDialogs 0` can hand control straight back.
		PreviousBinding = FCoreDelegates::ModalMessageDialog;
		FCoreDelegates::ModalMessageDialog.BindStatic(&Handle);
		UE_LOG(LogFableDialogs, Log,
			TEXT("[FableKit] modal dialogs are now answered in-process (fable.AutoAnswerDialogs 1). "
			     "A dialog can no longer block the game thread, the bridge or a build."));
	}

	static void Uninstall()
	{
		if (!bInstalled)
		{
			return;
		}
		bInstalled = false;
		FCoreDelegates::ModalMessageDialog = PreviousBinding;
		PreviousBinding.Unbind();
	}
}

class FFableKitModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		/* POST ENGINE INIT, not here. UnrealEd binds ModalMessageDialog itself during its init, and
		 * the delegate is single-cast — binding at module startup gets silently replaced a moment
		 * later and the whole feature would look like it simply did not work. */
		PostInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&FableDialogs::Install);
	}

	virtual void ShutdownModule() override
	{
		if (PostInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(PostInitHandle);
			PostInitHandle.Reset();
		}
		FableDialogs::Uninstall();
	}

private:
	FDelegateHandle PostInitHandle;
};

IMPLEMENT_MODULE(FFableKitModule, FableKit)
