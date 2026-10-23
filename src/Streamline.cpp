#include "Streamline.hpp"
#include "PCH.hpp"
#include "Upscaling.hpp"

#include <cmath>
#include <sl_matrix_helpers.h>

#if F4R_HAS_STREAMLINE

namespace F4R_Upscaling
{
	Streamline& Streamline::GetSingleton()
	{
		static Streamline instance;
		return instance;
	}

	namespace
	{
		void SLLogCallback(sl::LogType a_type, const char* a_msg)
		{
			switch (a_type) {
				case sl::LogType::eError:
					REX::LogError("[SL]: {}", a_msg);
					break;
				case sl::LogType::eWarn:
					REX::LogWarning("[SL]: {}", a_msg);
					break;
				default:
					break;
			}
		}

		sl::float4x4 ToSLMatrix(const float* a_mat)
		{
			sl::float4x4 result;
			for (int i = 0; i < 4; ++i) {
				const float* row = a_mat + i * 4;
				result[i] = sl::float4(row[0], row[1], row[2], row[3]);
			}
			return result;
		}

		sl::float3 ToSLFloat3(const float* a_v)
		{
			return sl::float3(a_v[0], a_v[1], a_v[2]);
		}
	}

	void Streamline::LoadInterposer()
	{
		if (!interposer.Load(L"Data/F4SE/Plugins/Streamline/sl.interposer.dll")) {
			REX::LogCritical("Failed to load interposer: Error Code {0:x}", Platform::LastErrorCode());
		}
	}

	void Streamline::Initialize()
	{
		REX::LogInformation("Streamline: Initializing");

		if (!interposer.IsValid()) {
			REX::LogCritical("Streamline: initialize failed interposer not loaded");
			return;
		}

		slInit = interposer.Resolve<PFun_slInit*>("slInit");
		slIsFeatureSupported = interposer.Resolve<PFun_slIsFeatureSupported*>("slIsFeatureSupported");
		slIsFeatureLoaded = interposer.Resolve<PFun_slIsFeatureLoaded*>("slIsFeatureLoaded");
		slSetFeatureLoaded = interposer.Resolve<PFun_slSetFeatureLoaded*>("slSetFeatureLoaded");
		slEvaluateFeature = interposer.Resolve<PFun_slEvaluateFeature*>("slEvaluateFeature");
		slSetTagForFrame = interposer.Resolve<PFun_slSetTagForFrame*>("slSetTagForFrame");
		slGetFeatureRequirements = interposer.Resolve<PFun_slGetFeatureRequirements*>("slGetFeatureRequirements");
		slUpgradeInterface = interposer.Resolve<PFun_slUpgradeInterface*>("slUpgradeInterface");
		slSetConstants = interposer.Resolve<PFun_slSetConstants*>("slSetConstants");
		slGetFeatureFunction = interposer.Resolve<PFun_slGetFeatureFunction*>("slGetFeatureFunction");
		slGetNewFrameToken = interposer.Resolve<PFun_slGetNewFrameToken*>("slGetNewFrameToken");
		slSetD3DDevice = interposer.Resolve<PFun_slSetD3DDevice*>("slSetD3DDevice");

		if (!slInit) {
			REX::LogCritical("slInit not found in interposer");
			return;
		}

		static const std::array<sl::Feature, 3> kFeatures = { sl::kFeatureDLSS, sl::kFeaturePCL, sl::kFeatureReflex };

		sl::Preferences pref{};
		pref.showConsole = false;
		pref.logLevel = sl::LogLevel::eOff;
		pref.pathsToPlugins = nullptr;
		pref.numPathsToPlugins = 0;
		pref.pathToLogsAndData = nullptr;
		pref.allocateCallback = nullptr;
		pref.releaseCallback = nullptr;
		pref.logMessageCallback = &SLLogCallback;
		pref.flags = sl::PreferenceFlags::eUseManualHooking |
					 sl::PreferenceFlags::eDisableCLStateTracking |
					 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
		pref.featuresToLoad = kFeatures.data();
		pref.numFeaturesToLoad = static_cast<std::uint32_t>(kFeatures.size());
		pref.applicationId = 0;
		pref.engine = sl::EngineType::eCustom;
		pref.engineVersion = "1.0.0";
		pref.projectId = "4aa42191c0a946178e8a3318bc33ca24";
		pref.renderAPI = sl::RenderAPI::eD3D11;

		sl::Result result = slInit(pref, sl::kSDKVersion);
		if (result == sl::Result::eOk) {
			initialized = true;
			REX::LogInformation("Streamline: initialized");
		}
		else {
			REX::LogCritical("Streamline: failed to initialize (result={})", static_cast<int>(result));
		}
	}

	void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
	{
		REX::LogInformation("DLSS: Checking features");

		if (!a_adapter) {
			REX::LogWarning("CheckFeatures: adapter is null");
			return;
		}

		DXGI_ADAPTER_DESC desc{};
		a_adapter->GetDesc(&desc);

		sl::AdapterInfo adapterInfo{};
		adapterInfo.deviceLUID = reinterpret_cast<std::uint8_t*>(&desc.AdapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
		adapterInfo.vkPhysicalDevice = nullptr;

		featureReflex = false;
		nvidiaAdapter = desc.VendorId == 0x10DE;
		if (nvidiaAdapter && slSetFeatureLoaded) {
			sl::Result res = slSetFeatureLoaded(sl::kFeatureReflex, true);
			if (res != sl::Result::eOk) {
				REX::LogWarning("Could not request NVIDIA Reflex (result={})", static_cast<int>(res));
			}
		}

		featureDLSS = false;
		slIsFeatureLoaded(sl::kFeatureDLSS, featureDLSS);

		if (!featureDLSS) {
			REX::LogWarning("DLSS: feature not loaded");

			sl::FeatureRequirements req{};
			sl::Result res = slGetFeatureRequirements(sl::kFeatureDLSS, req);
			if (res != sl::Result::eOk) {
				REX::LogError("slGetFeatureRequirements failed (result={})", static_cast<int>(res));
			}
			return;
		}

		REX::LogInformation("DLSS: feature loaded");

		sl::Result res = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
		featureDLSS = (res == sl::Result::eOk);
		if (!featureDLSS) {
			REX::LogError("DLSS is not supported (result={})", static_cast<int>(res));
			return;
		}

		REX::LogInformation("DLSS: available");
	}

	void Streamline::PostDevice()
	{
		if (featureDLSS) {
			const bool dlssBound =
				slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(slDLSSSetOptions)) == sl::Result::eOk;
			if (!dlssBound) {
				REX::LogWarning("Streamline: slDLSSSetOptions not resolved, DLSS dispatch unavailable");
			}
		}

		if (nvidiaAdapter) {
			bool bound = slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(slReflexSetOptions)) == sl::Result::eOk;
			bound &= slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(slReflexSleep)) == sl::Result::eOk;
			featureReflex = bound && slReflexSetOptions && slReflexSleep;
			REX::LogInformation("Streamline: Reflex {}", featureReflex ? "initialized" : "not initialized");
		}
		else {
			REX::LogInformation("Streamline: Reflex is not available");
		}
	}

	void Streamline::UpdateConstants(float a_jitterX, float a_jitterY)
	{
		sl::Constants constants{};

		auto& state = RE::BSGraphics::State::GetSingleton();
		const auto& camView = state.cameraState.camViewData;

		sl::float4x4 viewMatrix = ToSLMatrix(reinterpret_cast<const float*>(camView.viewMat.data()));
		sl::float4x4 invView;
		sl::matrixFullInvert(invView, viewMatrix);
		sl::float4x4 vpUnjittered = ToSLMatrix(reinterpret_cast<const float*>(camView.viewProjUnjittered.data()));
		sl::matrixMul(constants.cameraViewToClip, invView, vpUnjittered);
		sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

		sl::float4x4 currentVP = ToSLMatrix(reinterpret_cast<const float*>(camView.currentViewProjUnjittered.data()));
		sl::float4x4 previousVP = ToSLMatrix(reinterpret_cast<const float*>(camView.previousViewProjUnjittered.data()));
		sl::float4x4 invCurrentVP;
		sl::matrixFullInvert(invCurrentVP, currentVP);
		sl::matrixMul(constants.clipToPrevClip, invCurrentVP, previousVP);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

		const auto& camState = state.cameraState;
		constants.cameraPos = sl::float3(camState.posAdjust.x, camState.posAdjust.y, camState.posAdjust.z);
		constants.cameraUp = ToSLFloat3(reinterpret_cast<const float*>(&camView.viewUp));
		constants.cameraRight = ToSLFloat3(reinterpret_cast<const float*>(&camView.viewRight));
		constants.cameraFwd = ToSLFloat3(reinterpret_cast<const float*>(&camView.viewDir));

		float cameraNear = 0.0f;
		float cameraFar = 1.0f;
		GetCameraNearFar(cameraNear, cameraFar);
		constants.cameraNear = cameraNear;
		constants.cameraFar = cameraFar;
		constants.cameraAspectRatio = (state.screenHeight != 0) ? static_cast<float>(state.screenWidth) / static_cast<float>(state.screenHeight) : 1.0f;
		constants.cameraFOV = 2.0f * std::atan(1.0f / constants.cameraViewToClip[1].y);

		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.cameraPinholeOffset = { 0.0f, 0.0f };
		constants.depthInverted = sl::Boolean::eFalse;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.reset = Upscaling::GetSingleton().resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		constants.jitterOffset = { -a_jitterX, -a_jitterY };
		constants.mvecScale = { 1.0f, 1.0f };
		constants.motionVectorsJittered = sl::Boolean::eFalse;

		if (!AcquireFrameToken()) {
			static bool tokenLogged = false;
			if (!tokenLogged) {
				tokenLogged = true;
				REX::LogError("Could not get frame token");
			}
			return;
		}

		sl::Result res = slSetConstants(constants, *frameToken, viewport);
		if (res != sl::Result::eOk) {
			static bool constantsLogged = false;
			if (!constantsLogged) {
				constantsLogged = true;
				REX::LogError("Could not set constants (result={})", static_cast<int>(res));
			}
		}
	}

	bool Streamline::AcquireFrameToken()
	{
		if (!slGetNewFrameToken) {
			return false;
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		if (frameToken && frameTokenFrame == state.frameCount) {
			return true;
		}

		sl::Result res = slGetNewFrameToken(frameToken, nullptr);
		if (res != sl::Result::eOk) {
			frameToken = nullptr;
			frameTokenFrame = UINT64_MAX;
			return false;
		}

		frameTokenFrame = state.frameCount;
		return true;
	}

	void Streamline::UpdateLatency()
	{
		if (!initialized || !featureReflex || !slReflexSetOptions || !slReflexSleep) {
			return;
		}

		auto& aa = Upscaling::GetSingleton();

		sl::ReflexMode mode = sl::ReflexMode::eOff;
		std::uint32_t frameLimitUs = 0;

		if (aa.settings.enableReflex && aa.upsclEnabled) {
			mode = aa.settings.reflexBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
			if (aa.settings.reflexUseFPSLimit) {
				frameLimitUs = static_cast<std::uint32_t>(1000000.0f / aa.settings.reflexFPSLimit + 0.5f);
			}
		}

		if (!reflexOptionsValid || reflexMode != mode || reflexFrameLimitUs != frameLimitUs) {
			sl::ReflexOptions options{};
			options.mode = mode;
			options.frameLimitUs = frameLimitUs;
			if (slReflexSetOptions(options) != sl::Result::eOk) {
				REX::LogError("Could not set Reflex options");
				return;
			}
			reflexOptionsValid = true;
			reflexMode = mode;
			reflexFrameLimitUs = frameLimitUs;
		}

		if (mode == sl::ReflexMode::eOff && frameLimitUs == 0) {
			return;
		}

		auto& state = RE::BSGraphics::State::GetSingleton();
		if (lastReflexFrame == state.frameCount) {
			return;
		}
		lastReflexFrame = state.frameCount;

		if (AcquireFrameToken()) {
			sl::Result res = slReflexSleep(*frameToken);
			if (res != sl::Result::eOk) {
				REX::LogWarning("Could not submit Reflex sleep (result={})", static_cast<int>(res));
			}
		}
	}

	void Streamline::Evaluate(
		ID3D11Resource* a_colorResource,
		ID3D11ShaderResourceView* a_colorSRV,
		ID3D11Resource* a_colorOutResource,
		ID3D11Resource* a_motionVectorsResource,
		float a_jitterX,
		float a_jitterY,
		std::uint32_t a_renderWidth,
		std::uint32_t a_renderHeight,
		QualityMode a_qualityMode)
	{
		if (!AcquireFrameToken()) {
			static bool tokenLogged = false;
			if (!tokenLogged) {
				tokenLogged = true;
				REX::LogError("Could not get frame token");
			}
			return;
		}

		UpdateConstants(a_jitterX, a_jitterY);

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		if (!ctx) {
			return;
		}

		sl::DLSSMode mode = sl::DLSSMode::eDLAA;
		if (a_qualityMode == QualityMode::Quality) {
			mode = sl::DLSSMode::eMaxQuality;
		}
		else if (a_qualityMode == QualityMode::Balanced) {
			mode = sl::DLSSMode::eBalanced;
		}
		else if (a_qualityMode == QualityMode::Performance) {
			mode = sl::DLSSMode::eMaxPerformance;
		}

		bool isHDR = false;
		if (a_colorSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			a_colorSRV->GetDesc(&srvDesc);
			isHDR = srvDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
					srvDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
					srvDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		sl::DLSSOptions options{};
		options.mode = mode;
		options.outputWidth = state.screenWidth;
		options.outputHeight = state.screenHeight;
		options.colorBuffersHDR = isHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;

		static constexpr std::array<sl::DLSSPreset, 4> kDLSSPresetMap{
			sl::DLSSPreset::ePresetJ,
			sl::DLSSPreset::ePresetK,
			sl::DLSSPreset::ePresetL,
			sl::DLSSPreset::ePresetM
		};

		const sl::DLSSPreset preset = kDLSSPresetMap.at(
			static_cast<std::size_t>(Upscaling::GetSingleton().settings.dlssPreset));

		options.dlaaPreset = options.qualityPreset = options.balancedPreset =
			options.performancePreset = options.ultraPerformancePreset =
			options.ultraQualityPreset = preset;

		sl::Result res = slDLSSSetOptions(viewport, options);
		if (res != sl::Result::eOk) {
			static bool optionsLogged = false;
			if (!optionsLogged) {
				optionsLogged = true;
				REX::LogCritical("Could not enable DLSS (result={})", static_cast<int>(res));
			}
		}

		sl::Resource colorIn(sl::ResourceType::eTex2d, a_colorResource);
		sl::Resource colorOut(sl::ResourceType::eTex2d, a_colorOutResource ? a_colorOutResource : a_colorResource);
		sl::Resource depth(sl::ResourceType::eTex2d, reinterpret_cast<void*>(rendererData->depthStencilTargets[2].texture));
		sl::Resource motionVectors(sl::ResourceType::eTex2d, a_motionVectorsResource);

		sl::Extent lowExtent{ 0, 0, a_renderWidth, a_renderHeight };
		sl::Extent fullExtent{ 0, 0, state.screenWidth, state.screenHeight };
		const bool isQuality = (mode != sl::DLSSMode::eDLAA);
		const sl::Extent& inputExtent = isQuality ? lowExtent : fullExtent;
		const sl::Extent& outputExtent = fullExtent;
		const sl::Extent& vectorExtent = isQuality ? lowExtent : fullExtent;

		std::array<sl::ResourceTag, 4> tags = {
			sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::eOnlyValidNow, &inputExtent),
			sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::eOnlyValidNow, &outputExtent),
			sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &vectorExtent),
			sl::ResourceTag(&motionVectors, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &vectorExtent),
		};

		if (!slSetTagForFrame) {
			REX::LogError("slSetTagForFrame not found in interposer");
			return;
		}

		res = slSetTagForFrame(*frameToken, viewport, tags.data(), static_cast<std::uint32_t>(tags.size()), ctx);
		if (res != sl::Result::eOk) {
			static bool tagLogged = false;
			if (!tagLogged) {
				tagLogged = true;
				REX::LogError("slSetTagForFrame failed (result={})", static_cast<int>(res));
			}
		}

		std::array<const sl::BaseStructure*, 1> inputs = { &viewport };
		res = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs.data(), static_cast<std::uint32_t>(inputs.size()), ctx);
		if (res != sl::Result::eOk) {
			static bool evalLogged = false;
			if (!evalLogged) {
				evalLogged = true;
				REX::LogError("slEvaluateFeature failed (result={})", static_cast<int>(res));
			}
		}

		ID3D11Buffer* nullCB = nullptr;
		std::array<ID3D11UnorderedAccessView*, 8> nullUAVs{};
		std::array<ID3D11ShaderResourceView*, 16> nullSRVs{};
		ctx->CSSetConstantBuffers(0, 1, &nullCB);
		ctx->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr);
		ctx->CSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
		ctx->CSSetShader(nullptr, nullptr, 0);
	}
}
#endif
