#include "Upscaling.hpp"
#include "Config.hpp"
#include "PCH.hpp"
#if F4R_HAS_STREAMLINE
#include "Streamline.hpp"
#endif
#if F4R_HAS_XESS
#include "XeSS.hpp"
#endif

#include "Shaders/DepthCopy.hpp"
#include "Shaders/DepthUpscale.hpp"
#include "Shaders/MVFix.hpp"
#include "Shaders/RCAS.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

namespace F4R_Upscaling
{
	namespace
	{
		float Halton(std::int32_t a_index, std::int32_t a_base)
		{
			float f = 1.0f, result = 0.0f;
			for (std::int32_t currentIndex = a_index; currentIndex > 0;) {
				f /= static_cast<float>(a_base);
				result = result + f * static_cast<float>(currentIndex % a_base);
				currentIndex = static_cast<std::uint32_t>(floorf(static_cast<float>(currentIndex) / static_cast<float>(a_base)));
			}
			return result;
		}

		std::int32_t GetJitterPhaseCount(std::int32_t a_renderWidth, std::int32_t a_displayWidth, float a_basePhaseCount)
		{
			const std::int32_t jitterPhaseCount =
				std::int32_t(a_basePhaseCount * pow(float(a_displayWidth) / a_renderWidth, 2.0f));
			return jitterPhaseCount;
		}

		void GetJitterOffset(float* a_outX, float* a_outY, std::int32_t a_index, std::int32_t a_phaseCount)
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
			if (!a_bytecode || !a_bytecodeSize || !a_device) {
				return nullptr;
			}

			ID3D11ComputeShader* shader = nullptr;
			HRESULT hr = a_device->CreateComputeShader(a_bytecode, a_bytecodeSize, nullptr, &shader);
			if (FAILED(hr)) {
				REX::LogError("CreateComputeShader failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			return shader;
		}

		std::unique_ptr<Texture2D> CreateSharpenTexture(
			ID3D11Device* a_device,
			std::uint32_t a_width,
			std::uint32_t a_height,
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
				REX::LogError("CreateTexture2D(sharpenTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = uavFormat;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = a_device->CreateUnorderedAccessView(texture->resource, &uavDesc, &texture->uav);
			if (FAILED(hr)) {
				REX::LogError("CreateUnorderedAccessView(sharpenTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			return texture;
		}

		std::unique_ptr<Texture2D> CreateR32Texture(
			ID3D11Device* a_device,
			std::uint32_t a_width,
			std::uint32_t a_height,
			const char* a_tag)
		{
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
				REX::LogError("{}: CreateTexture2D({}x{}) failed hr=0x{:x}, will retry", a_tag, a_width, a_height, static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = a_device->CreateShaderResourceView(texture->resource, &srvDesc, &texture->srv);
			if (FAILED(hr)) {
				REX::LogError("{}: CreateSRV failed hr=0x{:x}, will retry", a_tag, static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = a_device->CreateUnorderedAccessView(texture->resource, &uavDesc, &texture->uav);
			if (FAILED(hr)) {
				REX::LogError("{}: CreateUAV failed hr=0x{:x}, will retry", a_tag, static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			return texture;
		}

		void RunComputePass(
			ID3D11DeviceContext* a_ctx,
			ID3D11ComputeShader* a_shader,
			ID3D11Buffer* a_constants,
			ID3D11ShaderResourceView* const* a_srvs,
			std::uint32_t a_numSRVs,
			ID3D11UnorderedAccessView* a_uav,
			std::uint32_t a_groupX,
			std::uint32_t a_groupY)
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

		ID3D11Buffer* CreateConstantBuffer(ID3D11Device* a_device, std::string_view a_name, std::uint32_t a_size)
		{
			D3D11_BUFFER_DESC desc = {};
			desc.ByteWidth = a_size;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			ID3D11Buffer* buffer = nullptr;
			HRESULT hr = a_device->CreateBuffer(&desc, nullptr, &buffer);
			if (FAILED(hr)) {
				REX::LogError("CreateBuffer({}) failed hr=0x{:x}", a_name, static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			return buffer;
		}

		constexpr std::array<std::string_view, 4> kDLSSPresetLetters{
			"J", "K", "L", "M"
		};

		

		std::string_view MethodName(Method a_method)
		{
			switch (a_method) {
				case Method::FSR3:
					return "FSR3";
				case Method::DLSS:
					return "DLSS";
				case Method::XeSS:
					return "XeSS";
				case Method::Off:
					return "Off";
			}
			return "Off";
		}

		std::string_view QualityName(QualityMode a_qualityMode)
		{
			switch (a_qualityMode) {
				case QualityMode::Quality:
					return "Quality";
				case QualityMode::Balanced:
					return "Balanced";
				case QualityMode::Performance:
					return "Performance";
				case QualityMode::Native:
				default:
					return "Native";
			}
		}

	}

	struct MotionVectorConstants
	{
		std::uint32_t screenWidth;
		std::uint32_t screenHeight;
		std::uint32_t renderWidth;
		std::uint32_t renderHeight;
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
		std::uint32_t targetWidth;
		std::uint32_t targetHeight;
		std::uint32_t sourceWidth;
		std::uint32_t sourceHeight;
	};

	Upscaling& Upscaling::GetSingleton()
	{
		static Upscaling instance;
		return instance;
	}

	Upscaling::~Upscaling()
	{
		for (int i = 0; i < 320; i++) {
			SafeRelease(biasedSamplerStates[i]);
			SafeRelease(originalSamplerStates[i]);
		}
		SafeRelease(hbaoDepthBackedUp);
		SafeRelease(hbaoDepthShader);
		SafeRelease(mvFixShader);
		SafeRelease(mvFixCB);
		SafeRelease(rcasShader);
		SafeRelease(rcasCB);
		SafeRelease(depthCopyShader);
		SafeRelease(depthOverrideShader);
		SafeRelease(flareDepthShader);
		SafeRelease(flareDepthCB);
		SafeRelease(flareDepthBackup);
		SafeRelease(savedPassRTV);
		SafeRelease(savedPassDSV);
		SafeRelease(depthOverrideBackup);
		ReleaseProxyTargets();
		depthOverrideTexture.reset();
	}

	void Upscaling::LoadSettings()
	{
		auto& config = Config::GetSingleton();

const auto rawMethod = config.GetInt("Settings", "iMethod", static_cast<std::int32_t>(settings.method));
	if constexpr (kSoleBackend != Method::Off) {
		settings.method = kSoleBackend;
	} else {
		if (rawMethod >= static_cast<std::int32_t>(Method::Off) && rawMethod <= static_cast<std::int32_t>(Method::XeSS)) {
			settings.method = static_cast<Method>(rawMethod);
		}
	}

		settings.sharpness = std::clamp(config.GetFloat("Settings", "fSharpness", settings.sharpness), 0.0f, 1.0f);
		const auto rawQuality = config.GetInt("Settings", "iQualityMode", static_cast<std::int32_t>(settings.qualityMode));
	if (rawQuality >= 0 && rawQuality <= 3) {
		settings.qualityMode = static_cast<QualityMode>(rawQuality);
	}

settings.qualityScale = std::clamp(config.GetFloat("Settings", "fQualityScale", settings.qualityScale), 0.60f, 0.67f);
	settings.balancedScale = std::clamp(config.GetFloat("Settings", "fBalancedScale", settings.balancedScale), 0.55f, 0.62f);
	settings.performanceScale = std::clamp(config.GetFloat("Settings", "fPerformanceScale", settings.performanceScale), 0.50f, 0.55f);

		settings.anisotropicMipBias = std::clamp(config.GetFloat("Settings", "fAnisotropicMipBias", settings.anisotropicMipBias), -2.0f, 0.0f);

if constexpr (kHasDLSS) {
		settings.enableReflex = config.GetBool("Settings", "bEnableReflex", settings.enableReflex);
		settings.reflexBoost = config.GetBool("Settings", "bReflexBoost", settings.reflexBoost);
		settings.reflexUseFPSLimit = config.GetBool("Settings", "bReflexUseFPSLimit", settings.reflexUseFPSLimit);
		settings.reflexFPSLimit = std::clamp(config.GetFloat("Settings", "fReflexFPSLimit", settings.reflexFPSLimit), 20.0f, 240.0f);

			const auto rawPreset = config.GetInt("Settings", "iDLSSPreset", static_cast<std::int32_t>(settings.dlssPreset));
			if (rawPreset >= 0 && rawPreset <= 3) {
				settings.dlssPreset = static_cast<DLSSPresetChoice>(rawPreset);
			}
		}

		std::string_view methodName = MethodName(settings.method);
		std::string_view qname = QualityName(settings.qualityMode);

		if (settings.method == Method::Off) {
			REX::LogInformation("Settings loaded: method=Off");
		}
		else {
			if constexpr (kHasDLSS) {
				if (settings.method == Method::DLSS) {
					REX::LogInformation("Settings loaded: method={} mode={} dlssPreset={} sharpness={} reflex={} mipBias={}",
						methodName, qname,
						kDLSSPresetLetters.at(static_cast<std::size_t>(settings.dlssPreset)),
						settings.sharpness, settings.enableReflex ? "enabled" : "disabled",
						settings.anisotropicMipBias);
				} else {
					REX::LogInformation("Settings loaded: method={} mode={} sharpness={} reflex={} mipBias={}",
						methodName, qname, settings.sharpness, settings.enableReflex ? "enabled" : "disabled",
						settings.anisotropicMipBias);
				}
			}
			else {
				REX::LogInformation("Settings loaded: method={} mode={} sharpness={} mipBias={}",
					methodName, qname, settings.sharpness, settings.anisotropicMipBias);
			}
		}

		PollRuntimeSettings();
	}

	void Upscaling::PollRuntimeSettings()
	{
		auto& config = Config::GetSingleton();
		if (!config.IsLoaded()) {
			return;
		}

		const auto writeTime = GetFileWriteTime(config.Path());
		if (!writeTime) {
			return;
		}
		if (!settingsIniHasTime) {
			settingsIniHasTime = true;
			settingsIniWriteTime = *writeTime;
			return;
		}
		if (*writeTime == settingsIniWriteTime) {
			return;
		}
		settingsIniWriteTime = *writeTime;

		if (!config.Reload()) {
			return;
		}

		const auto previousDlssPreset = settings.dlssPreset;

		if constexpr (kHasDLSS) {
			const auto rawPreset = config.GetInt("Settings", "iDLSSPreset", static_cast<std::int32_t>(settings.dlssPreset));
			if (rawPreset >= 0 && rawPreset <= 3) {
				settings.dlssPreset = static_cast<DLSSPresetChoice>(rawPreset);
			}
		}

		bool switchedToDLSS = false;

if constexpr (kBackendCount > 1) {
		const auto requestedRaw = config.GetInt("Settings", "iMethod", static_cast<std::int32_t>(settings.method));
		if (requestedRaw >= static_cast<std::int32_t>(Method::Off) && requestedRaw <= static_cast<std::int32_t>(Method::XeSS)) {
			const Method requested = static_cast<Method>(requestedRaw);
			if (requested != settings.method) {
				dlssUnsupportedLogged = false;
#if F4R_HAS_DLSS
				auto& streamline = Streamline::GetSingleton();
				if (requested == Method::DLSS && streamline.initialized && !streamline.featureDLSS) {
					settings.method = lastWorkingMethod;
					REX::LogInformation("Settings updated: method=Unsupported, falling back to {}", MethodName(lastWorkingMethod));
					dlssUnsupportedLogged = true;
					resetHistory = true;
				}
				else
#endif
				{
					settings.method = requested;
					if (requested == Method::Off) {
						REX::LogInformation("Settings updated: method=Off");
					}
					else if (requested == Method::DLSS) {
						switchedToDLSS = true;
						REX::LogInformation("Settings updated: method=DLSS dlssPreset={}",
							kDLSSPresetLetters.at(static_cast<std::size_t>(settings.dlssPreset)));
					}
					else {
						REX::LogInformation("Settings updated: method={}", MethodName(requested));
					}
					resetHistory = true;
				}
			}
		}
	}

		bool qualityChanged = false;
		bool sharpnessChanged = false;
		bool reflexChanged = false;
		bool mipBiasChanged = false;
		bool scalesChanged = false;
		std::string reflexDesc;
		std::string scalesDesc;

const float sharpness = std::clamp(config.GetFloat("Settings", "fSharpness", settings.sharpness), 0.0f, 1.0f);
	if (sharpness != settings.sharpness) {
		settings.sharpness = sharpness;
		sharpnessChanged = true;
	}

	const auto rawQualityMode = config.GetInt("Settings", "iQualityMode", static_cast<std::int32_t>(settings.qualityMode));
	QualityMode qualityMode = settings.qualityMode;
	if (rawQualityMode >= 0 && rawQualityMode <= 3) {
		qualityMode = static_cast<QualityMode>(rawQualityMode);
	}
	if (qualityMode != settings.qualityMode) {
		settings.qualityMode = qualityMode;
		qualityChanged = true;
	}

const float qualityScale = std::clamp(config.GetFloat("Settings", "fQualityScale", settings.qualityScale), 0.60f, 0.67f);
	if (qualityScale != settings.qualityScale) {
		settings.qualityScale = qualityScale;
		scalesChanged = true;
	}

	const float balancedScale = std::clamp(config.GetFloat("Settings", "fBalancedScale", settings.balancedScale), 0.55f, 0.62f);
	if (balancedScale != settings.balancedScale) {
		settings.balancedScale = balancedScale;
		scalesChanged = true;
	}

	const float performanceScale = std::clamp(config.GetFloat("Settings", "fPerformanceScale", settings.performanceScale), 0.50f, 0.55f);
	if (performanceScale != settings.performanceScale) {
		settings.performanceScale = performanceScale;
		scalesChanged = true;
	}

		if (scalesChanged) {
			scalesDesc = std::format(" scales={:.2f}/{:.2f}/{:.2f}",
				settings.qualityScale,
				settings.balancedScale,
				settings.performanceScale);
		}

const bool reflexEnabled = config.GetBool("Settings", "bEnableReflex", settings.enableReflex);
	const bool reflexBoost = config.GetBool("Settings", "bReflexBoost", settings.reflexBoost);
	const bool reflexUseLimit = config.GetBool("Settings", "bReflexUseFPSLimit", settings.reflexUseFPSLimit);
	const float reflexFPSLimit = std::clamp(config.GetFloat("Settings", "fReflexFPSLimit", settings.reflexFPSLimit), 20.0f, 240.0f);

	reflexChanged = (reflexEnabled != settings.enableReflex) ||
					(reflexBoost != settings.reflexBoost) ||
					(reflexUseLimit != settings.reflexUseFPSLimit) ||
					(reflexFPSLimit != settings.reflexFPSLimit);

	settings.enableReflex = reflexEnabled;
	settings.reflexBoost = reflexBoost;
	settings.reflexUseFPSLimit = reflexUseLimit;
	settings.reflexFPSLimit = reflexFPSLimit;

		if (reflexChanged) {
			reflexDesc = std::format(" reflex={}{}{}",
				settings.enableReflex ? "enabled" : "disabled",
				settings.reflexBoost ? "+boost" : "",
				settings.reflexUseFPSLimit ? " limit" : "");
		}

		if constexpr (kHasDLSS) {
			if (previousDlssPreset != settings.dlssPreset &&
				settings.method == Method::DLSS &&
				!switchedToDLSS) {
				REX::LogInformation("Settings updated: dlssPreset={}",
					kDLSSPresetLetters.at(static_cast<std::size_t>(settings.dlssPreset)));
			}
		}

const float mipBias = std::clamp(config.GetFloat("Settings", "fAnisotropicMipBias", settings.anisotropicMipBias), -2.0f, 0.0f);
	if (mipBias != settings.anisotropicMipBias) {
		settings.anisotropicMipBias = mipBias;
			mipBiasChanged = true;
			samplerCacheValid = false;
		}

		if (qualityChanged || sharpnessChanged || reflexChanged || mipBiasChanged || scalesChanged) {
			std::string line = "Settings updated:";
			if (qualityChanged) {
				line += " mode=";
				line += QualityName(settings.qualityMode);
			}
			if (sharpnessChanged) {
				line += " sharpness=";
				line += std::format("{:.1f}", settings.sharpness);
			}
			if (reflexChanged) {
				line += reflexDesc;
			}
			if (scalesChanged) {
				line += scalesDesc;
			}
			if (mipBiasChanged) {
				std::string value = std::format("{:.4f}", settings.anisotropicMipBias);
				const std::size_t dot = value.find('.');
				if (dot != std::string::npos) {
					std::size_t end = value.size();
					while (end > dot + 2 && value[end - 1] == '0') {
						--end;
					}
					value.resize(end);
				}
				line += " mipBias=";
				line += value;
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
		if (initialized) {
			REX::LogWarning("Init called twice: ignoring (hooks already installed)");
			return;
		}
		initialized = true;
		ResetProxyPassProfiles();

		auto* branchPool = F4SE::GetTrampolineInterface()->AllocateFromBranchPool(256);
		REL::GetTrampoline()->Init(branchPool, 256);

		InstallHooks();
		UpdateGameSettings();
	}

	bool IsMenuBlocked()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}

		static const std::array<RE::BSFixedString, 3> blockedMenus = {
			"ExamineMenu", "LockpickingMenu"
		};
		for (const auto& name : blockedMenus) {
			if (ui->IsMenuOpen(name).value_or(false)) {
				return true;
			}
		}
		return false;
	}

	void Upscaling::Update()
	{
		auto* main = RE::Main::GetSingleton();
		const bool prevEnabled = wasUpsclEnabled;
		const bool shouldBlock = IsMenuBlocked();
		if (main && main->gameActive && !shouldBlock) {
			PollRuntimeSettings();
		}

auto method = settings.method;
		upsclEnabled = false;
		if (method == Method::Off) {
			if (wasUpsclEnabled || fxaaStateSaved || taaFlagSaved || samplerBiasActive) {
				ReturnToVanilla();
			}
			currentScale = 1.0f;
			return;
		}

#if F4R_HAS_DLSS
		if (method == Method::DLSS) {
			auto& streamline = Streamline::GetSingleton();
			if (streamline.initialized && !streamline.featureDLSS) {
				if (!dlssUnsupportedLogged) {
					settings.method = lastWorkingMethod;
					REX::LogInformation("Settings updated: method=Unsupported, falling back to {}", MethodName(lastWorkingMethod));
					dlssUnsupportedLogged = true;
method = settings.method;
					resetHistory = true;
				}
			}
			else {
				dlssUnsupportedLogged = false;
			}
			if (method == Method::DLSS) {
				upsclEnabled = streamline.initialized && streamline.featureDLSS;
			}
		}
#endif
		if constexpr (kHasFSR3) {
			if (method == Method::FSR3) {
				upsclEnabled = true;
			}
		}
#if F4R_HAS_XESS
		if (method == Method::XeSS) {
			upsclEnabled = !XeSS::GetSingleton().disabled;
		}
#endif

		if (method != Method::Off && upsclEnabled) {
			lastWorkingMethod = method;
		}

		if (main && (!main->gameActive || shouldBlock)) {
			upsclEnabled = false;
		}
		if (!prevEnabled && upsclEnabled) {
			resetHistory = true;
		}
		wasUpsclEnabled = upsclEnabled;

#if F4R_HAS_STREAMLINE
		Streamline::GetSingleton().UpdateLatency();
#endif

		if ((method == Method::FSR3 || method == Method::XeSS) && g_enbLoaded && !g_realDevice && !g_enbExtractionFailed) {
			ExtractRealD3D11();
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		float desiredScale = 1.0f;
		if (upsclEnabled && !g_enbLoaded) {
			if (settings.qualityMode == QualityMode::Quality) {
				desiredScale = settings.qualityScale;
			}
			else if (settings.qualityMode == QualityMode::Balanced) {
				desiredScale = settings.balancedScale;
			}
			else if (settings.qualityMode == QualityMode::Performance) {
				desiredScale = settings.performanceScale;
			}
		}
		currentScale = desiredScale;

		if (currentScale < 0.999f && startupFrameGuard < 5) {
			desiredScale = 1.0f;
			currentScale = 1.0f;
		}

		{
			std::int32_t displayWidth = static_cast<std::int32_t>(state.screenWidth);
			std::int32_t renderWidth = static_cast<std::int32_t>(static_cast<float>(displayWidth) * desiredScale);
			if (renderWidth < 1) {
				renderWidth = 1;
			}
			if (upsclEnabled) {
#if F4R_HAS_FSR3
				if (method == Method::FSR3) {
					std::int32_t phaseCount = ffxFsr3GetJitterPhaseCount(renderWidth, displayWidth);
					ffxFsr3GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
				}
				else
#endif
				{
					float basePhaseCount = 8.0f;
					if constexpr (kHasXeSS) {
						if (method == Method::XeSS) {
							basePhaseCount = 16.0f;
						}
					}
					std::int32_t phaseCount = GetJitterPhaseCount(renderWidth, displayWidth, basePhaseCount);
					GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
				}

				state.offsetX = (jitterX * -2.0f) / static_cast<float>(state.screenWidth);
				state.offsetY = (jitterY * 2.0f) / static_cast<float>(state.screenHeight);
			}
		}

		{
			const bool wantSamplerBias = upsclEnabled;
			const bool scaled = desiredScale < 0.999f;
			const float scaleCompensation = scaled ? std::log2(desiredScale) : 0.0f;
			const float samplerBias = scaleCompensation + settings.anisotropicMipBias;
			samplerBiasActive = wantSamplerBias && (samplerBias != 0.0f);

			auto* samplerStates = samplerBiasActive ? GetGlobalSamplers() : nullptr;
			if (samplerStates) {
				RefreshSamplerCache(samplerStates, samplerBias);
			}
		}

		if (!upsclEnabled || desiredScale >= 0.999f) {
			SetGlobalDynamicRatio(rtMgr, 1.0f, 1.0f);
		}
		else if (state.screenWidth >= 1 && state.screenHeight >= 1) {
			std::uint32_t snapW = static_cast<std::uint32_t>(static_cast<float>(state.screenWidth) * desiredScale);
			std::uint32_t snapH = static_cast<std::uint32_t>(static_cast<float>(state.screenHeight) * desiredScale);
			if (snapW < 1) {
				snapW = 1;
			}
			if (snapH < 1) {
				snapH = 1;
			}
			SetGlobalDynamicRatio(rtMgr,
				static_cast<float>(snapW) / static_cast<float>(state.screenWidth),
				static_cast<float>(snapH) / static_cast<float>(state.screenHeight));
		}
		else {
			SetGlobalDynamicRatio(rtMgr, desiredScale, desiredScale);
		}

		if (prevScale != desiredScale) {
			resetHistory = true;
			prevScale = desiredScale;
			ResetProxyPassProfiles();
		}

		UpdateScaledTargets(GetDynWidthRatio(rtMgr), GetDynHeightRatio(rtMgr));

		UpdateGameSettings();
		CheckResources();
		RefreshHBAOCache();
	}

	void Upscaling::Apply()
	{
		const auto method = settings.method;
		if (method == Method::Off) {
			return;
		}

		auto* main = RE::Main::GetSingleton();

		if (main && main->gameActive) {
			if (startupFrameGuard < 5) {
				startupFrameGuard++;

				return;
			}
		}

		if (!upsclEnabled) {
			return;
		}

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) {
			return;
		}

		auto* ctx = GetImmediateContext();
		if (!ctx) {
			return;
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		auto* backBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kFrameBuffer].srView);
		if (!backBufferSRV) {
			return;
		}

		ID3D11Resource* backBufferResource = nullptr;
		backBufferSRV->GetResource(&backBufferResource);
		if (!backBufferResource) {
			return;
		}

#if F4R_HAS_XESS
		if (method == Method::XeSS) {
			if (!xessColorTexture || !xessColorTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(xessColorTexture->resource), backBufferResource);
		}
		else
#endif
		{
			if (!workingTexture || !workingTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(workingTexture->resource), backBufferResource);
		}

		std::uint32_t renderW = static_cast<std::uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(rtMgr) + 0.5f);
		std::uint32_t renderH = static_cast<std::uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(rtMgr) + 0.5f);
		if (renderW < 1) {
			renderW = 1;
		}
		if (renderH < 1) {
			renderH = 1;
		}

#if F4R_HAS_DLSS
		if (method == Method::DLSS) {
			auto& streamline = Streamline::GetSingleton();

			if (!workingTexture || !workingTexture->resource || !workingTexture->srv || !dlssOutputTexture || !dlssOutputTexture->resource || !dlssOutputTexture->srv) {
				backBufferResource->Release();
				return;
			}

			if (motionVectorTexture && motionVectorTexture->resource && motionVectorTexture->uav && motionVectorTexture->srv && mvFixShader && mvFixCB) {

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

			QualityMode dlssQuality = QualityMode::Native;
			if (currentScale < 0.999f) {
				if (settings.qualityMode != QualityMode::Native) {
					dlssQuality = settings.qualityMode;
				}
			}
			streamline.Evaluate(
				workingTexture->resource,
				workingTexture->srv,
				dlssOutputTexture->resource,
				motionVectorTexture ? motionVectorTexture->resource : nullptr,
				jitterX, jitterY, renderW, renderH, dlssQuality);
			resetHistory = false;

			if (settings.sharpness > 0.0f && tempTexture && tempTexture->resource && tempTexture->uav && rcasShader && rcasCB && dlssOutputTexture->srv) {

				float sharpness = settings.sharpness;
				if (sharpness < 0.0f) {
					sharpness = 0.0f;
				}
				if (sharpness > 1.0f) {
					sharpness = 1.0f;
				}

				RCASConstants constants{};
				constants.sharpness = exp2f(2.0f * sharpness - 2.0f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { dlssOutputTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, tempTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				ctx->CopyResource(backBufferResource, tempTexture->resource);
			}
			else {
				ctx->CopyResource(backBufferResource, dlssOutputTexture->resource);
			}
		}
#endif
#if F4R_HAS_FSR3
		if (method == Method::FSR3) {
			if (fidelityFX) {
				fidelityFX->Apply(workingTexture->resource, jitterX, jitterY, renderW, renderH);
			}

			ctx->CopyResource(backBufferResource, reinterpret_cast<ID3D11Resource*>(workingTexture->resource));
		}
#endif
#if F4R_HAS_XESS
		if (method == Method::XeSS) {
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

			if (xessMotionVectorTexture && xessMotionVectorTexture->resource && xessMotionVectorTexture->uav && mvFixShader && mvFixCB) {

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

			std::uint32_t reset = resetHistory ? 1u : 0u;
			resetHistory = false;

			XeSS::GetSingleton().Execute(
				xessColorTexture.get(),
				xessMotionVectorTexture.get(),
				xessDepthTexture.get(),
				xessOutputTexture.get(),
				jitterX, jitterY, renderW, renderH, reset);

			if (settings.sharpness > 0.0f && tempTexture && tempTexture->uav && xessOutputTexture && xessOutputTexture->srv && rcasShader && rcasCB) {

				float sharpness = settings.sharpness;
				if (sharpness < 0.0f) {
					sharpness = 0.0f;
				}
				if (sharpness > 1.0f) {
					sharpness = 1.0f;
				}

				RCASConstants constants{};
				constants.sharpness = 0.25f + 0.75f * powf(sharpness, 0.2f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { xessOutputTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, tempTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				ctx->CopyResource(backBufferResource, tempTexture->resource);
			}
			else {
				ctx->CopyResource(backBufferResource, xessOutputTexture->resource);
			}
		}
#endif

		backBufferResource->Release();
	}

	void Upscaling::UpdateGameSettings()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (imageSpaceManager && imageSpaceManager->effectList.size() > 0x11 && imageSpaceManager->effectList[0x11]) {
			if (upsclEnabled) {
				if (!fxaaStateSaved) {
					fxaaOriginalActive = imageSpaceManager->effectList[0x11]->isActive;
					fxaaStateSaved = true;
				}
				imageSpaceManager->effectList[0x11]->isActive = false;
			}
		}

		if (upsclEnabled) {
			auto enableTAAReloc = IsAE() || IsNG() ? REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x294512 } } : REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x70681 } };
			auto* taaFlag = reinterpret_cast<bool*>(enableTAAReloc.GetAddress());
			if (!taaFlagSaved) {
				taaFlagOriginal = *taaFlag;
				taaFlagSaved = true;
			}
			*taaFlag = true;
		}
	}

	void Upscaling::RestoreAASettings()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (fxaaStateSaved && imageSpaceManager && imageSpaceManager->effectList.size() > 0x11 && imageSpaceManager->effectList[0x11]) {
			imageSpaceManager->effectList[0x11]->isActive = fxaaOriginalActive;
			fxaaStateSaved = false;
		}

		if (taaFlagSaved) {
			auto enableTAAReloc = IsAE() || IsNG() ? REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x294512 } } : REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x70681 } };
			*reinterpret_cast<bool*>(enableTAAReloc.GetAddress()) = taaFlagOriginal;
			taaFlagSaved = false;
		}
	}

	void Upscaling::ReturnToVanilla()
	{
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		SetGlobalDynamicRatio(rtMgr, 1.0f, 1.0f);

		auto& state = RE::BSGraphics::State::GetSingleton();
		state.offsetX = 0.0f;
		state.offsetY = 0.0f;

		samplerBiasActive = false;
		samplerCacheValid = false;

		InvalidateFlareDepth();
		ReleaseHBAOCache();
		ReleaseScaledTargets();
		RestoreAASettings();

		currentScale = 1.0f;
		wasUpsclEnabled = false;
		resetHistory = true;
	}

	void Upscaling::RefreshSamplerCache(SamplerStates* a_states, float a_bias)
	{
		if (!a_states) {
			return;
		}

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
		if (!upsclEnabled || !samplerBiasActive) {
			return;
		}

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) {
			return;
		}

		for (int i = 0; i < 320; i++) {
			if (biasedSamplerStates[i]) {
				samplerStates->a[i] = biasedSamplerStates[i];
			}
		}
	}

	void Upscaling::ResetSamplerStates()
	{
		if (!upsclEnabled || !samplerBiasActive) {
			return;
		}

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) {
			return;
		}

		for (int i = 0; i < 320; i++) {
			samplerStates->a[i] = originalSamplerStates[i];
		}
	}

	void Upscaling::EnsureSharpenResources(
		ID3D11Device* a_device,
		std::uint32_t a_width,
		std::uint32_t a_height,
		DXGI_FORMAT a_backBufferFormat,
		DXGI_FORMAT a_srvFormat)
	{
		if (!a_device || settings.sharpness <= 0.0f) {
			return;
		}
		if (settings.method != Method::DLSS &&
			settings.method != Method::XeSS) {
			return;
		}
		if (!rcasCB) {
			rcasCB = CreateConstantBuffer(a_device, "rcasCB", sizeof(RCASConstants));
		}
		if (!rcasShader) {
			rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, a_device);
		}
		if (!tempTexture) {
			tempTexture = CreateSharpenTexture(a_device, a_width, a_height, a_backBufferFormat, a_srvFormat);
		}
	}

	void Upscaling::EnsureMotionVectorFixResources(ID3D11Device* a_device)
	{
		if (!a_device) {
			return;
		}
		if (settings.method != Method::DLSS &&
			settings.method != Method::XeSS) {
			return;
		}
		if (!mvFixCB) {
			mvFixCB = CreateConstantBuffer(a_device, "mvFixCB", sizeof(MotionVectorConstants));
		}
		if (!mvFixShader) {
			mvFixShader = CreateComputeShaderFromBytecode(kMVFix, kMVFixSize, a_device);
		}
	}

	void Upscaling::EnsureFlareResources(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!a_device || a_width < 1 || a_height < 1) {
			return;
		}

		bool needTexture = false;
		if (!flareDepthTexture || !flareDepthTexture->resource) {
			needTexture = true;
		}
		else {
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
			auto texture = CreateR32Texture(a_device, a_width, a_height, "EnsureFlareResources");
			if (!texture) {
				return;
			}
			flareDepthTexture = std::move(texture);
		}

		if (!flareDepthCB) {
			flareDepthCB = CreateConstantBuffer(a_device, "flareDepthCB", sizeof(FlareDepthConstants));
			if (!flareDepthCB) {
				return;
			}
		}
		if (!flareDepthShader) {
			flareDepthShader = CreateComputeShaderFromBytecode(kDepthUpscale, kDepthUpscaleSize, a_device);
			if (!flareDepthShader) {
				return;
			}
		}
	}

	void Upscaling::InvalidateFlareDepth()
	{
		PopFlareDepth();
		flareValid = false;
	}

	ID3D11ShaderResourceView* Upscaling::EnsureFlareDepthCopy(REX::W32::ID3D11Device* a_device, REX::W32::ID3D11Texture2D* a_srcTexture)
	{
		if (!a_device || !a_srcTexture) {
			return nullptr;
		}

		REX::W32::D3D11_TEXTURE2D_DESC srcDesc = {};
		a_srcTexture->GetDesc(&srcDesc);

		REX::W32::DXGI_FORMAT srvFormat = REX::W32::DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		if (srcDesc.format == REX::W32::DXGI_FORMAT_D32_FLOAT || srcDesc.format == REX::W32::DXGI_FORMAT_R32_TYPELESS) {
			srvFormat = REX::W32::DXGI_FORMAT_R32_FLOAT;
		}

		if (flareDepthCopyTexture && flareDepthCopySRV &&
			flareDepthCopyFormat == srvFormat &&
			flareDepthCopyWidth == srcDesc.width &&
			flareDepthCopyHeight == srcDesc.height) {
			return flareDepthCopySRV;
		}

		SafeRelease(flareDepthCopySRV);
		SafeRelease(flareDepthCopyTexture);

		REX::W32::D3D11_TEXTURE2D_DESC tmpDesc = srcDesc;
		tmpDesc.format = srvFormat;
		tmpDesc.bindFlags = D3D11_BIND_SHADER_RESOURCE;
		tmpDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
		tmpDesc.cpuAccessFlags = 0;
		tmpDesc.miscFlags = 0;

		REX::W32::ID3D11Texture2D* tmpTex = nullptr;
		HRESULT hr = a_device->CreateTexture2D(&tmpDesc, nullptr, &tmpTex);

		if (FAILED(hr) || !tmpTex) {
			return nullptr;
		}

		flareDepthCopyTexture = reinterpret_cast<ID3D11Texture2D*>(tmpTex);

		REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.format = srvFormat;
		srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.texture2D.mipLevels = 1;
		REX::W32::ID3D11ShaderResourceView* tmpSRV = nullptr;
		hr = a_device->CreateShaderResourceView(tmpTex, &srvDesc, &tmpSRV);

		if (FAILED(hr) || !tmpSRV) {
			SafeRelease(flareDepthCopyTexture);
			return nullptr;
		}

		flareDepthCopySRV = reinterpret_cast<ID3D11ShaderResourceView*>(tmpSRV);
		flareDepthCopyFormat = srvFormat;
		flareDepthCopyWidth = srcDesc.width;
		flareDepthCopyHeight = srcDesc.height;

		return flareDepthCopySRV;
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

		std::uint32_t renderW = static_cast<std::uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(a_rtMgr) + 0.5f);
		std::uint32_t renderH = static_cast<std::uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(a_rtMgr) + 0.5f);
		if (renderW < 1) {
			renderW = 1;
		}
		if (renderH < 1) {
			renderH = 1;
		}

		auto* device = GetRenderer();
		if (!device) {
			flareValid = false;
			return;
		}
		EnsureFlareResources(device, state.screenWidth, state.screenHeight);
		if (!flareDepthTexture || !flareDepthTexture->resource || !flareDepthTexture->uav || !flareDepthShader || !flareDepthCB) {
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
		if (!flareValid) {
			return;
		}
		if (!flareDepthTexture || !flareDepthTexture->srv) {
			return;
		}
		auto& state = RE::BSGraphics::State::GetSingleton();
		if (flareWidth != state.screenWidth || flareHeight != state.screenHeight) {
			return;
		}
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) {
			return;
		}
		if (flareDepthBackup) {
			return;
		}
		auto* liveDepth = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
		if (!liveDepth) {
			return;
		}
		flareDepthBackup = liveDepth;
		flareDepthBackup->AddRef();
		rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(flareDepthTexture->srv);
	}

	void Upscaling::PopFlareDepth()
	{
		if (!flareDepthBackup) {
			return;
		}
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) {
			return;
		}
		rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(flareDepthBackup);
		flareDepthBackup->Release();
		flareDepthBackup = nullptr;
	}

	void Upscaling::ReleaseScaledTargets()
	{
		sslrPassOpen = false;
		compositePassOpen = false;
		PopFullExtentDepth();
		PopUnitScreenTriangle();
		ReleasePooledSSLRProxy();
		ReleasePooledSSLRTexture();
		if (scaledTargetsOverridden) {
			auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
			auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
			const auto* targetIDs = GetRenderTargetIDs(rtMgr);
			if (rendererData && targetIDs) {
				for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
					const std::uint32_t slot = targetIDs[kProxyTargets[i]];
					if (slot >= rendererData->renderTargets.size()) {
						continue;
					}
					auto* proxyTexture = reinterpret_cast<ID3D11Texture2D*>(proxyScaledTargets[i].texture);
					auto& live = rendererData->renderTargets[slot];
					if (proxyTexture &&
						live.texture == reinterpret_cast<REX::W32::ID3D11Texture2D*>(proxyTexture) &&
						savedScaledTargets[i].texture) {
						live = savedScaledTargets[i];
					}
				}
				for (std::uint32_t i = 0; i < kProxyTargetMetaCount; ++i) {
					rtMgr.renderTargetDataArray[i] = savedRenderTargetMeta[i];
				}
				GetSetUseDynViewportAsDefault()(&rtMgr, savedUseDynViewportAsDefault);
			}
			scaledTargetsOverridden = false;
		}
		if (auto* ctxRelease = GetImmediateContext()) {
			if (savedPassRTV || savedPassDSV) {
				ID3D11RenderTargetView* restoreRTV = savedPassRTV;
				ctxRelease->OMSetRenderTargets(1, &restoreRTV, savedPassDSV);
			}
		}
SafeRelease(savedPassRTV);
SafeRelease(savedPassDSV);
		ReleaseProxyTargets();
		scaledTargetsValid = false;
		scaledTargetWidthRatio = 0.0f;
		scaledTargetHeightRatio = 0.0f;
		scaledTargetScreenWidth = 0;
		scaledTargetScreenHeight = 0;
		depthOverrideTexture.reset();
	}

	void Upscaling::ReleaseProxyTargets()
	{
		for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
			auto& proxy = proxyScaledTargets[i];
			if (proxy.uaView) {
				reinterpret_cast<ID3D11UnorderedAccessView*>(proxy.uaView)->Release();
			}
			if (proxy.srView) {
				reinterpret_cast<ID3D11ShaderResourceView*>(proxy.srView)->Release();
			}
			if (proxy.rtView) {
				reinterpret_cast<ID3D11RenderTargetView*>(proxy.rtView)->Release();
			}
			if (proxy.texture) {
				reinterpret_cast<ID3D11Texture2D*>(proxy.texture)->Release();
			}
			proxy = {};
			savedScaledTargets[i] = {};
		}
	}

	void Upscaling::UpdateScaledTargets(float a_widthRatio, float a_heightRatio)
	{
		if (a_widthRatio >= 0.999f && a_heightRatio >= 0.999f) {
			if (scaledTargetsValid || scaledTargetsOverridden) {
				ReleaseScaledTargets();
			}
			return;
		}

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();
		auto* device = GetRenderer();
		auto* ctx = GetImmediateContext();
		const auto* targetIDs = GetRenderTargetIDs(rtMgr);
		if (!rendererData || !device || !targetIDs) {
			return;
		}

		if (scaledTargetsValid && a_widthRatio == scaledTargetWidthRatio && a_heightRatio == scaledTargetHeightRatio && state.screenWidth == scaledTargetScreenWidth && state.screenHeight == scaledTargetScreenHeight) {
			return;
		}

		ReleaseScaledTargets();

		for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
			const std::uint32_t slot = targetIDs[kProxyTargets[i]];
			if (slot >= rendererData->renderTargets.size()) {
				ReleaseScaledTargets();
				return;
			}
			auto& original = rendererData->renderTargets[slot];
			if (!original.texture) {
				ReleaseScaledTargets();
				return;
			}

			D3D11_TEXTURE2D_DESC texDesc{};
			reinterpret_cast<ID3D11Texture2D*>(original.texture)->GetDesc(&texDesc);
			texDesc.Width = static_cast<std::uint32_t>(static_cast<float>(texDesc.Width) * a_widthRatio);
			texDesc.Height = static_cast<std::uint32_t>(static_cast<float>(texDesc.Height) * a_heightRatio);
			if (texDesc.Width < 1 || texDesc.Height < 1) {
				ReleaseScaledTargets();
				return;
			}

			D3D11_RENDER_TARGET_VIEW_DESC rtViewDesc{};
			if (original.rtView) {
				reinterpret_cast<ID3D11RenderTargetView*>(original.rtView)->GetDesc(&rtViewDesc);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC srViewDesc{};
			if (original.srView) {
				reinterpret_cast<ID3D11ShaderResourceView*>(original.srView)->GetDesc(&srViewDesc);
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC uaViewDesc{};
			if (original.uaView) {
				reinterpret_cast<ID3D11UnorderedAccessView*>(original.uaView)->GetDesc(&uaViewDesc);
			}

			auto& proxy = proxyScaledTargets[i];
			auto& saved = savedScaledTargets[i];
			saved = original;
			proxy = {};
			proxy.copyTexture = original.copyTexture;
			proxy.copySRView = original.copySRView;

			if (FAILED(device->CreateTexture2D(&texDesc, nullptr, reinterpret_cast<ID3D11Texture2D**>(&proxy.texture)))) {
				ReleaseScaledTargets();
				return;
			}
			auto* proxyTexture = reinterpret_cast<ID3D11Texture2D*>(proxy.texture);
			if (original.rtView &&
				FAILED(device->CreateRenderTargetView(proxyTexture, &rtViewDesc,
					reinterpret_cast<ID3D11RenderTargetView**>(&proxy.rtView)))) {
				ReleaseScaledTargets();
				return;
			}
			if (original.srView &&
				FAILED(device->CreateShaderResourceView(proxyTexture, &srViewDesc,
					reinterpret_cast<ID3D11ShaderResourceView**>(&proxy.srView)))) {
				ReleaseScaledTargets();
				return;
			}
			if (original.uaView &&
				FAILED(device->CreateUnorderedAccessView(proxyTexture, &uaViewDesc,
					reinterpret_cast<ID3D11UnorderedAccessView**>(&proxy.uaView)))) {
				ReleaseScaledTargets();
				return;
			}
		}

		scaledTargetWidthRatio = a_widthRatio;
		scaledTargetHeightRatio = a_heightRatio;
		scaledTargetScreenWidth = state.screenWidth;
		scaledTargetScreenHeight = state.screenHeight;
		scaledTargetsValid = true;

		if (ctx) {
			static const std::array<float, 4> kClearZero = { 0.0f, 0.0f, 0.0f, 0.0f };
			const std::uint32_t slot = targetIDs[LogicalRenderTarget::kMain];
			if (slot < rendererData->renderTargets.size()) {
				auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rendererData->renderTargets[slot].rtView);
				if (rtv) {
					ctx->ClearRenderTargetView(rtv, kClearZero.data());
				}
			}
		}
	}

	bool Upscaling::PushFullExtentDepth()
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		auto* device = GetRenderer();
		if (!rendererData || !ctx || !device) {
			return false;
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		const std::uint32_t screenW = state.screenWidth;
		const std::uint32_t screenH = state.screenHeight;
		if (screenW < 1 || screenH < 1) {
			return false;
		}

		const std::uint32_t renderW = static_cast<std::uint32_t>(static_cast<float>(screenW) * scaledTargetWidthRatio);
		const std::uint32_t renderH = static_cast<std::uint32_t>(static_cast<float>(screenH) * scaledTargetHeightRatio);
		if (renderW < 1 || renderH < 1) {
			return false;
		}

		bool recreateDepth = !depthOverrideTexture || !depthOverrideTexture->resource;
		if (!recreateDepth) {
			D3D11_TEXTURE2D_DESC liveDesc{};
			depthOverrideTexture->resource->GetDesc(&liveDesc);
			recreateDepth = (liveDesc.Width != renderW || liveDesc.Height != renderH);
		}
		if (recreateDepth) {
			auto texture = CreateR32Texture(device, renderW, renderH, "PushFullExtentDepth");
			if (!texture) {
				return false;
			}
			depthOverrideTexture = std::move(texture);
			depthOverrideFrame = 0;
		}

		if (!depthOverrideShader) {
			depthOverrideShader = CreateComputeShaderFromBytecode(kDepthCopy, kDepthCopySize, device);
		}
		if (!depthOverrideShader) {
			return false;
		}

		auto* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(
			rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
		if (!depthSRV) {
			return false;
		}

		const bool needCopy = depthOverrideFrame != state.frameCount || depthOverrideFrame == 0;
		if (needCopy) {
			ID3D11RenderTargetView* prevRTV = nullptr;
			ID3D11DepthStencilView* prevDSV = nullptr;
			ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);

			ID3D11ShaderResourceView* srvs[1] = { depthSRV };
			RunComputePass(ctx, depthOverrideShader, nullptr, srvs, 1, depthOverrideTexture->uav,
				(renderW + 7) / 8, (renderH + 7) / 8);

			ID3D11RenderTargetView* prevTargets[1] = { prevRTV };
			ctx->OMSetRenderTargets(1, prevTargets, prevDSV);
			if (prevRTV) {
				prevRTV->Release();
			}
			if (prevDSV) {
				prevDSV->Release();
			}
			depthOverrideFrame = state.frameCount;
		}

		if (depthOverrideBackup) {
			PopFullExtentDepth();
		}
		depthOverrideBackup = depthSRV;
		depthOverrideBackup->AddRef();
		rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth =
			reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthOverrideTexture->srv);
		return true;
	}

	void Upscaling::PopFullExtentDepth()
	{
		if (!depthOverrideBackup) {
			return;
		}
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (rendererData) {
			rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth =
				reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthOverrideBackup);
		}
		depthOverrideBackup->Release();
		depthOverrideBackup = nullptr;
	}

	bool Upscaling::EnterFullExtentPass(ProxyPass a_pass, std::span<const std::uint32_t> a_copyIn,
		bool a_clearProxies)
	{
		if (a_pass >= ProxyPass::Count) {
			return false;
		}
		if (!scaledTargetsValid || scaledTargetsOverridden) {
			return false;
		}

		auto& a_rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		const auto* targetIDs = GetRenderTargetIDs(a_rtMgr);
		if (!rendererData || !ctx || !targetIDs) {
			return false;
		}

		for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
			const std::uint32_t slot = targetIDs[kProxyTargets[i]];
			if (slot >= rendererData->renderTargets.size()) {
				return false;
			}
			if (!savedScaledTargets[i].texture || !proxyScaledTargets[i].texture) {
				return false;
			}
			if (rendererData->renderTargets[slot].texture !=
				reinterpret_cast<REX::W32::ID3D11Texture2D*>(savedScaledTargets[i].texture)) {
				ReleaseScaledTargets();
				return false;
			}
		}

		auto& profile = proxyPassProfiles[static_cast<std::size_t>(a_pass)];

		std::uint32_t staticMask = 0;
		for (const auto target : a_copyIn) {
			for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
				if (kProxyTargets[i] == target) {
					staticMask |= (1u << i);
					break;
				}
			}
		}
		profile.staticMask = staticMask;

		const bool narrowed = profile.narrowable && profile.learned && !profile.fallback;
		std::uint32_t copyMask = staticMask;
		if (narrowed) {
			copyMask = profile.copyInMask & staticMask;
		}
		profile.fallback = false;

		static const std::array<float, 4> kClearZero = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (a_clearProxies) {
			for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
				auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(proxyScaledTargets[i].rtView);
				if (rtv) {
					ctx->ClearRenderTargetView(rtv, kClearZero.data());
				}
			}
		}

		for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
			if ((copyMask & (1u << i)) == 0) {
				continue;
			}
			auto* proxyTexture = reinterpret_cast<ID3D11Texture2D*>(proxyScaledTargets[i].texture);
			D3D11_TEXTURE2D_DESC proxyDesc{};
			proxyTexture->GetDesc(&proxyDesc);
			D3D11_BOX box{};
			box.right = static_cast<LONG>(proxyDesc.Width);
			box.bottom = static_cast<LONG>(proxyDesc.Height);
			box.back = 1;
			ctx->CopySubresourceRegion(proxyTexture, 0, 0, 0, 0,
				reinterpret_cast<ID3D11Texture2D*>(savedScaledTargets[i].texture), 0, &box);
		}

		for (std::uint32_t i = 0; i < kProxyTargetMetaCount; ++i) {
			savedRenderTargetMeta[i] = a_rtMgr.renderTargetDataArray[i];
		}

		ctx->OMGetRenderTargets(1, &savedPassRTV, &savedPassDSV);

		ID3D11ShaderResourceView* boundSRVs[kProxySRVSlotCount] = {};
		ctx->PSGetShaderResources(0, kProxySRVSlotCount, boundSRVs);
		for (std::uint32_t slot = 0; slot < kProxySRVSlotCount; ++slot) {
			if (!boundSRVs[slot]) {
				continue;
			}
			for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
				auto* originalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(savedScaledTargets[i].srView);
				auto* proxySRV = reinterpret_cast<ID3D11ShaderResourceView*>(proxyScaledTargets[i].srView);
				if (originalSRV && proxySRV && boundSRVs[slot] == originalSRV) {
					ctx->PSSetShaderResources(slot, 1, &proxySRV);
					break;
				}
			}
			boundSRVs[slot]->Release();
		}

		savedUseDynViewportAsDefault = GetUseDynViewportAsDefault(a_rtMgr);
		ApplyFullExtentOverride();
		scaledTargetsOverridden = true;
		activeProxyPass = a_pass;
		activeCopyInMask = copyMask;

		PushFullExtentDepth();
		return true;
	}

	void Upscaling::ReapplyFullExtentOverride()
	{
		if (!scaledTargetsOverridden) {
			return;
		}
		ApplyFullExtentOverride();
	}

	void Upscaling::ApplyFullExtentOverride()
	{
		auto& a_rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		const auto* targetIDs = GetRenderTargetIDs(a_rtMgr);
		if (!rendererData || !targetIDs) {
			return;
		}

		for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
			const std::uint32_t slot = targetIDs[kProxyTargets[i]];
			if (slot >= rendererData->renderTargets.size()) {
				continue;
			}
			rendererData->renderTargets[slot] = proxyScaledTargets[i];
		}

		for (std::uint32_t i = 0; i < kProxyTargetMetaCount; ++i) {
			a_rtMgr.renderTargetDataArray[i] = savedRenderTargetMeta[i];
			a_rtMgr.renderTargetDataArray[i].width = static_cast<std::uint32_t>(
				static_cast<float>(savedRenderTargetMeta[i].width) * scaledTargetWidthRatio);
			a_rtMgr.renderTargetDataArray[i].height = static_cast<std::uint32_t>(
				static_cast<float>(savedRenderTargetMeta[i].height) * scaledTargetHeightRatio);
		}

		SetGlobalDynamicRatio(a_rtMgr, 1.0f, 1.0f);
		GetSetUseDynViewportAsDefault()(&a_rtMgr, false);
		GetSetCurrentViewportDefault()(&a_rtMgr);
	}

	void Upscaling::ResetProxyPassProfiles()
	{
		for (auto& profile : proxyPassProfiles) {
			profile = ProxyPassProfile{};
		}
		proxyPassProfiles[static_cast<std::size_t>(ProxyPass::CompositeA)].narrowable = true;
		proxyPassProfiles[static_cast<std::size_t>(ProxyPass::CompositeB)].narrowable = true;
		activeCopyInMask = 0;
		pendingObservedMask = 0;
		activeProxyPass = ProxyPass::Count;
	}

	bool Upscaling::WillDeferredScatterRun() const
	{
		if (!IsOG()) {
			return false;
		}

		constexpr std::uintptr_t kDeferredScatterManagerOffset = 0x6723350;
		constexpr std::uintptr_t kDeferredScatterCountOffset = 0x420;
		constexpr std::uintptr_t kDeferredScatterArrayStride = 0xA * sizeof(void*);
		constexpr std::uintptr_t kDeferredScatterPendingOffset = 0x1C;

		const auto baseAddress = REL::Module::GetSingleton()->GetBaseAddress();
		if (baseAddress == 0) {
			return false;
		}
		const auto* base = reinterpret_cast<const std::uint8_t*>(baseAddress);
		auto* managerSlot = reinterpret_cast<void**>(const_cast<std::uint8_t*>(base) + kDeferredScatterManagerOffset);
		if (!*managerSlot) {
			return false;
		}
		auto* countSlot = *reinterpret_cast<void**>(
			reinterpret_cast<std::uint8_t*>(*managerSlot) + kDeferredScatterCountOffset +
			kDeferredScatterArrayStride);
		if (!countSlot) {
			return false;
		}
		return *reinterpret_cast<const int*>(
			reinterpret_cast<std::uint8_t*>(countSlot) + kDeferredScatterPendingOffset) > 0;
	}

	std::uint32_t Upscaling::ObserveBoundProxySRVs(bool a_rebind)
	{
		auto* ctx = GetImmediateContext();
		std::uint32_t observedMask = 0;
		if (!ctx) {
			return observedMask;
		}

		ID3D11ShaderResourceView* boundSRVs[kProxySRVSlotCount] = {};
		ctx->PSGetShaderResources(0, kProxySRVSlotCount, boundSRVs);
		for (std::uint32_t slot = 0; slot < kProxySRVSlotCount; ++slot) {
			if (!boundSRVs[slot]) {
				continue;
			}
			for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
				auto* originalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(savedScaledTargets[i].srView);
				auto* proxySRV = reinterpret_cast<ID3D11ShaderResourceView*>(proxyScaledTargets[i].srView);
				if (originalSRV && proxySRV && boundSRVs[slot] == proxySRV) {
					observedMask |= (1u << i);
					if (a_rebind) {
						ctx->PSSetShaderResources(slot, 1, &originalSRV);
					}
					break;
				}
			}
			boundSRVs[slot]->Release();
		}
		return observedMask;
	}

	void Upscaling::ExitFullExtentPass(ProxyPass a_pass, std::span<const std::uint32_t> a_copyOut)
	{
		if (!scaledTargetsOverridden) {
			return;
		}
		auto& a_rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* ctx = GetImmediateContext();
		const auto* targetIDs = GetRenderTargetIDs(a_rtMgr);

		PopFullExtentDepth();

		std::uint32_t observedMask = ObserveBoundProxySRVs(true);
		observedMask |= pendingObservedMask;
		pendingObservedMask = 0;

		const bool validPass = a_pass < ProxyPass::Count;
		if (validPass) {
			auto& profile = proxyPassProfiles[static_cast<std::size_t>(a_pass)];
			if (profile.narrowable) {
				const std::uint32_t missing = observedMask & profile.staticMask & ~activeCopyInMask;
				if (missing) {
					profile.observedMask |= missing;
					profile.stableFrames = 0;
					profile.fallback = true;
				}
				else {
					const std::uint32_t merged = profile.observedMask | observedMask;
					if (merged != profile.observedMask) {
						profile.observedMask = merged;
						profile.stableFrames = 0;
					}
					else if (profile.stableFrames < kProxyLearnFrames) {
						profile.stableFrames++;
					}
					if (!profile.learned && profile.stableFrames >= kProxyLearnFrames && profile.observedMask != 0) {
						profile.learned = true;
					}
				}

				if (profile.learned) {
					profile.copyInMask = profile.observedMask & profile.staticMask;
				}
			}
		}

		for (std::uint32_t i = 0; i < kProxyTargetMetaCount; ++i) {
			a_rtMgr.renderTargetDataArray[i] = savedRenderTargetMeta[i];
		}

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (rendererData && targetIDs) {
			for (std::uint32_t i = 0; i < kProxyTargetCount; ++i) {
				const std::uint32_t slot = targetIDs[kProxyTargets[i]];
				if (slot >= rendererData->renderTargets.size()) {
					continue;
				}
				if (ctx && std::ranges::find(a_copyOut, kProxyTargets[i]) != a_copyOut.end()) {
					auto* proxyTexture = reinterpret_cast<ID3D11Texture2D*>(proxyScaledTargets[i].texture);
					auto* originalTexture = reinterpret_cast<ID3D11Texture2D*>(savedScaledTargets[i].texture);
					if (proxyTexture && originalTexture) {
						D3D11_TEXTURE2D_DESC proxyDesc{};
						proxyTexture->GetDesc(&proxyDesc);
						D3D11_BOX box{};
						box.right = static_cast<LONG>(proxyDesc.Width);
						box.bottom = static_cast<LONG>(proxyDesc.Height);
						box.back = 1;
						ctx->CopySubresourceRegion(originalTexture, 0, 0, 0, 0, proxyTexture, 0, &box);
					}
				}
				rendererData->renderTargets[slot] = savedScaledTargets[i];
			}
		}

		SetGlobalDynamicRatio(a_rtMgr, scaledTargetWidthRatio, scaledTargetHeightRatio);
		GetSetUseDynViewportAsDefault()(&a_rtMgr, savedUseDynViewportAsDefault);
		GetSetCurrentViewportDefault()(&a_rtMgr);

		if (ctx && (savedPassRTV || savedPassDSV)) {
			ID3D11RenderTargetView* restoreRTV = savedPassRTV;
			ctx->OMSetRenderTargets(1, &restoreRTV, savedPassDSV);
		}
SafeRelease(savedPassRTV);
SafeRelease(savedPassDSV);
		scaledTargetsOverridden = false;
		activeProxyPass = ProxyPass::Count;
		activeCopyInMask = 0;
	}

	void Upscaling::AbortFullExtentPass()
	{
		if (!scaledTargetsOverridden) {
			return;
		}
		sslrPassOpen = false;
		compositePassOpen = false;
		ExitFullExtentPass(activeProxyPass, std::span<const std::uint32_t>{});
		PopUnitScreenTriangle();
	}

	void Upscaling::AcquirePooledSSLRProxy()
	{
		if (!sslrPassOpen || pooledProxyActive) {
			return;
		}

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* device = GetRenderer();
		const auto* targetIDs = GetRenderTargetIDs(rtMgr);
		if (!rendererData || !device || !targetIDs) {
			return;
		}

		const std::uint32_t slot = targetIDs[LogicalRenderTarget::kSSLRBlurH];
		if (slot >= rendererData->renderTargets.size()) {
			return;
		}
		auto& live = rendererData->renderTargets[slot];
		if (!live.texture) {
			return;
		}

		D3D11_TEXTURE2D_DESC texDesc{};
		reinterpret_cast<ID3D11Texture2D*>(live.texture)->GetDesc(&texDesc);
		texDesc.Width = static_cast<std::uint32_t>(
			static_cast<float>(texDesc.Width) * scaledTargetWidthRatio);
		texDesc.Height = static_cast<std::uint32_t>(
			static_cast<float>(texDesc.Height) * scaledTargetHeightRatio);
		if (texDesc.Width < 1 || texDesc.Height < 1) {
			return;
		}

		if (!pooledProxyTexture || !pooledProxyTexture->resource || pooledProxyWidth != texDesc.Width || pooledProxyHeight != texDesc.Height || pooledProxyFormat != texDesc.Format) {
			ReleasePooledSSLRTexture();
			D3D11_RENDER_TARGET_VIEW_DESC rtViewDesc{};
			if (live.rtView) {
				reinterpret_cast<ID3D11RenderTargetView*>(live.rtView)->GetDesc(&rtViewDesc);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC srViewDesc{};
			if (live.srView) {
				reinterpret_cast<ID3D11ShaderResourceView*>(live.srView)->GetDesc(&srViewDesc);
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC uaViewDesc{};
			if (live.uaView) {
				reinterpret_cast<ID3D11UnorderedAccessView*>(live.uaView)->GetDesc(&uaViewDesc);
			}

			auto texture = std::make_unique<Texture2D>();
			if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &texture->resource))) {
				return;
			}
			if (live.rtView &&
				FAILED(device->CreateRenderTargetView(texture->resource, &rtViewDesc,
					&texture->rtv))) {
				return;
			}
			if (live.srView &&
				FAILED(device->CreateShaderResourceView(texture->resource, &srViewDesc,
					&texture->srv))) {
				return;
			}
			if (live.uaView &&
				FAILED(device->CreateUnorderedAccessView(texture->resource, &uaViewDesc,
					&texture->uav))) {
				return;
			}
			pooledProxyTexture = std::move(texture);
			pooledProxyWidth = texDesc.Width;
			pooledProxyHeight = texDesc.Height;
			pooledProxyFormat = texDesc.Format;
		}

		pooledProxySaved = live;
		live = {};
		live.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(pooledProxyTexture->resource);
		live.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(pooledProxyTexture->rtv);
		live.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(pooledProxyTexture->srv);
		live.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(pooledProxyTexture->uav);
		live.copyTexture = pooledProxySaved.copyTexture;
		live.copySRView = pooledProxySaved.copySRView;
		pooledProxySlot = slot;
		pooledProxyActive = true;
	}

	void Upscaling::ReleasePooledSSLRProxy()
	{
		if (!pooledProxyActive) {
			return;
		}
		pooledProxyActive = false;

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		const auto* targetIDs = GetRenderTargetIDs(rtMgr);

		const bool slotReused = rendererData && targetIDs && targetIDs[LogicalRenderTarget::kSSLRBlurH] != pooledProxySlot;
		if (slotReused) {
			REX::LogWarning("SSLR pooled slot moved while it was held: slot={} now={}",
				pooledProxySlot, targetIDs[LogicalRenderTarget::kSSLRBlurH]);
		}

		if (rendererData && pooledProxySlot < rendererData->renderTargets.size() && rendererData->renderTargets[pooledProxySlot].texture == reinterpret_cast<REX::W32::ID3D11Texture2D*>(pooledProxyTexture->resource)) {
			rendererData->renderTargets[pooledProxySlot] = pooledProxySaved;
		}
		pooledProxySaved = {};
	}

	void Upscaling::ReleasePooledSSLRTexture()
	{
		pooledProxyTexture.reset();
		pooledProxyWidth = 0;
		pooledProxyHeight = 0;
		pooledProxyFormat = DXGI_FORMAT_UNKNOWN;
	}

	void Upscaling::BeginSSLRPass()
	{
		if (sslrPassOpen) {
			return;
		}
		if (!EnterFullExtentPass(ProxyPass::SSLR, kProxySSLRCopyIn, false)) {
			return;
		}
		sslrPassOpen = true;
		PushUnitScreenTriangle();
	}

	void Upscaling::EndSSLRPass()
	{
		if (!sslrPassOpen) {
			return;
		}
		sslrPassOpen = false;
		PopUnitScreenTriangle();
		ExitFullExtentPass(ProxyPass::SSLR, kProxySSLRCopyOut);
	}

	void Upscaling::PushUnitScreenTriangle()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (!imageSpaceManager || unitScreenTriangle) {
			return;
		}
		if (!imageSpaceManager->dynamicScreenTriShape || !imageSpaceManager->screenTriShape) {
			return;
		}
		unitScreenTriangle = imageSpaceManager->dynamicScreenTriShape;
		imageSpaceManager->dynamicScreenTriShape = imageSpaceManager->screenTriShape;
	}

	void Upscaling::PopUnitScreenTriangle()
	{
		if (!unitScreenTriangle) {
			return;
		}
		if (auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton()) {
			imageSpaceManager->dynamicScreenTriShape = unitScreenTriangle;
		}
		unitScreenTriangle = nullptr;
	}

	void Upscaling::CheckResources()
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* device = GetRenderer();
		if (!device) {
			return;
		}

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

		if (resourcesCreated) {
			const bool outputChanged = cachedWidth != state.screenWidth ||
									   cachedHeight != state.screenHeight ||
									   cachedFormat != backBufferFormat;

			if (!outputChanged && cachedMethod == settings.method) {
				if (currentScale < 0.999f) {
					if (!flareDepthTexture || !flareDepthTexture->resource || !flareDepthShader || !flareDepthCB || !flareValid) {
						EnsureFlareResources(device, state.screenWidth, state.screenHeight);
					}
				}
				else if (flareValid) {
					flareValid = false;
				}
				EnsureSharpenResources(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
				EnsureMotionVectorFixResources(device);
				return;
			}
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
				if (outputChanged) {
					xess.DestroyContext();
				}
			}
#endif
			SafeRelease(mvFixCB);
			SafeRelease(rcasCB);
			SafeRelease(mvFixShader);
			SafeRelease(rcasShader);
			SafeRelease(depthCopyShader);
			SafeRelease(flareDepthCB);
			SafeRelease(flareDepthShader);
			PopFlareDepth();
			flareDepthTexture.reset();
			SafeRelease(flareDepthCopySRV);
			SafeRelease(flareDepthCopyTexture);
			flareDepthCopyFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
			flareDepthCopyWidth = 0;
			flareDepthCopyHeight = 0;
			flareValid = false;
			flareWidth = 0;
			flareHeight = 0;
			resourcesCreated = false;
		}

		const auto method = settings.method;

#if F4R_HAS_FSR3
		if (method == Method::FSR3 && !workingTexture) {
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
				REX::LogError("CreateTexture2D(workingTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				workingTexture.reset();
				return;
			}
		}
#endif

#if F4R_HAS_DLSS
		if (method == Method::DLSS && !workingTexture) {
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
				REX::LogError("CreateTexture2D(workingTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				workingTexture.reset();
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = typedFormat;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(workingTexture->resource, &srvDesc, &workingTexture->srv);
			if (FAILED(hr)) {
				REX::LogError("CreateShaderResourceView(workingTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
			}
		}

		if (method == Method::DLSS && !dlssOutputTexture) {
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
				REX::LogError("CreateTexture2D(dlssOutputTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				dlssOutputTexture.reset();
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = typedFormat;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(dlssOutputTexture->resource, &srvDesc, &dlssOutputTexture->srv);
			if (FAILED(hr)) {
				REX::LogError("CreateShaderResourceView(dlssOutputTexture) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
				dlssOutputTexture.reset();
				return;
			}
		}
#endif

#if F4R_HAS_DLSS
		if (method == Method::DLSS) {
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
					REX::LogError("CreateTexture2D(motionVector) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
				hr = device->CreateUnorderedAccessView(motionVectorTexture->resource, &uavDesc, &motionVectorTexture->uav);
				if (FAILED(hr)) {
					REX::LogError("CreateUnorderedAccessView(motionVector) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				hr = device->CreateShaderResourceView(motionVectorTexture->resource, &srvDesc, &motionVectorTexture->srv);
				if (FAILED(hr)) {
					REX::LogError("CreateShaderResourceView(motionVector) failed hr=0x{:x}", static_cast<std::uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				EnsureMotionVectorFixResources(device);
				EnsureSharpenResources(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
			}
		}
#endif
#if F4R_HAS_FSR3
		if (method == Method::FSR3) {
			fidelityFX = std::make_unique<FidelityFX>();
			if (!fidelityFX->CreateFSRResources(
					device,
					state.screenWidth, state.screenHeight,
					backBufferFormat)) {
				REX::LogError("CheckResources: CreateFSRResources failed");
				fidelityFX.reset();
			}
			else if (g_enbLoaded && settings.qualityMode != QualityMode::Native) {
				REX::LogInformation("ENB forces Native for FSR3");
			}
		}
#endif
#if F4R_HAS_XESS
		if (method == Method::XeSS) {
			auto* context = GetImmediateContext();
			auto& xess = XeSS::GetSingleton();
			auto& rtMgrFb = RE::BSGraphics::RenderTargetManager::GetSingleton();
			auto fallbackNative = [&](const char* a_reason) {
				if (!xess.disabled) {
					REX::LogWarning("XeSS disabled: {} fallback to Native", a_reason);
					xess.disabled = true;
				}
				xess.DestroyContext();
				xess.TeardownD3D12();
				upsclEnabled = false;
				currentScale = 1.0f;
				SetGlobalDynamicRatio(rtMgrFb, 1.0f, 1.0f);
				GetSetCurrentViewportDefault()(&rtMgrFb);
				resetHistory = true;
			};
			if (xess.disabled) {
				return;
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

			std::uint32_t width = state.screenWidth;
			std::uint32_t height = state.screenHeight;

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

			EnsureSharpenResources(device, width, height, backBufferFormat, typedFormat);

			if (!depthCopyShader) {
				depthCopyShader = CreateComputeShaderFromBytecode(kDepthCopy, kDepthCopySize, device);
			}

			EnsureMotionVectorFixResources(device);

			QualityMode xessQualityMode = settings.qualityMode;
			if (g_enbLoaded && xessQualityMode != QualityMode::Native) {
				xessQualityMode = QualityMode::Native;
				REX::LogInformation("ENB forces Native for XeSS");
			}

			if (!xess.initialized) {
				if (!xess.CreateContext(width, height, xessQualityMode)) {
					fallbackNative("context creation failed");
					return;
				}
				cachedQuality = xessQualityMode;
			}
			else if (cachedQuality != xessQualityMode) {
				xess.DestroyContext();
				if (!xess.CreateContext(width, height, xessQualityMode)) {
					fallbackNative("context creation failed");
					return;
				}
				cachedQuality = xessQualityMode;
			}
		}
#endif

		if (currentScale < 0.999f) {
			EnsureFlareResources(device, state.screenWidth, state.screenHeight);
		}
		else {
			if (flareDepthBackup) {
				PopFlareDepth();
			}
			flareValid = false;
		}

		resourcesCreated = true;
		cachedWidth = state.screenWidth;
		cachedHeight = state.screenHeight;
		cachedFormat = backBufferFormat;
		cachedMethod = settings.method;
	}

	namespace
	{
		constexpr std::uint32_t kScaledTargetCount = 2;
		const std::uint32_t kScaledTargets[kScaledTargetCount] = {
			RenderTarget::kGbufferNormal, RenderTarget::kSSAOFinal
		};
	}

	bool Upscaling::EnterHBAO()
	{
		if (!upsclEnabled || currentScale >= 0.999f) {
			return false;
		}
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		if (GetDynWidthRatio(rtMgr) >= 0.999f && GetDynHeightRatio(rtMgr) >= 0.999f) {
			return false;
		}
		if (hbaoActive) {
			return false;
		}
		RefreshHBAOCache();
		if (!hbaoCacheValid) {
			return false;
		}
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		if (!rendererData || !ctx) {
			return false;
		}
		for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
			std::uint32_t idx = kScaledTargets[k];
			if (idx >= rendererData->renderTargets.size()) {
				continue;
			}
			auto* liveTex = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[idx].texture);
			if (!liveTex) {
				continue;
			}
			D3D11_TEXTURE2D_DESC liveDesc{};
			liveTex->GetDesc(&liveDesc);
			const auto& fullDesc = hbaoFullDesc[idx];
			if (liveDesc.Width != fullDesc.Width || liveDesc.Height != fullDesc.Height || liveDesc.Format != fullDesc.Format || liveDesc.SampleDesc.Count != fullDesc.SampleDesc.Count || liveDesc.SampleDesc.Quality != fullDesc.SampleDesc.Quality) {
				ReleaseHBAOCache();
				RefreshHBAOCache();
				if (!hbaoCacheValid) {
					return false;
				}
				break;
			}
		}
		if (!hbaoDepthDescribed) {
			return false;
		}
		auto* depthTex = reinterpret_cast<ID3D11Texture2D*>(
			rendererData->depthStencilTargets[DepthStencil::kMain].texture);
		if (!depthTex) {
			return false;
		}
		D3D11_TEXTURE2D_DESC depthDesc{};
		depthTex->GetDesc(&depthDesc);
		if (depthDesc.Width != hbaoDepthFullDesc.Width || depthDesc.Height != hbaoDepthFullDesc.Height || depthDesc.Format != hbaoDepthFullDesc.Format) {
			ReleaseHBAOCache();
			RefreshHBAOCache();
			if (!hbaoCacheValid) {
				return false;
			}
		}
		auto& state = RE::BSGraphics::State::GetSingleton();
		if (hbaoDepthFrame != state.frameCount || hbaoDepthFrame == 0) {
			auto* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(
				rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
			if (!depthSRV || !hbaoDepth || !hbaoDepth->resource || !hbaoDepth->uav || !hbaoDepthShader) {
				return false;
			}
			D3D11_TEXTURE2D_DESC fedDesc{};
			hbaoDepth->resource->GetDesc(&fedDesc);
			ID3D11ComputeShader* keptCS = nullptr;
			ID3D11ShaderResourceView* keptSRV = nullptr;
			ID3D11UnorderedAccessView* keptUAV = nullptr;
			ID3D11Buffer* keptCB = nullptr;
			ctx->CSGetShader(&keptCS, nullptr, nullptr);
			ctx->CSGetShaderResources(0, 1, &keptSRV);
			ctx->CSGetUnorderedAccessViews(0, 1, &keptUAV);
			ctx->CSGetConstantBuffers(0, 1, &keptCB);
			ctx->CSSetShaderResources(0, 1, &depthSRV);
			ID3D11UnorderedAccessView* fedUAVs[1] = { hbaoDepth->uav };
			ctx->CSSetUnorderedAccessViews(0, 1, fedUAVs, nullptr);
			ctx->CSSetShader(hbaoDepthShader, nullptr, 0);
			ctx->Dispatch((fedDesc.Width + 7) / 8, (fedDesc.Height + 7) / 8, 1);
			ID3D11ShaderResourceView* clearedSRVs[1] = { nullptr };
			ctx->CSSetShaderResources(0, 1, clearedSRVs);
			ID3D11UnorderedAccessView* clearedUAVs[1] = { nullptr };
			ctx->CSSetUnorderedAccessViews(0, 1, clearedUAVs, nullptr);
			ctx->CSSetShader(nullptr, nullptr, 0);
			ctx->CSSetShader(keptCS, nullptr, 0);
			ctx->CSSetShaderResources(0, 1, &keptSRV);
			ctx->CSSetUnorderedAccessViews(0, 1, &keptUAV, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &keptCB);
			if (keptCS) {
				keptCS->Release();
			}
			if (keptSRV) {
				keptSRV->Release();
			}
			if (keptUAV) {
				keptUAV->Release();
			}
			if (keptCB) {
				keptCB->Release();
			}
			hbaoDepthFrame = state.frameCount;
		}
		hbaoSavedWidthRatio = GetDynWidthRatio(rtMgr);
		hbaoSavedHeightRatio = GetDynHeightRatio(rtMgr);
		if (hbaoDepth && hbaoDepth->srv) {
			auto* liveDepth = reinterpret_cast<ID3D11ShaderResourceView*>(
				rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
			if (liveDepth && !hbaoDepthBackedUp) {
				hbaoDepthBackedUp = liveDepth;
				hbaoDepthBackedUp->AddRef();
				rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(hbaoDepth->srv);
				hbaoDepthMapped = true;
			}
		}
		for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
			std::uint32_t idx = kScaledTargets[k];
			if (idx >= rendererData->renderTargets.size()) {
				continue;
			}
			if (!hbaoTargets[idx] || !hbaoTargets[idx]->resource) {
				continue;
			}
			auto& live = rendererData->renderTargets[idx];
			hbaoBackedUp[idx].texture = reinterpret_cast<ID3D11Texture2D*>(live.texture);
			hbaoBackedUp[idx].copyTexture = reinterpret_cast<ID3D11Texture2D*>(live.copyTexture);
			hbaoBackedUp[idx].rtView = reinterpret_cast<ID3D11RenderTargetView*>(live.rtView);
			hbaoBackedUp[idx].srView = reinterpret_cast<ID3D11ShaderResourceView*>(live.srView);
			hbaoBackedUp[idx].copySRView = reinterpret_cast<ID3D11ShaderResourceView*>(live.copySRView);
			hbaoBackedUp[idx].uaView = reinterpret_cast<ID3D11UnorderedAccessView*>(live.uaView);
			live.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(hbaoTargets[idx]->resource);
			live.copyTexture = nullptr;
			live.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(hbaoTargets[idx]->rtv);
			live.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(hbaoTargets[idx]->srv);
			live.copySRView = nullptr;
			live.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(hbaoTargets[idx]->uav);
			hbaoMapped[idx] = true;
			if (idx == RenderTarget::kGbufferNormal && hbaoBackedUp[idx].texture) {
				D3D11_TEXTURE2D_DESC lowDesc{};
				hbaoTargets[idx]->resource->GetDesc(&lowDesc);
				D3D11_BOX seedBox{};
				seedBox.left = 0;
				seedBox.top = 0;
				seedBox.front = 0;
				seedBox.right = lowDesc.Width;
				seedBox.bottom = lowDesc.Height;
				seedBox.back = 1;
				ctx->CopySubresourceRegion(hbaoTargets[idx]->resource, 0, 0, 0, 0, hbaoBackedUp[idx].texture, 0, &seedBox);
			}
		}
		if (!hbaoMetaHeld) {
			for (std::uint32_t i = 0; i < kHBAOMetaCount; i++) {
				hbaoSavedMeta[i] = rtMgr.renderTargetDataArray[i];
			}
			hbaoMetaHeld = true;
		}
		float widthRatio = GetDynWidthRatio(rtMgr);
		float heightRatio = GetDynHeightRatio(rtMgr);
		for (std::uint32_t i = 0; i < kHBAOMetaCount; i++) {
			auto& meta = rtMgr.renderTargetDataArray[i];
			meta.width = static_cast<std::uint32_t>(static_cast<float>(hbaoSavedMeta[i].width) * widthRatio);
			meta.height = static_cast<std::uint32_t>(static_cast<float>(hbaoSavedMeta[i].height) * heightRatio);
		}
		ID3D11ShaderResourceView* boundPS[16]{};
		ctx->PSGetShaderResources(0, 16, boundPS);
		for (std::uint32_t slot = 0; slot < 16; slot++) {
			auto* current = boundPS[slot];
			if (!current) {
				continue;
			}
			for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
				std::uint32_t idx = kScaledTargets[k];
				if (idx >= rendererData->renderTargets.size()) {
					continue;
				}
				auto* lowSRV = hbaoTargets[idx] ? hbaoTargets[idx]->srv : nullptr;
				auto* fullSRV = hbaoMapped[idx] ? hbaoBackedUp[idx].srView : nullptr;
				if (fullSRV && lowSRV && current == fullSRV) {
					ctx->PSSetShaderResources(slot, 1, &lowSRV);
					break;
				}
			}
			current->Release();
		}
		InvokeHBAODynRes(false);
		SetGlobalDynamicRatio(rtMgr, 1.0f, 1.0f);
		GetSetCurrentViewportDefault()(&rtMgr);
		hbaoActive = true;
		return true;
	}

	void Upscaling::RestoreHBAOState()
	{
		if (!hbaoActive) {
			return;
		}
		hbaoActive = false;
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		if (ctx && rendererData) {
			ID3D11ShaderResourceView* boundPS[16]{};
			ctx->PSGetShaderResources(0, 16, boundPS);
			for (std::uint32_t slot = 0; slot < 16; slot++) {
				auto* current = boundPS[slot];
				if (!current) {
					continue;
				}
				for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
					std::uint32_t idx = kScaledTargets[k];
					if (idx >= rendererData->renderTargets.size()) {
						continue;
					}
					auto* lowSRV = (hbaoMapped[idx] && hbaoTargets[idx]) ? hbaoTargets[idx]->srv : nullptr;
					auto* fullSRV = hbaoMapped[idx] ? hbaoBackedUp[idx].srView : nullptr;
					if (lowSRV && fullSRV && current == lowSRV) {
						ctx->PSSetShaderResources(slot, 1, &fullSRV);
						break;
					}
				}
				current->Release();
			}
		}
		if (rendererData) {
			for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
				std::uint32_t idx = kScaledTargets[k];
				if (idx >= rendererData->renderTargets.size()) {
					continue;
				}
				if (!hbaoMapped[idx]) {
					continue;
				}
				auto& live = rendererData->renderTargets[idx];
				live.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(hbaoBackedUp[idx].texture);
				live.copyTexture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(hbaoBackedUp[idx].copyTexture);
				live.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(hbaoBackedUp[idx].rtView);
				live.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(hbaoBackedUp[idx].srView);
				live.copySRView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(hbaoBackedUp[idx].copySRView);
				live.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(hbaoBackedUp[idx].uaView);
				hbaoMapped[idx] = false;
			}
			for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
				std::uint32_t idx = kScaledTargets[k];
				if (idx < kScaledTargetSpan) {
					hbaoBackedUp[idx] = HBAOSlot{};
				}
			}
		}
		if (hbaoMetaHeld) {
			for (std::uint32_t i = 0; i < kHBAOMetaCount; i++) {
				rtMgr.renderTargetDataArray[i] = hbaoSavedMeta[i];
			}
			hbaoMetaHeld = false;
		}
		if (hbaoDepthMapped) {
			if (rendererData && hbaoDepthBackedUp) {
				rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(hbaoDepthBackedUp);
			}
			if (hbaoDepthBackedUp) {
				hbaoDepthBackedUp->Release();
				hbaoDepthBackedUp = nullptr;
			}
			hbaoDepthMapped = false;
		}
		InvokeHBAODynRes(true);
		SetGlobalDynamicRatio(rtMgr, hbaoSavedWidthRatio, hbaoSavedHeightRatio);
		GetSetCurrentViewportDefault()(&rtMgr);
	}

	void Upscaling::ExitHBAO()
	{
		if (!hbaoActive) {
			return;
		}
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = GetImmediateContext();
		if (rendererData && ctx) {
			std::uint32_t idx = RenderTarget::kSSAOFinal;
			if (idx < kScaledTargetSpan && hbaoMapped[idx] && hbaoTargets[idx] && hbaoTargets[idx]->resource && hbaoBackedUp[idx].texture) {
				D3D11_TEXTURE2D_DESC lowDesc{};
				hbaoTargets[idx]->resource->GetDesc(&lowDesc);
				D3D11_BOX resultBox{};
				resultBox.left = 0;
				resultBox.top = 0;
				resultBox.front = 0;
				resultBox.right = lowDesc.Width;
				resultBox.bottom = lowDesc.Height;
				resultBox.back = 1;
				ctx->CopySubresourceRegion(hbaoBackedUp[idx].texture, 0, 0, 0, 0, hbaoTargets[idx]->resource, 0, &resultBox);
			}
		}
		RestoreHBAOState();
	}

	void Upscaling::RefreshHBAOCache()
	{
		if (!upsclEnabled || currentScale >= 0.999f) {
			ReleaseHBAOCache();
			return;
		}
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		float widthRatio = GetDynWidthRatio(rtMgr);
		float heightRatio = GetDynHeightRatio(rtMgr);
		if (!rendererData || widthRatio >= 0.999f || heightRatio >= 0.999f || widthRatio <= 0.001f || heightRatio <= 0.001f) {
			ReleaseHBAOCache();
			return;
		}
		if (hbaoCacheValid && widthRatio == hbaoCachedWidthRatio && heightRatio == hbaoCachedHeightRatio) {
			return;
		}
		ReleaseHBAOCache();
		auto* device = GetRenderer();
		if (!device) {
			return;
		}
		for (std::uint32_t k = 0; k < kScaledTargetCount; k++) {
			std::uint32_t idx = kScaledTargets[k];
			if (idx >= rendererData->renderTargets.size()) {
				continue;
			}
			auto* liveTex = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[idx].texture);
			if (!liveTex) {
				continue;
			}
			D3D11_TEXTURE2D_DESC liveDesc{};
			liveTex->GetDesc(&liveDesc);
			std::uint32_t lowWidth = static_cast<std::uint32_t>(static_cast<float>(liveDesc.Width) * widthRatio);
			std::uint32_t lowHeight = static_cast<std::uint32_t>(static_cast<float>(liveDesc.Height) * heightRatio);
			if (lowWidth < 1) {
				lowWidth = 1;
			}
			if (lowHeight < 1) {
				lowHeight = 1;
			}
			if (lowWidth >= liveDesc.Width && lowHeight >= liveDesc.Height) {
				continue;
			}
			auto slot = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC lowDesc = liveDesc;
			lowDesc.Width = lowWidth;
			lowDesc.Height = lowHeight;
			if (FAILED(device->CreateTexture2D(&lowDesc, nullptr, &slot->resource))) {
				REX::LogError("CreateTexture2D(hbaoTarget {}) failed", idx);
				ReleaseHBAOCache();
				return;
			}
			auto& live = rendererData->renderTargets[idx];
			if (live.rtView) {
				D3D11_RENDER_TARGET_VIEW_DESC rtDesc{};
				reinterpret_cast<ID3D11RenderTargetView*>(live.rtView)->GetDesc(&rtDesc);
				if (FAILED(device->CreateRenderTargetView(slot->resource, &rtDesc, &slot->rtv))) {
					REX::LogError("CreateRenderTargetView(hbaoTarget {}) failed", idx);
					ReleaseHBAOCache();
					return;
				}
			}
			if (live.srView) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				reinterpret_cast<ID3D11ShaderResourceView*>(live.srView)->GetDesc(&srvDesc);
				if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
					srvDesc.Texture2D.MostDetailedMip = 0;
					srvDesc.Texture2D.MipLevels = 1;
				}
				if (FAILED(device->CreateShaderResourceView(slot->resource, &srvDesc, &slot->srv))) {
					REX::LogError("CreateShaderResourceView(hbaoTarget {}) failed", idx);
					ReleaseHBAOCache();
					return;
				}
			}
			if (live.uaView) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				reinterpret_cast<ID3D11UnorderedAccessView*>(live.uaView)->GetDesc(&uavDesc);
				if (FAILED(device->CreateUnorderedAccessView(slot->resource, &uavDesc, &slot->uav))) {
					REX::LogError("CreateUnorderedAccessView(hbaoTarget {}) failed", idx);
					ReleaseHBAOCache();
					return;
				}
			}
			hbaoFullDesc[idx] = liveDesc;
			hbaoTargets[idx] = std::move(slot);
		}
		auto& liveDepth = rendererData->depthStencilTargets[DepthStencil::kMain];
		auto* depthTex = reinterpret_cast<ID3D11Texture2D*>(liveDepth.texture);
		if (!depthTex) {
			ReleaseHBAOCache();
			return;
		}
		D3D11_TEXTURE2D_DESC depthLiveDesc{};
		depthTex->GetDesc(&depthLiveDesc);
		std::uint32_t depthLowWidth = static_cast<std::uint32_t>(static_cast<float>(depthLiveDesc.Width) * widthRatio);
		std::uint32_t depthLowHeight = static_cast<std::uint32_t>(static_cast<float>(depthLiveDesc.Height) * heightRatio);
		if (depthLowWidth < 1) {
			depthLowWidth = 1;
		}
		if (depthLowHeight < 1) {
			depthLowHeight = 1;
		}
		auto depthSlot = std::make_unique<Texture2D>();
		D3D11_TEXTURE2D_DESC depthLowDesc{};
		depthLowDesc.Width = depthLowWidth;
		depthLowDesc.Height = depthLowHeight;
		depthLowDesc.MipLevels = 1;
		depthLowDesc.ArraySize = 1;
		depthLowDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthLowDesc.SampleDesc.Count = 1;
		depthLowDesc.Usage = D3D11_USAGE_DEFAULT;
		depthLowDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(device->CreateTexture2D(&depthLowDesc, nullptr, &depthSlot->resource))) {
			REX::LogError("CreateTexture2D(hbaoDepth) failed");
			ReleaseHBAOCache();
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
		depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		depthSrvDesc.Texture2D.MostDetailedMip = 0;
		depthSrvDesc.Texture2D.MipLevels = 1;
		if (FAILED(device->CreateShaderResourceView(depthSlot->resource, &depthSrvDesc, &depthSlot->srv))) {
			REX::LogError("CreateShaderResourceView(hbaoDepth) failed");
			ReleaseHBAOCache();
			return;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
		depthUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		depthUavDesc.Texture2D.MipSlice = 0;
		if (FAILED(device->CreateUnorderedAccessView(depthSlot->resource, &depthUavDesc, &depthSlot->uav))) {
			REX::LogError("CreateUnorderedAccessView(hbaoDepth) failed");
			ReleaseHBAOCache();
			return;
		}
		if (!hbaoDepthShader) {
			hbaoDepthShader = CreateComputeShaderFromBytecode(kDepthCopy, kDepthCopySize, device);
		}
		if (!hbaoDepthShader) {
			REX::LogError("CreateComputeShader(hbaoDepth) failed");
			ReleaseHBAOCache();
			return;
		}
		hbaoDepthFullDesc = depthLiveDesc;
		hbaoDepthDescribed = true;
		hbaoDepth = std::move(depthSlot);
		hbaoCachedWidthRatio = widthRatio;
		hbaoCachedHeightRatio = heightRatio;
		hbaoCacheValid = true;
	}

	void Upscaling::ReleaseHBAOCache()
	{
		RestoreHBAOState();
		if (hbaoDepthBackedUp) {
			hbaoDepthBackedUp->Release();
			hbaoDepthBackedUp = nullptr;
		}
		if (hbaoDepthShader) {
			hbaoDepthShader->Release();
			hbaoDepthShader = nullptr;
		}
		hbaoDepthMapped = false;
		for (std::uint32_t i = 0; i < kScaledTargetSpan; i++) {
			hbaoTargets[i].reset();
			hbaoBackedUp[i] = HBAOSlot{};
			hbaoMapped[i] = false;
			hbaoFullDesc[i] = D3D11_TEXTURE2D_DESC{};
		}
		hbaoDepth.reset();
		hbaoDepthFullDesc = D3D11_TEXTURE2D_DESC{};
		hbaoDepthDescribed = false;
		hbaoDepthFrame = 0;
		hbaoMetaHeld = false;
		hbaoCacheValid = false;
		hbaoCachedWidthRatio = 0.0f;
		hbaoCachedHeightRatio = 0.0f;
	}
}
