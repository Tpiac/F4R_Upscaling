#include "PCH.hpp"
#include "Upscaling.hpp"
#include "Streamline.hpp"
#include "XeSS.hpp"

#include "Shaders/MVFix.hpp"
#include "Shaders/RCAS.hpp"
#include "Shaders/DepthCopy.hpp"
#include "Shaders/DepthUpscale.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace F4R_Upscaling
{
	namespace
	{
		float Halton(int32_t a_index, int32_t a_base)
		{
			float f = 1.0f, result = 0.0f;
			for (int32_t currentIndex = a_index; currentIndex > 0;) {
				f /= (float)a_base;
				result = result + f * (float)(currentIndex % a_base);
				currentIndex = (uint32_t)(floorf((float)currentIndex / (float)a_base));
			}
			return result;
		}

		int32_t GetJitterPhaseCount(int32_t a_renderWidth, int32_t a_displayWidth, float a_basePhaseCount)
		{
			const int32_t jitterPhaseCount =
				int32_t(a_basePhaseCount * pow((float(a_displayWidth) / a_renderWidth), 2.0f));
			return jitterPhaseCount;
		}

		void GetJitterOffset(float* a_outX, float* a_outY, int32_t a_index, int32_t a_phaseCount)
		{
			const float x = Halton((a_index % a_phaseCount) + 1, 2) - 0.5f;
			const float y = Halton((a_index % a_phaseCount) + 1, 3) - 0.5f;
			*a_outX = x;
			*a_outY = y;
		}

		ID3D11ComputeShader* CreateComputeShaderFromBytecode(
			const unsigned char* a_bytecode,
			unsigned int a_bytecodeSize,
			ID3D11Device* a_device)
		{
			if (!a_bytecode || !a_bytecodeSize || !a_device) return nullptr;

			ID3D11ComputeShader* shader = nullptr;
			HRESULT hr = a_device->CreateComputeShader(a_bytecode, a_bytecodeSize, nullptr, &shader);
			if (FAILED(hr)) {
				REX::LogError("CreateComputeShader failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}
			return shader;
		}

		std::unique_ptr<Texture2D> CreateSharpenTexture(
			ID3D11Device* a_device,
			uint32_t a_width,
			uint32_t a_height,
			DXGI_FORMAT a_backBufferFormat,
			DXGI_FORMAT a_srvFormat)
		{
			DXGI_FORMAT uavFormat = a_srvFormat;
			if (uavFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
				uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			}

			auto texture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = a_width;
			texDesc.Height = a_height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = a_backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			HRESULT hr = a_device->CreateTexture2D(&texDesc, nullptr, &texture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(sharpenTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = uavFormat;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = a_device->CreateUnorderedAccessView(texture->resource, &uavDesc, &texture->uav);
			if (FAILED(hr)) {
				REX::LogError("CreateUnorderedAccessView(sharpenTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}

			return texture;
		}

		void RunComputePass(
			ID3D11DeviceContext* a_ctx,
			ID3D11ComputeShader* a_shader,
			ID3D11Buffer* a_constants,
			ID3D11ShaderResourceView* const* a_srvs,
			uint32_t a_numSRVs,
			ID3D11UnorderedAccessView* a_uav,
			uint32_t a_groupX,
			uint32_t a_groupY)
		{
			if (a_constants) {
				a_ctx->CSSetConstantBuffers(0, 1, &a_constants);
			}
			a_ctx->CSSetShaderResources(0, a_numSRVs, a_srvs);
			ID3D11UnorderedAccessView* uavs[1] = { a_uav };
			a_ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			a_ctx->CSSetShader(a_shader, nullptr, 0);
			a_ctx->Dispatch(a_groupX, a_groupY, 1);

			ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
			a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
			ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
			a_ctx->CSSetShaderResources(0, a_numSRVs, nullSrvs);
			ID3D11Buffer* nullBuf = nullptr;
			a_ctx->CSSetConstantBuffers(0, 1, &nullBuf);
			a_ctx->CSSetShader(nullptr, nullptr, 0);
		}

		ID3D11Buffer* CreateConstantBuffer(ID3D11Device* a_device, const char* a_name, uint32_t a_size)
		{
			D3D11_BUFFER_DESC desc = {};
			desc.ByteWidth = a_size;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			ID3D11Buffer* buffer = nullptr;
			HRESULT hr = a_device->CreateBuffer(&desc, nullptr, &buffer);
			if (FAILED(hr)) {
				REX::LogError("CreateBuffer({}) failed hr=0x{:x}", a_name, static_cast<uint32_t>(hr));
				return nullptr;
			}
			return buffer;
		}

		int32_t ParseInt32(const char* a_value, int32_t a_default)
		{
			char* end = nullptr;
			unsigned long value = std::strtoul(a_value, &end, 10);
			return (end != a_value && end && *end == '\0') ? static_cast<int32_t>(value) : a_default;
		}

		float ParseFloat(const char* a_value, float a_default)
		{
			char* end = nullptr;
			float value = std::strtof(a_value, &end);
			return (end != a_value && end && *end == '\0') ? value : a_default;
		}
	}

	struct MotionVectorConstants
	{
		uint32_t screenWidth;
		uint32_t screenHeight;
		uint32_t renderWidth;
		uint32_t renderHeight;
		float cameraFar;
		float cameraNear;
		float cameraFarMinusNear;
		float cameraFarTimesNear;
	};

	struct RCASConstants
	{
		float sharpness;
		float pad0;
		float pad1;
		float pad2;
	};

	struct FlareDepthConstants
	{
		uint32_t targetWidth;
		uint32_t targetHeight;
		uint32_t sourceWidth;
		uint32_t sourceHeight;
	};

	Upscaling& Upscaling::GetSingleton()
	{
		static Upscaling instance;
		return instance;
	}

	Upscaling::~Upscaling()
	{
		for (int i = 0; i < 320; i++) {
			if (biasedSamplerStates[i]) {
				biasedSamplerStates[i]->Release();
				biasedSamplerStates[i] = nullptr;
			}
			if (originalSamplerStates[i]) {
				originalSamplerStates[i]->Release();
				originalSamplerStates[i] = nullptr;
			}
		}
	}

	void Upscaling::LoadSettings(const std::string& a_iniPath)
	{
		char buf[64];

		GetPrivateProfileStringA("Settings", "iMethod", F4R_STRINGIFY(F4R_DEFAULT_Method), buf, sizeof(buf), a_iniPath.c_str());
		settings.iMethod = ParseInt32(buf, F4R_DEFAULT_Method);
#if F4R_HAS_DLSS && !F4R_HAS_FSR3 && !F4R_HAS_XESS
		settings.iMethod = static_cast<int32_t>(Method::DLSS);
#elif !F4R_HAS_DLSS && F4R_HAS_FSR3 && !F4R_HAS_XESS
		settings.iMethod = static_cast<int32_t>(Method::FSR3);
#elif !F4R_HAS_DLSS && !F4R_HAS_FSR3 && F4R_HAS_XESS
		settings.iMethod = static_cast<int32_t>(Method::XeSS);
#endif

		GetPrivateProfileStringA("Settings", "fSharpness", "0.5", buf, sizeof(buf), a_iniPath.c_str());
		settings.fSharpness = ParseFloat(buf, 0.5f);
		if (settings.fSharpness < 0.0f) settings.fSharpness = 0.0f;
		if (settings.fSharpness > 1.0f) settings.fSharpness = 1.0f;

		GetPrivateProfileStringA("Settings", "iQualityMode", "0", buf, sizeof(buf), a_iniPath.c_str());
		settings.iQualityMode = ParseInt32(buf, 0);
		if (settings.iQualityMode < 0) settings.iQualityMode = 0;
		if (settings.iQualityMode > 3) settings.iQualityMode = 3;

		GetPrivateProfileStringA("Advanced", "fAnisotropicMipBias", "-0.0001", buf, sizeof(buf), a_iniPath.c_str());
		settings.fAnisotropicMipBias = ParseFloat(buf, -0.0001f);
		if (settings.fAnisotropicMipBias < -2.0f) settings.fAnisotropicMipBias = -2.0f;
		if (settings.fAnisotropicMipBias > 0.0f) settings.fAnisotropicMipBias = 0.0f;

#if F4R_HAS_DLSS
    GetPrivateProfileStringA("Settings", "bEnableReflex", "0", buf, sizeof(buf), a_iniPath.c_str());
    settings.bEnableReflex = ParseInt32(buf, 0) != 0;

	    GetPrivateProfileStringA("Settings", "bReflexBoost", "0", buf, sizeof(buf), a_iniPath.c_str());
	    settings.bReflexBoost = ParseInt32(buf, 0) != 0;

	    GetPrivateProfileStringA("Advanced", "bReflexUseFPSLimit", "0", buf, sizeof(buf), a_iniPath.c_str());
	    settings.bReflexUseFPSLimit = ParseInt32(buf, 0) != 0;

	    GetPrivateProfileStringA("Advanced", "fReflexFPSLimit", "60", buf, sizeof(buf), a_iniPath.c_str());
	    float reflexFPSLimit = ParseFloat(buf, 60.0f);
	    if (reflexFPSLimit < 20.0f) reflexFPSLimit = 20.0f;
	    if (reflexFPSLimit > 240.0f) reflexFPSLimit = 240.0f;
	    settings.fReflexFPSLimit = reflexFPSLimit;
#endif

	const auto mode = static_cast<Method>(settings.iMethod);
	if (mode == Method::DLSS) {
		const char* qname = "Native";
		if (settings.iQualityMode == 1) qname = "Quality";
		else if (settings.iQualityMode == 2) qname = "Balanced";
		else if (settings.iQualityMode == 3) qname = "Performance";
#if F4R_HAS_DLSS
		REX::LogInformation("Settings loaded: method=DLSS quality={} sharpness={} mipBias={} reflex={}",
			qname, settings.fSharpness, settings.fAnisotropicMipBias,
			settings.bEnableReflex ? "enabled" : "disabled");
#else
		REX::LogInformation("Settings loaded: method=DLSS quality={} sharpness={} mipBias={}",
			qname, settings.fSharpness, settings.fAnisotropicMipBias);
#endif
	} else if (mode == Method::FSR3) {
			const char* qname = "Native";
			if (settings.iQualityMode == 1) qname = "Quality";
			else if (settings.iQualityMode == 2) qname = "Balanced";
			else if (settings.iQualityMode == 3) qname = "Performance";
			REX::LogInformation("Settings loaded: method=FSR3 quality={} sharpness={} mipBias={}",
				qname, settings.fSharpness, settings.fAnisotropicMipBias);
		} else if (mode == Method::XeSS) {
			const char* qname = "Native";
			if (settings.iQualityMode == 1) qname = "Quality";
			else if (settings.iQualityMode == 2) qname = "Balanced";
			else if (settings.iQualityMode == 3) qname = "Performance";
			REX::LogInformation("Settings loaded: method=XeSS quality={} sharpness={} mipBias={}",
				qname, settings.fSharpness, settings.fAnisotropicMipBias);
		} else {
			REX::LogInformation("Settings loaded: method=Off");
		}

		settingsIniPath = a_iniPath;
		PollRuntimeSettings();
	}

	void Upscaling::PollSettingsChanged()
	{
		if (settingsIniPath.empty()) return;
		pendingSettingsRefresh = true;
	}

	void Upscaling::PollRuntimeSettings()
	{
		if (settingsIniPath.empty()) return;
		const bool isXeSS = (settings.iMethod == static_cast<int32_t>(Method::XeSS));

		WIN32_FILE_ATTRIBUTE_DATA attrs{};
		if (!GetFileAttributesExA(settingsIniPath.c_str(), GetFileExInfoStandard, &attrs)) return;
		const std::uint64_t writeTime =
			(static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwHighDateTime) << 32) |
			static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwLowDateTime);
		if (!settingsIniHasTime) {
			settingsIniHasTime = true;
			settingsIniWriteTime = writeTime;
			return;
		}
		if (writeTime == settingsIniWriteTime) return;
		settingsIniWriteTime = writeTime;

		char buf[64];
		bool qualityChanged = false;
		bool sharpnessChanged = false;
		bool reflexChanged = false;
		bool mipBiasChanged = false;
		char reflexDesc[64]{};

		if (!isXeSS) {
			GetPrivateProfileStringA("Settings", "fSharpness", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				float v = ParseFloat(buf, settings.fSharpness);
				if (v < 0.0f) v = 0.0f;
				if (v > 1.0f) v = 1.0f;
				if (v != settings.fSharpness) {
					settings.fSharpness = v;
					sharpnessChanged = true;
				}
			}

			GetPrivateProfileStringA("Settings", "iQualityMode", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				int32_t v = ParseInt32(buf, settings.iQualityMode);
				if (v < 0) v = 0;
				if (v > 3) v = 3;
				if (v != settings.iQualityMode) {
					settings.iQualityMode = v;
					qualityChanged = true;
				}
			}
		}

		if (settings.iMethod == static_cast<int32_t>(Method::DLSS)) {
			GetPrivateProfileStringA("Settings", "bEnableReflex", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				const bool v = ParseInt32(buf, settings.bEnableReflex ? 1 : 0) != 0;
				if (v != settings.bEnableReflex) {
					settings.bEnableReflex = v;
					reflexChanged = true;
				}
			}

			GetPrivateProfileStringA("Settings", "bReflexBoost", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				const bool v = ParseInt32(buf, settings.bReflexBoost ? 1 : 0) != 0;
				if (v != settings.bReflexBoost) {
					settings.bReflexBoost = v;
					reflexChanged = true;
				}
			}

			GetPrivateProfileStringA("Advanced", "bReflexUseFPSLimit", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				const bool v = ParseInt32(buf, settings.bReflexUseFPSLimit ? 1 : 0) != 0;
				if (v != settings.bReflexUseFPSLimit) {
					settings.bReflexUseFPSLimit = v;
					reflexChanged = true;
				}
			}

			GetPrivateProfileStringA("Advanced", "fReflexFPSLimit", "", buf, sizeof(buf), settingsIniPath.c_str());
			if (buf[0] != '\0') {
				float v = ParseFloat(buf, settings.fReflexFPSLimit);
				if (v < 20.0f) v = 20.0f;
				if (v > 240.0f) v = 240.0f;
				if (v != settings.fReflexFPSLimit) {
					settings.fReflexFPSLimit = v;
					reflexChanged = true;
				}
			}

			if (reflexChanged) {
				snprintf(reflexDesc, sizeof(reflexDesc), " reflex=%s%s%s",
					settings.bEnableReflex ? "enabled" : "disabled",
					settings.bReflexBoost ? "+boost" : "",
					settings.bReflexUseFPSLimit ? " limit" : "");
			}
		}

		GetPrivateProfileStringA("Advanced", "fAnisotropicMipBias", "", buf, sizeof(buf), settingsIniPath.c_str());
		if (buf[0] != '\0') {
			float v = ParseFloat(buf, settings.fAnisotropicMipBias);
			if (v < -2.0f) v = -2.0f;
			if (v > 0.0f) v = 0.0f;
			if (v != settings.fAnisotropicMipBias) {
				settings.fAnisotropicMipBias = v;
				mipBiasChanged = true;
				samplerCacheValid = false;
			}
		}

		if (qualityChanged || sharpnessChanged || reflexChanged || mipBiasChanged) {
			const char* qname = "Native";
			if (settings.iQualityMode == 1) qname = "Quality";
			else if (settings.iQualityMode == 2) qname = "Balanced";
			else if (settings.iQualityMode == 3) qname = "Performance";
			std::string line = "Settings updated:";
			if (qualityChanged) {
				line += " quality=";
				line += qname;
			}
			if (sharpnessChanged) {
				char num[16];
				snprintf(num, sizeof(num), "%.1f", static_cast<double>(settings.fSharpness));
				line += " sharpness=";
				line += num;
			}
			if (reflexChanged) {
				line += reflexDesc;
			}
			if (mipBiasChanged) {
				char num[16];
				snprintf(num, sizeof(num), "%.4f", static_cast<double>(settings.fAnisotropicMipBias));
				size_t len = strlen(num);
				while (len > 0 && num[len - 1] == '0' && num[len - 2] != '.') {
					num[len - 1] = '\0';
					len--;
				}
				line += " mipBias=";
				line += num;
			}
			REX::LogInformation("{}", line);
		}
	}

	void Upscaling::RequestReset()
	{
		resetHistory = true;
	}

	void Upscaling::Init()
{
		static bool s_initialized = false;
		if (s_initialized) {
			REX::LogWarning("Init called twice: ignoring (hooks already installed)");
			return;
		}
		s_initialized = true;

		REX::LogDebug("Init called");

		if (settings.iMethod == static_cast<int32_t>(Method::Off)) {
			REX::LogInformation("Off mode: no hooks installed");
			return;
		}

		auto* branchPool = F4SE::GetTrampolineInterface()->AllocateFromBranchPool(256);
		REL::GetTrampoline()->Init(branchPool, 256);

		InstallHooks();
		UpdateGameSettings();

		REX::LogDebug("Init complete");
	}

	bool IsMenuBlocked()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) return false;

		static const std::initializer_list<const char*> blockedMenus = {
			"PauseMenu", "PipboyMenu", "InventoryMenu",
			"BarterMenu", "CraftingMenu", "MapMenu",
			"ExamineMenu", "TerminalMenu", "LockpickingMenu"
		};
		for (auto name : blockedMenus) {
			auto result = ui->IsMenuOpen(RE::BSFixedString(name));
			if (result.value_or(false)) return true;
		}
		return false;
	}

	void Upscaling::Update()
	{
		const auto mode = static_cast<Method>(settings.iMethod);
		upsclEnabled = false;
		if (mode == Method::Off) {
			currentScale = 1.0f;
			return;
		}

#if F4R_HAS_DLSS
		if (mode == Method::DLSS) {
			auto& streamline = Streamline::GetSingleton();
			upsclEnabled = streamline.initialized && streamline.featureDLSS;
		}
#endif
#if F4R_HAS_FSR3
		if (mode == Method::FSR3) {
			upsclEnabled = true;
		}
#endif
#if F4R_HAS_XESS
		if (mode == Method::XeSS) {
			upsclEnabled = !XeSS::GetSingleton().disabled;
		}
#endif

		auto* main = RE::Main::GetSingleton();
		const bool prevEnabled = wasUpsclEnabled;
		const bool shouldBlock = IsMenuBlocked();
		if (main && (!main->gameActive || shouldBlock)) {
			upsclEnabled = false;
		}
		if (!prevEnabled && upsclEnabled) {
			resetHistory = true;
		}
		wasUpsclEnabled = upsclEnabled;
		static float s_prevScale = 1.0f;

#if F4R_HAS_DLSS
		if (mode == Method::DLSS) {
			Streamline::GetSingleton().UpdateLatency();
		}
#endif

		if ((mode == Method::FSR3 || mode == Method::XeSS) && g_enbLoaded && !g_realDevice && !g_enbExtractionFailed) {
			ExtractRealD3D11();
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		if (pendingSettingsRefresh && main && main->gameActive && !shouldBlock) {
			pendingSettingsRefresh = false;
			PollRuntimeSettings();
		}

		if ((state.frameCount % 60) == 0 && main && main->gameActive && !shouldBlock) {
			PollRuntimeSettings();
		}

		float desiredScale = 1.0f;
#if F4R_HAS_DLSS
		if (mode == Method::DLSS && upsclEnabled && !g_enbLoaded) {
			if (settings.iQualityMode == 1) desiredScale = 0.6666667f;
			else if (settings.iQualityMode == 2) desiredScale = 0.5882353f;
			else if (settings.iQualityMode == 3) desiredScale = 0.5f;
		}
#endif
#if F4R_HAS_FSR3
		if (mode == Method::FSR3 && upsclEnabled && !g_enbLoaded) {
			if (settings.iQualityMode == 1) desiredScale = 0.6666667f;
			else if (settings.iQualityMode == 2) desiredScale = 0.5882353f;
			else if (settings.iQualityMode == 3) desiredScale = 0.5f;
		}
#endif
#if F4R_HAS_XESS
		if (mode == Method::XeSS && upsclEnabled && !g_enbLoaded) {
			if (settings.iQualityMode == 1) desiredScale = 0.6666667f;
			else if (settings.iQualityMode == 2) desiredScale = 0.5882353f;
			else if (settings.iQualityMode == 3) desiredScale = 0.5f;
		}
#endif
		if (!upsclEnabled) {
			desiredScale = 1.0f;
		}
		currentScale = desiredScale;

		{
			int32_t displayWidth = static_cast<int32_t>(state.screenWidth);
			int32_t renderWidth = static_cast<int32_t>(static_cast<float>(displayWidth) * desiredScale);
			if (renderWidth < 1) renderWidth = 1;
			if (upsclEnabled) {
#if F4R_HAS_FSR3
				if (mode == Method::FSR3) {
					int32_t phaseCount = ffxFsr3GetJitterPhaseCount(renderWidth, displayWidth);
					ffxFsr3GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
				} else
#endif
				{
#if F4R_HAS_XESS
					float basePhaseCount = (mode == Method::XeSS) ? 16.0f : 8.0f;
#else
					float basePhaseCount = 8.0f;
#endif
					int32_t phaseCount = GetJitterPhaseCount(renderWidth, displayWidth, basePhaseCount);
					GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
				}

			state.offsetX = (jitterX * -2.0f) / static_cast<float>(state.screenWidth);
			state.offsetY = (jitterY * 2.0f) / static_cast<float>(state.screenHeight);
		}
		}

		{
			const bool wantSamplerBias = upsclEnabled;
			const float samplerBias = (desiredScale < 0.999f) ? std::log2(desiredScale) : settings.fAnisotropicMipBias;
			samplerBiasActive = wantSamplerBias && (samplerBias != 0.0f);

			auto* samplerStates = samplerBiasActive ? GetGlobalSamplers() : nullptr;
			if (samplerStates) {
				RefreshSamplerCache(samplerStates, samplerBias);
			}
		}

		if (!upsclEnabled || desiredScale >= 0.999f) {
			GetDynWidthRatio(rtMgr) = 1.0f;
			GetDynHeightRatio(rtMgr) = 1.0f;
			GetDynResActivated(rtMgr) = false;
		} else if (state.screenWidth >= 1 && state.screenHeight >= 1) {
			uint32_t snapW = static_cast<uint32_t>(static_cast<float>(state.screenWidth) * desiredScale);
			uint32_t snapH = static_cast<uint32_t>(static_cast<float>(state.screenHeight) * desiredScale);
			if (snapW < 1) snapW = 1;
			if (snapH < 1) snapH = 1;
			GetDynWidthRatio(rtMgr) = static_cast<float>(snapW) / static_cast<float>(state.screenWidth);
			GetDynHeightRatio(rtMgr) = static_cast<float>(snapH) / static_cast<float>(state.screenHeight);
			GetDynResActivated(rtMgr) = true;
		} else {
			GetDynHeightRatio(rtMgr) = desiredScale;
			GetDynWidthRatio(rtMgr) = desiredScale;
			GetDynResActivated(rtMgr) = (desiredScale < 0.999f);
		}

		if (s_prevScale != desiredScale) {
			resetHistory = true;
			s_prevScale = desiredScale;
		}

		UpdateGameSettings();
		CheckResources();
	}

	void Upscaling::Apply()
	{
		const auto mode = static_cast<Method>(settings.iMethod);
		if (mode == Method::Off) return;

		auto* main = RE::Main::GetSingleton();

		if (main && main->gameActive) {
			if (startupFrameGuard < 5) {
				startupFrameGuard++;
				return;
			}
		}

		if (!upsclEnabled) return;

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		auto* backBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kFrameBuffer].srView);
		if (!backBufferSRV) return;

		ID3D11Resource* backBufferResource = nullptr;
		backBufferSRV->GetResource(&backBufferResource);
		if (!backBufferResource) return;

#if F4R_HAS_XESS
		if (mode == Method::XeSS) {
			if (!xessColorTexture || !xessColorTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(xessColorTexture->resource), backBufferResource);
		} else
#endif
		{
			if (!workingTexture || !workingTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(workingTexture->resource), backBufferResource);
		}

		uint32_t renderW = static_cast<uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(rtMgr) + 0.5f);
		uint32_t renderH = static_cast<uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(rtMgr) + 0.5f);
		if (renderW < 1) renderW = 1;
		if (renderH < 1) renderH = 1;

#if F4R_HAS_DLSS
		if (mode == Method::DLSS) {
			auto& streamline = Streamline::GetSingleton();

			if (!workingTexture || !workingTexture->resource || !workingTexture->srv ||
				!dlssOutputTexture || !dlssOutputTexture->resource || !dlssOutputTexture->srv) {
				backBufferResource->Release();
				return;
			}

			if (motionVectorTexture && motionVectorTexture->resource &&
				motionVectorTexture->uav && motionVectorTexture->srv &&
				mvFixShader && mvFixCB) {

				float cameraNear = 0.0f;
				float cameraFar = 1.0f;
				GetCameraNearFar(cameraNear, cameraFar);

				MotionVectorConstants constants{};
				constants.screenWidth = state.screenWidth;
				constants.screenHeight = state.screenHeight;
				constants.renderWidth = renderW;
				constants.renderHeight = renderH;
				constants.cameraFar = cameraFar;
				constants.cameraNear = cameraNear;
				constants.cameraFarMinusNear = cameraFar - cameraNear;
				constants.cameraFarTimesNear = cameraFar * cameraNear;
				ctx->UpdateSubresource(mvFixCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* mvSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kMotionVectors].srView);
				ID3D11ShaderResourceView* depthSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);

				if (mvSRV && depthSRV) {
					ID3D11ShaderResourceView* srvs[2] = { mvSRV, depthSRV };
					RunComputePass(ctx, mvFixShader, mvFixCB, srvs, 2, motionVectorTexture->uav,
						(renderW + 7) / 8, (renderH + 7) / 8);
				}
			}

			uint32_t dlssQuality = 0u;
			if (currentScale < 0.999f) {
				if (settings.iQualityMode >= 1 && settings.iQualityMode <= 3) dlssQuality = static_cast<uint32_t>(settings.iQualityMode);
			}
			streamline.Evaluate(
				workingTexture->resource,
				workingTexture->srv,
				dlssOutputTexture->resource,
				motionVectorTexture ? motionVectorTexture->resource : nullptr,
				jitterX, jitterY, renderW, renderH, dlssQuality);
			resetHistory = false;

			if (settings.fSharpness > 0.0f && tempTexture && tempTexture->resource &&
				tempTexture->uav && rcasShader && rcasCB && dlssOutputTexture->srv) {

				float sharpness = settings.fSharpness;
				if (sharpness < 0.0f) sharpness = 0.0f;
				if (sharpness > 1.0f) sharpness = 1.0f;

				RCASConstants constants{};
				constants.sharpness = exp2f(2.0f * sharpness - 2.0f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { dlssOutputTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, tempTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				ctx->CopyResource(backBufferResource, tempTexture->resource);
			} else {
				ctx->CopyResource(backBufferResource, dlssOutputTexture->resource);
			}
		}
#endif
#if F4R_HAS_FSR3
		if (mode == Method::FSR3) {
			if (fidelityFX) {
				fidelityFX->Apply(workingTexture->resource, jitterX, jitterY, renderW, renderH);
			}

			ctx->CopyResource(backBufferResource, reinterpret_cast<ID3D11Resource*>(workingTexture->resource));
		}
#endif
#if F4R_HAS_XESS
		if (mode == Method::XeSS) {
			if (xessMotionVectorTexture && xessMotionVectorTexture->resource) {
				ID3D11Resource* rawMV = reinterpret_cast<ID3D11Resource*>(rendererData->renderTargets[RenderTarget::kMotionVectors].texture);
				if (rawMV) {
					ctx->CopyResource(xessMotionVectorTexture->resource, rawMV);
				}
			}

if (xessDepthTexture && xessDepthTexture->uav && depthCopyShader) {
				ID3D11ShaderResourceView* depthSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);

				if (depthSRV) {
					ID3D11ShaderResourceView* srvs[1] = { depthSRV };
					RunComputePass(ctx, depthCopyShader, nullptr, srvs, 1, xessDepthTexture->uav,
						(renderW + 7) / 8, (renderH + 7) / 8);
				}
			}

			if (xessMotionVectorTexture && xessMotionVectorTexture->resource &&
				xessMotionVectorTexture->uav && mvFixShader && mvFixCB) {

				float cameraNear = 0.0f, cameraFar = 1.0f;
				GetCameraNearFar(cameraNear, cameraFar);

				MotionVectorConstants constants{};
				constants.screenWidth = state.screenWidth;
				constants.screenHeight = state.screenHeight;
				constants.renderWidth = renderW;
				constants.renderHeight = renderH;
				constants.cameraFar = cameraFar;
				constants.cameraNear = cameraNear;
				constants.cameraFarMinusNear = cameraFar - cameraNear;
				constants.cameraFarTimesNear = cameraFar * cameraNear;
				ctx->UpdateSubresource(mvFixCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* mvSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kMotionVectors].srView);
				ID3D11ShaderResourceView* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);

				if (mvSRV && depthSRV) {
					ID3D11ShaderResourceView* srvs[2] = { mvSRV, depthSRV };
					RunComputePass(ctx, mvFixShader, mvFixCB, srvs, 2, xessMotionVectorTexture->uav,
						(renderW + 7) / 8, (renderH + 7) / 8);
				}
			}

			uint32_t reset = resetHistory ? 1u : 0u;
			resetHistory = false;

			XeSS::GetSingleton().Execute(
				xessColorTexture.get(),
				xessMotionVectorTexture.get(),
				xessDepthTexture.get(),
				xessOutputTexture.get(),
				jitterX, jitterY, renderW, renderH, reset);

			if (settings.fSharpness > 0.0f && tempTexture && tempTexture->uav &&
				xessOutputTexture && xessOutputTexture->srv && rcasShader && rcasCB) {

				float sharpness = settings.fSharpness;
				if (sharpness < 0.0f) sharpness = 0.0f;
				if (sharpness > 1.0f) sharpness = 1.0f;

				RCASConstants constants{};
				constants.sharpness = 0.25f + 0.75f * powf(sharpness, 0.2f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { xessOutputTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, tempTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				ctx->CopyResource(backBufferResource, tempTexture->resource);
			} else {
				ctx->CopyResource(backBufferResource, xessOutputTexture->resource);
			}
		}
#endif

		backBufferResource->Release();
	}

	void Upscaling::UpdateGameSettings()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (imageSpaceManager && imageSpaceManager->effectList.size() > 0x11 &&
			imageSpaceManager->effectList[0x11]) {
			if (upsclEnabled) {
				imageSpaceManager->effectList[0x11]->isActive = false;
			}
		}

		if (upsclEnabled) {
			auto enableTAAReloc = IsAE() || IsNG()
				? REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x294512 } }
				: REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x70681 } };
			*reinterpret_cast<bool*>(enableTAAReloc.GetAddress()) = true;
		}
	}

	void Upscaling::RefreshSamplerCache(SamplerStates* a_states, float a_bias)
	{
		if (!a_states) return;

		for (int i = 0; i < 320; i++) {
			if (originalSamplerStates[i] != a_states->a[i]) {
				if (originalSamplerStates[i]) {
					originalSamplerStates[i]->Release();
				}
				originalSamplerStates[i] = a_states->a[i];
				if (originalSamplerStates[i]) {
					originalSamplerStates[i]->AddRef();
				}
				samplerCacheValid = false;
			}
		}

		if (!samplerCacheValid || cachedSamplerBias != a_bias) {
			RebuildSamplerCache(a_bias);
		}
	}

	void Upscaling::RebuildSamplerCache(float a_bias)
	{
		auto* device = GetRenderer();

		for (int i = 0; i < 320; i++) {
			if (biasedSamplerStates[i]) {
				biasedSamplerStates[i]->Release();
				biasedSamplerStates[i] = nullptr;
			}

			ID3D11SamplerState* src = originalSamplerStates[i];
			if (src && device) {
				D3D11_SAMPLER_DESC desc;
				src->GetDesc(&desc);
				if (desc.Filter == D3D11_FILTER_ANISOTROPIC) {
					desc.MaxAnisotropy = 8;
					desc.MipLODBias = a_bias;
					HRESULT hr = device->CreateSamplerState(&desc, &biasedSamplerStates[i]);
					if (FAILED(hr)) {
						biasedSamplerStates[i] = nullptr;
					}
				}
			}
		}

		cachedSamplerBias = a_bias;
		samplerCacheValid = true;
	}

	void Upscaling::OverrideSamplerStates()
	{
		if (!upsclEnabled || !samplerBiasActive) return;

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) return;

		for (int i = 0; i < 320; i++) {
			if (biasedSamplerStates[i]) {
				samplerStates->a[i] = biasedSamplerStates[i];
			}
		}
	}

	void Upscaling::ResetSamplerStates()
	{
		if (!upsclEnabled || !samplerBiasActive) return;

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) return;

		for (int i = 0; i < 320; i++) {
			samplerStates->a[i] = originalSamplerStates[i];
		}
	}

	void Upscaling::EnsureFlareResources(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height)
	{
		if (!a_device || a_width < 1 || a_height < 1) return;

		bool needTexture = false;
		if (!flareDepthTexture || !flareDepthTexture->resource) {
			needTexture = true;
		} else {
			D3D11_TEXTURE2D_DESC curDesc = {};
			flareDepthTexture->resource->GetDesc(&curDesc);
			if (curDesc.Width != a_width || curDesc.Height != a_height) {
				needTexture = true;
			}
		}
		if (needTexture) {
			if (flareDepthBackup) {
				PopFlareDepth();
			}
			flareDepthTexture.reset();
			flareValid = false;
			auto texture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = a_width;
			texDesc.Height = a_height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_R32_FLOAT;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			HRESULT hr = a_device->CreateTexture2D(&texDesc, nullptr, &texture->resource);
			if (FAILED(hr)) {
				REX::LogError("EnsureFlareResources: CreateTexture2D({}x{}) failed hr=0x{:x}, will retry",
					a_width, a_height, static_cast<uint32_t>(hr));
				return;
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = a_device->CreateShaderResourceView(texture->resource, &srvDesc, &texture->srv);
			if (FAILED(hr)) {
				REX::LogError("EnsureFlareResources: CreateSRV failed hr=0x{:x}, will retry", static_cast<uint32_t>(hr));
				return;
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = a_device->CreateUnorderedAccessView(texture->resource, &uavDesc, &texture->uav);
			if (FAILED(hr)) {
				REX::LogError("EnsureFlareResources: CreateUAV failed hr=0x{:x}, will retry", static_cast<uint32_t>(hr));
				return;
			}
			flareDepthTexture = std::move(texture);
		}

		if (!flareDepthCB) {
			flareDepthCB = CreateConstantBuffer(a_device, "flareDepthCB", sizeof(FlareDepthConstants));
			if (!flareDepthCB) return;
		}
		if (!flareDepthShader) {
			flareDepthShader = CreateComputeShaderFromBytecode(kDepthUpscale, kDepthUpscaleSize, a_device);
			if (!flareDepthShader) return;
		}
	}

	void Upscaling::InvalidateFlareDepth()
	{
		PopFlareDepth();
		flareValid = false;
	}

	void Upscaling::BuildFlareDepth(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		if (!rendererData || !ctx) {
			flareValid = false;
			return;
		}

		auto& state = RE::BSGraphics::State::GetSingleton();

		if (flareDepthBackup) {
			PopFlareDepth();
		}

		uint32_t renderW = static_cast<uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(a_rtMgr) + 0.5f);
		uint32_t renderH = static_cast<uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(a_rtMgr) + 0.5f);
		if (renderW < 1) renderW = 1;
		if (renderH < 1) renderH = 1;

		auto* device = GetRenderer();
		if (!device) {
			flareValid = false;
			return;
		}
		EnsureFlareResources(device, state.screenWidth, state.screenHeight);
		if (!flareDepthTexture || !flareDepthTexture->resource || !flareDepthTexture->uav ||
			!flareDepthShader || !flareDepthCB) {
			flareValid = false;
			return;
		}

		auto* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
		if (!depthSRV) {
			flareValid = false;
			return;
		}

		FlareDepthConstants consts{};
		consts.targetWidth = state.screenWidth;
		consts.targetHeight = state.screenHeight;
		consts.sourceWidth = renderW;
		consts.sourceHeight = renderH;
		ctx->UpdateSubresource(flareDepthCB, 0, nullptr, &consts, 0, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthSRV };
		ID3D11UnorderedAccessView* uavs[1] = { flareDepthTexture->uav };
		ctx->CSSetConstantBuffers(0, 1, &flareDepthCB);
		ctx->CSSetShaderResources(0, 1, srvs);
		ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		ctx->CSSetShader(flareDepthShader, nullptr, 0);
		ctx->Dispatch((state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8, 1);

		ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
		ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
		ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
		ctx->CSSetShaderResources(0, 1, nullSRVs);
		ctx->CSSetShader(nullptr, nullptr, 0);

		flareValid = true;
		flareWidth = state.screenWidth;
		flareHeight = state.screenHeight;
	}

	void Upscaling::PushFlareDepth()
	{
		if (!flareValid) return;
		if (!flareDepthTexture || !flareDepthTexture->srv) return;
		auto& state = RE::BSGraphics::State::GetSingleton();
		if (flareWidth != state.screenWidth || flareHeight != state.screenHeight) return;
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;
		if (flareDepthBackup) return;
		auto* liveDepth = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
		if (!liveDepth) return;
		flareDepthBackup = liveDepth;
		flareDepthBackup->AddRef();
		rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(flareDepthTexture->srv);
	}

	void Upscaling::PopFlareDepth()
	{
		if (!flareDepthBackup) return;
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;
		rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(flareDepthBackup);
		flareDepthBackup->Release();
		flareDepthBackup = nullptr;
	}

	void Upscaling::CheckResources()
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* device = GetRenderer();
		if (!device) return;

		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
		DXGI_FORMAT typedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		auto* bbSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kFrameBuffer].srView);
		if (bbSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			bbSRV->GetDesc(&srvDesc);
			if (srvDesc.Format != DXGI_FORMAT_UNKNOWN) {
				typedFormat = srvDesc.Format;
			}

			ID3D11Resource* bbRes = nullptr;
			bbSRV->GetResource(&bbRes);
			if (bbRes) {
				D3D11_RESOURCE_DIMENSION dim;
				bbRes->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC texDesc;
					reinterpret_cast<ID3D11Texture2D*>(bbRes)->GetDesc(&texDesc);
					backBufferFormat = texDesc.Format;
				}
				bbRes->Release();
			}
		}

		bool fsrNeedsRetry = false;
#if F4R_HAS_FSR3
		fsrNeedsRetry = (settings.iMethod == static_cast<int32_t>(Method::FSR3)) && !fidelityFX &&
			(state.frameCount - fsrRetryFrame >= 600);
#endif

		if (resourcesCreated) {
			const bool qualityAffectsResources = (settings.iMethod == static_cast<int32_t>(Method::XeSS));
			const bool qualityMatch = !qualityAffectsResources || (cachedQuality == settings.iQualityMode);
			if (cachedWidth == state.screenWidth && cachedHeight == state.screenHeight && cachedFormat == backBufferFormat && cachedMethod == settings.iMethod && qualityMatch && !fsrNeedsRetry) {
				if (currentScale < 0.999f) {
					if (!flareDepthTexture || !flareDepthTexture->resource ||
						!flareDepthShader || !flareDepthCB || !flareValid) {
						EnsureFlareResources(device, state.screenWidth, state.screenHeight);
					}
				} else if (flareValid) {
					flareValid = false;
				}
#if F4R_HAS_DLSS
				if (settings.iMethod == static_cast<int32_t>(Method::DLSS) && settings.fSharpness > 0.0f) {
					if (!rcasCB) {
						rcasCB = CreateConstantBuffer(device, "rcasCB", sizeof(RCASConstants));
					}
					if (!rcasShader) {
						rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, device);
					}
					if (!tempTexture) {
						tempTexture = CreateSharpenTexture(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
					}
				}
#endif
#if F4R_HAS_XESS
				if (settings.iMethod == static_cast<int32_t>(Method::XeSS) && settings.fSharpness > 0.0f) {
					if (!tempTexture) {
						tempTexture = CreateSharpenTexture(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
					}
					if (!rcasCB) {
						rcasCB = CreateConstantBuffer(device, "rcasCB", sizeof(RCASConstants));
					}
					if (!rcasShader) {
						rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, device);
					}
				}
#endif
				return;
			}
			REX::LogDebug("CheckResources: mode/resolution/format/quality changed {}x{} fmt{} mode{} q{} -> {}x{} fmt{} mode{} q{}: recreating",
				cachedWidth, cachedHeight, static_cast<int>(cachedFormat), cachedMethod, cachedQuality,
				state.screenWidth, state.screenHeight, static_cast<int>(backBufferFormat), settings.iMethod, settings.iQualityMode);
			motionVectorTexture.reset();
			tempTexture.reset();
			workingTexture.reset();
			dlssOutputTexture.reset();
#if F4R_HAS_FSR3
			fidelityFX.reset();
#endif
#if F4R_HAS_XESS
			xessColorTexture.reset();
			xessMotionVectorTexture.reset();
			xessDepthTexture.reset();
			xessOutputTexture.reset();
			{
				auto& xess = XeSS::GetSingleton();
				xess.disabled = false;
				if (xess.context && xess.xessDestroyContext) {
					xess.xessDestroyContext(xess.context);
					xess.context = nullptr;
					xess.initialized = false;
				}
			}
#endif
			if (mvFixCB) { mvFixCB->Release(); mvFixCB = nullptr; }
			if (rcasCB) { rcasCB->Release(); rcasCB = nullptr; }
			if (mvFixShader) { mvFixShader->Release(); mvFixShader = nullptr; }
			if (rcasShader) { rcasShader->Release(); rcasShader = nullptr; }
			if (depthCopyShader) { depthCopyShader->Release(); depthCopyShader = nullptr; }
			if (flareDepthCB) { flareDepthCB->Release(); flareDepthCB = nullptr; }
			if (flareDepthShader) { flareDepthShader->Release(); flareDepthShader = nullptr; }
			PopFlareDepth();
			flareDepthTexture.reset();
			flareValid = false;
			flareWidth = 0;
			flareHeight = 0;
			resourcesCreated = false;
		}

		const auto mode = static_cast<Method>(settings.iMethod);

		REX::LogDebug("CheckResources: creating resources (method={})", settings.iMethod);

#if F4R_HAS_FSR3
		if (mode == Method::FSR3 && !workingTexture) {
			workingTexture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = state.screenWidth;
			texDesc.Height = state.screenHeight;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
			HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &workingTexture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(workingTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				workingTexture.reset();
				return;
			}
			REX::LogDebug("workingTexture {}x{} fmt={}", texDesc.Width, texDesc.Height, static_cast<int>(backBufferFormat));
		}
#endif

#if F4R_HAS_DLSS
		if (mode == Method::DLSS && !workingTexture) {
			workingTexture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = state.screenWidth;
			texDesc.Height = state.screenHeight;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &workingTexture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(workingTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				workingTexture.reset();
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = typedFormat;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(workingTexture->resource, &srvDesc, &workingTexture->srv);
			if (FAILED(hr)) {
				REX::LogError("CreateShaderResourceView(workingTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
			} else {
				REX::LogDebug("workingTexture {}x{} fmt={}", texDesc.Width, texDesc.Height, static_cast<int>(backBufferFormat));
			}
		}

		if (mode == Method::DLSS && !dlssOutputTexture) {
			dlssOutputTexture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = state.screenWidth;
			texDesc.Height = state.screenHeight;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
			HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &dlssOutputTexture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(dlssOutputTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				dlssOutputTexture.reset();
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = typedFormat;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(dlssOutputTexture->resource, &srvDesc, &dlssOutputTexture->srv);
			if (FAILED(hr)) {
				REX::LogError("CreateShaderResourceView(dlssOutputTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				dlssOutputTexture.reset();
				return;
			}
			REX::LogDebug("dlssOutputTexture {}x{} fmt={}", texDesc.Width, texDesc.Height, static_cast<int>(backBufferFormat));
		}
#endif



#if F4R_HAS_DLSS
		if (mode == Method::DLSS) {
			if (!motionVectorTexture) {
				motionVectorTexture = std::make_unique<Texture2D>();
				D3D11_TEXTURE2D_DESC texDesc = {};
				texDesc.Width = state.screenWidth;
				texDesc.Height = state.screenHeight;
				texDesc.MipLevels = 1;
				texDesc.ArraySize = 1;
				texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				texDesc.SampleDesc.Count = 1;
				texDesc.Usage = D3D11_USAGE_DEFAULT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &motionVectorTexture->resource);
				if (FAILED(hr)) {
					REX::LogError("CreateTexture2D(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
				hr = device->CreateUnorderedAccessView(motionVectorTexture->resource, &uavDesc, &motionVectorTexture->uav);
				if (FAILED(hr)) {
					REX::LogError("CreateUnorderedAccessView(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				hr = device->CreateShaderResourceView(motionVectorTexture->resource, &srvDesc, &motionVectorTexture->srv);
				if (FAILED(hr)) {
					REX::LogError("CreateShaderResourceView(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				REX::LogDebug("motionVectorTexture {}x{} R16G16_FLOAT", texDesc.Width, texDesc.Height);
			}

			if (!mvFixCB) {
				mvFixCB = CreateConstantBuffer(device, "mvFixCB", sizeof(MotionVectorConstants));
			}

			if (!mvFixShader) {
				mvFixShader = CreateComputeShaderFromBytecode(kMVFix, kMVFixSize, device);
			}

			if (settings.fSharpness > 0.0f) {
				if (!rcasCB) {
					rcasCB = CreateConstantBuffer(device, "rcasCB", sizeof(RCASConstants));
				}
				if (!rcasShader) {
					rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, device);
				}
				if (!tempTexture) {
					tempTexture = CreateSharpenTexture(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
				}
			}
		}
#endif
#if F4R_HAS_FSR3
		if (mode == Method::FSR3) {
			fidelityFX = std::make_unique<FidelityFX>();
			if (!fidelityFX->CreateFSRResources(
					device,
					state.screenWidth, state.screenHeight,
					backBufferFormat)) {
				REX::LogError("CheckResources: CreateFSRResources failed");
				fidelityFX.reset();
				fsrRetryFrame = state.frameCount;
			} else if (g_enbLoaded && settings.iQualityMode >= 1 && settings.iQualityMode <= 3) {
				REX::LogInformation("ENB forces Native for FSR3");
			}
		}
#endif
#if F4R_HAS_XESS
		if (mode == Method::XeSS) {
			auto* context = GetImmediateContext();
			auto& xess = XeSS::GetSingleton();
			auto& rtMgrFb = RE::BSGraphics::RenderTargetManager::GetSingleton();
			auto fallbackNative = [&](const char* a_reason) {
				if (!xess.disabled) {
					REX::LogWarning("XeSS disabled: {} fallback to Native", a_reason);
					xess.disabled = true;
					xessRetryFrame = state.frameCount;
				}
				xess.TeardownD3D12();
				upsclEnabled = false;
				currentScale = 1.0f;
				GetDynWidthRatio(rtMgrFb) = 1.0f;
				GetDynHeightRatio(rtMgrFb) = 1.0f;
				GetDynResActivated(rtMgrFb) = false;
				resetHistory = true;
			};
			if (xess.disabled) {
				if (state.frameCount - xessRetryFrame >= 600) {
					xess.disabled = false;
				} else {
					fallbackNative("session disabled");
					return;
				}
			}
			if (!xess.loaded) {
				xess.Load();
			}
			if (!xess.loaded) {
				fallbackNative("libxess unavailable");
				return;
			}

			if (!xess.device) {
				if (!xess.CreateD3D12(device, context)) {
					REX::LogError("CheckResources: D3D12 interop failed");
					fallbackNative("D3D12 interop unavailable");
					return;
				}
			}

			uint32_t width = state.screenWidth;
			uint32_t height = state.screenHeight;

			if (!xessColorTexture) {
				xessColorTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessColorTexture.get(), width, height, backBufferFormat, D3D11_BIND_SHADER_RESOURCE, typedFormat)) {
					REX::LogError("CreateSharedTexture(color) failed");
					xessColorTexture.reset();
					fallbackNative("shared texture creation failed");
					return;
				}
			}

			if (!xessMotionVectorTexture) {
				xessMotionVectorTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessMotionVectorTexture.get(), width, height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT)) {
					REX::LogError("CreateSharedTexture(motionVector) failed");
					xessMotionVectorTexture.reset();
					fallbackNative("shared texture creation failed");
					return;
				}
			}

			if (!xessDepthTexture) {
				xessDepthTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessDepthTexture.get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT)) {
					REX::LogError("CreateSharedTexture(depth) failed");
					xessDepthTexture.reset();
					fallbackNative("shared texture creation failed");
					return;
				}
			}

			if (!xessOutputTexture) {
				DXGI_FORMAT uavFormat = typedFormat;
				if (uavFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
					uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
				}
				xessOutputTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessOutputTexture.get(), width, height, backBufferFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, typedFormat, uavFormat)) {
					REX::LogError("CreateSharedTexture(output) failed");
					xessOutputTexture.reset();
					fallbackNative("shared texture creation failed");
					return;
				}
			}

			if (settings.fSharpness > 0.0f) {
				if (!tempTexture) {
					tempTexture = CreateSharpenTexture(device, width, height, backBufferFormat, typedFormat);
				}
				if (!rcasCB) {
					rcasCB = CreateConstantBuffer(device, "rcasCB", sizeof(RCASConstants));
				}
				if (!rcasShader) {
					rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, device);
				}
			}

			if (!depthCopyShader) {
				depthCopyShader = CreateComputeShaderFromBytecode(kDepthCopy, kDepthCopySize, device);
			}

			if (!mvFixCB) {
				mvFixCB = CreateConstantBuffer(device, "mvFixCB", sizeof(MotionVectorConstants));
			}

			if (!mvFixShader) {
				mvFixShader = CreateComputeShaderFromBytecode(kMVFix, kMVFixSize, device);
			}

			int xessQualityMode = settings.iQualityMode;
			if (g_enbLoaded && xessQualityMode >= 1 && xessQualityMode <= 3) {
				xessQualityMode = 0;
				REX::LogInformation("ENB forces Native for XeSS");
			}

			if (!xess.initialized) {
				if (!xess.CreateContext(width, height, xessQualityMode)) {
					REX::LogError("CheckResources: CreateContext failed");
					fallbackNative("context creation failed");
					return;
				}
			}
		}
#endif

		if (currentScale < 0.999f) {
			EnsureFlareResources(device, state.screenWidth, state.screenHeight);
		} else {
			if (flareDepthBackup) { PopFlareDepth(); }
			flareValid = false;
		}

		resourcesCreated = true;
		cachedWidth = state.screenWidth;
		cachedHeight = state.screenHeight;
		cachedFormat = backBufferFormat;
		cachedMethod = settings.iMethod;
		cachedQuality = settings.iQualityMode;
		if (settings.iQualityMode >= 1 && settings.iQualityMode <= 3 && settings.iMethod == static_cast<int32_t>(Method::DLSS) && !g_enbLoaded) {
			float s = 0.6666667f;
			const char* qname = "Quality";
			if (settings.iQualityMode == 2) { s = 0.5882353f; qname = "Balanced"; }
			else if (settings.iQualityMode == 3) { s = 0.5f; qname = "Performance"; }
			else if (settings.iQualityMode == 1) { qname = "Quality"; }
			REX::LogDebug("DLSS {}: scale={:.3f} {}x{} -> {}x{}", qname, s, state.screenWidth, state.screenHeight, uint32_t(state.screenWidth * s), uint32_t(state.screenHeight * s));
		}
#if F4R_HAS_FSR3
		if (settings.iQualityMode >= 1 && settings.iQualityMode <= 3 && settings.iMethod == static_cast<int32_t>(Method::FSR3)) {
			float s = 0.6666667f;
			const char* qname = "Quality";
			if (settings.iQualityMode == 2) { s = 0.5882353f; qname = "Balanced"; }
			else if (settings.iQualityMode == 3) { s = 0.5f; qname = "Performance"; }
			else if (settings.iQualityMode == 1) { qname = "Quality"; }
			REX::LogDebug("FSR3 {}: scale={:.3f} {}x{} -> {}x{}", qname, s, state.screenWidth, state.screenHeight, uint32_t(state.screenWidth * s), uint32_t(state.screenHeight * s));
		}
#endif
#if F4R_HAS_XESS
		if (settings.iQualityMode >= 1 && settings.iQualityMode <= 3 && settings.iMethod == static_cast<int32_t>(Method::XeSS)) {
			float s = 0.6666667f;
			const char* qname = "Quality";
			if (settings.iQualityMode == 2) { s = 0.5882353f; qname = "Balanced"; }
			else if (settings.iQualityMode == 3) { s = 0.5f; qname = "Performance"; }
			else if (settings.iQualityMode == 1) { qname = "Quality"; }
			REX::LogDebug("XeSS {}: scale={:.3f} {}x{} -> {}x{}", qname, s, state.screenWidth, state.screenHeight, uint32_t(state.screenWidth * s), uint32_t(state.screenHeight * s));
		}
#endif
	}
}
