#pragma once

#include <atomic>
#include <span>

#include "Common.hpp"

#if F4R_HAS_FSR3
	#include "FidelityFX.hpp"
#endif

namespace F4R_Upscaling
{
	class Upscaling
	{
	public:
		[[nodiscard]] static Upscaling& GetSingleton();

		Upscaling(const Upscaling&) = delete;
		Upscaling(Upscaling&&) = delete;
		Upscaling& operator=(const Upscaling&) = delete;
		Upscaling& operator=(Upscaling&&) = delete;
		~Upscaling();

		void Init();
		void LoadSettings();

		void InstallHooks();

		void Update();
		void Apply();

		void OverrideSamplerStates();
		void ResetSamplerStates();

		void RefreshSamplerCache(SamplerStates* a_states, float a_bias);
		void RebuildSamplerCache(float a_bias);

		void UpdateGameSettings();
		void RestoreAASettings();
		void ReturnToVanilla();
		void CheckResources();
		void EnsureFlareResources(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height);
		void InvalidateFlareDepth();
		void PollRuntimeSettings();
		void RequestReset();
		void BuildFlareDepth(RE::BSGraphics::RenderTargetManager& a_rtMgr);
		void PushFlareDepth();
		void PopFlareDepth();

		void RefreshHBAOCache();
		void ReleaseHBAOCache();
		void EnsureSharpenResources(
			ID3D11Device* a_device,
			uint32_t a_width,
			uint32_t a_height,
			DXGI_FORMAT a_backBufferFormat,
			DXGI_FORMAT a_srvFormat);
		void EnsureMotionVectorFixResources(ID3D11Device* a_device);
		bool EnterHBAO();
		void ExitHBAO();
		void InvokeHBAODynRes(bool a_dynamic);

		void UpdateScaledTargets(float a_widthRatio, float a_heightRatio);
		bool EnterFullExtentPass(std::span<const std::uint32_t> a_copyIn, bool a_clearProxies);
		void ExitFullExtentPass(std::span<const std::uint32_t> a_copyOut);
		void AbortFullExtentPass();
		void AcquirePooledSSLRProxy();
		void ReleasePooledSSLRProxy();
		void BeginSSLRPass();
		void EndSSLRPass();
		void PushUnitScreenTriangle();
		void PopUnitScreenTriangle();
		[[nodiscard]] bool HasScaledTargets() const noexcept { return scaledTargetsValid; }
		void ReleaseScaledTargets();

		Settings settings;

		bool upsclEnabled = false;
		bool wasUpsclEnabled = false;
		bool resetHistory = false;
		bool samplerBiasActive = false;

		std::unique_ptr<Texture2D> workingTexture;
		std::unique_ptr<Texture2D> dlssOutputTexture;
		std::unique_ptr<Texture2D> motionVectorTexture;
		std::unique_ptr<Texture2D> tempTexture;
		ID3D11ComputeShader* mvFixShader = nullptr;
		ID3D11Buffer* mvFixCB = nullptr;
		ID3D11ComputeShader* rcasShader = nullptr;
		ID3D11Buffer* rcasCB = nullptr;

		std::unique_ptr<SharedTexture2D> xessColorTexture;
		std::unique_ptr<SharedTexture2D> xessMotionVectorTexture;
		std::unique_ptr<SharedTexture2D> xessDepthTexture;
		std::unique_ptr<SharedTexture2D> xessOutputTexture;
		ID3D11ComputeShader* depthCopyShader = nullptr;

		std::unique_ptr<Texture2D> flareDepthTexture;
		ID3D11ComputeShader* flareDepthShader = nullptr;
		ID3D11Buffer* flareDepthCB = nullptr;
		ID3D11ShaderResourceView* flareDepthBackup = nullptr;
		bool flareValid = false;
		std::uint32_t flareWidth = 0;
		std::uint32_t flareHeight = 0;

		static constexpr std::uint32_t kScaledTargetSpan = 101;
		static constexpr std::uint32_t kHBAOMetaCount = 100;

		struct HBAOSlot
		{
			ID3D11Texture2D* texture = nullptr;
			ID3D11Texture2D* copyTexture = nullptr;
			ID3D11RenderTargetView* rtView = nullptr;
			ID3D11ShaderResourceView* srView = nullptr;
			ID3D11ShaderResourceView* copySRView = nullptr;
			ID3D11UnorderedAccessView* uaView = nullptr;
		};

		std::unique_ptr<Texture2D> hbaoTargets[kScaledTargetSpan];
		HBAOSlot hbaoBackedUp[kScaledTargetSpan];
		bool hbaoMapped[kScaledTargetSpan]{};
		D3D11_TEXTURE2D_DESC hbaoFullDesc[kScaledTargetSpan]{};
		RE::BSGraphics::RenderTargetProperties hbaoSavedMeta[kHBAOMetaCount];
		bool hbaoMetaHeld = false;
		std::unique_ptr<Texture2D> hbaoDepth;
		D3D11_TEXTURE2D_DESC hbaoDepthFullDesc{};
		bool hbaoDepthDescribed = false;
		ID3D11ShaderResourceView* hbaoDepthBackedUp = nullptr;
		bool hbaoDepthMapped = false;
		ID3D11ComputeShader* hbaoDepthShader = nullptr;
		std::uint32_t hbaoDepthFrame = 0;
		bool hbaoActive = false;
		float hbaoCachedWidthRatio = 0.0f;
		float hbaoCachedHeightRatio = 0.0f;
		float hbaoSavedWidthRatio = 1.0f;
		float hbaoSavedHeightRatio = 1.0f;
		bool hbaoCacheValid = false;

		ID3D11SamplerState* biasedSamplerStates[320]{};
		ID3D11SamplerState* originalSamplerStates[320]{};
		float cachedSamplerBias = 0.0f;
		bool samplerCacheValid = false;

		static constexpr std::uint32_t kProxyTargetCount = 24;
		static constexpr std::uint32_t kProxyTargetMetaCount = 100;
		static constexpr std::uint32_t kProxyTargets[kProxyTargetCount] = {
			LogicalRenderTarget::kGbufferNormal,
			LogicalRenderTarget::kGbufferMetadata,
			LogicalRenderTarget::kGbufferMaterial,
			LogicalRenderTarget::kAmbientOcclusion,
			LogicalRenderTarget::kGbufferEmissive,
			LogicalRenderTarget::kDiffuseBufferA,
			LogicalRenderTarget::kSpecularBufferA,
			LogicalRenderTarget::kAmbientOcclusionHalf,
			LogicalRenderTarget::kMain,
			LogicalRenderTarget::kSSLRBlurV,
			LogicalRenderTarget::kDiffuseBufferB,
			LogicalRenderTarget::kSpecularBufferB,
			LogicalRenderTarget::kMainTemp,
			LogicalRenderTarget::kMotionVectors,
			LogicalRenderTarget::kRefractionNormal,
			LogicalRenderTarget::kUIDownscaled,
			LogicalRenderTarget::kUIDownscaledComposite,
			LogicalRenderTarget::kGbufferAlbedo,
			LogicalRenderTarget::kSSLRRayStart,
			LogicalRenderTarget::kSSLRSurfaceDepth,
			LogicalRenderTarget::kSSLRRayResult,
			LogicalRenderTarget::kLuminanceDownscale,
			LogicalRenderTarget::kMainVerticalBlur,
			LogicalRenderTarget::kHdrImagespaceAux,
		};
		static constexpr std::uint32_t kProxyCopyIn[] = {
			LogicalRenderTarget::kRefractionNormal,
			LogicalRenderTarget::kMainTemp,
			LogicalRenderTarget::kMotionVectors,
			LogicalRenderTarget::kHdrImagespaceAux,
		};
		static constexpr std::uint32_t kProxyMainTempOut[] = { LogicalRenderTarget::kMainTemp };
		static constexpr std::uint32_t kProxyCompositeCopyIn[] = {
			LogicalRenderTarget::kGbufferNormal,
			LogicalRenderTarget::kAmbientOcclusion,
			LogicalRenderTarget::kGbufferMetadata,
			LogicalRenderTarget::kGbufferMaterial,
			LogicalRenderTarget::kGbufferEmissive,
			LogicalRenderTarget::kDiffuseBufferA,
			LogicalRenderTarget::kSpecularBufferA,
			LogicalRenderTarget::kMain,
			LogicalRenderTarget::kDiffuseBufferB,
			LogicalRenderTarget::kSpecularBufferB,
			LogicalRenderTarget::kAmbientOcclusionHalf,
		};
		static constexpr std::uint32_t kProxySSLRCopyIn[] = {
			LogicalRenderTarget::kGbufferNormal,
			LogicalRenderTarget::kGbufferMetadata,
			LogicalRenderTarget::kMain,
		};
		static constexpr std::uint32_t kProxySSLRCopyOut[] = {
			LogicalRenderTarget::kSSLRRayResult,
			LogicalRenderTarget::kSSLRBlurV,
		};

		RE::BSGraphics::RenderTarget savedScaledTargets[kProxyTargetCount]{};
		RE::BSGraphics::RenderTarget proxyScaledTargets[kProxyTargetCount]{};
		RE::BSGraphics::RenderTargetProperties savedRenderTargetMeta[kProxyTargetMetaCount]{};
		ID3D11RenderTargetView* savedPassRTV = nullptr;
		ID3D11DepthStencilView* savedPassDSV = nullptr;
		bool scaledTargetsOverridden = false;
		bool scaledTargetsValid = false;
		float scaledTargetWidthRatio = 0.0f;
		float scaledTargetHeightRatio = 0.0f;
		std::uint32_t scaledTargetScreenWidth = 0;
		std::uint32_t scaledTargetScreenHeight = 0;
		bool savedUseDynViewportAsDefault = true;

		bool seamVisited = false;

		std::unique_ptr<Texture2D> depthOverrideTexture;
		ID3D11ComputeShader* depthOverrideShader = nullptr;
		ID3D11ShaderResourceView* depthOverrideBackup = nullptr;

		RE::NiPointer<RE::BSTriShape> unitScreenTriangle;
		bool sslrPassOpen = false;

		RE::BSGraphics::RenderTarget pooledProxySaved{};
		std::unique_ptr<Texture2D> pooledProxyTexture;
		std::uint32_t pooledProxySlot = 0;
		std::uint32_t pooledProxyWidth = 0;
		std::uint32_t pooledProxyHeight = 0;
		DXGI_FORMAT pooledProxyFormat = DXGI_FORMAT_UNKNOWN;
		bool pooledProxyActive = false;

		float jitterX = 0.0f;
		float jitterY = 0.0f;

		float savedWidthRatio = 1.0f;
		float savedHeightRatio = 1.0f;
		float currentScale = 1.0f;

#if F4R_HAS_FSR3
		std::unique_ptr<FidelityFX> fidelityFX;
#endif

	private:
		void RestoreHBAOState();
		bool PushFullExtentDepth();
		void PopFullExtentDepth();
		void ReleaseProxyTargets();
		void ReleasePooledSSLRTexture();

		Upscaling() = default;

		std::uint64_t settingsIniWriteTime = 0;
		bool settingsIniHasTime = false;
		bool fxaaStateSaved = false;
		bool fxaaOriginalActive = false;
		bool taaFlagSaved = false;
		bool taaFlagOriginal = false;
		bool resourcesCreated = false;
		uint32_t cachedWidth = 0;
		uint32_t cachedHeight = 0;
		DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;
		int32_t cachedMethod = -1;
#if F4R_HAS_XESS
		int32_t cachedQuality = -1;
#endif

		int startupFrameGuard = 0;
		bool initialized = false;
		float prevScale = 1.0f;
		int32_t lastWorkingMethod = static_cast<int32_t>(Method::FSR3);
		bool dlssUnsupportedLogged = false;
	};
}