// The two guards around every crossing. Guard.h is the reasoning; this file is
// the small amount of code that reasoning demands, and it is small on purpose:
// MSVC refuses __try in a function that holds anything with a destructor
// (C2712), so every entry below holds nothing but scalars and pointers and the
// C++ work stays on the other side of the call.
//
// The two macros come first, for the reason audio/Capture.cpp gives: spdlog
// reaches for <Windows.h> on its own, and whoever gets there first decides what
// the header defines. Guarded rather than defined outright, because an
// identical redefinition is silent while a different one is C4005 - which /WX
// turns into a failure.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif

#include "models/Guard.h"

#include "Loc.h"

#include <cstdint>
#include <cstdio>

#include <Windows.h>

// _resetstkoflw: the one recovery step any fault here has, and the reason the
// stack overflow is kept apart from the rest in Fault.
#include <malloc.h>

namespace Voice::Models
{
	namespace
	{
		// THE CODES THAT ARE NOT FAULTS, and every one of them is something the
		// process does to itself on an ordinary day.
		//
		// 0xE06D7363 is a C++ throw ('msc' in the low three bytes). It MUST pass:
		// the contract says a throw out of a model is a refusal, not a fault, and
		// swallowing it here would eject a model for the session where the caller
		// only had to catch it (Guard.h, "THE C++ HANDLER LIVES OUTSIDE").
		//
		// 0x406D1388 is the debugger's thread-naming exception: a thread telling
		// Visual Studio what it is called raises it and expects nobody to answer.
		//
		// 0x80000003 and 0x80000004 are the breakpoint and the single step. They
		// arrive when somebody is debugging the game, and treating a breakpoint as
		// a model's fault would make the adapter unusable with a debugger attached
		// - exactly when one is most wanted.
		constexpr unsigned long kCppThrow = 0xE06D7363UL;
		constexpr unsigned long kThreadName = 0x406D1388UL;

		// The filter, and it is a FUNCTION rather than an expression on purpose:
		// the __except of every entry below is then one call, identical in all
		// six, and the decision of what is a fault lives in one place that can be
		// read on its own.
		//
		// It runs on the faulting stack with the guard page still gone, so it
		// allocates nothing, logs nothing and touches nothing but the POD the
		// caller lent it. _resetstkoflw belongs to the __except BODY, which runs
		// after the unwind, and calling it here would run it while the page is
		// still missing.
		int FaultFilter(unsigned long a_code, EXCEPTION_POINTERS* a_info, Fault* a_fault)
		{
			switch (a_code) {
			case kCppThrow:
			case kThreadName:
			case static_cast<unsigned long>(EXCEPTION_BREAKPOINT):
			case static_cast<unsigned long>(EXCEPTION_SINGLE_STEP):
				return EXCEPTION_CONTINUE_SEARCH;
			default:
				break;
			}

			if (a_fault != nullptr) {
				a_fault->code = static_cast<std::uint32_t>(a_code);

				// The INSTRUCTION and not the data: it is the only thing that tells
				// a model author which of their own functions went wrong.
				a_fault->address = (a_info != nullptr && a_info->ExceptionRecord != nullptr) ?
					a_info->ExceptionRecord->ExceptionAddress :
					nullptr;

				a_fault->stackOverflow = a_code == static_cast<unsigned long>(EXCEPTION_STACK_OVERFLOW);
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// The one line of the __except body, said once. It is not inside the
		// guarded functions themselves because it does not have to be: everything
		// it needs is in the Fault, and a body kept to a single call is a body
		// that cannot grow a local with a destructor by accident.
		void Recover(Fault* a_fault)
		{
			if (a_fault != nullptr && a_fault->stackOverflow) {
				// The guard page is back by now - the unwind has happened - and
				// without this call the next overflow on this thread is not caught
				// at all but kills the process (Guard.h, Fault::stackOverflow).
				_resetstkoflw();
			}
		}
	}

	// ------------------------------------------------------------------ outward

	bool GuardedStart(StartFn a_fn, void* a_user, std::int32_t* a_status, Fault* a_fault)
	{
		__try {
			*a_status = a_fn(a_user);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	bool GuardedStop(StopFn a_fn, void* a_user, Fault* a_fault)
	{
		__try {
			a_fn(a_user);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	bool GuardedSetVocabulary(SetVocabularyFn a_fn, void* a_user,
		const char* const* a_phrases, std::int32_t a_count, Fault* a_fault)
	{
		__try {
			a_fn(a_user, a_phrases, a_count);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	bool GuardedSubmit(SubmitFn a_fn, void* a_user,
		const struct SpeechBrokerVoiceRequest* a_request, std::int32_t* a_status, Fault* a_fault)
	{
		__try {
			*a_status = a_fn(a_user, a_request);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	bool GuardedCancel(CancelFn a_fn, void* a_user, std::int64_t a_utteranceId, Fault* a_fault)
	{
		__try {
			a_fn(a_user, a_utteranceId);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	// ------------------------------------------------------------------- inward

	bool GuardedHostCall(HostBody a_body, void* a_context, Fault* a_fault)
	{
		// OUR code, over pointers a model supplied, on a thread we never created.
		// The body is a plain function pointer and the context a POD the entry
		// point built on its own stack - both for C2712 and for nothing else.
		__try {
			a_body(a_context);
			return true;
		} __except (FaultFilter(GetExceptionCode(), GetExceptionInformation(), a_fault)) {
			Recover(a_fault);
			return false;
		}
	}

	// -------------------------------------------------------------- the threads

	void PrepareDispatchThread(std::uint32_t a_guaranteeBytes)
	{
		if (a_guaranteeBytes == 0) {
			return;  // a player who set it to zero asked for the system's own default
		}

		// SetThreadStackGuarantee takes the size in and hands the PREVIOUS one
		// back through the same variable, which is why it is not const.
		ULONG bytes = static_cast<ULONG>(a_guaranteeBytes);
		if (SetThreadStackGuarantee(&bytes) == FALSE) {
			// One line, and it is a warning rather than a note: without the
			// guarantee the guard above is decoration, because the overflow that
			// should eject one model takes the whole process with it instead.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_STACK_GUARANTEE_REFUSED",
				a_guaranteeBytes, static_cast<std::uint32_t>(GetLastError()));
		}
	}

	const char* FaultName(std::uint32_t a_code)
	{
		// A NAME FOR THE FEW THAT HAVE ONE, and the number for everything else.
		// The point of this function is that the code always reaches the log, not
		// that we have a word for it: a name we invented for an unknown code would
		// be one more thing for a model author to disbelieve.
		switch (a_code) {
		case static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION):
			return "ACCESS_VIOLATION";
		case static_cast<std::uint32_t>(EXCEPTION_ARRAY_BOUNDS_EXCEEDED):
			return "ARRAY_BOUNDS_EXCEEDED";
		case static_cast<std::uint32_t>(EXCEPTION_DATATYPE_MISALIGNMENT):
			return "DATATYPE_MISALIGNMENT";
		case static_cast<std::uint32_t>(EXCEPTION_FLT_DIVIDE_BY_ZERO):
			return "FLT_DIVIDE_BY_ZERO";
		case static_cast<std::uint32_t>(EXCEPTION_ILLEGAL_INSTRUCTION):
			return "ILLEGAL_INSTRUCTION";
		case static_cast<std::uint32_t>(EXCEPTION_IN_PAGE_ERROR):
			return "IN_PAGE_ERROR";
		case static_cast<std::uint32_t>(EXCEPTION_INT_DIVIDE_BY_ZERO):
			return "INT_DIVIDE_BY_ZERO";
		case static_cast<std::uint32_t>(EXCEPTION_INT_OVERFLOW):
			return "INT_OVERFLOW";
		case static_cast<std::uint32_t>(EXCEPTION_NONCONTINUABLE_EXCEPTION):
			return "NONCONTINUABLE_EXCEPTION";
		case static_cast<std::uint32_t>(EXCEPTION_PRIV_INSTRUCTION):
			return "PRIV_INSTRUCTION";
		case static_cast<std::uint32_t>(EXCEPTION_STACK_OVERFLOW):
			return "STACK_OVERFLOW";
		case static_cast<std::uint32_t>(EXCEPTION_GUARD_PAGE):
			return "GUARD_PAGE";
		case static_cast<std::uint32_t>(EXCEPTION_INVALID_HANDLE):
			return "INVALID_HANDLE";
		default:
			break;
		}

		// thread_local, so that two dispatch threads faulting at the same instant
		// do not overwrite each other's name between the formatting of it and the
		// writing of the line. Static storage, never freed: the header promises a
		// pointer the caller need not own.
		thread_local char spelled[16] = { 0 };
		std::snprintf(spelled, sizeof(spelled), "0x%08X", a_code);
		return spelled;
	}
}
