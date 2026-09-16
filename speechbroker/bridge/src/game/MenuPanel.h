#pragma once

namespace SpeechBroker
{
	// The bridge's window in the in-game mod menu.
	//
	// It is drawn through SKSE Menu Framework - somebody else's mod, which draws
	// one shared panel and lets plugins into it. The dependency is **soft**: the
	// header links nothing and looks its functions up in a library that is already
	// loaded. No such mod, no window, and the bridge never even finds out.
	//
	// The window changes the current session only. The lasting value is set in the
	// settings file, the same way the adapters' source is switched and for the
	// same reason: a menu is convenient, but the next launch must not depend on
	// what somebody once clicked.
	class MenuPanel
	{
	public:
		static void Install();
	};
}
