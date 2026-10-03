#pragma once

// The contract is not on the include path of this target today; one line in
// CMakeLists.txt - the contract folder added to target_include_directories -
// would let every header here spell it <speechbroker-voice-model.h>. Until that
// line exists the relative path is the honest spelling: it is relative, so it
// breaks no rule about absolute paths, and it breaks loudly rather than quietly
// if the layout moves.
#include "../../contract/speechbroker-voice-model.h"

#include <cstdint>

namespace Voice::Models
{
	// THE TWO GUARDS AROUND EVERY CROSSING, AND WHY ONE IS NOT ENOUGH.
	//
	// A crossing is a call that leaves adapter code for third-party code, or
	// arrives from it. There are two of them and they are NOT mirror images:
	//
	//   OUTWARD - Start, Stop, SetVocabulary, Submit, Cancel. Our thread, their
	//   code. A fault here ejects the model for the session.
	//
	//   INWARD - Register, Unregister, Complete, Ready, Log. THEIR thread, our
	//   code, over pointers they supplied. This is the likelier fault of the two
	//   and the one an earlier draft of the contract did not guard at all: a
	//   stale char* out of third-party code is the most probable in-process
	//   defect in the whole design, and it lands inside the adapter on a thread
	//   the adapter never created, inside a call the adapter never made
	//   (docs/model-host.md, "Both directions are guarded").
	//
	// Each crossing needs BOTH of these, because they catch different things:
	//
	//   catch(...)          takes the C++ throw. The realistic one is a
	//                       std::runtime_error out of a third-party library that
	//                       could not load its own dependency - not a bad_alloc -
	//                       so what() is the diagnostic and must be logged.
	//   __try/__except      takes the access violation, the illegal instruction,
	//                       the division by zero. No catch compiled the ordinary
	//                       way sees any of them.
	//
	// THE C++ HANDLER LIVES OUTSIDE THE STRUCTURED ONE, AND THAT ORDER IS LOAD-
	// BEARING. A C++ throw is raised as structured exception 0xE06D7363. Our
	// filter lets that code pass (EXCEPTION_CONTINUE_SEARCH) so that unwinding
	// carries on out of the guard and into the caller's catch(...). If the filter
	// swallowed it, the ledger would record a fault - and eject the model for the
	// session - where the contract promises a throw is merely a refusal.
	//
	// WHICH IS WHY NOTHING IN THIS FILE IS noexcept. A noexcept guard would turn
	// exactly that pass-through into std::terminate, which is the one failure
	// this whole page exists to prevent. Declaring these noexcept "for tidiness"
	// silently converts every third-party throw into a dead game.
	//
	// __try NEEDS NO COMPILER FLAG AND REACHING FOR ONE IS A TRAP. MSVC generates
	// structured exception support regardless of /EH, and SKSE's own plugin
	// manager wraps a plugin's entry point in __try/__except without /EHa. Do NOT
	// switch this target to /EHa: Microsoft counter-recommends mixing it with
	// /EHs in one module, and /EHsc is already set twice in this build, so
	// appending would produce a contradictory command line rather than a change.
	//
	// WHAT __try DOES NEED is its own small non-inline function taking POD
	// arguments, because MSVC refuses __try in a function that holds anything
	// with a destructor. That is the whole reason this file exists as a file:
	// every entry below is declared here and DEFINED IN Guard.cpp, holds nothing
	// but scalars and pointers, and is called from the C++ code rather than
	// containing it.

	// What the structured handler saw. POD, so it may be a local of a guarded
	// frame and may be filled from inside an __except body.
	struct Fault
	{
		// 0 when nothing faulted. Otherwise the EXCEPTION_RECORD's own code -
		// 0xC0000005 for an access violation, and so on - written into the log as
		// a number, because a name we invented for it would be one more thing to
		// disbelieve.
		std::uint32_t code{ 0 };

		// ExceptionAddress: the instruction, not the data. It is the only thing
		// that tells a model author which of their functions went wrong.
		const void* address{ nullptr };

		// EXCEPTION_STACK_OVERFLOW, kept apart because it is the one fault with a
		// recovery step: _resetstkoflw() must be called, and it must be called
		// from the BODY of the __except and never from the filter - the guard page
		// is not back yet while the filter runs.
		bool stackOverflow{ false };

		bool Happened() const noexcept { return code != 0; }
	};

	// The entries of a model's table, named once so that the guards below and the
	// copy Model keeps cannot drift apart in their spelling.
	using StartFn         = std::int32_t (SPEECHBROKERVOICE_CALL*)(void*);
	using StopFn          = void (SPEECHBROKERVOICE_CALL*)(void*);
	using SetVocabularyFn = void (SPEECHBROKERVOICE_CALL*)(void*, const char* const*, std::int32_t);
	using SubmitFn        = std::int32_t (SPEECHBROKERVOICE_CALL*)(void*, const struct SpeechBrokerVoiceRequest*);
	using CancelFn        = void (SPEECHBROKERVOICE_CALL*)(void*, std::int64_t);

	// The body of an inward call: our own work, behind a POD context the caller
	// built on its stack. It is a plain function pointer and not a std::function
	// on purpose - a std::function has a destructor and could not be a local of
	// the guarded frame.
	//
	// NOT noexcept, for the reason at the head of this file: a std::bad_alloc out
	// of our own copying has to reach the catch(...) of the entry point.
	using HostBody = void (*)(void*);

	// ------------------------------------------------------------------ outward
	//
	// THREAD: that model's own dispatch thread, and no other. Calls to one model
	// are serialised by that, which is what makes a shim that misbehaves in one
	// of them starve only itself.
	//
	// EVERY ONE RETURNS true WHEN THE CALL CAME BACK and false when it faulted.
	// On false, a_fault is filled and the caller MUST perform the same retraction
	// a non-OK return performs - the debt released, the entry removed from the
	// expected set - and MUST additionally eject the model for the session:
	// __except unwound without running a destructor in any frame between, so the
	// model's locks are still held and the state on the far side is of unknown
	// shape. Containment here means the GAME survived, not that the model did.
	//
	// A C++ THROW DOES NOT COME BACK THROUGH THESE AT ALL. It passes the filter
	// and leaves through the caller's catch(...). So `false` means exactly one
	// thing - a fault - and the caller's two handlers stay distinguishable.

	bool GuardedStart(StartFn a_fn, void* a_user, std::int32_t* a_status, Fault* a_fault);

	bool GuardedStop(StopFn a_fn, void* a_user, Fault* a_fault);

	bool GuardedSetVocabulary(SetVocabularyFn a_fn, void* a_user,
		const char* const* a_phrases, std::int32_t a_count, Fault* a_fault);

	// a_request is borrowed for the length of the call and belongs to the
	// caller's stack; the snapshot behind a_request->samples is held alive by the
	// dispatch entry that is making this call (Model.h, DispatchEntry::audio).
	bool GuardedSubmit(SubmitFn a_fn, void* a_user,
		const struct SpeechBrokerVoiceRequest* a_request, std::int32_t* a_status, Fault* a_fault);

	bool GuardedCancel(CancelFn a_fn, void* a_user, std::int64_t a_utteranceId, Fault* a_fault);

	// ------------------------------------------------------------------- inward
	//
	// THREAD: whatever thread the model called us on. Never the game thread -
	// except Register, which the contract deliberately allows from an SKSE
	// message handler because it is trivial and allocates only a copy of the
	// strings.
	//
	// One generic guard rather than five typed ones, because unlike the outward
	// direction the thing being guarded is OUR code with our own signatures: the
	// entry point fills a POD context on its stack, this runs the body behind it,
	// and the context carries both the arguments and the result.
	//
	// false means the body faulted while reading the model's memory. The entry
	// point then ejects that model for the session and returns the status the
	// contract names for its call - STALE out of Complete, REFUSED out of
	// Register, silence out of Unregister, Ready and Log - and NEVER returns into
	// the model as if nothing had happened.
	bool GuardedHostCall(HostBody a_body, void* a_context, Fault* a_fault);

	// -------------------------------------------------------------- the threads
	//
	// Called FIRST THING on every thread this half creates - each model's
	// dispatch thread, the scheduler, every worker - and nowhere else.
	//
	// SetThreadStackGuarantee reserves a_guaranteeBytes of the stack so that a
	// stack overflow inside third-party code still leaves room to run our filter
	// and our log line. Without it the overflow that ejects a model would take
	// the process with it, and the guard would be decoration.
	//
	// It is a settings value and not a constant for the reason every number here
	// is: 64 KB is what the shipped settings say, and the day a model needs more
	// is the day somebody has to be able to reach it (HostSettings::stackGuaranteeBytes).
	void PrepareDispatchThread(std::uint32_t a_guaranteeBytes);

	// What a fault code is called in the log. Returns a bare hexadecimal string
	// for anything it does not know, which is most of them - the point is that
	// the number always reaches the log, not that we have a name for it.
	//
	// THREAD: any. Returns a pointer to static storage; never freed, never
	// translated - the log line around it is the localised part, this is the
	// datum inside it.
	const char* FaultName(std::uint32_t a_code);
}
