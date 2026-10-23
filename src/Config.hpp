#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include <SimpleIni.h>

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

		bool Load(const std::filesystem::path& a_path);
		bool Reload();

		[[nodiscard]] bool IsLoaded() const { return m_loaded; }
		[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

		void WriteDefaults(const std::filesystem::path& a_path) const;

		[[nodiscard]] std::int32_t GetInt(std::string_view a_section, std::string_view a_key, std::int32_t a_default) const;
		[[nodiscard]] float GetFloat(std::string_view a_section, std::string_view a_key, float a_default) const;
		[[nodiscard]] bool GetBool(std::string_view a_section, std::string_view a_key, bool a_default) const;

	private:
		Config() = default;

		CSimpleIniA m_ini;
		std::filesystem::path m_path;
		bool m_loaded = false;
	};
}
