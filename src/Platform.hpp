#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace F4R_Upscaling::Platform
{
	using ModuleHandle = std::uintptr_t;

	[[nodiscard]] inline std::optional<std::filesystem::path> ModulePath(ModuleHandle a_module)
	{
		constexpr std::size_t kMaxPathChars = 32768;

		std::wstring buffer(MAX_PATH, L'\0');
		for (;;) {
			const DWORD written = GetModuleFileNameW(
				reinterpret_cast<HMODULE>(a_module), buffer.data(), static_cast<DWORD>(buffer.size()));
			if (written == 0) {
				return std::nullopt;
			}
			if (written < buffer.size()) {
				buffer.resize(written);
				return std::filesystem::path(std::move(buffer));
			}
			if (buffer.size() >= kMaxPathChars) {
				return std::nullopt;
			}
			buffer.resize(buffer.size() * 2);
		}
	}

	[[nodiscard]] inline ModuleHandle ModuleFromAddress(const void* a_address) noexcept
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCWSTR>(a_address), &module)) {
			return 0;
		}

		return reinterpret_cast<ModuleHandle>(module);
	}

	[[nodiscard]] inline ModuleHandle ModuleByName(const wchar_t* a_name) noexcept
	{
		return reinterpret_cast<ModuleHandle>(GetModuleHandleW(a_name));
	}

	[[nodiscard]] inline ModuleHandle ExecutableModule() noexcept
	{
		return ModuleByName(nullptr);
	}

	[[nodiscard]] inline std::vector<ModuleHandle> ProcessModules()
	{
		constexpr std::size_t kMaxModules = 512;

		std::vector<ModuleHandle> modules;

		std::array<HMODULE, kMaxModules> handles{};
		DWORD needed = 0;
		if (!K32EnumProcessModules(
				GetCurrentProcess(), handles.data(), static_cast<DWORD>(sizeof(handles)), &needed)) {
			return modules;
		}

		const DWORD count = needed / static_cast<DWORD>(sizeof(HMODULE));
		const std::size_t limit = count < kMaxModules ? count : kMaxModules;
		modules.reserve(limit);
		for (std::size_t i = 0; i < limit; ++i) {
			modules.push_back(reinterpret_cast<ModuleHandle>(handles[i]));
		}

		return modules;
	}

	template <class T>
	[[nodiscard]] inline T ResolveExport(ModuleHandle a_module, std::string_view a_name) noexcept
	{
		if (!a_module || a_name.empty()) {
			return nullptr;
		}

		return reinterpret_cast<T>(GetProcAddress(reinterpret_cast<HMODULE>(a_module), a_name.data()));
	}

	[[nodiscard]] inline std::uint32_t LastErrorCode() noexcept
	{
		return GetLastError();
	}

	inline void CloseNativeHandle(void* a_handle) noexcept
	{
		if (a_handle) {
			CloseHandle(a_handle);
		}
	}

	[[nodiscard]] inline std::optional<std::filesystem::path> SelfModulePath()
	{
		static const char anchor = 0;

		const ModuleHandle module = ModuleFromAddress(&anchor);
		if (!module) {
			return std::nullopt;
		}

		return ModulePath(module);
	}

	[[nodiscard]] inline std::filesystem::path ModuleDirectory()
	{
		static const std::filesystem::path directory = []() {
			const auto path = SelfModulePath();
			return path ? path->parent_path() : std::filesystem::path{};
		}();

		return directory;
	}

	[[nodiscard]] inline std::filesystem::path PluginINIPath()
	{
		return ModuleDirectory() / (std::string(F4R_MODULE_NAME) + ".ini");
	}

	class Library
	{
	public:
		Library() = default;
		// Not unloaded on destruction
		// IAT entries persist until process exit
		~Library() = default;

		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;

		[[nodiscard]] bool Load(const std::filesystem::path& a_path)
		{
			Unload();

			const HMODULE module = LoadLibraryW(a_path.c_str());
			if (!module) {
				return false;
			}

			m_module = module;
			return true;
		}

		void Unload()
		{
			if (!m_module) {
				return;
			}

			FreeLibrary(m_module);
			m_module = nullptr;
		}

		[[nodiscard]] bool IsValid() const noexcept { return m_module != nullptr; }

		template <class T>
		[[nodiscard]] T Resolve(std::string_view a_name) const noexcept
		{
			return ResolveExport<T>(reinterpret_cast<ModuleHandle>(m_module), a_name);
		}

	private:
		HMODULE m_module = nullptr;
	};

	class FenceEvent
	{
	public:
		FenceEvent() = default;
		~FenceEvent() { Close(); }

		FenceEvent(const FenceEvent&) = delete;
		FenceEvent& operator=(const FenceEvent&) = delete;

		[[nodiscard]] bool Create()
		{
			Close();

			m_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return m_handle != nullptr;
		}

		void Close() noexcept
		{
			CloseNativeHandle(m_handle);
			m_handle = nullptr;
		}

		[[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr; }
		[[nodiscard]] void* Get() const noexcept { return m_handle; }

		void Wait() const noexcept
		{
			if (m_handle) {
				WaitForSingleObject(m_handle, INFINITE);
			}
		}

	private:
		void* m_handle = nullptr;
	};
}
