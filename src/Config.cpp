#include "PCH.hpp"
#include "Config.hpp"

#include <cstdio>

namespace F4R_Upscaling
{
	namespace
	{
		std::string FormatInt(std::int32_t a_value)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%d", a_value);
			return buf;
		}

		std::string FormatFloat(float a_value)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%g", static_cast<double>(a_value));
			return buf;
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

	bool Config::Load(const std::string& a_path)
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
		if (m_path.empty()) return false;
		return Load(m_path);
	}

	std::int32_t Config::GetInt(const char* a_section, const char* a_key, std::int32_t a_default) const
	{
		return static_cast<std::int32_t>(m_ini.GetLongValue(a_section, a_key, a_default));
	}

	float Config::GetFloat(const char* a_section, const char* a_key, float a_default) const
	{
		return static_cast<float>(m_ini.GetDoubleValue(a_section, a_key, a_default));
	}

	bool Config::GetBool(const char* a_section, const char* a_key, bool a_default) const
	{
		return m_ini.GetBoolValue(a_section, a_key, a_default);
	}

	void Config::WriteDefaults(const std::string& a_path) const
	{
		const Settings settings{};

		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetSpaces(false);

		bool headerWritten = false;
		const auto setEntry = [&](const char* a_key, const std::string& a_value, const char* a_comment) {
			if (headerWritten) {
				ini.SetValue("Settings", a_key, a_value.c_str(), a_comment);
				return;
			}
			headerWritten = true;
			std::string comment = "; Settings apply in-game without restarting\n";
			if (a_comment) {
				comment += a_comment;
			}
			ini.SetValue("Settings", a_key, a_value.c_str(), comment.c_str());
		};

#if F4R_HAS_MULTI
		setEntry("iMethod", FormatInt(settings.iMethod),
		    "; XeSS does not support changing iMethod & iQualityMode at runtime\n"
			"; FSR3 - Nvidia & AMD GPU, DLSS - RTX Only, XeSS - Intel & any GPU\n"
			"; 1 - FSR3, 2 - DLSS, 3 - XeSS, 0 - Off");
#endif
		setEntry("iQualityMode", FormatInt(settings.iQualityMode),
#if F4R_HAS_XESS && !F4R_HAS_MULTI
			"; iQualityMode is not changeable at runtime\n"
#endif
			"; 0 = Native, 1 = Quality, 2 = Balanced, 3 = Performance\n"
			"; ENB forces Native");
		setEntry("fSharpness", FormatFloat(settings.fSharpness),
			"; RCAS sharpness - 0.0 = no sharpening, 1.0 = max");
#if F4R_HAS_DLSS
		setEntry("bEnableReflex", FormatBool(settings.bEnableReflex),
			"; NVIDIA Reflex to reduce game latency");
		setEntry("bReflexBoost", FormatBool(settings.bReflexBoost), nullptr);
		setEntry("bReflexUseFPSLimit", FormatBool(settings.bReflexUseFPSLimit), nullptr);
		setEntry("fReflexFPSLimit", FormatFloat(settings.fReflexFPSLimit), nullptr);
#endif
		setEntry("fQualityScale", FormatFloat(settings.fQualityScale),
			"; Quality mode resolution scales\n"
			"; Ranging from 0.60 to 0.67");
		setEntry("fBalancedScale", FormatFloat(settings.fBalancedScale), "; Ranging from 0.55 to 0.62");
		setEntry("fPerformanceScale", FormatFloat(settings.fPerformanceScale), "; Ranging from 0.50 to 0.55");
		setEntry("fAnisotropicMipBias", FormatFloat(settings.fAnisotropicMipBias),
			"; -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; 0.0 = allows other mods to override samplers (ranging from -2.0 to 0.0)\n"
			"; Custom values apply in Native mode only");

		ini.SaveFile(a_path.c_str());
	}
}
