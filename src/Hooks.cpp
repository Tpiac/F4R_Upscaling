#include "PCH.hpp"
#include "Upscaling.hpp"
#include <Detours.h>

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

	inline void LogHookResult(const char* a_name, std::uintptr_t a_original)
	{
		if (!a_original)
			REX::LogWarning("{} FAILED", a_name);
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
		bool suppressed = false;
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
		} else {
			a_gate.fx12Stable = 0;
			deferred = false;
		}

		if (deferred != a_gate.suppressed) {
			a_gate.suppressed = deferred;
		}
		return deferred;
	}

	using Effects_PreserveJitterFunc = void(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
	Effects_PreserveJitterFunc* g_originalEffects_PreserveJitter = nullptr;

	void Hook_Effects_PreserveJitter(void* a_this, std::uint32_t a_p2, std::uint32_t a_p3, std::uint32_t a_p4, std::uint32_t a_p5)
	{
		auto& up = Upscaling::GetSingleton();
		auto& state = State::GetSingleton();
		float savedX = state.offsetX;
		float savedY = state.offsetY;

		if (!up.upsclEnabled || a_p2 != 0 || a_p3 != 13 || !up.HasScaledTargets() ||
			IsContainerMenuOpen() || VatsSplitDeferred(g_vatsSettle)) {
			g_originalEffects_PreserveJitter(a_this, a_p2, a_p3, a_p4, a_p5);
			state.offsetX = savedX;
			state.offsetY = savedY;
			return;
		}

		g_originalEffects_PreserveJitter(a_this, 0, 3, a_p4, a_p5);

		const bool entered = up.EnterFullExtentPass(Upscaling::kProxyCopyIn, true);
		g_originalEffects_PreserveJitter(a_this, 4, 13, a_p4, a_p5);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::kProxyMainTempOut);
		}

		state.offsetX = savedX;
		state.offsetY = savedY;
	}

	using Hook_DeferredCompositeFunc = void(void*, std::uint32_t, bool);
	Hook_DeferredCompositeFunc* g_originalDeferredCompositeRenderPass = nullptr;
	Hook_DeferredCompositeFunc* g_originalDeferredCompositeFogPass = nullptr;

	template<Hook_DeferredCompositeFunc*& OriginalFunc, const auto& a_copyIn>
	void Hook_DeferredCompositeTemplate(void* a_this, std::uint32_t a_p2, bool a_p3)
	{
		auto& up = Upscaling::GetSingleton();
		const bool entered = up.EnterFullExtentPass(a_copyIn, false);
		OriginalFunc(a_this, a_p2, a_p3);
		if (entered) {
			up.ExitFullExtentPass(Upscaling::kProxyMainTempOut);
		}
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
		const bool need = dynLow && up.upsclEnabled && up.currentScale < 0.999f &&
			up.flareValid && up.flareDepthTexture && up.flareDepthTexture->srv;
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
			} else {
				vtableAddr = REL::Relocation<std::uintptr_t>{ RE::VTABLE::ImageSpaceEffectTemporalAA[0] }.GetAddress();
			}
			auto result = Detours::X64::DetourVTable(vtableAddr, reinterpret_cast<std::uintptr_t>(&Hook_TAA_OffWhileUpsc), 8);
			g_originalTAA_OffWhileUpsc = reinterpret_cast<TAA_OffWhileUpscFunc*>(result);
			LogHookResult("TAA_OffWhileUpsc", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x14b, 0x235FF1, 0x29F);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PreRender_UpdateUpscState));
			g_originalPreRender_UpdateUpscState = reinterpret_cast<PreRender_UpdateUpscStateFunc*>(result);
			LogHookResult("PreRender_UpdateUpscState", result);
		}

		{
			auto addr = ResolveAddr(0x2857110 + 0xe1, 0x235FF2, 0xC5);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PostRender_UpscThenRestoreFullRes));
			g_originalPostRender_UpscThenRestoreFullRes = reinterpret_cast<PostRender_UpscThenRestoreFullResFunc*>(result);
			LogHookResult("PostRender_UpscThenRestoreFullRes", result);
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

		{
			auto addr = ResolveAddr(0x2857110 + 0x9f, 0x235FF2, 0x83);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Effects_PreserveJitter));
			g_originalEffects_PreserveJitter = reinterpret_cast<Effects_PreserveJitterFunc*>(result);
			LogHookResult("Effects_PreserveJitter", result);
		}

		{
			if (IsOG()) {
				auto addr = GetOGAddr(0x2855E60 + 0x8DC);
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_DeferredCompositeTemplate<g_originalDeferredCompositeRenderPass, Upscaling::kProxyCompositeCopyIn>));
				g_originalDeferredCompositeRenderPass = reinterpret_cast<Hook_DeferredCompositeFunc*>(result);
				LogHookResult("Hook_DeferredCompositeRenderPass", result);
			}
		}

		{
			if (IsOG()) {
				auto addr = GetOGAddr(0x2855E60 + 0x989);
				auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_DeferredCompositeTemplate<g_originalDeferredCompositeFogPass, Upscaling::kProxyTargets>));
				g_originalDeferredCompositeFogPass = reinterpret_cast<Hook_DeferredCompositeFunc*>(result);
				LogHookResult("Hook_DeferredCompositeFogPass", result);
			}
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

		{
			bool vatsHooked = false;
			if (IsAE() || IsNG()) {
				try {
					auto addr = REL::Relocation{ REL::Id<>{ 0x235E9F } }.GetAddress() + 0x110;
					if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
						auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_VatsTarget_SetPixelConstant));
						g_originalVatsTarget_SetPixelConstant = reinterpret_cast<VatsTarget_SetPixelConstantFunc*>(result);
						vatsHooked = (result != 0);
					}
				} catch (...) {
				}
			} else {
				try {
					auto addr = REL::Relocation{ REL::Id<>{ 0xFE897 } }.GetAddress() + 0xBB;
					if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
						auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_VatsTarget_SetPixelConstant));
						g_originalVatsTarget_SetPixelConstant = reinterpret_cast<VatsTarget_SetPixelConstantFunc*>(result);
						vatsHooked = (result != 0);
					}
				} catch (...) {
				}
			}
			LogHookResult("VatsTargetOutline", vatsHooked ? 1 : 0);
		}

		{
			bool hbaoHooked = false;
			auto addr = ResolveAddr(0x2857480 + 0x1BA, 0x235FF1, 0x397);
			try {
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_HBAO_Render));
					g_originalHBAO_Render = reinterpret_cast<HBAO_RenderFunc*>(result);
					hbaoHooked = (result != 0);
				}
			} catch (...) {
			}
			LogHookResult("NVHBAO", hbaoHooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x3D9, 0x235FF2, -0x1F60 + 0x3EE);
			try {
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_SetRayStart));
					g_originalSSLR_SetCurrentRenderTarget = reinterpret_cast<SSLR_SetCurrentRenderTargetFunc*>(result);
					hooked = result != 0;
				}
			} catch (...) {
			}
			LogHookResult("SSLR_SetCurrentRenderTarget", hooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x5FB, 0x235FF2, -0x1F60 + 0x60E);
			try {
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_AcquireBlurH));
					g_originalSSLR_AcquireRenderTarget = reinterpret_cast<SSLR_AcquireRenderTargetFunc*>(result);
					hooked = result != 0;
				}
			} catch (...) {
			}
			LogHookResult("SSLR_AcquireRenderTarget", hooked ? 1 : 0);
		}

		{
			bool hooked = false;
			auto addr = ResolveAddr(0x2855E60 + 0x6BA, 0x235FF2, -0x1F60 + 0x6C3);
			try {
				if (*reinterpret_cast<std::uint8_t*>(addr) == 0xE8) {
					auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SSLR_ReleaseBlurH));
					g_originalSSLR_ReleaseRenderTarget = reinterpret_cast<SSLR_ReleaseRenderTargetFunc*>(result);
					hooked = result != 0;
				}
			} catch (...) {
			}
			LogHookResult("SSLR_ReleaseRenderTarget", hooked ? 1 : 0);
		}
	}
}

