#include "Config.hpp"
#include "PCH.hpp"

#include <format>
#include <string>

namespace F4R_Upscaling
{
	namespace
	{
		std::string FormatInt(std::int32_t a_value)
		{
			return std::to_string(a_value);
		}

		std::string FormatFloat(float a_value)
		{
			return std::format("{:g}", a_value);
		}

		std::string FormatBool(bool a_value)
		{
			return a_value ? "1" : "0";
		}
	}

	Config& Config::GetSingleton()
	{
		static Config instance;
		return instance;
	}

	bool Config::Load(const std::filesystem::path& a_path)
	{
		m_ini.Reset();
		m_ini.SetUnicode();

		const SI_Error result = m_ini.LoadFile(a_path.c_str());
		m_loaded = (result >= 0);
		if (m_loaded) {
			m_path = a_path;
		}
		return m_loaded;
	}

	bool Config::Reload()
	{
		if (m_path.empty()) {
			return false;
		}
		return Load(m_path);
	}

	std::int32_t Config::GetInt(std::string_view a_section, std::string_view a_key, std::int32_t a_default) const
	{
		const std::string section(a_section);
		const std::string key(a_key);
		return static_cast<std::int32_t>(m_ini.GetLongValue(section.c_str(), key.c_str(), a_default));
	}

	float Config::GetFloat(std::string_view a_section, std::string_view a_key, float a_default) const
	{
		const std::string section(a_section);
		const std::string key(a_key);
		return static_cast<float>(m_ini.GetDoubleValue(section.c_str(), key.c_str(), a_default));
	}

	bool Config::GetBool(std::string_view a_section, std::string_view a_key, bool a_default) const
	{
		const std::string section(a_section);
		const std::string key(a_key);
		return m_ini.GetBoolValue(section.c_str(), key.c_str(), a_default);
	}

	void Config::WriteDefaults(const std::filesystem::path& a_path) const
	{
		const Settings settings{};

		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetSpaces(false);

		bool headerWritten = false;
		const auto setEntry = [&](std::string_view a_key, const std::string& a_value, std::string_view a_comment) {
			if (headerWritten) {
				ini.SetValue("Settings", a_key.data(), a_value.c_str(), a_comment.empty() ? nullptr : a_comment.data());
				return;
			}
			headerWritten = true;
			std::string comment = "; Settings apply in-game without restarting\n";
			if (!a_comment.empty()) {
				comment += a_comment;
			}
			ini.SetValue("Settings", a_key.data(), a_value.c_str(), comment.c_str());
		};

		if constexpr (kBackendCount > 1) {
			setEntry("iMethod", FormatInt(static_cast<std::int32_t>(settings.method)),
				"; FSR3 - Nvidia & AMD GPU, DLSS - RTX Only, XeSS - Intel & any GPU\n"
				"; 1 - FSR3, 2 - DLSS, 3 - XeSS, 0 - Off");
		}
		setEntry("iQualityMode", FormatInt(static_cast<std::int32_t>(settings.qualityMode)),
			"; 0 = Native, 1 = Quality, 2 = Balanced, 3 = Performance\n"
			"; ENB forces Native");
		setEntry("fSharpness", FormatFloat(settings.sharpness),
			"; RCAS sharpness - 0.0 = no sharpening, 1.0 = max");
		if constexpr (kHasDLSS) {
		    setEntry("iDLSSPreset", FormatInt(static_cast<std::int32_t>(settings.dlssPreset)),
				"; 0 = J, 1 = K (default), 2 = L, 3 = M");
			setEntry("bEnableReflex", FormatBool(settings.enableReflex),
				"; NVIDIA Reflex to reduce game latency");
			setEntry("bReflexBoost", FormatBool(settings.reflexBoost), {});
			setEntry("bReflexUseFPSLimit", FormatBool(settings.reflexUseFPSLimit), {});
			setEntry("fReflexFPSLimit", FormatFloat(settings.reflexFPSLimit), {});
		}
		setEntry("fQualityScale", FormatFloat(settings.qualityScale),
			"; Quality mode resolution scales\n"
			"; Ranging from 0.60 to 0.67");
		setEntry("fBalancedScale", FormatFloat(settings.balancedScale), "; Ranging from 0.55 to 0.62");
		setEntry("fPerformanceScale", FormatFloat(settings.performanceScale), "; Ranging from 0.50 to 0.55");
		setEntry("fAnisotropicMipBias", FormatFloat(settings.anisotropicMipBias),
			"; -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; Ranging from -2.0 to 0.0");

		ini.SaveFile(a_path.c_str());
	}
}
