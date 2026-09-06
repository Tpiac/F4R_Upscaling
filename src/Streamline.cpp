#include "PCH.hpp"
#include "Streamline.hpp"
#include "Upscaling.hpp"

#include <psapi.h>
#include <xmmintrin.h>
#include <cmath>
#include <sl_matrix_helpers.h>

#if F4R_HAS_DLSS

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
				REX::LogDebug("[SL]: {}", a_msg);
				break;
			}
		}

		sl::float4x4 ToSLMatrix(const __m128* a_mat)
		{
			sl::float4x4 result;
			for (int i = 0; i < 4; ++i) {
				alignas(16) float row[4];
				_mm_store_ps(row, a_mat[i]);
				result[i] = sl::float4(row[0], row[1], row[2], row[3]);
			}
			return result;
		}

		sl::float3 ToSLFloat3(const __m128* a_v)
		{
			alignas(16) float vals[4];
			_mm_store_ps(vals, *a_v);
			return sl::float3(vals[0], vals[1], vals[2]);
		}
	}

	void Streamline::LoadInterposer()
	{
		interposer = LoadLibraryW(L"Data/F4SE/Plugins/Streamline/sl.interposer.dll");
		if (!interposer) {
			REX::LogCritical("Failed to load interposer: Error Code {0:x}", GetLastError());
		}
	}

	void Streamline::Initialize()
	{
		REX::LogInformation("Streamline: Initializing");

		if (!interposer) {
			REX::LogCritical("Streamline: initialize failed interposer not loaded");
			return;
		}

		auto resolve = [&](const char* name) -> void* {
			return GetProcAddress(interposer, name);
		};

		slInit = reinterpret_cast<PFun_slInit*>(resolve("slInit"));
		slShutdown = reinterpret_cast<PFun_slShutdown*>(resolve("slShutdown"));
		slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(resolve("slIsFeatureSupported"));
		slIsFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(resolve("slIsFeatureLoaded"));
		slSetFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(resolve("slSetFeatureLoaded"));
		slEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature*>(resolve("slEvaluateFeature"));
		slAllocateResources = reinterpret_cast<PFun_slAllocateResources*>(resolve("slAllocateResources"));
		slFreeResources = reinterpret_cast<PFun_slFreeResources*>(resolve("slFreeResources"));
		slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(resolve("slSetTagForFrame"));
		slGetFeatureRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(resolve("slGetFeatureRequirements"));
		slGetFeatureVersion = reinterpret_cast<PFun_slGetFeatureVersion*>(resolve("slGetFeatureVersion"));
		slUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(resolve("slUpgradeInterface"));
		slSetConstants = reinterpret_cast<PFun_slSetConstants*>(resolve("slSetConstants"));
		slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface*>(resolve("slGetNativeInterface"));
		slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(resolve("slGetFeatureFunction"));
		slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(resolve("slGetNewFrameToken"));
		slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(resolve("slSetD3DDevice"));

		if (!slInit) {
			REX::LogCritical("slInit not found in interposer");
			return;
		}

		static const sl::Feature kFeatures[] = { sl::kFeatureDLSS, sl::kFeaturePCL, sl::kFeatureReflex };

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
		pref.featuresToLoad = kFeatures;
		pref.numFeaturesToLoad = sizeof(kFeatures) / sizeof(kFeatures[0]);
		pref.applicationId = 0;
		pref.engine = sl::EngineType::eCustom;
		pref.engineVersion = "1.0.0";
		pref.projectId = "4aa42191c0a946178e8a3318bc33ca24";
		pref.renderAPI = sl::RenderAPI::eD3D11;

		sl::Result result = slInit(pref, sl::kSDKVersion);
		if (result == sl::Result::eOk) {
			initialized = true;
			REX::LogInformation("Streamline: initialized");
		} else {
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
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
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
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(slDLSSGetOptimalSettings));
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", reinterpret_cast<void*&>(slDLSSGetState));
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(slDLSSSetOptions));
		}

		if (nvidiaAdapter) {
			bool bound = slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(slReflexSetOptions)) == sl::Result::eOk;
			bound &= slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(slReflexSleep)) == sl::Result::eOk;
			featureReflex = bound && slReflexSetOptions && slReflexSleep;
			REX::LogInformation("Streamline: Reflex {}", featureReflex ? "initialized" : "not initialized");
		}
	}

	void Streamline::DestroyDLSSResources()
	{
		if (!featureDLSS || !slDLSSSetOptions || !slFreeResources)
			return;

		sl::DLSSOptions options{};
		options.mode = static_cast<sl::DLSSMode>(0xFFFFFFFFu);
		slDLSSSetOptions(viewport, options);
		slFreeResources(sl::kFeatureDLSS, viewport);
	}

	void Streamline::UpdateConstants(float a_jitterX, float a_jitterY)
	{
		sl::Constants constants{};

		auto& state = RE::BSGraphics::State::GetSingleton();
		const auto& camView = state.cameraState.camViewData;

		sl::float4x4 viewMatrix = ToSLMatrix(camView.viewMat.data());
		sl::float4x4 invView;
		sl::matrixFullInvert(invView, viewMatrix);
		sl::float4x4 vpUnjittered = ToSLMatrix(camView.viewProjUnjittered.data());
		sl::matrixMul(constants.cameraViewToClip, invView, vpUnjittered);
		sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

		sl::float4x4 currentVP = ToSLMatrix(camView.currentViewProjUnjittered.data());
		sl::float4x4 previousVP = ToSLMatrix(camView.previousViewProjUnjittered.data());
		sl::float4x4 invCurrentVP;
		sl::matrixFullInvert(invCurrentVP, currentVP);
		sl::matrixMul(constants.clipToPrevClip, invCurrentVP, previousVP);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

		const auto& camState = state.cameraState;
		constants.cameraPos = sl::float3(camState.posAdjust.x, camState.posAdjust.y, camState.posAdjust.z);
		constants.cameraUp = ToSLFloat3(&camView.viewUp);
		constants.cameraRight = ToSLFloat3(&camView.viewRight);
		constants.cameraFwd = ToSLFloat3(&camView.viewDir);

		float cameraNear = 0.0f;
		float cameraFar = 1.0f;
		GetCameraNearFar(cameraNear, cameraFar);
		constants.cameraNear = cameraNear;
		constants.cameraFar = cameraFar;
		constants.cameraAspectRatio = (state.screenHeight != 0)
			? static_cast<float>(state.screenWidth) / static_cast<float>(state.screenHeight)
			: 1.0f;
		constants.cameraFOV = 2.0f * std::atan(1.0f / constants.cameraViewToClip[1].y);

		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.cameraPinholeOffset = { 0.0f, 0.0f };
		constants.depthInverted = sl::Boolean::eFalse;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.reset = Upscaling::GetSingleton().resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		constants.jitterOffset = { -a_jitterX, -a_jitterY };
		constants.mvecScale = { 1.0f, 1.0f };
		constants.motionVectorsInvalidValue = 1.1754944e-38f;
		constants.orthographicProjection = sl::Boolean::eFalse;
		constants.motionVectorsDilated = sl::Boolean::eFalse;
		constants.motionVectorsJittered = sl::Boolean::eFalse;

		if (!AcquireFrameToken()) {
			REX::LogError("Could not get frame token");
			return;
		}

		sl::Result res = slSetConstants(constants, *frameToken, viewport);
		if (res != sl::Result::eOk) {
			REX::LogError("Could not set constants (result={})", static_cast<int>(res));
		}
	}

	bool Streamline::AcquireFrameToken()
	{
		if (!slGetNewFrameToken)
			return false;

		auto& state = RE::BSGraphics::State::GetSingleton();
		if (frameToken && frameTokenFrame == state.frameCount)
			return true;

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
		if (!initialized || !featureReflex || !slReflexSetOptions || !slReflexSleep)
			return;

		auto& aa = Upscaling::GetSingleton();

		sl::ReflexMode mode = sl::ReflexMode::eOff;
		uint32_t frameLimitUs = 0;

		if (aa.settings.bEnableReflex && aa.upsclEnabled) {
			mode = aa.settings.bReflexBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
			if (aa.settings.bReflexUseFPSLimit)
				frameLimitUs = static_cast<uint32_t>(1000000.0f / aa.settings.fReflexFPSLimit + 0.5f);
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

		if (mode == sl::ReflexMode::eOff && frameLimitUs == 0)
			return;

		auto& state = RE::BSGraphics::State::GetSingleton();
		if (lastReflexFrame == state.frameCount)
			return;
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
		ID3D11Resource* a_motionVectorsResource,
		float a_jitterX,
		float a_jitterY,
		uint32_t a_renderWidth,
		uint32_t a_renderHeight,
		uint32_t a_qualityMode)
	{
		if (!AcquireFrameToken()) {
			REX::LogError("Could not get frame token");
			return;
		}

		UpdateConstants(a_jitterX, a_jitterY);

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		if (!ctx) return;

		sl::DLSSMode mode = sl::DLSSMode::eDLAA;
		if (a_qualityMode == 1) mode = sl::DLSSMode::eMaxQuality;
		else if (a_qualityMode == 2) mode = sl::DLSSMode::eBalanced;
		else if (a_qualityMode == 3) mode = sl::DLSSMode::eMaxPerformance;

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
		options.dlaaPreset = sl::DLSSPreset::ePresetK;
		options.qualityPreset = sl::DLSSPreset::ePresetK;
		options.balancedPreset = sl::DLSSPreset::ePresetK;
		options.performancePreset = sl::DLSSPreset::ePresetK;
		options.ultraPerformancePreset = sl::DLSSPreset::ePresetK;
		options.ultraQualityPreset = sl::DLSSPreset::ePresetK;

		sl::Result res = slDLSSSetOptions(viewport, options);
		if (res != sl::Result::eOk) {
			REX::LogCritical("Could not enable DLSS (result={})", static_cast<int>(res));
		}

		sl::Resource colorIn(sl::ResourceType::eTex2d, a_colorResource);
		sl::Resource colorOut(sl::ResourceType::eTex2d, a_colorResource);
		sl::Resource depth(sl::ResourceType::eTex2d, reinterpret_cast<void*>(rendererData->depthStencilTargets[2].texture));
		sl::Resource motionVectors(sl::ResourceType::eTex2d, a_motionVectorsResource);

		sl::Extent lowExtent{ 0, 0, a_renderWidth, a_renderHeight };
		sl::Extent fullExtent{ 0, 0, state.screenWidth, state.screenHeight };
		const bool isQuality = (mode != sl::DLSSMode::eDLAA);
		const sl::Extent& inputExtent = isQuality ? lowExtent : fullExtent;
		const sl::Extent& outputExtent = fullExtent;
		const sl::Extent& vectorExtent = isQuality ? lowExtent : fullExtent;

		sl::ResourceTag tags[] = {
			sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::eOnlyValidNow, &inputExtent),
			sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::eOnlyValidNow, &outputExtent),
			sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &vectorExtent),
			sl::ResourceTag(&motionVectors, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &vectorExtent),
		};

		if (!slSetTagForFrame) {
			REX::LogError("slSetTagForFrame not found in interposer");
			return;
		}

		res = slSetTagForFrame(*frameToken, viewport, tags, 4, ctx);
		if (res != sl::Result::eOk) {
			REX::LogError("slSetTagForFrame failed (result={})", static_cast<int>(res));
		}

		const sl::BaseStructure* inputs[] = { &viewport };
		res = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, 1, ctx);
		if (res != sl::Result::eOk) {
			REX::LogError("slEvaluateFeature failed (result={})", static_cast<int>(res));
		}

		ID3D11Buffer* nullCB = nullptr;
		ID3D11UnorderedAccessView* nullUAVs[8] = {};
		ID3D11ShaderResourceView* nullSRVs[16] = {};
		ctx->CSSetConstantBuffers(0, 1, &nullCB);
		ctx->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ctx->CSSetShaderResources(0, 16, nullSRVs);
		ctx->CSSetShader(nullptr, nullptr, 0);
	}
}
#endif
