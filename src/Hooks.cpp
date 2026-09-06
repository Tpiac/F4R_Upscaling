#include "PCH.hpp"
#include "Upscaling.hpp"
#include <Detours.h>
#include <unordered_map>

namespace
{
	using namespace RE;
	using namespace RE::BSGraphics;
	using namespace F4R_Upscaling;

	inline std::uintptr_t GetAEAddr(std::uintptr_t a_aeID, std::ptrdiff_t a_subOffset)
	{
		return REL::Relocation{ REL::Id<>{ a_aeID } }.GetAddress() + a_subOffset;
	}

	inline std::uintptr_t GetOGAddr(std::uintptr_t a_ogRVA)
	{
		return REL::Module::GetSingleton()->GetBaseAddress() + a_ogRVA;
	}

	inline std::uintptr_t ResolveAddr(std::uintptr_t a_ogRVA, std::uintptr_t a_aeID, std::ptrdiff_t a_aeSub)
	{
		if (IsAE() || IsNG())
			return GetAEAddr(a_aeID, a_aeSub);
		return GetOGAddr(a_ogRVA);
	}

#if F4R_HAS_FSR3
	inline bool IsFSR3Method()
	{
		return Upscaling::GetSingleton().settings.iMethod == static_cast<int32_t>(Method::FSR3);
	}
#endif

	inline void LogHookResult(const char* a_name, std::uintptr_t a_original)
	{
		if (a_original)
			REX::LogDebug("{} installed", a_name);
		else
			REX::LogWarning("{} FAILED", a_name);
	}

	using TemporalAA_IsActiveFunc = bool(ImageSpaceEffectTemporalAA*);
	TemporalAA_IsActiveFunc* g_originalTemporalAA_IsActive = nullptr;

	bool Hook_TemporalAA_IsActive(ImageSpaceEffectTemporalAA* a_this)
	{
		auto& up = Upscaling::GetSingleton();
		if (up.upsclEnabled) {
			return false;
		}
		return g_originalTemporalAA_IsActive(a_this);
	}

	using PreRender_UpdateUpscStateFunc = void(RenderTargetManager*, void*, void*, void*, void*);
	PreRender_UpdateUpscStateFunc* g_originalPreRender_UpdateUpscState = nullptr;

	void Hook_PreRender_UpdateUpscState(RenderTargetManager* a_this, void* a_p2, void* a_p3, void* a_p4, void* a_p5)
	{
		g_originalPreRender_UpdateUpscState(a_this, a_p2, a_p3, a_p4, a_p5);
		Upscaling::GetSingleton().Update();
	}

	using PostRender_UpscAndPreparePostFXFunc = void(RenderTargetManager*, bool);
	PostRender_UpscAndPreparePostFXFunc* g_originalPostRender_UpscAndPreparePostFX = nullptr;

	void Hook_PostRender_UpscAndPreparePostFX(RenderTargetManager* a_this, bool a_p2)
	{
		g_originalPostRender_UpscAndPreparePostFX(a_this, a_p2);

		auto& up = Upscaling::GetSingleton();
		up.Apply();

		if (up.upsclEnabled && up.currentScale < 0.999f) {
			up.BuildFlareDepth(*a_this);
		}

		up.savedWidthRatio = GetDynWidthRatio(*a_this);
		up.savedHeightRatio = GetDynHeightRatio(*a_this);
		GetDynWidthRatio(*a_this) = 1.0f;
		GetDynHeightRatio(*a_this) = 1.0f;
		GetDynResActivated(*a_this) = false;
	}

	using SamplerState_OverrideMipBiasFunc = void(void*);
	SamplerState_OverrideMipBiasFunc* g_originalSamplerState_OverrideMipBias = nullptr;

	void Hook_SamplerState_OverrideMipBias(void* a_this)
	{
		Upscaling::GetSingleton().OverrideSamplerStates();
		g_originalSamplerState_OverrideMipBias(a_this);
		Upscaling::GetSingleton().ResetSamplerStates();
	}

	using SamplerState_RestoreMipBiasFunc = void(void*);
	SamplerState_RestoreMipBiasFunc* g_originalSamplerState_RestoreMipBias = nullptr;

	void Hook_SamplerState_RestoreMipBias(void* a_this)
	{
		Upscaling::GetSingleton().OverrideSamplerStates();
		g_originalSamplerState_RestoreMipBias(a_this);
		Upscaling::GetSingleton().ResetSamplerStates();

#if F4R_HAS_FSR3
		auto& up = Upscaling::GetSingleton();
		if (IsFSR3Method() && up.upsclEnabled && up.fidelityFX) {
			up.fidelityFX->GenerateReactiveMask();
		}
#endif
	}

#if F4R_HAS_FSR3
	using CaptureOpaque_ForReactiveMaskFunc = void(BSShaderAccumulator*);
	CaptureOpaque_ForReactiveMaskFunc* g_originalCaptureOpaque_ForReactiveMask = nullptr;

	void Hook_CaptureOpaque_ForReactiveMask(BSShaderAccumulator* a_this)
	{
		g_originalCaptureOpaque_ForReactiveMask(a_this);

		if (!IsFSR3Method()) return;

		auto& up = Upscaling::GetSingleton();
		if (!up.upsclEnabled || !up.fidelityFX ||
			!up.fidelityFX->colorOpaqueOnlyTexture ||
			!up.fidelityFX->colorOpaqueOnlyTexture->resource)
			return;

		auto* rendererData = BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto* rt4 = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[F4R_Upscaling::RenderTarget::kMainTemp].texture);
		if (!rt4) return;

		ctx->CopyResource(up.fidelityFX->colorOpaqueOnlyTexture->resource, rt4);
	}
#endif

	using Effects_PreserveJitterFunc = void(RenderTargetManager*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
	Effects_PreserveJitterFunc* g_originalEffects_PreserveJitter = nullptr;

	void Hook_Effects_PreserveJitter(RenderTargetManager* a_this, std::uint32_t a_p2, std::uint32_t a_p3, std::uint32_t a_p4, std::uint32_t a_p5)
	{
		auto& state = State::GetSingleton();
		float savedX = state.offsetX;
		float savedY = state.offsetY;

		g_originalEffects_PreserveJitter(a_this, a_p2, a_p3, a_p4, a_p5);

		state.offsetX = savedX;
		state.offsetY = savedY;
	}

	using Loading_ResetDynamicResolutionFunc = void();
	Loading_ResetDynamicResolutionFunc* g_originalLoading_ResetDynamicResolution = nullptr;

	void Hook_Loading_ResetDynamicResolution()
	{
		g_originalLoading_ResetDynamicResolution();

		auto& rtMgr = RenderTargetManager::GetSingleton();
		GetDynWidthRatio(rtMgr) = 1.0f;
		GetDynHeightRatio(rtMgr) = 1.0f;
		GetDynResActivated(rtMgr) = false;
		auto& up = Upscaling::GetSingleton();
		up.InvalidateFlareDepth();
		up.RequestReset();
	}

	using ImageSpace_RestoreDynamicResolutionFunc = void(void*);
	ImageSpace_RestoreDynamicResolutionFunc* g_originalImageSpace_RestoreDynamicResolution = nullptr;

	void Hook_ImageSpace_RestoreDynamicResolution(void* a_this)
	{
		g_originalImageSpace_RestoreDynamicResolution(a_this);

		auto& rtMgr = RenderTargetManager::GetSingleton();
		auto& up = Upscaling::GetSingleton();
		GetDynWidthRatio(rtMgr) = up.savedWidthRatio;
		GetDynHeightRatio(rtMgr) = up.savedHeightRatio;
		GetDynResActivated(rtMgr) =
			(up.savedWidthRatio != 1.0f || up.savedHeightRatio != 1.0f);
	}

	

	using LensFlare_ForceFullResDepthFunc = void(RE::NiCamera*);
	LensFlare_ForceFullResDepthFunc* g_originalLensFlare_ForceFullResDepth = nullptr;

void Hook_LensFlare_ForceFullResDepth(RE::NiCamera* a_camera)
{
	auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
	auto& up = Upscaling::GetSingleton();
	up.PopFlareDepth();
	const bool dynLow = (GetDynWidthRatio(rtMgr) != 1.0f || GetDynHeightRatio(rtMgr) != 1.0f);
	const bool need = dynLow && up.upsclEnabled && up.currentScale < 0.999f &&
		up.flareValid && up.flareDepthTexture && up.flareDepthTexture->srv;
	if (need) {
		up.PushFlareDepth();
	}
	g_originalLensFlare_ForceFullResDepth(a_camera);
	up.PopFlareDepth();
}

	using SSLRRaytracing_SkipInQualityModesFunc = void(RE::BSShader*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
	SSLRRaytracing_SkipInQualityModesFunc* g_originalSSLRRaytracing_SkipInQualityModes = nullptr;

	void Hook_SSLRRaytracing_SkipInQualityModes(RE::BSShader* a_this, std::uint32_t a_p2, std::uint32_t a_p3, std::uint32_t a_p4, std::uint32_t a_p5)
	{
		if (g_originalSSLRRaytracing_SkipInQualityModes) {
			g_originalSSLRRaytracing_SkipInQualityModes(a_this, a_p2, a_p3, a_p4, a_p5);
		}
		auto& up = Upscaling::GetSingleton();
		if (up.upsclEnabled && up.currentScale < 0.999f && up.settings.iQualityMode >= 1 && !g_enbLoaded) {
			auto* rendererData = BSGraphics::RendererData::GetSingleton();
			auto* ctx = GetImmediateContext();
			if (rendererData && ctx) {
				static const float kClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				static const std::uint32_t kSSRTargets[] = {
					F4R_Upscaling::RenderTarget::kSSRRaw,
					F4R_Upscaling::RenderTarget::kSSRBlurred,
					F4R_Upscaling::RenderTarget::kSSRBlurredExtra,
					F4R_Upscaling::RenderTarget::kSSRDirection,
					F4R_Upscaling::RenderTarget::kSSRMask,
				};
				for (auto idx : kSSRTargets) {
					if (idx >= rendererData->renderTargets.size()) {
						continue;
					}
					auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rendererData->renderTargets[idx].rtView);
					if (rtv) {
						ctx->ClearRenderTargetView(rtv, kClear);
					}
				}
			}
		}
	}

	std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_samplerCache;
	constexpr std::size_t kSamplerCacheLimit = 512;

	ID3D11SamplerState* EnsureCappedSampler(ID3D11SamplerState* a_original)
	{
		if (!a_original) return nullptr;
		auto it = g_samplerCache.find(a_original);
		if (it != g_samplerCache.end()) return it->second;

		if (g_samplerCache.size() >= kSamplerCacheLimit) {
			for (const auto& [key, value] : g_samplerCache) {
				if (value && value != key) {
					value->Release();
				}
			}
			g_samplerCache.clear();
		}

		D3D11_SAMPLER_DESC desc;
		a_original->GetDesc(&desc);
		if (desc.Filter == D3D11_FILTER_ANISOTROPIC && desc.MaxAnisotropy > 8) {
			desc.MaxAnisotropy = 8;
			auto* device = GetRenderer();
			ID3D11SamplerState* capped = nullptr;
			if (device && SUCCEEDED(device->CreateSamplerState(&desc, &capped))) {
				g_samplerCache[a_original] = capped;
				return capped;
			}
		}
		g_samplerCache[a_original] = a_original;
		return a_original;
	}

	using PSSetSamplersFunc = void(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
	PSSetSamplersFunc* g_originalPSSetSamplers = nullptr;
	bool g_contextHooksInstalled = false;

	void Hook_PSSetSamplers(ID3D11DeviceContext* a_ctx, UINT a_startSlot, UINT a_numSamplers, ID3D11SamplerState* const* a_samplers)
	{
		ID3D11SamplerState* capped[16] = {};
		bool changed = false;
		for (UINT i = 0; i < a_numSamplers && i < 16; i++) {
			capped[i] = EnsureCappedSampler(a_samplers[i]);
			if (capped[i] != a_samplers[i]) changed = true;
		}
		g_originalPSSetSamplers(a_ctx, a_startSlot, a_numSamplers, changed ? capped : a_samplers);
	}
}

namespace F4R_Upscaling
{
	void InstallContextHooks()
	{
		if (g_contextHooksInstalled) return;
		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto vtableAddr = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(ctx));
		auto result = Detours::X64::DetourVTable(vtableAddr,
			reinterpret_cast<uintptr_t>(&Hook_PSSetSamplers), 13);
		g_originalPSSetSamplers = reinterpret_cast<PSSetSamplersFunc*>(result);
		g_contextHooksInstalled = true;
		REX::LogDebug("PSSetSamplers hook installed");
	}

	void Upscaling::InstallHooks()
	{
		REX::LogDebug("Installing hooks...");

		{
			std::uintptr_t vtableAddr;
			if (IsAE() || IsNG()) {
				vtableAddr = REL::Relocation{ REL::Id<>{ 0x1473CC } }.GetAddress();
			} else {
				vtableAddr = REL::Relocation<std::uintptr_t>{ RE::VTABLE::ImageSpaceEffectTemporalAA[0] }.GetAddress();
			}
			auto result = Detours::X64::DetourVTable(vtableAddr, reinterpret_cast<std::uintptr_t>(&Hook_TemporalAA_IsActive), 8);
			g_originalTemporalAA_IsActive = reinterpret_cast<TemporalAA_IsActiveFunc*>(result);
			LogHookResult("TemporalAA_IsActive", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x14b, 0x235FF1, 0x29F);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PreRender_UpdateUpscState));
			g_originalPreRender_UpdateUpscState = reinterpret_cast<PreRender_UpdateUpscStateFunc*>(result);
			LogHookResult("PreRender_UpdateUpscState", result);
		}

		{
			auto addr = ResolveAddr(0x2857110 + 0xe1, 0x235FF2, 0xC5);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PostRender_UpscAndPreparePostFX));
			g_originalPostRender_UpscAndPreparePostFX = reinterpret_cast<PostRender_UpscAndPreparePostFXFunc*>(result);
			LogHookResult("PostRender_UpscAndPreparePostFX", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x17f, 0x235FF1, 0x2E3);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerState_OverrideMipBias));
			g_originalSamplerState_OverrideMipBias = reinterpret_cast<SamplerState_OverrideMipBiasFunc*>(result);
			LogHookResult("SamplerState_OverrideMipBias", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x1c9, 0x235FF1, 0x3A6);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerState_RestoreMipBias));
			g_originalSamplerState_RestoreMipBias = reinterpret_cast<SamplerState_RestoreMipBiasFunc*>(result);
			LogHookResult("SamplerState_RestoreMipBias", result);
		}

#if F4R_HAS_FSR3
		{
			auto addr = ResolveAddr(0x28568B0 + 0x1dc, 0x235FEB, 0x4C6);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_CaptureOpaque_ForReactiveMask));
			g_originalCaptureOpaque_ForReactiveMask = reinterpret_cast<CaptureOpaque_ForReactiveMaskFunc*>(result);
			LogHookResult("CaptureOpaque_ForReactiveMask", result);
		}
#endif

		{
			auto addr = ResolveAddr(0x2857110 + 0x9f, 0x235FF2, 0x83);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Effects_PreserveJitter));
			g_originalEffects_PreserveJitter = reinterpret_cast<Effects_PreserveJitterFunc*>(result);
			LogHookResult("Effects_PreserveJitter", result);
		}

		

		{
			auto addr = ResolveAddr(0x1297BE0 + 0x2bd, 0x225209, 0x275);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Loading_ResetDynamicResolution));
			g_originalLoading_ResetDynamicResolution = reinterpret_cast<Loading_ResetDynamicResolutionFunc*>(result);
			LogHookResult("Loading_ResetDynamicResolution", result);
		}

		{
			auto addr = ResolveAddr(0x2857110, 0x235FF2, 0);
			auto result = Detours::X64::DetourFunction(addr, reinterpret_cast<std::uintptr_t>(&Hook_ImageSpace_RestoreDynamicResolution));
			g_originalImageSpace_RestoreDynamicResolution = reinterpret_cast<ImageSpace_RestoreDynamicResolutionFunc*>(result);
			LogHookResult("ImageSpace_RestoreDynamicResolution", result);
		}

		{
			std::uintptr_t addr;
			if (IsAE() || IsNG()) {
				addr = REL::Relocation{ REL::Id<>{ 2317547 } }.GetAddress();
			} else {
				addr = REL::Relocation{ REL::Id<>{ 676108 } }.GetAddress();
			}
			auto result = Detours::X64::DetourFunction(addr, reinterpret_cast<std::uintptr_t>(&Hook_LensFlare_ForceFullResDepth));
			g_originalLensFlare_ForceFullResDepth = reinterpret_cast<LensFlare_ForceFullResDepthFunc*>(result);
			LogHookResult("LensFlare_ForceFullResDepth", result);
		}

		if (!g_enbLoaded) {
			std::uintptr_t addr;
			if (IsAE() || IsNG()) {
				addr = REL::Relocation{ REL::Id<>{ 2317302 } }.GetAddress() + 0x1C;
			} else {
				addr = REL::Relocation{ REL::Id<>{ 779077 } }.GetAddress() + 0x1C;
			}
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLRRaytracing_SkipInQualityModes));
			g_originalSSLRRaytracing_SkipInQualityModes = reinterpret_cast<SSLRRaytracing_SkipInQualityModesFunc*>(result);
			LogHookResult("SSLRRaytracing_SkipInQualityModes", result);
		}

		REX::LogDebug("All hooks installed");
	}
}
