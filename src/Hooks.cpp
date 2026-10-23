#include "PCH.hpp"
#include "Upscaling.hpp"
#include "WorldTransformFix.hpp"
#include <Detours.h>
#include <string_view>

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
		if (IsAE() || IsNG()) {
			return GetAEAddr(a_aeID, a_aeSub);
		}
		return GetOGAddr(a_ogRVA);
	}

	inline void LogHookResult(std::string_view a_name, std::uintptr_t a_original)
	{
		if (!a_original) {
			REX::LogWarning("{} FAILED", a_name);
		}
	}

	inline bool IsContainerMenuOpen()
	{
		static const RE::BSFixedString containerMenu("ContainerMenu");
		if (auto* ui = RE::UI::GetSingleton()) {
			return ui->IsMenuOpen(containerMenu).value_or(false);
		}
		return false;
	}

	using TAA_OffWhileUpscFunc = bool(ImageSpaceEffectTemporalAA*);
	TAA_OffWhileUpscFunc* g_originalTAA_OffWhileUpsc = nullptr;

	bool Hook_TAA_OffWhileUpsc(ImageSpaceEffectTemporalAA* a_this)
	{
		auto& up = Upscaling::GetSingleton();
		if (up.upsclEnabled) {
			return false;
		}
		return g_originalTAA_OffWhileUpsc(a_this);
	}

	using PreRender_UpdateUpscStateFunc = void(RenderTargetManager*, void*, void*, void*, void*);
	PreRender_UpdateUpscStateFunc* g_originalPreRender_UpdateUpscState = nullptr;

	void Hook_PreRender_UpdateUpscState(RenderTargetManager* a_this, void* a_p2, void* a_p3, void* a_p4, void* a_p5)
	{
		WorldTransformFix::Update();

		g_originalPreRender_UpdateUpscState(a_this, a_p2, a_p3, a_p4, a_p5);
		Upscaling::GetSingleton().Update();
	}

	using PostRender_UpscThenRestoreFullResFunc = void(RenderTargetManager*, bool);
	PostRender_UpscThenRestoreFullResFunc* g_originalPostRender_UpscThenRestoreFullRes = nullptr;

	void Hook_PostRender_UpscThenRestoreFullRes(RenderTargetManager* a_this, bool a_p2)
	{
		g_originalPostRender_UpscThenRestoreFullRes(a_this, a_p2);

		auto& up = Upscaling::GetSingleton();
		up.seamVisited = true;
		up.Apply();

		if (up.upsclEnabled && up.currentScale < 0.999f && !IsContainerMenuOpen()) {
			up.BuildFlareDepth(*a_this);
		}

		up.savedWidthRatio = GetDynWidthRatio(*a_this);
		up.savedHeightRatio = GetDynHeightRatio(*a_this);
		SetGlobalDynamicRatio(*a_this, 1.0f, 1.0f);
		GetSetCurrentViewportDefault()(a_this);
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
	}

	struct VatsSettleGate
	{
		std::uint32_t fx12Stable = 0;
	};

	VatsSettleGate g_vatsSettle;

	inline bool VatsSplitDeferred(VatsSettleGate& a_gate)
	{
		bool vats = false;
		if (auto* ui = RE::UI::GetSingleton()) {
			static const RE::BSFixedString vatsMenu("VATSMenu");
			vats = ui->IsMenuOpen(vatsMenu).value_or(false);
		}

		constexpr auto fxVatsTarget = static_cast<std::size_t>(
			RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_VATS_TARGET);
		constexpr std::uint32_t kVatsStableFrames = 20;

		bool deferred = false;
		if (vats) {
			bool fx12 = false;
			if (auto* ism = RE::ImageSpaceManager::GetSingleton()) {
				if (ism->effectList.size() > fxVatsTarget && ism->effectList[fxVatsTarget]) {
					fx12 = ism->effectList[fxVatsTarget]->isActive;
				}
			}
			a_gate.fx12Stable = fx12 ?
									(a_gate.fx12Stable < kVatsStableFrames ? a_gate.fx12Stable + 1 : kVatsStableFrames) :
									0;
			deferred = a_gate.fx12Stable < kVatsStableFrames;
		}
		else {
			a_gate.fx12Stable = 0;
			deferred = false;
		}

		return deferred;
	}

	constexpr std::uint32_t kEffectHdrCs = static_cast<std::uint32_t>(
		RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_HDR_CS);
	constexpr std::uint32_t kEffectRefraction = static_cast<std::uint32_t>(
		RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_REFRACTION);
	constexpr std::uint32_t kEffectDepthOfField = static_cast<std::uint32_t>(
		RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_DEPTH_OF_FIELD);
	constexpr std::uint32_t kEffectDepthOfFieldSplitScreen = static_cast<std::uint32_t>(
		RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_DEPTH_OF_FIELD_SPLIT_SCREEN);
	constexpr std::uint32_t kEffectFullScreenColor = static_cast<std::uint32_t>(
		RE::ImageSpaceManager::ImageSpaceEffectEnum::EFFECT_FULLSCREEN_COLOR);

	inline RE::ImageSpaceEffect* GetImageSpaceEffect(std::uint32_t a_index)
	{
		auto* ism = RE::ImageSpaceManager::GetSingleton();
		if (!ism) {
			return nullptr;
		}
		using Index = decltype(ism->effectList)::size_type;
		const auto index = static_cast<Index>(a_index);
		if (index >= ism->effectList.size()) {
			return nullptr;
		}
		return ism->effectList[index];
	}

	inline bool IsEffectActive(RE::ImageSpaceEffect* a_effect)
	{
		if (!a_effect) {
			return false;
		}
		using IsActiveFunc = bool(RE::ImageSpaceEffect*);
		constexpr std::size_t isActiveSlot = 8;
		auto** vtable = *reinterpret_cast<void***>(a_effect);
		auto fn = reinterpret_cast<IsActiveFunc*>(vtable[isActiveSlot]);
		return fn(a_effect);
	}

	using Effects_PreserveJitterFunc = void(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
	Effects_PreserveJitterFunc* g_originalEffects_PreserveJitter = nullptr;

	void Hook_Effects_PreserveJitter(void* a_this, std::uint32_t a_p2, std::uint32_t a_p3, std::uint32_t a_p4, std::uint32_t a_p5)
	{
		auto& up = Upscaling::GetSingleton();
		auto& state = State::GetSingleton();
		float savedX = state.offsetX;
		float savedY = state.offsetY;

		if (!up.upsclEnabled || a_p2 != 0 || a_p3 != 13 || !up.HasScaledTargets() || IsContainerMenuOpen() || VatsSplitDeferred(g_vatsSettle)) {
			g_originalEffects_PreserveJitter(a_this, a_p2, a_p3, a_p4, a_p5);
			state.offsetX = savedX;
			state.offsetY = savedY;
			return;
		}

		g_originalEffects_PreserveJitter(a_this, 0, kEffectHdrCs - 1, a_p4, a_p5);

		bool entered = up.EnterFullExtentPass(Upscaling::ProxyPass::IsFirst, Upscaling::kProxyCopyIn, true);
		g_originalEffects_PreserveJitter(a_this, kEffectHdrCs, kEffectRefraction, a_p4, a_p5);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::ProxyPass::IsFirst, Upscaling::kProxyMainTempOut);
		}

		if (IsEffectActive(GetImageSpaceEffect(kEffectDepthOfField))) {
			g_originalEffects_PreserveJitter(a_this, kEffectDepthOfField, kEffectDepthOfField, a_p4, a_p5);
		}

		entered = up.EnterFullExtentPass(Upscaling::ProxyPass::IsSecond, Upscaling::kProxyCopyIn, false);
		g_originalEffects_PreserveJitter(a_this, kEffectDepthOfFieldSplitScreen, kEffectFullScreenColor, a_p4, a_p5);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::ProxyPass::IsSecond, Upscaling::kProxyMainTempOut);
		}

		state.offsetX = savedX;
		state.offsetY = savedY;
	}

	using Hook_DeferredCompositeFunc = void(void*, std::uint32_t, bool);
	Hook_DeferredCompositeFunc* g_originalDeferredCompositeRenderPass = nullptr;
	Hook_DeferredCompositeFunc* g_originalDeferredCompositeFogPass = nullptr;
	using DeferredCompositeEndFunc = void(void*);
	DeferredCompositeEndFunc* g_originalDeferredCompositeEnd = nullptr;

	void Hook_DeferredCompositeRenderPass(void* a_this, std::uint32_t a_p2, bool a_p3)
	{
		auto& up = Upscaling::GetSingleton();

		if (up.WillDeferredScatterRun()) {
			const bool entered = up.EnterFullExtentPass(Upscaling::ProxyPass::CompositeA,
				Upscaling::kProxyCompositeCopyIn, false);
			if (entered) {
				up.compositePassOpen = true;
				g_originalDeferredCompositeRenderPass(a_this, a_p2, a_p3);
				up.pendingObservedMask |= up.ObserveBoundProxySRVs(false);
				return;
			}
		}

		const bool entered = up.EnterFullExtentPass(Upscaling::ProxyPass::CompositeA,
			Upscaling::kProxyCompositeCopyIn, false);
		g_originalDeferredCompositeRenderPass(a_this, a_p2, a_p3);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::ProxyPass::CompositeA, Upscaling::kProxyMainTempOut);
		}
	}

	void Hook_DeferredCompositeFogPass(void* a_this, std::uint32_t a_p2, bool a_p3)
	{
		auto& up = Upscaling::GetSingleton();

		if (up.compositePassOpen) {
			up.ReapplyFullExtentOverride();
			g_originalDeferredCompositeFogPass(a_this, a_p2, a_p3);
			up.ExitFullExtentPass(Upscaling::ProxyPass::CompositeA, Upscaling::kProxyMainTempOut);
			up.compositePassOpen = false;
			return;
		}

		const bool entered = up.EnterFullExtentPass(Upscaling::ProxyPass::CompositeB,
			Upscaling::kProxyCompositeCopyIn, false);
		g_originalDeferredCompositeFogPass(a_this, a_p2, a_p3);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::ProxyPass::CompositeB, Upscaling::kProxyMainTempOut);
		}
	}

	void Hook_DeferredCompositeEnd(void* a_desc)
	{
		auto& up = Upscaling::GetSingleton();
		if (up.compositePassOpen) {
			up.ExitFullExtentPass(Upscaling::ProxyPass::CompositeA, Upscaling::kProxyMainTempOut);
			up.compositePassOpen = false;
		}
		g_originalDeferredCompositeEnd(a_desc);
	}

	using Loading_ResetDynamicResolutionFunc = void();
	Loading_ResetDynamicResolutionFunc* g_originalLoading_ResetDynamicResolution = nullptr;

	void Hook_Loading_ResetDynamicResolution()
	{
		g_originalLoading_ResetDynamicResolution();

		auto& rtMgr = RenderTargetManager::GetSingleton();
		SetGlobalDynamicRatio(rtMgr, 1.0f, 1.0f);
		GetSetCurrentViewportDefault()(&rtMgr);
		auto& up = Upscaling::GetSingleton();
		up.InvalidateFlareDepth();
		up.RequestReset();
		up.ReleaseHBAOCache();
		up.ReleaseScaledTargets();

		WorldTransformFix::RequestSceneSweep();
	}

	using ImageSpace_RestoreDynamicResolutionFunc = void(void*);
	ImageSpace_RestoreDynamicResolutionFunc* g_originalImageSpace_RestoreDynamicResolution = nullptr;

	void Hook_ImageSpace_RestoreDynamicResolution(void* a_this)
	{
		auto& rtMgr = RenderTargetManager::GetSingleton();
		auto& up = Upscaling::GetSingleton();
		const auto& state = State::GetSingleton();

		if (up.scaledTargetsOverridden) {
			REX::LogWarning("Leak frame={} full-extent pass still open: force-closing",
				state.frameCount);
			up.AbortFullExtentPass();
		}
		if (up.hbaoActive) {
			REX::LogWarning("Leak frame={} HBAO pass still open: force-closing",
				state.frameCount);
			up.ExitHBAO();
		}

		const bool needUpscale = up.upsclEnabled && up.currentScale < 0.999f;
		up.seamVisited = false;

		g_originalImageSpace_RestoreDynamicResolution(a_this);

		if (needUpscale && !up.seamVisited) {
			REX::LogWarning(
				"MissedSeam frame={} scale={:.3f} ratio={:.3f}/{:.3f}: upscale call site missed, recovering",
				state.frameCount, up.currentScale,
				GetDynWidthRatio(rtMgr), GetDynHeightRatio(rtMgr));
			up.Apply();
			up.savedWidthRatio = GetDynWidthRatio(rtMgr);
			up.savedHeightRatio = GetDynHeightRatio(rtMgr);
		}

		SetGlobalDynamicRatio(rtMgr, up.savedWidthRatio, up.savedHeightRatio);
		GetSetCurrentViewportDefault()(&rtMgr);
	}

	using LensFlare_ForceFullResDepthFunc = void(RE::NiCamera*);
	LensFlare_ForceFullResDepthFunc* g_originalLensFlare_ForceFullResDepth = nullptr;

	void Hook_LensFlare_ForceFullResDepth(RE::NiCamera* a_camera)
	{
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
		auto& up = Upscaling::GetSingleton();
		up.PopFlareDepth();
		const bool dynLow = (GetDynWidthRatio(rtMgr) != 1.0f || GetDynHeightRatio(rtMgr) != 1.0f);
		const bool need = dynLow && up.upsclEnabled && up.currentScale < 0.999f && up.flareValid && up.flareDepthTexture && up.flareDepthTexture->srv;
		if (need) {
			up.PushFlareDepth();
		}
		g_originalLensFlare_ForceFullResDepth(a_camera);
		up.PopFlareDepth();
	}

	using VatsTarget_SetPixelConstantFunc = void(void*, std::uint32_t, float, float);
	VatsTarget_SetPixelConstantFunc* g_originalVatsTarget_SetPixelConstant = nullptr;

	void Hook_VatsTarget_SetPixelConstant(void* a_param, std::uint32_t a_slot, float a_f3, float a_f4)
	{
		auto& up = Upscaling::GetSingleton();
		if (up.upsclEnabled && up.currentScale < 0.999f && up.currentScale > 0.001f) {
			a_f3 *= up.currentScale;
			a_f4 *= up.currentScale;
		}
		g_originalVatsTarget_SetPixelConstant(a_param, a_slot, a_f3, a_f4);
	}

	using HBAO_RenderFunc = void(void*);
	HBAO_RenderFunc* g_originalHBAO_Render = nullptr;

	void Hook_HBAO_Render(void* a_this)
	{
		auto& up = Upscaling::GetSingleton();
		const bool entered = up.EnterHBAO();
		g_originalHBAO_Render(a_this);
		if (entered) {
			up.ExitHBAO();
		}
	}

	using SSLR_SetCurrentRenderTargetFunc = void(RE::BSGraphics::RenderTargetManager*, std::int32_t, std::int32_t, RE::BSGraphics::SetRenderTargetMode);
	SSLR_SetCurrentRenderTargetFunc* g_originalSSLR_SetCurrentRenderTarget = nullptr;

	void Hook_SSLR_SetRayStart(RE::BSGraphics::RenderTargetManager* a_this, std::int32_t a_slot, std::int32_t a_logicalID, RE::BSGraphics::SetRenderTargetMode a_mode)
	{
		if (a_slot == 0 && a_logicalID == LogicalRenderTarget::kSSLRRayStart) {
			Upscaling::GetSingleton().BeginSSLRPass();
		}
		g_originalSSLR_SetCurrentRenderTarget(a_this, a_slot, a_logicalID, a_mode);
	}

	using SSLR_AcquireRenderTargetFunc = void(RE::BSGraphics::RenderTargetManager*, std::int32_t);
	SSLR_AcquireRenderTargetFunc* g_originalSSLR_AcquireRenderTarget = nullptr;

	void Hook_SSLR_AcquireBlurH(RE::BSGraphics::RenderTargetManager* a_this, std::int32_t a_logicalID)
	{
		g_originalSSLR_AcquireRenderTarget(a_this, a_logicalID);
		if (a_logicalID == LogicalRenderTarget::kSSLRBlurH) {
			Upscaling::GetSingleton().AcquirePooledSSLRProxy();
		}
	}

	using SSLR_ReleaseRenderTargetFunc = void(RE::BSGraphics::RenderTargetManager*, std::int32_t);
	SSLR_ReleaseRenderTargetFunc* g_originalSSLR_ReleaseRenderTarget = nullptr;

	void Hook_SSLR_ReleaseBlurH(RE::BSGraphics::RenderTargetManager* a_this, std::int32_t a_logicalID)
	{
		auto& up = Upscaling::GetSingleton();
		if (a_logicalID == LogicalRenderTarget::kSSLRBlurH) {
			up.ReleasePooledSSLRProxy();
			up.EndSSLRPass();
		}
		g_originalSSLR_ReleaseRenderTarget(a_this, a_logicalID);
	}
}

namespace F4R_Upscaling
{
	void Upscaling::InvokeHBAODynRes(bool a_dynamic)
	{
		if (g_originalPostRender_UpscThenRestoreFullRes) {
			auto& rtMgr = RenderTargetManager::GetSingleton();
			g_originalPostRender_UpscThenRestoreFullRes(&rtMgr, a_dynamic);
		}
	}

	void Upscaling::InstallHooks()
	{

		{
			std::uintptr_t vtableAddr;
			if (IsAE() || IsNG()) {
				vtableAddr = REL::Relocation{ REL::Id<>{ 0x1473CC } }.GetAddress();
			}
			else {
				vtableAddr = REL::Relocation<std::uintptr_t>{ RE::VTABLE::ImageSpaceEffectTemporalAA[0] }.GetAddress();
			}
			auto result = Detours::X64::DetourVTable(vtableAddr, reinterpret_cast<std::uintptr_t>(&Hook_TAA_OffWhileUpsc), 8);
			g_originalTAA_OffWhileUpsc = reinterpret_cast<TAA_OffWhileUpscFunc*>(result);
			LogHookResult("TAA_OffWhileUpsc", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x14B, 0x235FF1, 0x29F);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PreRender_UpdateUpscState));
			g_originalPreRender_UpdateUpscState = reinterpret_cast<PreRender_UpdateUpscStateFunc*>(result);
			LogHookResult("PreRender_UpdateUpscState", result);
		}

		{
			auto addr = ResolveAddr(0x2857110 + 0xE1, 0x235FF2, 0xC5);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PostRender_UpscThenRestoreFullRes));
			g_originalPostRender_UpscThenRestoreFullRes = reinterpret_cast<PostRender_UpscThenRestoreFullResFunc*>(result);
			LogHookResult("PostRender_UpscThenRestoreFullRes", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x17F, 0x235FF1, 0x2E3);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerState_OverrideMipBias));
			g_originalSamplerState_OverrideMipBias = reinterpret_cast<SamplerState_OverrideMipBiasFunc*>(result);
			LogHookResult("SamplerState_OverrideMipBias", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x1C9, 0x235FF1, 0x3A6);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerState_RestoreMipBias));
			g_originalSamplerState_RestoreMipBias = reinterpret_cast<SamplerState_RestoreMipBiasFunc*>(result);
			LogHookResult("SamplerState_RestoreMipBias", result);
		}

		{
			auto addr = ResolveAddr(0x2857110 + 0x9F, 0x235FF2, 0x83);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Effects_PreserveJitter));
			g_originalEffects_PreserveJitter = reinterpret_cast<Effects_PreserveJitterFunc*>(result);
			LogHookResult("Effects_PreserveJitter", result);
		}

		{
			if (IsOG()) {
				auto addr = GetOGAddr(0x2855E60 + 0x8DC);
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_DeferredCompositeRenderPass));
				g_originalDeferredCompositeRenderPass = reinterpret_cast<Hook_DeferredCompositeFunc*>(result);
				LogHookResult("Hook_DeferredCompositeRenderPass", result);
			}
		}

		{
			if (IsOG()) {
				auto addr = GetOGAddr(0x2855E60 + 0x989);
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_DeferredCompositeFogPass));
				g_originalDeferredCompositeFogPass = reinterpret_cast<Hook_DeferredCompositeFunc*>(result);
				LogHookResult("Hook_DeferredCompositeFogPass", result);
			}
		}

		{
			if (IsOG()) {
				auto addr = GetOGAddr(0x2855E60 + 0xA1E);
				bool hooked = false;
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_DeferredCompositeEnd));
					g_originalDeferredCompositeEnd = reinterpret_cast<DeferredCompositeEndFunc*>(result);
					hooked = result != 0;
				}
				LogHookResult("Hook_DeferredCompositeEnd", hooked ? 1 : 0);
			}
		}

		{
			auto addr = ResolveAddr(0x1297BE0 + 0x2BD, 0x225209, 0x275);
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
			}
			else {
				addr = REL::Relocation{ REL::Id<>{ 676108 } }.GetAddress();
			}
			auto result = Detours::X64::DetourFunction(addr, reinterpret_cast<std::uintptr_t>(&Hook_LensFlare_ForceFullResDepth));
			g_originalLensFlare_ForceFullResDepth = reinterpret_cast<LensFlare_ForceFullResDepthFunc*>(result);
			LogHookResult("LensFlare_ForceFullResDepth", result);
		}

		{
			bool vatsHooked = false;
			if (IsAE() || IsNG()) {
				auto addr = REL::Relocation{ REL::Id<>{ 0x235E9F } }.GetAddress() + 0x110;
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_VatsTarget_SetPixelConstant));
					g_originalVatsTarget_SetPixelConstant = reinterpret_cast<VatsTarget_SetPixelConstantFunc*>(result);
					vatsHooked = (result != 0);
				}
			}
			else {
				auto addr = REL::Relocation{ REL::Id<>{ 0xFE897 } }.GetAddress() + 0xBB;
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_VatsTarget_SetPixelConstant));
					g_originalVatsTarget_SetPixelConstant = reinterpret_cast<VatsTarget_SetPixelConstantFunc*>(result);
					vatsHooked = (result != 0);
				}
			}
			LogHookResult("VatsTargetOutline", vatsHooked ? 1 : 0);
		}

		{
			bool hbaoHooked = false;
			auto addr = ResolveAddr(0x2857480 + 0x1BA, 0x235FF1, 0x397);
			if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_HBAO_Render));
				g_originalHBAO_Render = reinterpret_cast<HBAO_RenderFunc*>(result);
				hbaoHooked = (result != 0);
			}
			LogHookResult("NVHBAO", hbaoHooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x3D9, 0x235FF2, -0x1F60 + 0x3EE);
			if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_SetRayStart));
				g_originalSSLR_SetCurrentRenderTarget = reinterpret_cast<SSLR_SetCurrentRenderTargetFunc*>(result);
				hooked = result != 0;
			}
			LogHookResult("SSLR_SetCurrentRenderTarget", hooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x5FB, 0x235FF2, -0x1F60 + 0x60E);
			if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_AcquireBlurH));
				g_originalSSLR_AcquireRenderTarget = reinterpret_cast<SSLR_AcquireRenderTargetFunc*>(result);
				hooked = result != 0;
			}
			LogHookResult("SSLR_AcquireRenderTarget", hooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x6BA, 0x235FF2, -0x1F60 + 0x6C3);
			if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_ReleaseBlurH));
				g_originalSSLR_ReleaseRenderTarget = reinterpret_cast<SSLR_ReleaseRenderTargetFunc*>(result);
				hooked = result != 0;
			}
			LogHookResult("SSLR_ReleaseRenderTarget", hooked ? 1 : 0);
		}
	}
}
