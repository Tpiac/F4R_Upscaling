#pragma once

#include <string>

#include <SimpleIni.h>

#include "Common.hpp"

namespace F4R_Upscaling
{
	class Config
	{
	public:
		static constexpr std::ptrdiff_t kENBDeviceOffset = 0x28;
		static constexpr std::ptrdiff_t kENBContextOffset = 0x6C20;

		[[nodiscard]] static Config& GetSingleton();

		Config(const Config&) = delete;
		Config& operator=(const Config&) = delete;
		~Config() = default;

		bool Load(const std::string& a_path);
		bool Reload();

		[[nodiscard]] bool IsLoaded() const { return m_loaded; }
		[[nodiscard]] const std::string& Path() const { return m_path; }

		void WriteDefaults(const std::string& a_path) const;

		[[nodiscard]] std::int32_t GetInt(const char* a_section, const char* a_key, std::int32_t a_default) const;
		[[nodiscard]] float GetFloat(const char* a_section, const char* a_key, float a_default) const;
		[[nodiscard]] bool GetBool(const char* a_section, const char* a_key, bool a_default) const;

	private:
		Config() = default;

		CSimpleIniA m_ini;
		std::string m_path;
		bool m_loaded = false;
	};
}
