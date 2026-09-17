/* Speech Broker - the interface between the voice adapter and a model mod. The
 * contract version is SPEECHBROKERVOICE_MODEL_ABI_VERSION below.
 *
 * A model mod is a mod just like the adapter: an ordinary SKSE plugin living in
 * the same process of the game. That is why they talk by calling a function
 * rather than over a network.
 *
 * Outward - towards the model itself, which need not be part of the game at all
 * - only the shim looks, and it chooses how: weights linked into this very
 * process, a child process, a server behind four hundred milliseconds of
 * network. There is nothing in that for the adapter to know. The shim knows
 * exactly two things: how to bring its model up, and how to hand it the sound.
 *
 * The microphone belongs to the adapter and to nobody else: one per game, and
 * where an utterance begins and ends is its business alone. The weights belong
 * to the model mod. The text belongs to the bridge - and a model never speaks to
 * the bridge. It answers the adapter, and the adapter carries the text on. There
 * is no ISpeechBroker in this header and no way to reach one from here.
 *
 * The handshake is the one the bridge uses towards adapters: the adapter
 * broadcasts an SKSE message with a pointer to its host table, and every model
 * mod catches it. Its exact shape and moment are written down at
 * SPEECHBROKERVOICE_MESSAGE_HOST, because they are the first four facts a shim
 * needs and none of them can be guessed from this file.
 *
 * WHY PLAIN C AND NOT AN ABSTRACT CLASS. The bridge hands its adapters a C++
 * abstract class, and that is sound there, because the bridge and its adapters
 * are ours and are built from one tree by one compiler. Model mods are not: a
 * third party will build one with another toolset, another version of MSVC,
 * possibly clang-cl. A struct of function pointers has one layout under every
 * one of them; a vtable does not have to. So nothing crosses this line but
 * fixed-width integers, float, char and pointers - no std::string, no
 * std::vector, no exception, no object with a destructor.
 *
 * THE SHAPE OF THE HANDOFF: A WHOLE UTTERANCE. The adapter listens, decides
 * where speech ended, cuts the trailing silence off itself, and only then offers
 * a finished buffer from sample zero. There is no streaming and no delta. A
 * model re-reads all of it and gives back its own full segmentation of it. That
 * is what makes any two answers comparable with each other and with what has
 * been sent onward already - the question "did it give me B, or A and B" must
 * not be askable, or the adapter cannot reconcile two models at all. The adapter
 * may ask several times inside one turn, each time with a buffer from sample
 * zero that ENDS NO EARLIER than the one before it, each with its own
 * utteranceId, and only the last is final.
 *
 * "NO EARLIER", NOT "LONGER", AND THE DIFFERENCE IS LOAD-BEARING. Every pass is
 * trimmed of the trailing silence that triggered it, and the silence that
 * triggers the last pass is the longest one. In the ordinary case - one phrase,
 * then the speaker stops - the final pass therefore carries a buffer of exactly
 * the same length as the interim pass before it, sample for sample: the 1300 ms
 * of extra silence the adapter waited through is the same 1300 ms it then trims
 * away. engine/engine.py:100 and :106-109 do that arithmetic in the reference,
 * against the two thresholds at engine/turn.py:26-27. So two passes of one turn
 * may be byte-identical, answering them identically is correct rather than a
 * cache fault, and nothing anywhere may test a pass for being LONGER than its
 * predecessor.
 *
 * WHAT THAT COSTS, IN NUMBERS, BECAUSE IT IS NOT FREE. At 16000 Hz float32 mono
 * a second of sound is 64 kB, and every pass re-sends the buffer from sample
 * zero. A ten-second turn at five or six passes moves about 2.5 MB in total, of
 * which the final pass alone is 640 kB; the largest buffer that can ever be sent
 * is maxRequestSamples, 1.28 MB at the shipping settings. ("Five or six" is not
 * a property of the adapter: a pass is armed by speech and fired by a pause, so
 * the count is a property of how the speaker pauses, and the thresholds behind
 * it are settings a player may change. See the note to Submit.) For a model
 * inside this process that is a memcpy and beneath notice. For a model across a
 * network it is the dominant term: on a household uplink the final pass alone
 * takes longer to upload than a fast model takes to answer. The honest summary
 * for a remote model is written in the note to Submit, and the honest conclusion
 * is that a remote model should declare finalOnly - which cuts the traffic of a
 * turn by four fifths, where the sixteen-bit sample format that was drafted for
 * this contract and then taken out of it (see SPEECHBROKERVOICE_FMT_FLOAT32)
 * would only have halved it.
 *
 * NOTHING HERE EVER TOUCHES THE THREAD OF THE GAME except Register, which is
 * deliberately trivial. Not Start, not Submit, not Complete, not Stop, not
 * Unregister, not Ready, not Log. Register is the ONE call a shim makes from its
 * SKSE message handler; every other host call in this file must be made from a
 * thread of your own, and Unregister says why in its own note.
 */
#ifndef SPEECHBROKER_VOICE_MODEL_H
#define SPEECHBROKER_VOICE_MODEL_H

#include <stddef.h>  /* offsetof, for the layout assertions at the foot of this file */
#include <stdint.h>

/* It grows by one, never branches. */
#define SPEECHBROKERVOICE_MODEL_ABI_VERSION 1

/* The type of the SKSE message the adapter hands its host table over with.
   FOUR FACTS, AND NOT ONE OF THEM IS INVENTABLE FROM THIS FILE, so they are
   written down rather than left to the reader:

     1. THE SENDER is "SpeechBrokerVoiceAdapter" - the PLUGIN_NAME of the
        adapter. Check a_message->type; the sender name is for your log.
     2. THE PAYLOAD IS A POINTER TO THE POINTER, which is the house convention
        and not a slip: a_message->dataLen == sizeof(void*), and
          const struct SpeechBrokerVoiceHost* host =
              *(const struct SpeechBrokerVoiceHost**)a_message->data;
        This is exactly what the bridge does towards its own adapters
        (bridge/src/main.cpp:93 dispatches &api, and the adapter dereferences
        once at adapter-voice/src/main.cpp:170), and a model mod that copies that
        line is right. Refuse the message if dataLen is not sizeof(void*).
     3. THE MOMENT IS SKSE's kDataLoaded. That is later than the bridge's own
        kPostPostLoad broadcast on purpose: by kDataLoaded every model plugin has
        certainly been loaded and has had its chance to subscribe. REGISTER YOUR
        SKSE MESSAGING LISTENER IN SKSEPlugin_Load - it is the only place
        guaranteed to be early enough.
     4. THERE IS NO SECOND BROADCAST and no entry point to ask for the table. A
        model that was not listening is simply not registered, is never asked for
        anything, and costs nobody anything. Recognition carries on with whatever
        else is installed.

   The number is written as a number and not as a four-character literal on
   purpose. Such a literal has an implementation-defined value and is not even
   the same thing in C as in C++; the bridge lives with 'ENVY' only because that
   value is already on the wire and can no longer be changed. This one is not
   yet, so it is nailed down here. The four characters are V M D L. */
#define SPEECHBROKERVOICE_MESSAGE_HOST 0x564D444CuL

/* The calling convention is stated and not assumed. On x64 - and SkyrimVR.exe
   ships as x64 and nothing else - the switch that genuinely changes how a call
   is made is /Gv (__vectorcall): it passes float and vector arguments in
   different registers, and a model mod built with it would hand this contract's
   float arguments to the wrong place and crash on the first call, with nothing
   in any log to say why. (/Gz, __stdcall, is an x86 switch that x64 accepts and
   ignores; do not let the next person edit that back in as the reason.) The
   explicit __cdecl annotation below suppresses /Gv for these function types,
   which is the whole job.

   Two smaller things belong in the same breath.

   The samples buffer carries no alignment guarantee beyond alignof(float). A
   shim that wants aligned SIMD loads must copy into a buffer of its own - which
   Submit already obliges it to do - and must not assume 16 or 32 bytes.

   These function pointers are declared inside extern "C". Under MSVC the
   language linkage is not part of a function type and the assignment is
   untroubled; under some other toolchains it is. If your compiler refuses to
   assign your function to one of these members, declare your function extern
   "C" as well rather than casting the pointer. */
#if defined(_MSC_VER)
	#define SPEECHBROKERVOICE_CALL __cdecl
#else
	#define SPEECHBROKERVOICE_CALL
#endif

/* A compile-time check in YOUR build, not only in ours. The layout assertions at
   the foot of this file use it; see the note there for why they exist. */
#if defined(__cplusplus)
	#define SPEECHBROKERVOICE_ASSERT(a_cond, a_msg) static_assert(a_cond, a_msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
	#define SPEECHBROKERVOICE_ASSERT(a_cond, a_msg) _Static_assert(a_cond, a_msg)
#else
	/* Last resort for a pre-C11 compiler, and MSVC's own default C mode is one
	   of them - cl /TC without /std:c11 does not know the _Static_assert
	   keyword. An array of negative size is a diagnosable error in every C there
	   has ever been, and the failing line number points straight at the struct.
	   The message is dropped here; the line is enough. */
	#define SPEECHBROKERVOICE_ASSERT_GLUE2(a, b) a##b
	#define SPEECHBROKERVOICE_ASSERT_GLUE(a, b)  SPEECHBROKERVOICE_ASSERT_GLUE2(a, b)
	#define SPEECHBROKERVOICE_ASSERT(a_cond, a_msg) \
		typedef char SPEECHBROKERVOICE_ASSERT_GLUE(speechbrokervoice_assert_, __LINE__)[(a_cond) ? 1 : -1]
#endif

/* The alignment of a struct, which is the ONE property that catches a packing
   this file's offsets cannot see. See the note at the foot of the file. Every
   spelling is covered: C++11, C11, and MSVC's own extension for its pre-C11 C
   mode. If your compiler has none of the three the alignment assertions are
   skipped and the offset assertions still run. */
#if defined(__cplusplus)
	#define SPEECHBROKERVOICE_ALIGNOF(a_type) alignof(a_type)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
	#define SPEECHBROKERVOICE_ALIGNOF(a_type) _Alignof(a_type)
#elif defined(_MSC_VER)
	#define SPEECHBROKERVOICE_ALIGNOF(a_type) __alignof(a_type)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 * How a struct grows, and how it is read
 *
 * Every struct here opens with structBytes, which the SENDER fills with sizeof
 * of the struct as IT was compiled. The receiver reads a field only when
 * structBytes covers it. A version number is a promise about what the fields
 * MEAN; structBytes is a measurement of whether they are THERE, and the second
 * one is what decides. The bridge paid for this lesson at its own contract
 * version 3: at the offset of an appended field an older sender does not have a
 * zero, it has somebody else's memory.
 *
 * THERE IS NO SUCH THING AS A PARTIAL VERSION-1 STRUCT. structBytes must be a
 * multiple of 8 AND at least the version-1 size of its struct, which is named by
 * a constant in this file for every one of them
 * (SPEECHBROKERVOICE_MODELINFO_BYTES_V1 and its siblings). Anything less is
 * refused outright, on both sides. Without that floor a sender could declare
 * structBytes 68 for a 72-byte ModelInfo and, by the gating rule below, put its
 * last field out of reach of the very check that guards it.
 *
 * ONE EXCEPTION, AND IT IS DELIBERATE: SpeechBrokerVoiceFragment has no
 * structBytes. It is the only struct that travels as an ARRAY, and an array
 * needs one stride for all of its elements rather than a size inside each of
 * them, so its size travels once in the answer as fragmentStride. Do not go
 * looking for a structBytes in it; do not add one.
 *
 * READ IN THIS ORDER, ON BOTH SIDES:
 *
 *   1. the pointer is not NULL;
 *   2. structBytes is a multiple of 8 and at least the version-1 size of that
 *      struct;
 *   3. read abiVersion, IF THAT STRUCT HAS ONE, and range-check it;
 *   4. only then read any later field, each one gated on structBytes covering
 *      its offset plus its size.
 *
 * ONLY FOUR STRUCTS CARRY abiVersion, AND STEP 3 DOES NOT APPLY TO THE REST.
 * They are SpeechBrokerVoiceModelInfo, SpeechBrokerVoiceAnswer,
 * SpeechBrokerVoiceSession and SpeechBrokerVoiceHost - each of them the first
 * thing one side hands the other, where there is no settled version yet.
 * SpeechBrokerVoiceFormat, SpeechBrokerVoiceRequest, SpeechBrokerVoiceModel and
 * SpeechBrokerVoiceFragment do NOT carry one and must not be searched for one:
 * they travel inside a conversation whose version was settled at Register and is
 * reported back in SpeechBrokerVoiceSession::abiVersion. For those four, step 3
 * is that settled version, and steps 2 and 4 are unchanged. The point matters
 * most at SpeechBrokerVoiceRequest, which a shim receives five or six times a
 * turn and whose offset 4 is `serial`: a receiver that ran step 3 there would be
 * range-checking the pass number against the version range and refusing every
 * pass after the first.
 *
 * FIELDS ARE ONLY EVER APPENDED, under a banner naming the version. Never
 * inserted, never reordered, never retyped. An array is always a pointer plus a
 * sibling count - never a container, never a sentinel - and an array of a
 * growing struct also carries its stride (see SpeechBrokerVoiceAnswer).
 *
 * A STRUCT EMBEDDED BY VALUE MUST BE THE LAST MEMBER OF ITS HOST, AND ONCE
 * ANYTHING IS APPENDED AFTER IT THE EMBEDDED STRUCT CAN NEVER GROW AGAIN.
 * SpeechBrokerVoiceFormat is the last member of SpeechBrokerVoiceRequest and of
 * SpeechBrokerVoiceSession, and that is not tidiness: appending to Format today
 * merely grows both hosts, which structBytes handles; appending a field AFTER
 * format in either host - which the append-only rule otherwise licenses - freezes
 * Format for the life of the contract, because any later growth of it would
 * silently move that new field and both sizes would stay plausible. If you ever
 * need both, embed a POINTER to it instead, or repeat its fields inline.
 *
 * EVERY STRUCT HERE ENDS WHERE ITS LAST FIELD ENDS: sizeof is a multiple of 8
 * and there is no tail padding, which is why two of them carry an explicitly
 * named reserved word. That is not tidiness either. A struct with four bytes of
 * tail padding can gain a four-byte field without sizeof changing, and then
 * structBytes is the same before and after and the receiver cannot tell the
 * field is missing - the exact failure structBytes exists to prevent. The
 * assertions at the foot of this file pin it.
 *
 * AN OUT-PARAMETER INVERTS THE RULE, so it is spelled out separately. For
 * SpeechBrokerVoiceSession - a struct the adapter FILLS IN and you ALLOCATE -
 * structBytes is the size of the buffer YOU allocated, you set it before the
 * call, and the adapter writes at most that many bytes and not one more. It does
 * not write its own sizeof. Without that rule, an adapter one version newer than
 * your shim would fill 40 bytes into your 36-byte stack object, and the first
 * version bump would be a stack smash in somebody else's mod.
 *
 * EVERY STRING THAT CROSSES THIS LINE, IN EITHER DIRECTION, IS UTF-8 AND IS
 * NUL-TERMINATED WITHIN SPEECHBROKERVOICE_MAX_STRING_BYTES BYTES. Both sides
 * read with strnlen against that ceiling and TRUNCATE rather than refuse, so a
 * shim that forgets the zero byte loses text instead of costing somebody the
 * process. This has to be said out loud: the rule one banner above forbids a
 * sentinel-terminated array, and then every const char* in this file is one.
 * They are the exception, they are bounded, and nothing else in this contract
 * may follow them.
 * ------------------------------------------------------------------------- */

/* The version-1 size of every struct here, so that a third party has a number to
   compare against rather than a description. Each is pinned to sizeof by an
   assertion at the foot of this file. */
#define SPEECHBROKERVOICE_FORMAT_BYTES_V1    16
#define SPEECHBROKERVOICE_MODELINFO_BYTES_V1 72
#define SPEECHBROKERVOICE_REQUEST_BYTES_V1   64
#define SPEECHBROKERVOICE_FRAGMENT_BYTES_V1  40
#define SPEECHBROKERVOICE_ANSWER_BYTES_V1    56
#define SPEECHBROKERVOICE_MODEL_BYTES_V1     56
#define SPEECHBROKERVOICE_SESSION_BYTES_V1   32
#define SPEECHBROKERVOICE_HOST_BYTES_V1      48

/* The ceiling on every string in this contract, terminator included. Read with
   strnlen, never strlen; longer is truncated, never refused. */
#define SPEECHBROKERVOICE_MAX_STRING_BYTES 4096

/* The ceiling on one answer's fragment array. Twenty seconds of speech -
   maxRequestSamples - cannot honestly produce more, and a count above it refuses
   the whole answer before the array is indexed even once. */
#define SPEECHBROKERVOICE_MAX_FRAGMENTS 4096

/* The ceiling on Host::Log's argument array. */
#define SPEECHBROKERVOICE_MAX_LOG_ARGS 16

/* The ceiling on one SetVocabulary call. The list is merged across every
   installed subscriber and would otherwise have no bound at all; the adapter
   clips to this before it calls you. */
#define SPEECHBROKERVOICE_MAX_VOCABULARY 64

/* Given back by every call that can refuse. Zero is the refusal, as everywhere
   in this project: a zeroed variable nobody ever wrote must not read as
   success. Anything outside this enum is treated as SPEECHBROKERVOICE_REFUSED
   and logged. */
enum SpeechBrokerVoiceStatus
{
	SPEECHBROKERVOICE_REFUSED   = 0,  /* not taken at all; the log says why */
	SPEECHBROKERVOICE_OK        = 1,
	SPEECHBROKERVOICE_BUSY      = 2,  /* Submit: my queue is full. I owe you nothing */
	SPEECHBROKERVOICE_NOT_READY = 3,  /* alive, but my model is not up */
	SPEECHBROKERVOICE_RETRY     = 4,  /* Start: nothing is wrong, come back later */
	SPEECHBROKERVOICE_CANCELLED = 5,  /* an answer whose work was dropped on request */
	SPEECHBROKERVOICE_FAILED    = 6,  /* taken, and it went wrong; failed says how */
	SPEECHBROKERVOICE_VERSION   = 7,  /* Register: the versions do not meet */
	SPEECHBROKERVOICE_DUPLICATE = 8,  /* Register: that id is taken already */

	/* Complete: nobody is waiting for this any more. FOUR different histories
	   end here, and a shim does not have to tell them apart - the response to
	   all four is the same, stop working and forget the utterance:
	     - the deadline of that pass went by and the pass was closed without you;
	     - a LATER PASS OF THE SAME TURN has already been reconciled, so your
	       answer describes a buffer the adapter has moved past;
	     - the turn itself ended;
	     - your handle is draining or dead because you called Unregister.
	   STALE is never an accusation of malformedness. A malformed answer is
	   SPEECHBROKERVOICE_MALFORMED. */
	SPEECHBROKERVOICE_STALE     = 9,

	/* What you handed over is wrong, and the log line names the field. It is a
	   SEPARATE code from REFUSED because it is the only refusal you can fix by
	   editing your own code, and at Register you have no handle yet and
	   therefore no way to ask the adapter's log what happened. MALFORMED is
	   counted against you; REFUSED at Register is not your defect (the player
	   forbade your kind, or the adapter is in a state that cannot take you) and
	   REFUSED out of Complete is the adapter's own failure and is never counted
	   against a model. */
	SPEECHBROKERVOICE_MALFORMED = 10
};

/* The sample formats. The adapter owns every conversion, as it owns the
   microphone; a shim NEVER resamples and never re-quantises on its own.

   AT CONTRACT VERSION 1 THERE IS EXACTLY ONE, AND THAT IS DELIBERATE. A 16-bit
   variant was drafted here to halve what a remote model uploads and was taken
   out again, because a second element width cannot be added to this contract by
   defining a constant: the byte length of the buffer would then depend on a
   value in a nested struct rather than on the pointer's own type, the adapter
   would have to hold one quantised snapshot per format per pass while two models
   disagreed about which they wanted, and the saving it bought - one half - is
   smaller than the four fifths finalOnly already buys. If version 2 brings it
   back, it must bring a sibling element-size field in
   SpeechBrokerVoiceRequest with it, and `samples` must stop being a float
   pointer on that day. Until then: sampleCount floats, sampleCount * 4 bytes,
   and a shim that sees any other sampleFormat must REFUSE at Start rather than
   convert. */
#define SPEECHBROKERVOICE_FMT_FLOAT32 1  /* 32-bit float, native endian, nominally [-1, 1] */

/* Where the model actually runs. It is a DECLARATION and the adapter cannot
   check it - and that is exactly why it is here. The contract this header
   replaces forbade a model to name an address or a program, and that rule was
   enforceable because a model was then a data file somebody parsed. A model mod
   is now a DLL the player chose to install, so the guarantee moves out of a
   parser and into the open: the shim declares its kind, the adapter writes it
   into the log in words at every registration, and the player may forbid REMOTE
   in the settings of the adapter. A shim that declares INPROCESS and then opens
   a socket is lying, and lying is the one thing this interface cannot catch.

   THE GATE IS EVALUATED AT Register AND NOWHERE LATER. A kind the player has
   forbidden is refused there, before Start is ever called, so a forbidden remote
   model never resolves a name, never opens a socket and never contacts anybody:
   the refusal has to happen before bring-up or it is not a refusal at all. Do
   not open your transport in Register; that is what Start is for, and Start does
   not come for a model that was refused. */
enum SpeechBrokerVoiceKind
{
	SPEECHBROKERVOICE_KIND_INPROCESS = 1,  /* a library inside SkyrimVR.exe */
	SPEECHBROKERVOICE_KIND_CHILD     = 2,  /* a process this shim started */
	SPEECHBROKERVOICE_KIND_REMOTE    = 3   /* another machine */
};

/* How quick you say you are. It decides ONE thing and only until it is measured:
   who is asked on an INTERIM pass. The final pass of a turn always asks
   everybody. See SpeechBrokerVoiceModelInfo::declaredClass. */
enum SpeechBrokerVoiceClass
{
	SPEECHBROKERVOICE_CLASS_FAST     = 1,  /* answers inside the pause between phrases */
	SPEECHBROKERVOICE_CLASS_ACCURATE = 2   /* worth waiting for at the end of a turn */
};

/* Levels for Host::Log. */
#define SPEECHBROKERVOICE_LOG_DEBUG 0
#define SPEECHBROKERVOICE_LOG_INFO  1
#define SPEECHBROKERVOICE_LOG_WARN  2
#define SPEECHBROKERVOICE_LOG_ERROR 3

/* Everything from here to the closing pop is laid out at pack 8, and THIS LINE
   IS THE GUARD. #pragma pack lowers the maximum alignment, it never raises it,
   so pack(8) is the natural layout on x64 and the push is here for one reason:
   to survive a build that packs differently. A local pack directive beats the
   command line, so with this line in place /Zp1, /Zp2 and /Zp4 all produce the
   identical layout, and so does including this header inside somebody else's
   unbalanced #pragma pack(push, 1) region - all four were compiled and compared,
   see the note at the foot of the file. Without it, such a build would compile a
   differently-packed SpeechBrokerVoiceRequest, send a plausible structBytes, and
   the adapter would read every field at the wrong offset: a fabricated pointer
   handed straight to memcpy.

   DO NOT DELETE IT AS REDUNDANT because the assertions at the foot of the file
   pass without it in an ordinary build. They pass because an ordinary build
   packs at 8 anyway; they are what catches its loss in a build that does not. */
#pragma pack(push, 8)

/* What the adapter is actually capturing. Handed over in the session and again
   in every request, although it does not change: a constant in a header is what
   the adapter was COMPILED with, the field is what it CAUGHT, and only the
   second one is a fact. WASAPI in shared mode hands out the mix format of the
   endpoint - usually 48000 - and the adapter resamples; by the time a buffer
   reaches this header that is all settled. Every length in this system is
   samples * 1000 / sampleRate. A shim that sees a format it does not know must
   refuse, never convert: conversion belongs to whoever owns the microphone. */
struct SpeechBrokerVoiceFormat
{
	uint32_t structBytes;
	uint32_t sampleRate;    /* 16000 today, and still said out loud */
	uint32_t channels;      /* 1 - the adapter de-interleaves before it asks */
	uint32_t sampleFormat;  /* SPEECHBROKERVOICE_FMT_* - only FLOAT32 at version 1 */
};

/* ------------------------------------------------------------------------- *
 * What a model mod says about itself at registration
 *
 * All strings are UTF-8, are NUL-terminated within
 * SPEECHBROKERVOICE_MAX_STRING_BYTES, and are borrowed for the length of the
 * Register call only; the adapter copies what it keeps.
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceModelInfo
{
	uint32_t    structBytes;
	uint32_t    abiVersion;  /* SPEECHBROKERVOICE_MODEL_ABI_VERSION as YOU built it */

	const char* id;    /* "whisper-ru-turbo" - short, no spaces; answers are signed with it */
	const char* name;  /* for a human, in the log of the adapter; NULL means use id */

	/* What the model listens in: an ISO 639-1 code, lower case - "ru", "en". A
	   region may be appended BCP-47 style ("ru-RU") and the adapter keeps only
	   the first subtag. ONE language per registration; a model that hears two
	   registers twice, with two ids. AT VERSION 1 THE ADAPTER LOGS THIS AND
	   ROUTES NOTHING ON IT - it does not exclude you from a pass and does not
	   weigh you down for a mismatch. Said plainly because a shim author who
	   thinks it gates routing will spend real time on it. */
	const char* language;

	/* What you do, comma separated, in the adapter's own vocabulary and not the
	   bridge's. AT VERSION 1 "asr" IS THE ONLY VALUE THE ADAPTER KNOWS; anything
	   else is logged and ignored, and a registration whose provides contains no
	   known value is refused MALFORMED. NULL is read as "asr". */
	const char* provides;

	uint32_t    kind;  /* SpeechBrokerVoiceKind - declared, unverifiable, logged, gated at Register */

	/* How long you ask to be given for a final pass, and how long Start may take.

	   READ THE BOOTSTRAP BEFORE YOU DECLARE A SMALL NUMBER. Everywhere else in
	   this system a measured number beats a promised one, and that is true here
	   too - but only AFTER the measuring has happened, and it has not happened
	   when a session begins. Latency samples are deliberately not carried
	   between runs (engine/reputation.py:201-207: hardware and surroundings
	   change, and a stored number would look like a measurement without being
	   one), and every run of this build starts a new game. So for your first
	   five passes the deadline the adapter gives you IS budgetMs plus a slack of
	   1000 ms - the arithmetic at engine/engine.py:126 - and from the sixth it
	   is derived from your own measured p90 (engine/reputation.py:83-93). Under-
	   declare and the first passes of every session close under you and are
	   counted against your standing. Declare what you can actually meet.

	   0 is read as the adapter's own default, as maxInFlight 0 is read as 1. */
	uint32_t    budgetMs;
	uint32_t    startupMs;

	/* How long Stop may take, and it is a SEPARATE number from startupMs on
	   purpose. Bringing a model up may legitimately take a minute; taking it
	   down may not, because one of the two moments Stop is called is the moment
	   the player wants out of the game, and a minute of that is the difference
	   between a build that can be edited afterwards and one that cannot. Keep
	   this small - a second or two - and do the slow part of your teardown by
	   tying it to the process, not to this call. See Stop. It is also the bound
	   on how long Unregister may block you. */
	uint32_t    stopMs;

	/* 1 - ask me only on the final pass of a turn.

	   THIS IS BINDING, NOT A HINT, and it is the one declaration measurement
	   does not override. Everywhere else in this system a measured number beats
	   a promised one, because promises are about speed and speed can be timed.
	   finalOnly is not about speed, it is about COST: a model may answer in
	   forty milliseconds and still be one that must not be asked five times a
	   turn, because each ask uploads half a megabyte or bills somebody. The
	   adapter has no way to measure that, so it believes you.

	   An honest declaration costs everybody less than a model that answers BUSY
	   to every interim request, because the adapter then stops paying for the
	   copy as well as for the wait. For SPEECHBROKERVOICE_KIND_REMOTE this is
	   the expected declaration: read the byte counts in the preamble and decide
	   with them in front of you.

	   IF EVERY INSTALLED MODEL DECLARES IT, THE ADAPTER RUNS NO INTERIM PASSES
	   AT ALL and says so once in the log. It does not ask a finalOnly model
	   anyway for want of anybody else - that would break the one promise on this
	   page that is binding. The draft-then-refine half of the system simply does
	   not run on that installation, every turn is answered once at its end, and
	   the player is told. The probe is not a pass of a turn and is submitted to a
	   finalOnly model like any other. */
	int32_t     finalOnly;

	/* How many utterances you can hold at once, at least 1. IT IS READ AGAINST
	   THE DEBT, NOT AGAINST THE QUEUE: the adapter does not enter Submit on you
	   while maxInFlight Completes are already outstanding. So a shim that
	   declares its true depth should never have to answer BUSY at all, and BUSY
	   stays the exception it is described as rather than the ordinary way of
	   saying no. 0 is read as 1. */
	uint32_t    maxInFlight;

	/* SpeechBrokerVoiceClass. A HINT, and the only thing it decides is whether
	   you are asked on an INTERIM pass; the final pass of every turn asks
	   everybody regardless.

	   It is a hint that matters, which is why it is here rather than left to
	   measurement alone: the adapter replaces it with your measured p90 once it
	   has five latency samples of you (engine/reputation.py:89-93), and until
	   then this declaration IS the interim roster (engine/models.py:248-258).
	   Since latencies are not carried between runs, "until then" is the start of
	   every session. 0 is read as ACCURATE - the cautious reading, which costs
	   you interim passes rather than costing the player a late draft. */
	uint32_t    declaredClass;

	/* Set it to 0. It is explicit tail padding, and it is here so that sizeof of
	   this struct is exactly where its last field ends: see the note on tail
	   padding in "How a struct grows". The adapter refuses a registration whose
	   reserved0 is not 0 with SPEECHBROKERVOICE_MALFORMED, which is what keeps a
	   later version from quietly giving it a meaning without changing the
	   version number. */
	uint32_t    reserved0;
};

/* ------------------------------------------------------------------------- *
 * One request: a finished buffer of sound
 *
 * THE STRUCT ITSELF IS BORROWED FOR THE LENGTH OF THE Submit CALL, and so is
 * every pointer in it. This is said here because every other pointer in this
 * file has its lifetime written down and an omission reads as permission: a shim
 * that puts `const struct SpeechBrokerVoiceRequest*` on its job queue - the
 * obvious way to avoid copying turnId, utteranceId, serial and final by hand -
 * is reading the adapter's dead stack when its worker wakes four hundred
 * milliseconds later, and it will find a plausible utteranceId there. Copy the
 * scalars you need onto your own job, exactly as you copy the samples.
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceRequest
{
	uint32_t structBytes;

	/* Which pass this is inside the turn, from 1. NOT a version: this struct has
	   no abiVersion and must not be searched for one - see "How a struct grows".

	   SERIALS SKIP, AND THERE ARE THREE REASONS, none of which you are owed an
	   account of: the pass was replaced in your queue by a later one of the same
	   turn (see Submit); the trimmed buffer came out shorter than the adapter's
	   floor and no model was asked at all (engine/engine.py:110-111); or the
	   roster for that pass was empty (engine/engine.py:113-115). You may see
	   serial 1 and then serial 4 and owe nothing for 2 and 3. */
	int32_t  serial;

	int64_t  turnId;       /* the speaking turn; monotone for the session, never reused */
	int64_t  utteranceId;  /* THIS buffer; monotone, never 0, never reused. Identifies the answer */

	/* 1 - the last pass of this turn; the buffer will not be extended again.
	   NOT a promise that it is longer than the pass before it: read the preamble
	   note "NO EARLIER, NOT LONGER". In the ordinary one-phrase turn the final
	   buffer is the same buffer, sample for sample. */
	int32_t  final;

	/* How many floats are at `samples`. THE BUFFER IS sampleCount * 4 BYTES; at
	   contract version 1 there is one sample format and the pointer's own type
	   states its width. Never below the adapter's floor: no buffer shorter than
	   250 ms of sound is ever submitted, because a model cannot align one and
	   Whisper invents on it (engine/engine.py:110-111). Never above
	   SpeechBrokerVoiceSession::maxRequestSamples. */
	uint32_t sampleCount;

	/* Mono, contiguous, de-interleaved, in format, with the trailing silence
	   ALREADY CUT OFF by the adapter. The adapter is the one that heard the
	   pause, and a model is not to be trusted with that: Whisper invents filler
	   on trailing silence with its own voice-activity filter switched on, and
	   the very first run of this system produced a whole sentence over nothing.

	   The guarantee is exactly this and no more: THE ADAPTER NEVER SENDS
	   TRAILING SILENCE THAT IT DETECTED. It is not a promise that the buffer
	   contains speech. A buffer that is quiet throughout is lawful and must be
	   answered like any other buffer - do not assert on it, do not trim it to
	   nothing, do not refuse it as malformed. (Read the note on the probe at the
	   foot of this file and you will see why that sentence is load-bearing.)

	   BOTH HALVES OF THE LIFETIME, BECAUSE ONLY ONE OF THEM USED TO BE WRITTEN
	   DOWN HERE:

	   YOUR HALF. The pointer is borrowed for the length of the Submit call and
	   not one instruction longer. Every model registered gets the same pointer;
	   writing through it by casting the const away spoils the sound for
	   everybody else as well as for you. A model that needs the audio afterwards
	   - and every model does, because Submit may not block - copies it inside
	   the call.

	   THE ADAPTER'S HALF. What you are given is an IMMUTABLE SNAPSHOT that
	   belongs to this pass, not a window into a buffer that is still being
	   filled. The adapter cuts it once, on its own thread, already trimmed;
	   after that nothing appends to it, nothing reallocates it and nothing
	   rewrites it. ITS LIFETIME IS A CLAIM, NOT AN EVENT: the moment a pass is
	   queued for a model, that queue entry takes a claim on the snapshot, and it
	   releases the claim either when Submit returns or when the entry is dropped
	   unsent. The snapshot is freed when the last claim is released - never
	   before. (The earlier wording, "alive until the last Submit of this
	   utteranceId has returned", named an event that does not occur for a model
	   whose queued copy was replaced before it was ever sent.) Before Submit is
	   entered and after it returns, the adapter guarantees nothing about this
	   address, and a shim that kept the pointer is reading memory that is not
	   its own.

	   That snapshot is the whole price of this design and it is what makes a
	   late answer harmless in both directions: the model has nothing of the
	   adapter's left to point at, and the adapter has nothing of the model's. It
	   is also what the python this is ported from did without saying so, though
	   less strictly than the sentence above - engine/turn.py:111-113 hands out
	   np.concatenate, a fresh array owned by that pass alone and referenced by
	   nothing else, and engine/engine.py:99 takes it under the lock. It was a
	   plain writable numpy array shared by every model, with no const anywhere:
	   what the reference actually guaranteed was OWNERSHIP PER PASS, and that is
	   the property being ported. The guard against a late answer was separate and
	   explicit - engine/engine.py:154, `if self._turn is not turn: return` - and
	   is what STALE is here. */
	const float* samples;

	/* Samples the ADAPTER lost inside this buffer - a capture overrun, a device
	   that went away. Not your doing, but you are told, because a hole in the
	   sound is a hole in the answer. 0 - the sound is whole. */
	uint32_t lostSamples;

	/* HOW LONG YOU HAVE LEFT, in milliseconds from the moment this call was
	   entered. It is not advisory and it is not a suggestion: at the instant it
	   names, the adapter seals the pass out of whoever answered, retires this
	   utteranceId, and stops caring.

	   IT NAMES ONE ABSOLUTE MOMENT FOR EVERY MODEL IN THE PASS, and the
	   subtraction is the adapter's job, not yours. The pass has one deadline,
	   set when the pass was created; each model is reached at a different moment,
	   because each has its own dispatch thread with its own queue; so the
	   adapter recomputes this number for each model as it enters Submit, from
	   what is left of that one deadline. You may simply take "now plus
	   deadlineMs" and be right. Two consequences you are owed:
	     - A REQUEST WHOSE BUDGET IS ALREADY SPENT IS NEVER SUBMITTED. It is
	       dropped from your queue unsent, and you are not told and owe nothing.
	     - YOU ARE NEVER CHARGED A TIMEOUT FOR A PASS YOU WERE REACHED TOO LATE
	       TO ANSWER. The standing that routes models is not allowed to punish a
	       model for the adapter's own queueing.

	   Nothing of yours is killed when it expires - the adapter cannot reach into
	   your process, and it will not pretend otherwise - but a Complete that
	   arrives afterwards is answered SPEECHBROKERVOICE_STALE and, if you were
	   reached in time and simply did not answer, is counted against you as a
	   timeout, exactly as engine/engine.py:128 counted an expired future.

	   0 MEANS NO DEADLINE IS STATED. It is used for a request that belongs to no
	   turn, of which the adapter holds at most one per model at a time: today
	   that is the probe straight after Start. Such a request is never retired by
	   anything another pass does; it is retired only when the model unregisters
	   or the session ends. */
	uint32_t deadlineMs;

	struct SpeechBrokerVoiceFormat format;
};

/* ------------------------------------------------------------------------- *
 * One fragment of the answer
 *
 * Times are milliseconds from sample zero of the SUBMITTED BUFFER, which is also
 * sample zero of the turn: the adapter never drops the head of a turn, and when
 * a turn would outgrow maxRequestSamples it ENDS the turn rather than sliding a
 * window (see maxRequestSamples). Time is the identity of a piece of speech
 * here: model timestamps wander by hundreds of milliseconds between passes over
 * the same audio, so the adapter snaps them onto anchors it derived from energy
 * alone (engine/turn.py:97-109, slack 350 ms) and matches pieces across models
 * and across passes by time - never by text and never by index.
 *
 * THE ORDER AND THE BOUNDS ARE PART OF THE CONTRACT, because the adapter matches
 * by time and has no defined behaviour on a tangle:
 *   - startMs <= endMs in every fragment;
 *   - fragments are ordered by startMs and do not overlap;
 *   - an answer that breaks either is SPEECHBROKERVOICE_MALFORMED, whole;
 *   - times outside the submitted buffer are CLAMPED to it and logged once, not
 *     refused: a model's last timestamp genuinely overruns by a few tens of
 *     milliseconds and refusing a whole reading for that would be absurd.
 *
 * Everything past text and score is here to answer one question the adapter has
 * to answer without you: has the sentence ENDED. A model that cannot supply one
 * of them leaves the sentinel and says "I do not know". It is not punished for
 * that - the adapter degrades to a flat guess.
 *
 * BUT FILLING lastWordProb OR medianGapMs IS A CLAIM WITH A CONSEQUENCE, and the
 * consequence is not the one you would guess from the paragraph above. The
 * adapter picks ONE model's segmentation as the lane onto which every other
 * model's text is matched, and it picks it from among the models that carry word
 * timings - tested as exactly these two fields (engine/arbiter.py:38-44). So
 * filling either of them says "my startMs and endMs come from an alignment to
 * the audio", and it promotes you to the model that defines the spans for
 * everybody. Only fill them if that is true. A shim that divides a segment's
 * time proportionally by string length - which is what the reference does when
 * the model returns no word timings (engine/models.py:127-143) - must leave both
 * sentinels, or it will win the lane with boundaries that move between passes
 * and re-create the drifting lane that arbiter.base() was written to fix. The
 * accuracy implied is the snap slack: a boundary more than 350 ms from an energy
 * anchor is not rescued.
 *
 * THIS IS THE ONE STRUCT WITHOUT structBytes; its size travels once per array as
 * SpeechBrokerVoiceAnswer::fragmentStride. When your stride is smaller than the
 * adapter's own sizeof - you built against an older contract - the adapter fills
 * the fields you did not send with THE SENTINELS DOCUMENTED BELOW and never with
 * zero. Zero here is a claim, and the dangerous one is lastWordProb: the test
 * that promotes a model to the lane is `last_word_prob >= 0.0`, so a
 * zero-filled field passes it and a model that sent no word timings at all wins
 * the lane. (medianGapMs is tested `> 0` in both places that read it -
 * engine/judge.py:56 and engine/arbiter.py:42 - so its zero was merely
 * ambiguous, not dangerous; it is -1 here for consistency, and so that "no gaps
 * to measure" and "the gaps measured zero" stop being the same number.)
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceFragment
{
	int32_t     startMs;
	int32_t     endMs;
	const char* text;  /* UTF-8, never NULL - "" instead, and "" is a lawful answer */

	/* YOUR MODEL'S OWN CONFIDENCE IN THIS PIECE, AS A PROBABILITY IN [0, 1],
	   HIGHER MEANING BETTER - exp(avg_logprob) for Whisper. Do NOT normalise it
	   and do NOT multiply it by any opinion you hold of yourself. Trust was once
	   multiplied into the score here, and a model that was right nine times in
	   ten cut a tenth off every number it produced, until correctly recognised
	   phrases fell under the threshold.

	   THE SCALE IS PART OF THE CONTRACT BECAUSE THE COMPARISON IS NOT ALWAYS
	   NORMALISED. Comparability is meant to be restored by the adapter, which
	   ranks your score inside your own recorded distribution - but it can only
	   do that once it holds ten of your scores, and until then it compares the
	   RAW numbers of different models directly (engine/reputation.py:116-127).
	   On a fresh install that is the first ten utterances of every session. A
	   log-probability or a 0-100 confidence would therefore win or lose every
	   comparison on scale alone. If your model's natural number is not a
	   probability, map it to one; that is not the massaging this note forbids. */
	float       score;

	int32_t     endsSentence;  /* 1 yes, 0 no, -1 unknown */
	float       lastWordProb;  /* -1.0f unknown; see the lane note above */
	float       noSpeechProb;  /* -1.0f unknown; 0.0f is a claim, not an absence */

	/* Median gap between words inside this piece. -1 unknown - AND A PIECE OF
	   FEWER THAN TWO WORDS REPORTS -1, not 0: there is no gap to measure, and 0
	   would say the words ran together. */
	int32_t     medianGapMs;

	int32_t     words;  /* -1 unknown */
};

/* ------------------------------------------------------------------------- *
 * The answer: one model's reading of the WHOLE submitted buffer
 *
 * Everything it points at belongs to the MODEL and is borrowed for the length of
 * the Complete call: the struct, the fragment array, every text, and failed. The
 * adapter copies what it keeps before it returns - a NULL becomes an empty
 * string rather than a crash, and every string is read with strnlen against
 * SPEECHBROKERVOICE_MAX_STRING_BYTES and truncated rather than refused - so a
 * shim may build its answer on the stack, or in a buffer it reuses for the next
 * request, and forget it the instant Complete gives back. There is no Free in
 * this contract, no Release, and no allocator crosses it, so two DLLs built
 * against different runtimes cannot corrupt each other's heap.
 *
 * There is no n-best list here: a model returns ONE reading. Alternatives are
 * what several models disagreeing about one stretch produce, and the adapter
 * builds them above this line.
 *
 * AN ANSWER IS REFUSED WHOLE - SPEECHBROKERVOICE_MALFORMED out of Complete, one
 * log line naming your id and the offending value, and a defect counted against
 * you - when any of these is true. None of them can happen to a shim that filled
 * the struct; all of them happen to a shim that value-initialised it and forgot
 * a line:
 *   - structBytes is not a multiple of 8, or is below
 *     SPEECHBROKERVOICE_ANSWER_BYTES_V1, or does not cover through failed;
 *   - abiVersion is outside [1, the version settled at Register];
 *   - status is not one of OK, CANCELLED, FAILED (0 is REFUSED, not success);
 *   - fragmentCount < 0, or above SPEECHBROKERVOICE_MAX_FRAGMENTS;
 *   - fragmentCount > 0 and fragments is NULL;
 *   - fragmentCount > 0 and fragmentStride is not exactly right (see
 *     fragmentStride);
 *   - a fragment with startMs > endMs, or fragments out of order, or overlapping.
 * The count and the stride are checked BEFORE `fragments` is dereferenced even
 * once.
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceAnswer
{
	uint32_t structBytes;
	uint32_t abiVersion;

	int64_t  utteranceId;  /* the one from the request. NOTHING else identifies this answer */
	int32_t  serial;       /* echoed from the request, so a pair needs no clock */

	/* SPEECHBROKERVOICE_OK, _CANCELLED or _FAILED, and nothing else. A zero here
	   is not "success by default", it is SPEECHBROKERVOICE_REFUSED wearing the
	   clothes of an uninitialised field, and the answer is refused as such. */
	int32_t  status;

	/* Measured by YOU, wall clock, from Submit to this call, so a remote model
	   reports its round trip honestly. This is the number by which the adapter
	   learns what a model IS rather than what it declared. */
	int32_t  latencyMs;

	/* THE STRIDE OF THE ARRAY BELOW. A struct that grows by appending is safe
	   for one instance - the receiver simply does not read past the sender's
	   structBytes - but an ARRAY of it is not: the receiver's own sizeof would
	   misindex every element after the first. So set this to
	   sizeof(struct SpeechBrokerVoiceFragment) as YOU compiled it, and the
	   adapter walks (const char*)fragments + i * fragmentStride, reading only
	   what fits inside the smaller of fragmentStride and its own sizeof and
	   filling the rest with the sentinels.

	   IT IS AN EQUALITY, NOT A RANGE, AND IT IS CHECKED BEFORE THE FIRST INDEX.
	   A valid stride is exactly the size of SpeechBrokerVoiceFragment at the
	   abiVersion YOU declared at Register - SPEECHBROKERVOICE_FRAGMENT_BYTES_V1
	   at version 1. Anything else refuses the whole answer. It has to be an
	   equality because a range has two ends and only one of them was ever
	   guarded: the house rule this contract keeps invoking, turned on itself, is
	   that a shim which writes `struct SpeechBrokerVoiceAnswer answer = {0};` and
	   forgets one line sends stride 0, and a stride of 0 makes every element
	   alias element 0, so fragmentCount copies of one fragment walk into the
	   arbiter as independent evidence and nothing anywhere looks wrong. A stride
	   of 1 to 15 is worse still: text sits at offset 8, and a partial pointer is
	   a pointer that gets dereferenced. And a stride of 4096 against a real
	   ten-element array puts element 9 thirty-six kilobytes past the end -
	   which a floor cannot see at all. The adapter knows your version, so it
	   knows the one number this may be. */
	uint32_t                                fragmentStride;

	const struct SpeechBrokerVoiceFragment* fragments;
	int32_t                                 fragmentCount;  /* >= 0; 0 with fragments NULL is lawful */

	/* Samples you lost or refused out of the buffer you were given. Anything but
	   zero says this answer speaks about sound with a hole in it, and the adapter
	   weighs it accordingly rather than merging it as an equal opinion. */
	uint32_t lostSamples;

	/* NULL unless status is FAILED. A STRING and not a code, because the adapter
	   writes it into the log and counts it against you, and a code would tell a
	   person nothing. A failure is an answer too: it is counted, it costs the
	   model its standing, and it does not take the other models down with it. */
	const char* failed;
};

/* ------------------------------------------------------------------------- *
 * What the model mod hands the adapter: its functions
 *
 * The table and the user pointer must outlive the registration - a static in the
 * shim. The adapter copies the table, so it may be built on the stack.
 *
 * Start, Stop and Submit MUST be set. A table with any of them NULL is refused
 * at Register with SPEECHBROKERVOICE_MALFORMED and a log line naming which one,
 * so that "the header did not say" cannot happen: silence about a mandatory
 * entry reads as permission, and an in-process model with nothing to tear down
 * will quite reasonably leave Stop NULL unless told. SetVocabulary and Cancel
 * may be NULL, and each says so again where it is declared.
 *
 * Every one of these is called on a worker thread of the ADAPTER, one such
 * thread per registered model, and NEVER on the thread of the game. Calls to one
 * model are therefore serialised against each other, and a shim that misbehaves
 * in one of them starves only itself - which is the structural form of the rule
 * that one broken model mod must not cost a person the other two. The adapter
 * holds no lock of its own across any of them, so a shim may answer from inside
 * the very call it was given.
 *
 * NO EXCEPTION MAY CROSS THIS LINE, IN EITHER DIRECTION, AND BOTH HALVES ARE
 * MECHANISMS RATHER THAN HOPES. Your half: a C++ shim catches everything at its
 * own boundary and turns it into a FAILED answer with the message as text. The
 * adapter's half, in this direction: IT WRAPS EVERY CALL IT MAKES INTO THIS
 * TABLE IN catch(...). An exception escaping Start, Stop, Submit, SetVocabulary
 * or Cancel is caught at the call site, logged against your id as a protocol
 * violation, and treated exactly as if that call had returned REFUSED - which
 * for Submit means the outstanding utterance is retracted, the same retraction a
 * non-OK return performs. It will not take the game down. That is a safety net
 * and not a licence: an exception crossing this line is a defect and is counted
 * against you. It matters because the header REQUIRES you to allocate inside
 * Submit, up to 1.28 MB of it, and a std::bad_alloc out of an otherwise perfect
 * shim would otherwise unwind through the adapter's frame with no handler and
 * take SkyrimVR.exe down through std::terminate. The same promise is made in the
 * other direction at the head of struct SpeechBrokerVoiceHost.
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceModel
{
	uint32_t structBytes;

	/* Set it to 0. Explicit padding, exactly as SpeechBrokerVoiceModelInfo's
	   reserved0: without it there is a four-byte unnamed hole here, the adapter
	   copies four bytes of your stack into itself at every registration, and a
	   version 2 could not use the hole anyway because filling it would be an
	   INSERTION, which this contract forbids. The adapter refuses a table whose
	   reserved0 is not 0 with SPEECHBROKERVOICE_MALFORMED. */
	uint32_t reserved0;

	/* Handed back to you in every call below. The adapter never looks inside. */
	void*    user;

	/* Bring the model up, or attach to one already up: load the weights, start
	   the child, reach the server. Called after Register returned - never inside
	   Register, because Register runs on the thread of the game during plugin
	   load and weights take seconds. Nothing is submitted to you until it has
	   returned OK.

	   OK - ready.
	   RETRY or NOT_READY - nothing is permanently wrong; come back later (a
	   server still starting, weights on a cold disk). THE TWO ARE TREATED
	   ALIKE HERE, deliberately: NOT_READY is the obvious code for "my model is
	   not up", it is what the enum says, and an earlier draft of this contract
	   ejected a model for the session for saying it. THE ADAPTER TRIES Start UP
	   TO FIVE TIMES, waiting startupMs between attempts - five is its shipping
	   default and the player's settings may change it - and then the model is
	   out for the session. You are not told which attempt you are on; a Start is
	   a Start.
	   Anything else - this model is out for the session at once, announced
	   LOUDLY with its id and the reason, and the others carry on untouched.
	   Working quietly without the accurate model turns every later question
	   about quality into guesswork.

	   A Start THAT NEVER RETURNS IS NOT TIMED OUT. The adapter cannot kill a
	   thread inside third-party code and will not pretend it can, so it simply
	   waits on your own dispatch thread; nothing else in the adapter waits on
	   that thread, no other model is delayed, and the game is never blocked. The
	   adapter says so once in the log when a Start has been running longer than
	   startupMs. A Start that returns OK long afterwards is honoured if the model
	   has not been ejected, and ignored with one log line if it has. */
	int32_t (SPEECHBROKERVOICE_CALL* Start)(void* a_user);

	/* Take the model down. Mandatory even if it does nothing.

	   WHEN IT IS CALLED AT ALL, since an earlier wording named no trigger and a
	   mandatory function with no trigger is a fair thing to resent. Two:
	     - the adapter is taking THIS model down and not the process - the player
	       switched it off in the adapter's settings, or it failed past the
	       adapter's tolerance. This is the trigger the mandate exists for: there
	       has to be a way to stop one model without stopping the game.
	     - the adapter's own shutdown, if and only if it has somewhere to run that
	       is not the loader lock (promise 2 below).

	   WHAT IS PROMISED, PRECISELY, BECAUSE THE PREVIOUS WORDING PROMISED
	   SOMETHING THAT CANNOT BE DELIVERED:

	   1. Stop MAY NEVER BE CALLED AT ALL. SKSE sends no shutdown message and a
	      plugin dies with the process. Write your shim so that never being
	      stopped is an ordinary ending, not a leak you were saved from.
	   2. IT IS NEVER CALLED UNDER THE LOADER LOCK. Not from DllMain, not from a
	      static destructor, not from an atexit handler. A join under the loader
	      lock is a documented deadlock, and a deadlock at game exit is this
	      project's named worst case: MO2 goes on believing the game is running
	      and the build cannot be edited at all.
	   3. WHEN IT IS CALLED, it is called on your own model's dispatch thread,
	      after that thread has stopped delivering work to you, and never on the
	      thread of the game.
	   4. THE ADAPTER WAITS stopMs FOR THAT THREAD AND THEN ABANDONS IT. It does
	      not kill it - killing a thread inside third-party code is worse than
	      waiting - it stops waiting, writes one line naming your id, and touches
	      that model no further. Your DLL stays loaded, which costs nothing: SKSE
	      never unloads a plugin anyway. The per-model state your abandoned
	      thread can still reach is deliberately never freed, so returning late
	      into it is not a use-after-free.
	   5. YOU ARE NOT OBLIGED TO JOIN ANYTHING inside Stop, and you are advised
	      not to try if joining could take longer than stopMs.

	   Because of 1 and 4, ANYTHING THAT MUST NOT OUTLIVE THE GAME CANNOT BE
	   TIED TO THIS CALL. A child process, a GPU context, a socket, a microphone
	   somebody else will want: tie it to the PROCESS, with a Job Object carrying
	   JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, or with --parent-pid and a watchdog. A
	   surviving child keeps the microphone, the video card, and MO2's belief
	   that the game is still running. Stop is a courtesy; the Job Object is the
	   guarantee. */
	void    (SPEECHBROKERVOICE_CALL* Stop)(void* a_user);

	/* The phrases the adapter has been told to listen for, UTF-8. ADVISORY by
	   contract: make a prompt of them, hot words, a grammar, or nothing at all -
	   ignoring them is not a failure. May be NULL in the table.

	   THERE ARE AT MOST SPEECHBROKERVOICE_MAX_VOCABULARY OF THEM and the adapter
	   clips to that before it calls you, because the list is merged across every
	   installed subscriber and is otherwise unbounded. Clip again to whatever
	   your model can carry: the reference uses the first 40 as a prompt and drops
	   the rest, because an initial_prompt competes for a finite window and a long
	   one makes recognition worse (engine/models.py:103-107). You will not be
	   refused for clipping. The ORDER is the adapter's merge order and means
	   nothing - it is not a ranking, and do not read one into it.

	   ORDERING, WHICH THE REST OF THIS TABLE STATES AND THIS ENTRY USED NOT TO:
	   it is delivered on your own serialised dispatch thread, never before Start
	   has returned OK, and never after Stop. The array and the strings live for
	   the length of the call only. */
	void    (SPEECHBROKERVOICE_CALL* SetVocabulary)(void* a_user,
		const char* const* a_phrases, int32_t a_count);

	/* Take a finished buffer. MUST RETURN WITHIN A FEW MILLISECONDS AND MUST NOT
	   WAIT - not on a file, not on a socket, not on a GPU, not on a lock your own
	   worker is holding. Copy the samples, put the job on your own queue, return.

	   OK - accepted, and you now owe EXACTLY ONE Complete for this utteranceId.
	   Not two, not none: not on a cancel, not on a timeout, not on a dead server.
	   The debt is discharged by any status - OK, CANCELLED or FAILED - and it is
	   also discharged, without your doing anything, when the pass closes at its
	   deadline: after that the answer is STALE and you may stop carrying it.
	   BUSY - you are full; you owe nothing and nothing is counted against you as
	   a failure, and your weight in an argument is not touched. It is not free,
	   though, and here is exactly what it costs, because "the adapter logs a
	   rate" is a log line and not a consequence: a model whose busy rate stays
	   above one half over the adapter's window is taken off the INTERIM roster
	   and offered final passes only, and one line says so. A model that is busy
	   on those too is dropped for the session, with another line. Otherwise a
	   model that is busy on every pass looks healthy for ever - it never fails,
	   never times out, never accumulates the latency samples that would
	   reclassify it, and contributes nothing.
	   NOT_READY - your model has gone; the adapter stops submitting until you say
	   Ready again.

	   THE DEBT IS RECORDED BEFORE THE CALL, NOT BY THE RETURN VALUE. The adapter
	   marks the utterance outstanding before it enters Submit, and a non-OK
	   return retracts it. That ordering is what makes Complete legal from inside
	   Submit - an in-process model that answers at once should not have to own a
	   thread to do it - because the answer then always arrives against an
	   utterance the adapter already knows. The consequence is a rule: DO NOT
	   CALL Complete AND THEN RETURN BUSY OR NOT_READY. That is a protocol
	   violation, it is logged, and it is counted against you.

	   AT MOST ONE UNSENT REQUEST IS QUEUED FOR YOU, AND A LATER PASS OF THE SAME
	   TURN REPLACES IT. Two bounds on that, and they are the contract, not
	   niceties:
	     - A REPLACEMENT NEVER CROSSES A TURN. Turn 6's first pass is different
	       speech from turn 5's, not a longer reading of it, and replacing one
	       with the other would throw away a turn nobody ever answered.
	     - A PASS WITH final == 1 IS NEVER REPLACED. It is the only pass whose
	       loss is not recoverable by a later one.
	   A queued request that is dropped for either reason - the turn ended before
	   it was sent, or its deadline was already spent - is COUNTED against that
	   model in the same ledger as a timeout, for the same reason: a model that
	   silently misses the passes that matter must not look healthy. You are not
	   told, and you owe nothing for a request you never received.

	   The adapter times this call. Exceeding a few milliseconds does not fail it,
	   it gets measured - and a shim that keeps overrunning is logged, because
	   otherwise a bad shim is indistinguishable from a slow model.

	   WHAT A PASS ACTUALLY COSTS YOU IF YOU ARE ACROSS A NETWORK, so that the
	   decision about finalOnly is made with the numbers rather than against
	   them. Three terms, and only the middle one is yours:
	     - 1600 ms from the last word to the final pass. That is the adapter's
	       end-of-turn silence. It is a SETTING (pacer.endSilenceMs, and the
	       interim threshold beside it is 300 ms, and the ceiling between passes
	       4000 ms) - these are the shipping defaults and a player may change
	       them, so do not hard-code any of the three;
	     - the upload of the buffer: 64 kB per second of speech, from sample zero
	       every single pass;
	     - your inference.
	   A 400 ms server on a household uplink therefore lands about two seconds
	   after the speaker stopped, and every interim answer it sends is discarded
	   before then. That is a final-pass model, and saying so with finalOnly is
	   better than being one by accident. The one thing you may rely on across
	   every setting is final == 1; everything before it may arrive at any
	   cadence, including not at all. */
	int32_t (SPEECHBROKERVOICE_CALL* Submit)(void* a_user,
		const struct SpeechBrokerVoiceRequest* a_request);

	/* Advisory: the adapter no longer wants that answer - the turn ended, a
	   later pass replaced it, the deadline went by. You STILL owe the Complete,
	   with status CANCELLED if you dropped the work. May be NULL.

	   IT IS DELIVERED ON YOUR OWN SERIALISED DISPATCH THREAD, so it queues
	   behind whatever that thread is already doing and it arrives, in the normal
	   case, after you have already paid. For a model across a network it is
	   close to useless: by the time it reaches you the upload has gone and the
	   inference is running or done. That is not a defect to be fixed by making
	   Cancel re-entrant - a re-entrant Cancel against a third-party shim buys a
	   new class of bug for a saving nobody measured. It is a reason finalOnly
	   exists: the cheapest cancelled pass is the one never sent. */
	void    (SPEECHBROKERVOICE_CALL* Cancel)(void* a_user, int64_t a_utteranceId);
};

/* ------------------------------------------------------------------------- *
 * What the adapter hands the model mod: its functions
 * ------------------------------------------------------------------------- */

/* 0 is not a handle. Handles are monotone and are NEVER REUSED for the life of
   the process - not after Unregister, not after a model dies. That is what makes
   a four-hundred-millisecond answer landing on a dead handle safe rather than a
   lottery: with reuse it would land on some other model's bookkeeping, and the
   wrong model would be credited or blamed. */
typedef uint32_t SpeechBrokerVoiceHandle;

/* Filled in by Register. Everything a shim needs in order to size its buffers
   ONCE, at Start, before a single sample has arrived.

   THIS IS AN OUT-PARAMETER, SO structBytes MEANS THE OTHER THING. You allocate
   it, so you set a_session->structBytes = sizeof(struct SpeechBrokerVoiceSession)
   as YOU compiled it, before the call. The adapter reads that first, refuses with
   SPEECHBROKERVOICE_MALFORMED if it is not a multiple of 8 or is below
   SPEECHBROKERVOICE_SESSION_BYTES_V1, and otherwise writes min(your structBytes,
   its own sizeof) bytes and never one past the end. A LARGER BUFFER THAN IT
   KNOWS ABOUT IS PERFECTLY LEGAL - the cautious reading is the right one, and an
   earlier wording refused it for being "a size it did not recognise", which was
   unguessable. The adapter never writes its own sizeof into your buffer. An
   adapter newer than your shim is accepted by design, and without this rule that
   acceptance would put its extra fields on your stack, past the end of your
   object.

   ON ANY STATUS BUT OK THE ADAPTER WRITES NOTHING INTO a_session AT ALL, so a
   failed Register leaves whatever you put there and reading session.handle after
   one is your own bug, not an undefined one. */
struct SpeechBrokerVoiceSession
{
	uint32_t                       structBytes;

	/* The version this conversation is actually conducted at: the lower of the
	   two. It governs every struct in this file that has no abiVersion of its
	   own - Request, Format, Model, Fragment - and it is the version your
	   fragmentStride must match. If it is below what you declared, an older
	   adapter is talking to you: gate your newer reads on it and on structBytes,
	   exactly as "How a struct grows" prescribes. */
	uint32_t                       abiVersion;

	SpeechBrokerVoiceHandle        handle;

	/* The longest buffer that can ever be submitted. Allocate this once and stop
	   allocating.

	   IT IS A HARD BOUND AND THE ADAPTER PAYS FOR IT BY ENDING THE TURN. When a
	   speaker runs past it without pausing, the adapter closes the turn at that
	   point - the pass you get carries final == 1 - and the next sample begins a
	   new turn with a new turnId, from its own sample zero. That is a visible
	   cut in the middle of a sentence and it is the honest price: the
	   alternative, sliding the window forward, would make buffer sample zero
	   stop being turn sample zero, and every time in this contract - the anchors,
	   the snap slack, the matching of a piece against what has already been sent
	   - is measured from turn zero (engine/parts.py:31-33 says so in as many
	   words). One visible cut beats a silently shifting clock. */
	uint32_t                       maxRequestSamples;

	/* What you will actually be given, for the life of the session. If you
	   cannot handle it, refuse at Start. */
	struct SpeechBrokerVoiceFormat format;
};

/* ------------------------------------------------------------------------- *
 * NONE OF THESE FUNCTIONS THROWS.
 *
 * The rule is symmetric with the one over struct SpeechBrokerVoiceModel, and it
 * has to be, because every one of them allocates on your behalf: Complete copies
 * the whole fragment array and every string in it, Register copies the info, the
 * table and its strings, Log formats. The adapter catches everything at its own
 * boundary and turns it into a status - REFUSED out of Register and Complete,
 * silence out of Unregister, Ready and Log - and writes its own log line about
 * it. It never lets std::bad_alloc, or anything else, unwind into your frame.
 * An adapter-side failure is REFUSED and never MALFORMED, and it is NEVER
 * counted against your standing: this contract blames a model only for what the
 * model controls.
 *
 * The reason is not tidiness. Your shim may be compiled as C, or with /EHs-, in
 * which case there is no handler between that throw and the top of your worker
 * thread, and std::terminate takes SkyrimVR.exe down - the very outcome this
 * header spends a paragraph forbidding in the other direction. You also could
 * not honour "exactly one Complete" if Complete were the thing that threw.
 *
 * THE TABLE ITSELF IS A STATIC INSIDE THE ADAPTER AND LIVES FOR THE LIFE OF THE
 * PROCESS. It is the one pointer you must keep - Complete comes from your own
 * thread hundreds of milliseconds later, Ready at any time, Log after Stop - so
 * keep the pointer you were handed and do NOT copy the struct: the adapter never
 * replaces it, and a copy would only freeze structBytes at the moment you read
 * it. Every entry below stays callable after Unregister, after Stop, and after
 * your model has been abandoned; that is what makes the dead-handle rules in
 * Complete, Ready and Log mean anything at all.
 * ------------------------------------------------------------------------- */
struct SpeechBrokerVoiceHost
{
	uint32_t structBytes;
	uint32_t abiVersion;  /* what the ADAPTER was built with */

	/* Register, once, out of your SKSE message handler. a_info and a_model and
	   every string in them are borrowed for the length of the call; the adapter
	   copies the info, the table and the strings, so both may be on your stack.
	   a_session is yours to allocate and the adapter fills it - read the note on
	   the out-parameter protocol above struct SpeechBrokerVoiceSession before
	   you write this call.

	   THE TWO SIDES TOLERATE EACH OTHER BEING NEWER, AND EACH CHECKS ONLY WHAT
	   IT NEEDS. The adapter accepts any abiVersion of at least 1 and reads only
	   through the fields its own version knows - structBytes tells it what is
	   there, and fragmentStride does the same for the one array. You do the
	   mirror image, and you do it BEFORE you call this: refuse an adapter whose
	   host->abiVersion is below THE MINIMUM YOUR OWN CODE NEEDS - not below your
	   own version. That minimum is 1 for almost every shim, and it rises only on
	   the day you start reading a field some later version added. Then a
	   version-5 shim that needs nothing past version 1 keeps working on every
	   adapter ever shipped, which is the whole point.

	   Strict equality was tried one level up, at the bridge, and reworking the
	   bridge to version three knocked its own adapter out entirely: speech never
	   arrived at all that day. Compatibility made from one side is not
	   compatibility. Do not write == here, and do not write > either.

	   Refusal is said out loud on both sides, never a crash, and never at the
	   cost of another model's microphone:
	     VERSION   - abiVersion is 0, or so far outside the range that nothing
	                 can be read at all.
	     DUPLICATE - that id is registered already. First registration wins.
	     MALFORMED - something in what you handed over is wrong and the log line
	                 names it: a NULL Start, Stop or Submit; a reserved0 that is
	                 not 0 in either struct; a structBytes that is not a multiple
	                 of 8 or is below its struct's version-1 size; an empty id;
	                 a provides with no value the adapter knows.
	     REFUSED   - the adapter will not take you and it is not your code: the
	                 player has forbidden your kind, or the adapter is in a state
	                 that cannot accept a registration. A model refused for its
	                 kind is refused HERE, before Start and before any transport
	                 of yours is opened.

	   Register runs on the thread of the game during plugin load and does nothing
	   slow. Start comes later, on a worker. Do not bring your model up here. */
	int32_t (SPEECHBROKERVOICE_CALL* Register)(const struct SpeechBrokerVoiceModelInfo* a_info,
		const struct SpeechBrokerVoiceModel* a_model, struct SpeechBrokerVoiceSession* a_session);

	/* Leave. Afterwards no callback of yours is entered again, the handle is
	   dead, and the handle is never handed to anyone else. Stop is NOT called for
	   a model that unregisters itself.

	   NEVER CALL IT ON THE THREAD OF THE GAME. It blocks until the calls already
	   inside your shim have come back, and nothing in this contract bounds how
	   long one of your own calls may take - an overrunning Submit is only
	   measured, and Complete may be entered from inside it. An SKSE message
	   handler runs on the game thread against an 11.1 ms frame budget, so a
	   handler may only START an unregistration, on a thread of its own. The
	   adapter bounds its side of the wait at stopMs: after that it leaves the
	   handle draining for ever, abandons the model exactly as Stop promise 4
	   describes, and returns. If all you want is to go quiet, say Ready(handle,
	   0, ...) instead - it costs nothing and blocks nobody.

	   DO NOT CALL IT WHILE ANY THREAD OF YOURS IS INSIDE A HOST CALL, and not
	   only "not from inside a callback", which is what this note used to say and
	   which does not cover the case that actually happens: your worker calls
	   Complete from outside any callback, on its own thread, at the same moment
	   your message handler calls Unregister. Quiesce your own workers first,
	   then unregister.

	   The adapter does its half: Unregister marks the handle draining, releases
	   its registry lock, and only then waits. Complete, Ready and Log on a
	   draining, dead or unknown handle read one atomic and return at once -
	   Complete with SPEECHBROKERVOICE_STALE, the others silently. Complete never
	   blocks on Unregister, so the two cannot deadlock against each other.
	   Unregistering also REMOVES you from every pass still open, which is a
	   removal and not a timeout: it does not cost you standing. */
	void    (SPEECHBROKERVOICE_CALL* Unregister)(SpeechBrokerVoiceHandle a_handle);

	/* The text, at last - and it is a call, not a returned value. That single
	   decision is what lets a shim spend four hundred milliseconds on its own
	   thread without the adapter waiting on it.

	   Call it from whatever thread of yours finished the work, at any time,
	   including long after the request it answers - but never from the thread of
	   the game. It MAY be called from inside Submit: see the note on the debt
	   under Submit for why that is legal and what it forbids.

	   It does not run the arbitration. It copies your answer into the slot of
	   that pass, signals, and returns; the pass is sealed by a timer and
	   assembled afterwards on a worker - see WHAT CLOSES A PASS. So it is quick,
	   it does not block on the game thread, and several models calling it at once
	   do not queue behind each other's merging.

	   OK - taken, and the pass is still open.
	   STALE - nobody is waiting for this any more; stop working on that
	   utterance, nothing more is owed. The four histories behind STALE are listed
	   at the enum. Say it anyway when you are already holding an answer you know
	   is late: it closes the bookkeeping, and it is how the side that discards
	   tells the side that is wasting its time.
	   MALFORMED - the answer is wrong; the list of ways is at the head of struct
	   SpeechBrokerVoiceAnswer, and it is counted against you.
	   REFUSED - the adapter could not take it for reasons of its own. It is NOT
	   counted against you and you may not retry: the pass goes on without your
	   answer. */
	int32_t (SPEECHBROKERVOICE_CALL* Complete)(SpeechBrokerVoiceHandle a_handle,
		const struct SpeechBrokerVoiceAnswer* a_answer);

	/* Your model came up, or went away, without being asked. A server that stops
	   answering says Ready(handle, 0, "..."); when it returns, Ready(handle, 1,
	   NULL). The adapter then stops, or resumes, submitting - and logs it once
	   rather than once per utterance. Going not-ready REMOVES you from every pass
	   still open, as a removal and not a timeout, so it does not cost you
	   standing. Never call it on the thread of the game. The reason is for the
	   log only and is borrowed for the length of the call. No re-registration,
	   ever: a model that never comes back is a model that is never asked, and
	   recognition carries on with whatever else is installed. */
	void    (SPEECHBROKERVOICE_CALL* Ready)(SpeechBrokerVoiceHandle a_handle,
		int32_t a_ready, const char* a_reason);

	/* Write a line into the log of the adapter, from a thread of your own and
	   never from the thread of the game: it formats and allocates.

	   a_key is a localisation key of YOUR OWN table with your own prefix; the
	   adapter neither parses nor translates it, it writes it down - otherwise
	   every new model mod would mean an edit to the adapter.

	   WHAT THE LINE LOOKS LIKE, because this is the only diagnostic channel a
	   remote model has and an author has to be able to predict what they will
	   later have to read: the adapter writes your id, then the key verbatim, then
	   the arguments in order, tab separated. It does not substitute them into
	   anything - it does not have your string - so the numbered placeholders
	   {0} {1} in your own table are for whoever renders that table later, not
	   for the adapter.

	   The arguments are an array plus a count, the house form used everywhere
	   else in this contract. One argument was not enough: the lines that actually
	   diagnose a model are of the shape "endpoint {0} answered {1} after {2} ms,
	   retry {3} of {4}", and with a single argument a shim must either pre-join
	   everything into one string - which defeats having a key at all - or print
	   four lines that have to be read back together. a_argCount is between 0 and
	   SPEECHBROKERVOICE_MAX_LOG_ARGS; a_args may be NULL only when a_argCount is
	   0, no element of it may be NULL, and a call that breaks either is dropped
	   with one log line of the adapter's own. Every string here, key and
	   arguments alike, is borrowed for the length of the call. */
	void    (SPEECHBROKERVOICE_CALL* Log)(SpeechBrokerVoiceHandle a_handle,
		int32_t a_level, const char* a_key, const char* const* a_args, int32_t a_argCount);
};

#pragma pack(pop)

/* ------------------------------------------------------------------------- *
 * WHAT CLOSES A PASS, AND WHY A SHIM IS TOLD
 *
 * A pass is one buffer offered to every model that is taking part in it. The
 * arbitration above this line needs a CLOSED SET of answers - it picks a lane
 * from among them and counts how many of them agree - so something has to decide
 * that no more answers are coming. In the python this is ported from, that
 * something was a blocking wait per model (engine/engine.py:126) and the set was
 * closed by arithmetic. Nothing blocks any more, so the rule is written here
 * instead, and it is part of the contract because it is what STALE means and
 * what your standing is computed from.
 *
 * THE ADAPTER KEEPS ONE COLLECTOR PER utteranceId, and it holds exactly four
 * things: the EXPECTED SET of models, the answers that have arrived, the
 * deadline, and a closed flag.
 *
 * THE EXPECTED SET IS FIXED WHEN THE PASS IS CREATED, and it only ever SHRINKS.
 * Three things remove a model from it, and none of the three is a timeout or
 * costs that model anything:
 *   - its queued copy of this pass is dropped before it is ever sent - replaced
 *     by a later pass of the same turn, or its budget was already spent;
 *   - it calls Unregister;
 *   - it says Ready(handle, 0).
 * Each removal is logged as a removal. Without this rule the set is either fixed
 * and charges timeouts to models that were never asked, or floating and can go
 * "complete" before a model's Submit has even been entered.
 *
 * AND THE ADAPTER NEVER ENTERS Submit FOR A PASS THAT HAS ALREADY CLOSED. Such a
 * queue entry is dropped instead. Sending it would buy nothing but a guaranteed
 * STALE - and, for a model across a network, a paid upload and a paid inference
 * for an answer that cannot be used.
 *
 * A COLLECTOR CLOSES ON WHICHEVER COMES FIRST:
 *   - every model in the expected set has answered - with OK, CANCELLED or
 *     FAILED, all three count as answered; or
 *   - the pass deadline goes by. There is a real timer behind it, on the
 *     adapter's own waiting thread, and not a hope that somebody will notice. A
 *     silent model must not be able to stop a pass forever: this project has
 *     already lost a whole evening of speech to a pass that was never assembled,
 *     and a silent loss is worse than a noisy failure because nothing in any log
 *     says it happened.
 *
 * THE TIMER ONLY SEALS; A WORKER ASSEMBLES. The waiting thread is the one from
 * Scheduler - "one thread for everything that waits" - and a task runs in that
 * thread. So when a collector closes, the waiting thread does one thing: it
 * marks it closed and hands it on. The folding of the answers, the snapping onto
 * anchors, the reconciliation and the handing of text to the bridge all happen
 * on a worker. They are not fast, and doing them on the waiting thread would
 * make every other armed deadline in the adapter late behind them - which would
 * produce out-of-order closes as a matter of course, which is the very thing the
 * next paragraph exists to guard against.
 *
 * ARBITRATION RUNS EXACTLY ONCE PER COLLECTOR, AT CLOSE, AND THE ORDER GUARD IS
 * APPLIED THERE AND NOWHERE ELSE. The passes of a turn are ordered by serial,
 * each one a re-read from sample zero, and the reconciliation above this line is
 * append-only: feeding it serial 3 after serial 4 has been reconciled would emit
 * corrections pointing backwards in time. So at close, under the turn's own
 * lock, the collector compares its serial with the last serial already
 * reconciled for that turn:
 *   - behind it: THE WHOLE COLLECTOR IS ABANDONED. No arbitration, no
 *     reconciliation, and every answer inside it is treated as STALE;
 *   - ahead of it: the arbitration runs and the turn's last-reconciled serial
 *     advances to this one, inside the same close path and under the same lock.
 * The guard cannot live at Complete, where an earlier draft put it: an answer is
 * admitted to its slot while its own pass is still legitimately open, and the
 * reconciliation happens later. Two collectors of one turn have independent
 * timers armed at different moments, so pass 4 can close all-answered while pass
 * 3 is still waiting out a slow model. Only the close order matters, and only
 * close can see it.
 *
 * Every model that was still in the expected set at close and had not delivered
 * is recorded as a TIMEOUT FAILURE against its standing - the same bookkeeping
 * engine/engine.py:128 did on an expired future - which is what keeps a
 * permanently stalled third-party model from holding its full voting weight
 * forever while contributing nothing.
 *
 * AFTER CLOSING, THE utteranceId IS RETIRED AND YOUR ANSWER IS STALE. Retirement
 * is by the SET OF OPEN COLLECTORS and not by a high-water mark over all ids: an
 * utteranceId that names no open collector is retired, and that is one lookup.
 * The set cannot grow without bound, because every collector has a deadline and
 * the timer that fires it. A high-water mark was tried in an earlier draft and
 * is wrong in two ways at once - a pass that closes early would retire the
 * answers of passes still legitimately open, and it would retire the probe, whose
 * utteranceId is issued before any real turn and whose collector is meant to
 * outlive them all.
 *
 * A REQUEST WITH deadlineMs 0 IS NOT RETIRED BY ANY OF THIS. The adapter holds
 * at most one per model, so a small explicit set costs nothing and cannot grow;
 * it is retired when that model unregisters or the session ends.
 *
 * AND THERE IS NO GRACE PERIOD. An earlier draft of this contract carried an
 * answerGraceMs, on the idea that a slow model's answer could still be folded in
 * after the pass had been assembled and its text sent onward. It is gone, and
 * the reason is written here so that nobody re-adds it as an obvious
 * improvement: re-folding one late answer on its own does not refine the
 * existing reading, it produces a SECOND reading of the same speech with its own
 * segmentation, and the bridge then has to revoke an utterance a person has
 * already acted on. Refinement across models happens inside one pass or not at
 * all. The honest consequence, and it is a real cost: A MODEL THAT ANSWERS AFTER
 * THE DEADLINE CONTRIBUTES NOTHING TO THAT PASS. For a fast local model that
 * never happens. For a model across a network it means the interim passes are
 * not its business, and that is what finalOnly is for.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * The probe, and why it carries no flag
 *
 * Straight after a successful Start the adapter submits one second of digital
 * silence as an ORDINARY request. It is ordinary in every field: a turnId and a
 * serial of its own, final == 1 because the buffer will not grow, and
 * deadlineMs 0 because it is not timed. Those values are stated so that a shim
 * can validate its inputs without having to guess, which it must be able to do.
 *
 * THERE IS NO PROBE FLAG AND THERE MUST NEVER BE ONE, and the honest reason is
 * narrower than the one an earlier draft gave. A buffer of digital silence is of
 * course recognisable to anyone who looks at it, so the probe does not defeat a
 * shim that is determined to cheat; nothing in a header can. What a flag would
 * do is make cheating FREE and obvious - one branch, no analysis - and turn the
 * one check in this system that does not rest on believing a model into a check
 * a model opts out of. It catches the case that actually happens: a model that
 * genuinely invents on silence and does not know it. This is also why the buffer
 * guarantee is worded as "no trailing silence that the adapter detected" rather
 * than "there is speech in here": a shim that refused a quiet buffer as
 * malformed would be recognising the probe by accident, and its refusal would be
 * indistinguishable from honesty.
 *
 * A model that answers silence with text is recorded as one that invents, and
 * its weight when two models disagree falls accordingly. This is not a
 * hypothetical: the first model ever registered here answered a buffer of zeros
 * with a full sentence, with its own voice-activity filter switched on.
 *
 * NOT ANSWERING IT IS THE WORST OPTION, NOT THE CHEAPEST. The reference probed
 * inside registration and would not let a model into the pool until it had
 * (engine/models.py:209-214), and that property is kept here even though nothing
 * blocks any more: A MODEL TAKES PART IN NO PASS UNTIL ITS PROBE ANSWER HAS
 * ARRIVED. A model that never answers is never asked for anything, and one log
 * line says so. Otherwise ignoring the probe would leave the invention count at
 * zero out of zero - which reads as a perfect record - while the unproven model
 * voted at full weight.
 *
 * THE PROBE IS NOT TIMED AND CANNOT COST YOU ANYTHING BUT HONESTY. It carries
 * deadlineMs 0, and its outcome feeds the invention record only - never a
 * latency sample, never a timeout failure. The reason is that the first
 * inference of a session pays for workspace allocation and autotune and can take
 * seconds where the steady state is a tenth of one; charging that to a model
 * would cut the standing of every heavy model before it had answered a single
 * real utterance, and the probe is also deliberately the warm-up that pays that
 * cost while the game is still loading rather than on the first thing the player
 * says. A finalOnly model is probed like any other - the probe is not a pass of
 * a turn. Answer it exactly as you would answer a real request.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * Two rules about the process, which is shared
 *
 * A model mod is a DLL inside SkyrimVR.exe alongside the bridge, the adapter,
 * ENB, Community Shaders and every other SKSE plugin. Two things a shim may be
 * tempted to do are process-global and are therefore forbidden here, because the
 * mod that pays for them is somebody else's:
 *
 *   - DO NOT CHANGE THE PROCESS DLL SEARCH POLICY. Not the PATH environment
 *     variable, and not SetDefaultDllDirectories, which is the same mistake in
 *     better clothes: it changes bare-name resolution for every subsequent
 *     LoadLibrary in the process, including other plugins' delay-loaded imports
 *     and the game's own lazy loads, and it cannot be scoped to one caller. To
 *     bring your own dependencies up, preload them by FULL PATH with
 *     LoadLibraryW; a later bare-name load finds the already-loaded module and
 *     resolves without any policy change at all. AddDllDirectory alone is
 *     harmless and is enough alongside it.
 *   - DO NOT LEAVE ANYTHING RUNNING. See Stop: the guarantee is a Job Object
 *     with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, or --parent-pid and a watchdog.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * The layout, asserted in YOUR compiler
 *
 * These are not decoration and they are not for us: every other guard in this
 * system lives on the adapter's side of the line and runs in the adapter's
 * build. This file is the only one a model author compiles, so this is the only
 * place a wrong layout can be caught in the build where it was created.
 *
 * structBytes can detect a TRUNCATION - an older sender with fewer fields - and
 * that is all it can detect. It cannot see a different LAYOUT: a struct of a
 * plausible size with its fields at the wrong offsets sends a plausible
 * structBytes, and the adapter reads a fabricated pointer out of what it
 * believes is `samples` and hands it to memcpy.
 *
 * BE CLEAR ABOUT WHAT GUARDS WHAT, BECAUSE THE PREVIOUS WORDING WAS WRONG AND
 * THE CORRECTION WAS MEASURED, NOT ARGUED. The #pragma pack(push, 8) above is
 * the guard, and it is sufficient by itself against every packing an outside
 * build can impose: this file was compiled at /Zp1, /Zp2 and /Zp4, and inside an
 * enclosing #pragma pack(push, 1) region, and all four produced byte-identical
 * structs - 16, 72, 64, 40, 56, 56, 32, 48 - because a local pack directive
 * beats the command line. So none of the assertions below fire for /Zp, and an
 * earlier draft of this note that promised they would was simply mistaken.
 *
 * WHAT THE ASSERTIONS ARE FOR IS THE PRAGMA BEING LOST - deleted by a later
 * editor, preprocessed away, or not honoured by a toolchain that spells packing
 * some other way - and a mistake made while editing this file. With the pragma
 * removed, /Zp1, /Zp2 and /Zp4 were all rejected, and in every one of the three
 * THE FIRST THING THAT FIRED WAS THE ALIGNMENT BATTERY, not an offset. That is
 * why the alignment assertions are here and must not be deleted as redundant: at
 * /Zp4 every offset in this file still holds and every size is unchanged -
 * members that are already 4- or 8-aligned do not move - and alignment is the
 * only property that changes. A clean offset battery is not proof that the
 * layout is right.
 *
 * If one of these fails, do not edit the number. Find the pack region or the
 * /Zp switch that changed your layout and take it out.
 * ------------------------------------------------------------------------- */

SPEECHBROKERVOICE_ASSERT(sizeof(void*) == 8,
	"Speech Broker: this contract is x64 only - SkyrimVR.exe ships as x64 and nothing else");
SPEECHBROKERVOICE_ASSERT(sizeof(float) == 4,
	"Speech Broker: float must be 32 bits");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceFormat) == SPEECHBROKERVOICE_FORMAT_BYTES_V1,
	"SpeechBrokerVoiceFormat: size must equal SPEECHBROKERVOICE_FORMAT_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFormat, structBytes)  ==  0, "Format::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFormat, sampleRate)   ==  4, "Format::sampleRate");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFormat, channels)     ==  8, "Format::channels");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFormat, sampleFormat) == 12, "Format::sampleFormat");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceModelInfo) == SPEECHBROKERVOICE_MODELINFO_BYTES_V1,
	"SpeechBrokerVoiceModelInfo: size must equal SPEECHBROKERVOICE_MODELINFO_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, structBytes)   ==  0, "ModelInfo::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, abiVersion)    ==  4, "ModelInfo::abiVersion");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, id)            ==  8, "ModelInfo::id");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, name)          == 16, "ModelInfo::name");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, language)      == 24, "ModelInfo::language");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, provides)      == 32, "ModelInfo::provides");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, kind)          == 40, "ModelInfo::kind");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, budgetMs)      == 44, "ModelInfo::budgetMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, startupMs)     == 48, "ModelInfo::startupMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, stopMs)        == 52, "ModelInfo::stopMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, finalOnly)     == 56, "ModelInfo::finalOnly");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, maxInFlight)   == 60, "ModelInfo::maxInFlight");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, declaredClass) == 64, "ModelInfo::declaredClass");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModelInfo, reserved0)     == 68, "ModelInfo::reserved0");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceRequest) == SPEECHBROKERVOICE_REQUEST_BYTES_V1,
	"SpeechBrokerVoiceRequest: size must equal SPEECHBROKERVOICE_REQUEST_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, structBytes) ==  0, "Request::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, serial)      ==  4, "Request::serial");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, turnId)      ==  8, "Request::turnId");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, utteranceId) == 16, "Request::utteranceId");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, final)       == 24, "Request::final");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, sampleCount) == 28, "Request::sampleCount");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, samples)     == 32, "Request::samples");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, lostSamples) == 40, "Request::lostSamples");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, deadlineMs)  == 44, "Request::deadlineMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceRequest, format)      == 48, "Request::format");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceFragment) == SPEECHBROKERVOICE_FRAGMENT_BYTES_V1,
	"SpeechBrokerVoiceFragment: size must equal SPEECHBROKERVOICE_FRAGMENT_BYTES_V1 - it is the stride");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, startMs)      ==  0, "Fragment::startMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, endMs)        ==  4, "Fragment::endMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, text)         ==  8, "Fragment::text");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, score)        == 16, "Fragment::score");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, endsSentence) == 20, "Fragment::endsSentence");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, lastWordProb) == 24, "Fragment::lastWordProb");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, noSpeechProb) == 28, "Fragment::noSpeechProb");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, medianGapMs)  == 32, "Fragment::medianGapMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceFragment, words)        == 36, "Fragment::words");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceAnswer) == SPEECHBROKERVOICE_ANSWER_BYTES_V1,
	"SpeechBrokerVoiceAnswer: size must equal SPEECHBROKERVOICE_ANSWER_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, structBytes)    ==  0, "Answer::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, abiVersion)     ==  4, "Answer::abiVersion");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, utteranceId)    ==  8, "Answer::utteranceId");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, serial)         == 16, "Answer::serial");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, status)         == 20, "Answer::status");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, latencyMs)      == 24, "Answer::latencyMs");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, fragmentStride) == 28, "Answer::fragmentStride");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, fragments)      == 32, "Answer::fragments");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, fragmentCount)  == 40, "Answer::fragmentCount");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, lostSamples)    == 44, "Answer::lostSamples");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceAnswer, failed)         == 48, "Answer::failed");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceModel) == SPEECHBROKERVOICE_MODEL_BYTES_V1,
	"SpeechBrokerVoiceModel: size must equal SPEECHBROKERVOICE_MODEL_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, structBytes)   ==  0, "Model::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, reserved0)     ==  4, "Model::reserved0");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, user)          ==  8, "Model::user");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, Start)         == 16, "Model::Start");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, Stop)          == 24, "Model::Stop");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, SetVocabulary) == 32, "Model::SetVocabulary");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, Submit)        == 40, "Model::Submit");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceModel, Cancel)        == 48, "Model::Cancel");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceSession) == SPEECHBROKERVOICE_SESSION_BYTES_V1,
	"SpeechBrokerVoiceSession: size must equal SPEECHBROKERVOICE_SESSION_BYTES_V1 - it is the out-parameter floor");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceSession, structBytes)       ==  0, "Session::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceSession, abiVersion)        ==  4, "Session::abiVersion");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceSession, handle)            ==  8, "Session::handle");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceSession, maxRequestSamples) == 12, "Session::maxRequestSamples");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceSession, format)            == 16, "Session::format");

SPEECHBROKERVOICE_ASSERT(sizeof(struct SpeechBrokerVoiceHost) == SPEECHBROKERVOICE_HOST_BYTES_V1,
	"SpeechBrokerVoiceHost: size must equal SPEECHBROKERVOICE_HOST_BYTES_V1");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, structBytes) ==  0, "Host::structBytes");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, abiVersion)  ==  4, "Host::abiVersion");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, Register)    ==  8, "Host::Register");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, Unregister)  == 16, "Host::Unregister");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, Complete)    == 24, "Host::Complete");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, Ready)       == 32, "Host::Ready");
SPEECHBROKERVOICE_ASSERT(offsetof(struct SpeechBrokerVoiceHost, Log)         == 40, "Host::Log");

/* The alignment battery. This is the half that catches /Zp4, where every offset
   above still holds. Skipped only on a compiler with no spelling of alignof at
   all; every toolchain named in this file has one. */
#if defined(SPEECHBROKERVOICE_ALIGNOF)
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceModelInfo) == 8,
	"SpeechBrokerVoiceModelInfo: alignment must be 8 - a pack region or /Zp is in force");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceRequest) == 8,
	"SpeechBrokerVoiceRequest: alignment must be 8 - a pack region or /Zp is in force");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceFragment) == 8,
	"SpeechBrokerVoiceFragment: alignment must be 8 - a pack region or /Zp is in force");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceAnswer) == 8,
	"SpeechBrokerVoiceAnswer: alignment must be 8 - a pack region or /Zp is in force");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceModel) == 8,
	"SpeechBrokerVoiceModel: alignment must be 8 - a pack region or /Zp is in force");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceHost) == 8,
	"SpeechBrokerVoiceHost: alignment must be 8 - a pack region or /Zp is in force");
/* Format and Session hold nothing wider than four bytes, so their alignment is 4
   in a correct build and this states the fact rather than catching anything. */
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceFormat) == 4,
	"SpeechBrokerVoiceFormat: alignment must be 4");
SPEECHBROKERVOICE_ASSERT(SPEECHBROKERVOICE_ALIGNOF(struct SpeechBrokerVoiceSession) == 4,
	"SpeechBrokerVoiceSession: alignment must be 4");
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPEECHBROKER_VOICE_MODEL_H */
