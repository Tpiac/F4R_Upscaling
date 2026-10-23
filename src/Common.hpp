#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d12.h>

#if !defined(F4R_HAS_DLSS) || !defined(F4R_HAS_FSR3) || !defined(F4R_HAS_XESS) || !defined(F4R_HAS_STREAMLINE) || !defined(F4R_DEFAULT_Method)
#error "Build configuration is missing: F4R_HAS_DLSS/F4R_HAS_FSR3/F4R_HAS_XESS/F4R_HAS_STREAMLINE/F4R_DEFAULT_Method (see CMakeLists.txt)"
#endif

#if F4R_HAS_DLSS && !F4R_HAS_STREAMLINE
#error "The DLSS backend requires Streamline"
#endif

namespace RE::BSGraphics
{
	class RenderTargetManager;
}

namespace F4R_Upscaling
{
	template <class T>
	inline void SafeRelease(T*& a_ptr) noexcept
	{
		if (a_ptr) {
			a_ptr->Release();
			a_ptr = nullptr;
		}
	}

	inline bool IsAE()
	{
		return F4SE::IsRuntimeOnlyAE();
	}

	inline bool IsNG()
	{
		return F4SE::IsRuntimeOnlyNG();
	}

	inline bool IsOG()
	{
		return !IsAE() && !IsNG();
	}

	inline void GetCameraNearFar(float& a_near, float& a_far)
	{
		if (IsOG()) {
			a_near = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0xE281 } }.GetAddress());
			a_far = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0xEA19D } }.GetAddress());
		}
		else {
			a_near = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x296532 } }.GetAddress());
			a_far = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x296533 } }.GetAddress());
		}
	}

	inline float& GetDynWidthRatio(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static constexpr std::ptrdiff_t OG_OFFSET = 0xF88;
		static constexpr std::ptrdiff_t AE_OFFSET = 0xFB8;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline float& GetDynHeightRatio(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static constexpr std::ptrdiff_t OG_OFFSET = 0xF8C;
		static constexpr std::ptrdiff_t AE_OFFSET = 0xFBC;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline bool& GetDynResActivated(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static constexpr std::ptrdiff_t OG_OFFSET = 0xFA8;
		static constexpr std::ptrdiff_t AE_OFFSET = 0xFE5;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline bool& GetUseDynViewportAsDefault(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static constexpr std::ptrdiff_t OG_OFFSET = 0xFA7;
		static constexpr std::ptrdiff_t AE_OFFSET = 0xFE4;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline const std::uint32_t* GetRenderTargetIDs(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static constexpr std::ptrdiff_t OG_OFFSET = 0xDC4;
		static constexpr std::ptrdiff_t AE_OFFSET = 0xDF4;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline void SetGlobalDynamicRatio(RE::BSGraphics::RenderTargetManager& a_rtMgr, float a_widthRatio, float a_heightRatio)
	{
		GetDynWidthRatio(a_rtMgr) = a_widthRatio;
		GetDynHeightRatio(a_rtMgr) = a_heightRatio;
		GetDynResActivated(a_rtMgr) = (a_widthRatio != 1.0f || a_heightRatio != 1.0f);
	}

	namespace LogicalRenderTarget
	{
		enum : std::uint32_t
		{
			kMainTemp = 1,
			kMain = 2,
			kSSLRRayStart = 4,
			kSSLRRayResult = 5,
			kSSLRBlurH = 6,
			kSSLRBlurV = 7,
			kSSLRSurfaceDepth = 8,
			kRefractionNormal = 14,
			kHdrImagespaceAux = 15,
			kUIDownscaled = 24,
			kUIDownscaledComposite = 25,
			kGbufferAlbedo = 26,
			kGbufferNormal = 27,
			kGbufferMetadata = 29,
			kGbufferMaterial = 30,
			kGbufferEmissive = 31,
			kMotionVectors = 32,
			kDiffuseBufferA = 33,
			kSpecularBufferA = 34,
			kDiffuseBufferB = 35,
			kSpecularBufferB = 36,
			kAmbientOcclusion = 37,
			kAmbientOcclusionHalf = 39,
			kMainVerticalBlur = 68,
			kLuminanceDownscale = 70
		};
	}

	using SetUseDynViewportAsDefaultFunc = void(RE::BSGraphics::RenderTargetManager*, bool);

	inline SetUseDynViewportAsDefaultFunc* GetSetUseDynViewportAsDefault()
	{
		const std::uintptr_t id = IsOG() ? 676851ull : 2277194ull;
		return reinterpret_cast<SetUseDynViewportAsDefaultFunc*>(
			REL::Relocation{ REL::Id<>{ id } }.GetAddress());
	}

	using SetCurrentViewportDefaultFunc = void(RE::BSGraphics::RenderTargetManager*);

	inline SetCurrentViewportDefaultFunc* GetSetCurrentViewportDefault()
	{
		const std::uintptr_t id = IsOG() ? 158420ull : 2277192ull;
		return reinterpret_cast<SetCurrentViewportDefaultFunc*>(
			REL::Relocation{ REL::Id<>{ id } }.GetAddress());
	}

	namespace RenderTarget
	{
		enum : std::uint32_t
		{
			kFrameBuffer = 0,
			kMainTemp = 4,
			kGbufferNormal = 20,
			kSSAOFinal = 25,
			kMotionVectors = 29,
			kPipboyTarget = 60,
			kPipboyTargetSwap = 61
		};
	}

	namespace DepthStencil
	{
		enum : std::uint32_t
		{
			kMain = 2
		};
	}

	enum class Method : std::int32_t
	{
		Off = 0,
		FSR3 = 1,
		DLSS = 2,
		XeSS = 3
	};

	inline constexpr bool kHasDLSS = F4R_HAS_DLSS != 0;
	inline constexpr bool kHasFSR3 = F4R_HAS_FSR3 != 0;
	inline constexpr bool kHasXeSS = F4R_HAS_XESS != 0;
	inline constexpr std::int32_t kBackendCount = (kHasDLSS ? 1 : 0) + (kHasFSR3 ? 1 : 0) + (kHasXeSS ? 1 : 0);
	inline constexpr Method kSoleBackend = kBackendCount == 1 ? (kHasDLSS ? Method::DLSS : (kHasFSR3 ? Method::FSR3 : Method::XeSS)) : Method::Off;
	inline constexpr Method kDefaultMethod = static_cast<Method>(F4R_DEFAULT_Method);

	enum class DLSSPresetChoice : std::int32_t
	{
		J = 0,
		K = 1,
		L = 2,
		M = 3
	};

enum class QualityMode : std::int32_t
	{
		Native = 0,
		Quality = 1,
		Balanced = 2,
		Performance = 3
	};

	struct Settings
	{
		Method method = kDefaultMethod;

		float sharpness = 0.5f;
		float anisotropicMipBias = -0.0001f;
		QualityMode qualityMode = QualityMode::Native;

		float qualityScale = 0.648f;
		float balancedScale = 0.59f;
		float performanceScale = 0.5f;

		bool enableReflex = false;
		bool reflexBoost = false;
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;

		DLSSPresetChoice dlssPreset = DLSSPresetChoice::K;
	};

	struct Texture2D
	{
		ID3D11Texture2D* resource = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;

		~Texture2D()
		{
			SafeRelease(uav);
			SafeRelease(srv);
			SafeRelease(rtv);
			SafeRelease(resource);
		}
	};

	struct SamplerStates
	{
		std::array<ID3D11SamplerState*, 320> a{};
	};

	struct SharedTexture2D
	{
		ID3D11Texture2D* resource = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D12Resource* resource12 = nullptr;

		~SharedTexture2D()
		{
			SafeRelease(uav);
			SafeRelease(srv);
			SafeRelease(resource);
			SafeRelease(resource12);
		}
	};
}

extern bool g_enbLoaded;
extern bool g_enbExtractionFailed;
extern ID3D11Device* g_realDevice;
extern ID3D11DeviceContext* g_realContext;

void ExtractRealD3D11();

namespace F4R_Upscaling
{
	inline std::optional<std::filesystem::file_time_type> GetFileWriteTime(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto writeTime = std::filesystem::last_write_time(a_path, ec);
		if (ec) {
			return std::nullopt;
		}

		return writeTime;
	}

	inline SamplerStates* GetGlobalSamplers()
	{
		std::uintptr_t id = IsOG() ? 0xAD18ull : 0x294447ull;
		return reinterpret_cast<SamplerStates*>(
			REL::Relocation<std::uintptr_t>{ REL::Id<>{ id } }.GetAddress());
	}

	inline ID3D11Device* GetRenderer()
	{
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) {
			return nullptr;
		}
		return (g_enbLoaded && g_realDevice) ? g_realDevice : reinterpret_cast<ID3D11Device*>(rd->device);
	}

	inline ID3D11DeviceContext* GetImmediateContext()
	{
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) {
			return nullptr;
		}
		return (g_enbLoaded && g_realContext) ? g_realContext : reinterpret_cast<ID3D11DeviceContext*>(rd->context);
	}

	struct FullResRenderScope
	{
		explicit FullResRenderScope(bool a_active) noexcept :
			manager(a_active ? &RE::BSGraphics::RenderTargetManager::GetSingleton() : nullptr),
			state(a_active ? &RE::BSGraphics::State::GetSingleton() : nullptr)
		{
			if (manager) {
				savedWidthRatio = GetDynWidthRatio(*manager);
				savedHeightRatio = GetDynHeightRatio(*manager);
				savedActivated = GetDynResActivated(*manager);
				savedUseDynViewportAsDefault = GetUseDynViewportAsDefault(*manager);
				SetGlobalDynamicRatio(*manager, 1.0f, 1.0f);
				GetSetUseDynViewportAsDefault()(manager, false);
				GetSetCurrentViewportDefault()(manager);
			}
			if (state) {
				savedOffsetX = state->offsetX;
				savedOffsetY = state->offsetY;
				state->offsetX = 0.0f;
				state->offsetY = 0.0f;
			}
		}

		~FullResRenderScope() noexcept
		{
			if (manager) {
				SetGlobalDynamicRatio(*manager, savedWidthRatio, savedHeightRatio);
				GetDynResActivated(*manager) = savedActivated;
				GetSetUseDynViewportAsDefault()(manager, savedUseDynViewportAsDefault);
				GetSetCurrentViewportDefault()(manager);
			}
			if (state) {
				state->offsetX = savedOffsetX;
				state->offsetY = savedOffsetY;
			}
		}

		FullResRenderScope(const FullResRenderScope&) = delete;
		FullResRenderScope(FullResRenderScope&&) = delete;
		FullResRenderScope& operator=(const FullResRenderScope&) = delete;
		FullResRenderScope& operator=(FullResRenderScope&&) = delete;

	private:
		RE::BSGraphics::RenderTargetManager* manager = nullptr;
		RE::BSGraphics::State* state = nullptr;
		float savedWidthRatio = 1.0f;
		float savedHeightRatio = 1.0f;
		bool savedActivated = false;
		bool savedUseDynViewportAsDefault = true;
		float savedOffsetX = 0.0f;
		float savedOffsetY = 0.0f;
	};

	inline bool IsPipboyRenderTarget(const RE::Interface3D::Renderer& a_renderer) noexcept
	{
		if (a_renderer.customRenderTarget != -1) {
			const auto target = static_cast<std::uint32_t>(a_renderer.customRenderTarget);
			return target == RenderTarget::kPipboyTarget || target == RenderTarget::kPipboyTargetSwap;
		}
		if (a_renderer.postfx == RE::Interface3D::PostEffect::kHUDGlass ||
			a_renderer.postfx == RE::Interface3D::PostEffect::kHUDGlassWithMod) {
			return false;
		}
		return a_renderer.omsize == RE::Interface3D::OffscreenMenuSize::kPipboy;
	}
}
