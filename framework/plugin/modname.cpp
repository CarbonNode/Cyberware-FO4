// modname.cpp - which DLL owns an address, for hook diagnostics (kept out of main.cpp so <windows.h> macros such as
// ERROR never meet REX::ERROR there). Used by arms::InstallDismemberHook: when the DoHitMe call site already calls
// someone else's hook, the log names that plugin instead of a bare trampoline address (cw-int-1, 2026-11-08).
#include <cstdint>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cwsys
{
	// follows up to 4 jump stubs: E9 rel32, FF 25 [rip+disp32] (commonlib's JMP14 and most trampolines), mov rax,imm64 / jmp rax
	std::uintptr_t FollowJumps(std::uintptr_t a)
	{
		for (int i = 0; i < 4 && a; ++i) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
				(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
				break;
			const auto* p = reinterpret_cast<const std::uint8_t*>(a);
			if (p[0] == 0xE9)
				a = a + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(p + 1));
			else if (p[0] == 0xFF && p[1] == 0x25)
				a = *reinterpret_cast<const std::uintptr_t*>(a + 6 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(p + 2)));
			else if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0)
				a = *reinterpret_cast<const std::uintptr_t*>(p + 2);
			else
				break;
		}
		return a;
	}

	// the file name of the module containing a, or "" (not in any module: a raw trampoline page)
	std::string ModuleOf(std::uintptr_t a)
	{
		HMODULE m = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a), &m) || !m)
			return {};
		wchar_t buf[MAX_PATH];
		const DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
		if (!n) return {};
		std::wstring w(buf, n);
		if (const auto s = w.find_last_of(L"\\/"); s != std::wstring::npos) w = w.substr(s + 1);
		std::string out;
		for (wchar_t c : w) out += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
		return out;
	}
}
