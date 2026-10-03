#include "audio/Devices.h"

#include "Loc.h"

// WASAPI, and nothing of WASAPI leaves this file. The header names no COM type
// on purpose (audio/Devices.h:84-86), so every Windows header the choosing of a
// microphone needs is included here and only here.
//
// NOMINMAX and WIN32_LEAN_AND_MEAN are set defensively: this translation unit
// pulls in no engine header of its own, and the min/max macros of Windows break
// perfectly ordinary C++ in any file that later grows an <algorithm> call.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <mmdeviceapi.h>
#include <objbase.h>
#include <propidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Voice
{
	// THE RATE BELONGS TO THE DEVICE, and this file is where that was learned even
	// though it is not where it is enforced.
	//
	// The reference asked for a rate and took the first that opened: 16000, then
	// 48000, then 44100 (engine/audio.py:91), and later 16000 then the declared
	// default of the device (voice-service.py:203). Through PortAudio that mostly
	// worked, because PortAudio would quietly put a resampler in the way. WASAPI in
	// shared mode does not: the endpoint runs at the rate of the mix and answers
	// AUDCLNT_E_UNSUPPORTED_FORMAT - the "Invalid sample rate" the reference kept
	// printing at :212 - to anything else. So nothing here asks for a rate at all.
	// The rate is read off the endpoint, published as CaptureFormat, and the
	// resampling to 16 kHz is ours (turn/Resample.h).
	//
	// Devices only NAMES the endpoints. Opening them, the three attempts and the
	// pause between them (ReopenPolicy, voice-service.py:199 and :218) and the
	// reopening on AUDCLNT_E_DEVICE_INVALIDATED all belong to Capture, which is the
	// one thing that owns a device. What this file owes that loop is the order it
	// walks: Rank decides which microphone is tried first and which is tried at
	// all, and it decides it without touching COM so that the deciding can be
	// tested on a machine with no microphone in it.

	namespace
	{
		// PKEY_Device_FriendlyName, spelled out rather than taken from
		// <functiondiscoverykeys_devpkey.h>. That header only DECLARES the key unless
		// INITGUID was defined ahead of it, and defining INITGUID in one file of a
		// project whose other files pull in COM headers of their own is a link-time
		// coin toss. The value is part of the interface of Windows and can no more
		// change than the interface itself.
		constexpr PROPERTYKEY kFriendlyName{
			{ 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
		};

		// The one sound interface this build has. It is a constant and not a setting
		// on purpose: a setting would pretend that MME and DirectSound are reachable
		// from inside the game, and a setting that cannot be obeyed is worse than no
		// setting at all. DeviceSettings::inputApi is read, answered once in the log
		// and then ignored - audio/Devices.h:49-59 says so, and this is the body of
		// that promise.
		constexpr std::wstring_view kWasapi{ L"wasapi" };

		// Said once per session rather than once per open: a microphone that keeps
		// going away would otherwise repeat the same complaint at every reopen.
		std::once_flag g_apiReported;

		// PROPVARIANT is a C thing with a rule attached - whoever initialises it
		// clears it - and an early return must not be able to forget the rule.
		struct PropValue
		{
			PropValue() { PropVariantInit(&value); }
			~PropValue() { PropVariantClear(&value); }

			PropValue(const PropValue&) = delete;
			PropValue(PropValue&&) = delete;
			PropValue& operator=(const PropValue&) = delete;
			PropValue& operator=(PropValue&&) = delete;

			PROPVARIANT value{};
		};

		// The same rule for the string IMMDevice::GetId hands out of the COM
		// allocator.
		class CoString
		{
		public:
			CoString() = default;

			~CoString()
			{
				if (_value != nullptr) {
					CoTaskMemFree(_value);
				}
			}

			CoString(const CoString&) = delete;
			CoString(CoString&&) = delete;
			CoString& operator=(const CoString&) = delete;
			CoString& operator=(CoString&&) = delete;

			LPWSTR* Receive() noexcept { return &_value; }

			const wchar_t* Get() const noexcept { return _value != nullptr ? _value : L""; }

		private:
			LPWSTR _value{ nullptr };
		};

		// UTF-8 out, because Device::name goes into the log and the log is UTF-8.
		std::string Narrow(std::wstring_view a_wide)
		{
			if (a_wide.empty()) {
				return {};
			}
			const auto span = static_cast<int>(a_wide.size());
			const auto need = WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), span,
				nullptr, 0, nullptr, nullptr);
			if (need <= 0) {
				return {};
			}
			std::string out(static_cast<std::size_t>(need), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), span, out.data(), need, nullptr, nullptr);
			return out;
		}

		// And wide back in, because the patterns arrive from the settings as UTF-8
		// and the names they are matched against are wide.
		std::wstring Widen(std::string_view a_utf8)
		{
			if (a_utf8.empty()) {
				return {};
			}
			const auto span = static_cast<int>(a_utf8.size());
			const auto need = MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), span, nullptr, 0);
			if (need <= 0) {
				return {};
			}
			std::wstring out(static_cast<std::size_t>(need), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), span, out.data(), need);
			return out;
		}

		// A case-insensitive substring, which is what the reference matched on
		// (engine/audio.py:45, `pattern.lower() in d["name"].lower()`).
		//
		// It is done in wide characters through the ordinal comparison of Windows
		// and not by folding bytes: a device name is whatever the driver felt like
		// writing, on a Russian machine it is Cyrillic, and tolower() over UTF-8
		// bytes would fold exactly none of it - a pattern would then match or not
		// match depending on which case the driver used, which is the kind of defect
		// that only ever shows up on somebody else machine.
		bool ContainsFold(std::wstring_view a_name, std::wstring_view a_pattern)
		{
			if (a_pattern.empty() || a_pattern.size() > a_name.size()) {
				return false;
			}
			const auto span = static_cast<int>(a_pattern.size());
			const auto last = a_name.size() - a_pattern.size();
			for (std::size_t at = 0; at <= last; ++at) {
				if (CompareStringOrdinal(a_name.data() + at, span, a_pattern.data(), span, TRUE) ==
					CSTR_EQUAL) {
					return true;
				}
			}
			return false;
		}

		// A code out of COM in the shape a person can look up. A number is not a
		// sentence and is not translated; what is translated is the line it goes in.
		std::string HexCode(HRESULT a_hr)
		{
			return fmt::format("0x{:08X}", static_cast<std::uint32_t>(a_hr));
		}

		std::wstring IdOf(IMMDevice* a_endpoint)
		{
			CoString id;
			if (FAILED(a_endpoint->GetId(id.Receive()))) {
				return {};
			}
			return std::wstring{ id.Get() };
		}

		std::string NameOf(IMMDevice* a_endpoint)
		{
			Microsoft::WRL::ComPtr<IPropertyStore> store;
			if (FAILED(a_endpoint->OpenPropertyStore(STGM_READ, store.GetAddressOf()))) {
				return {};
			}
			PropValue name;
			if (FAILED(store->GetValue(kFriendlyName, &name.value))) {
				return {};
			}
			if (name.value.vt != VT_LPWSTR || name.value.pwszVal == nullptr) {
				return {};
			}
			return Narrow(name.value.pwszVal);
		}

		// engine/audio.py:29-34 and :46-47 sorted the candidates by sound subsystem,
		// because through PortAudio one physical microphone was visible several times
		// over and the copies failed in different ways. Here there is one subsystem,
		// so the preference has nothing to sort - but the key still ships in the
		// settings file this module inherited, and a player who edits a key must not
		// be ignored in silence.
		void ReportForeignApis(const std::vector<std::string>& a_apis)
		{
			std::call_once(g_apiReported, [&a_apis]() {
				for (const auto& api : a_apis) {
					if (!ContainsFold(Widen(api), kWasapi)) {
						Loc::Info("$SPEECHBROKERVOICE_LOG_AUDIO_API_IGNORED", api);
					}
				}
			});
		}

		bool AlreadyTaken(const std::vector<Device>& a_taken, const std::wstring& a_id)
		{
			for (const auto& device : a_taken) {
				if (device.id == a_id) {
					return true;
				}
			}
			return false;
		}
	}

	std::vector<Device> Devices::Inputs()
	{
		std::vector<Device> out;

		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
		auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			IID_PPV_ARGS(enumerator.GetAddressOf()));
		if (FAILED(hr)) {
			// The usual reason is the one this class warns about in its header: a
			// thread that never initialised COM. It is a failure of ours, not of the
			// machine, and it has to read as one.
			Loc::Error("$SPEECHBROKERVOICE_LOG_AUDIO_ENUM_FAILED", HexCode(hr));
			return out;
		}

		// The default is asked for FIRST, so that the flag can be set while the list
		// is being filled instead of walking it twice.
		//
		// eConsole and not eCommunications, deliberately. The two roles are two
		// different microphones on a machine where the player has set them apart, and
		// the reference took the plain default of the system (engine/audio.py:90, the
		// `None` device of sounddevice). Quietly preferring the communications role
		// here would hand some players a different microphone than the service they
		// are migrating from gave them, which is exactly the sort of change that gets
		// reported as "the new version does not hear me".
		std::wstring                      defaultId;
		Microsoft::WRL::ComPtr<IMMDevice> fallback;
		hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, fallback.GetAddressOf());
		if (SUCCEEDED(hr)) {
			defaultId = IdOf(fallback.Get());
		} else {
			// Not an error: a machine with no capture endpoint at all answers exactly
			// this, and the caller is about to be told the list is empty anyway.
			Loc::Debug("$SPEECHBROKERVOICE_LOG_AUDIO_NO_DEFAULT", HexCode(hr));
		}

		Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
		hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.GetAddressOf());
		if (FAILED(hr)) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_AUDIO_ENUM_FAILED", HexCode(hr));
			return out;
		}

		// DEVICE_STATE_ACTIVE only. An endpoint that is unplugged or switched off in
		// the mixer opens with an error a second later, and offering it as a
		// candidate only spends one of the three attempts on something that cannot
		// work.
		UINT count = 0;
		hr = collection->GetCount(&count);
		if (FAILED(hr)) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_AUDIO_ENUM_FAILED", HexCode(hr));
			return out;
		}
		out.reserve(count);

		for (UINT at = 0; at < count; ++at) {
			Microsoft::WRL::ComPtr<IMMDevice> endpoint;
			hr = collection->Item(at, endpoint.GetAddressOf());
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_AUDIO_ENDPOINT_SKIPPED", HexCode(hr));
				continue;
			}

			Device device;
			device.id = IdOf(endpoint.Get());
			if (device.id.empty()) {
				// Without an id there is nothing to open later, so this one cannot be a
				// candidate however good its name is.
				Loc::Warn("$SPEECHBROKERVOICE_LOG_AUDIO_ENDPOINT_SKIPPED", HexCode(E_FAIL));
				continue;
			}

			device.name = NameOf(endpoint.Get());
			if (device.name.empty()) {
				// A nameless endpoint is still an openable one. It cannot be matched by a
				// pattern and it can still be the default, so it stays in the list under
				// its id - which reads badly but is the truth about what was found.
				device.name = Narrow(device.id);
			}
			device.isDefault = !defaultId.empty() && device.id == defaultId;

			if (device.isDefault) {
				Loc::Debug("$SPEECHBROKERVOICE_LOG_AUDIO_ENDPOINT_DEFAULT", device.name);
			} else {
				Loc::Debug("$SPEECHBROKERVOICE_LOG_AUDIO_ENDPOINT", device.name);
			}
			out.push_back(std::move(device));
		}

		if (out.empty()) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_AUDIO_NO_INPUTS");
		}
		return out;
	}

	std::vector<Device> Devices::Rank(const std::vector<Device>& a_all,
		const DeviceSettings&                                    a_settings)
	{
		ReportForeignApis(a_settings.inputApi);

		std::vector<Device> out;
		out.reserve(a_all.size());

		// Widened once for the whole search instead of once per pattern: five
		// patterns against a dozen endpoints is sixty conversions of the same names
		// otherwise, and the answer is the same every time.
		std::vector<std::wstring> names;
		names.reserve(a_all.size());
		for (const auto& device : a_all) {
			names.push_back(Widen(device.name));
		}

		for (const auto& pattern : a_settings.input) {
			const auto wanted = Widen(pattern);
			const auto before = out.size();
			for (std::size_t at = 0; at < a_all.size(); ++at) {
				// A DEVICE IS TAKEN BY THE FIRST PATTERN THAT MATCHES IT AND NEVER AGAIN.
				// The reference appended a device once per matching pattern
				// (engine/audio.py:48-50, the inner `out.append` with no memory of what is
				// already in the list), so "Quest" and "Oculus" over one headset put the
				// same endpoint in twice: the opening loop then spent two of its attempts
				// on one microphone, and the log claimed two candidates where there was
				// one. Nothing about it was fatal, which is why it survived.
				if (AlreadyTaken(out, a_all[at].id)) {
					continue;
				}
				if (ContainsFold(names[at], wanted)) {
					out.push_back(a_all[at]);
				}
			}
			if (out.size() == before) {
				// An empty pattern lands here too, and that is the intent: it matches
				// nothing rather than everything. In the settings an empty string in this
				// list is a typo, and a typo that silently promoted the first endpoint on
				// the machine to first choice would be indistinguishable from the list
				// working.
				Loc::Debug("$SPEECHBROKERVOICE_LOG_AUDIO_PATTERN_UNMATCHED", pattern);
			}
		}

		// The default goes last, as the reference put it last (engine/audio.py:90) -
		// but only once. The reference appended it unconditionally, so a default
		// microphone that had already matched a pattern was tried twice; here it is
		// skipped if it is already in the list, and the player who wants "this
		// microphone or none" turns it off entirely.
		if (a_settings.allowDefault) {
			for (const auto& device : a_all) {
				if (device.isDefault && !AlreadyTaken(out, device.id)) {
					out.push_back(device);
					break;
				}
			}
		}

		if (out.empty()) {
			// voice-service.py:181. The list of what was looked for belongs in this
			// line, because "no input device" without it is a report nobody can act on.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_AUDIO_NO_CANDIDATES", a_settings.input.size(),
				a_all.size());
			return out;
		}

		// voice-service.py:184 logged the candidates as one joined line. One line
		// each instead: a device name is allowed to contain the separator, and a
		// numbered line also says plainly which one will be tried first.
		std::size_t place = 0;
		for (const auto& device : out) {
			++place;
			Loc::Info("$SPEECHBROKERVOICE_LOG_AUDIO_CANDIDATE", place, device.name);
		}
		return out;
	}
}
