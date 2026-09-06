#include "PCH.hpp"
#include "FidelityFX.hpp"
#include "Upscaling.hpp"

#include <cmath>
#include <cstring>
#include <utility>

#if F4R_HAS_FSR3

namespace F4R_Upscaling
{
	namespace
	{
		bool s_frameTimerInitialized = false;
		LARGE_INTEGER s_frequency = { 0 };
		LARGE_INTEGER s_lastFrameTime = { 0 };
		uint32_t s_dispatchCount = 0;

		void FfxResourceFromDX11Texture(
			FfxResource& res,
			ID3D11Resource* texture)
		{
			std::memset(&res, 0, sizeof(FfxResource));
			res.resource = texture;
			res.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
			res.description.usage = static_cast<FfxResourceUsage>(FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

			if (texture) {
				D3D11_RESOURCE_DIMENSION dim;
				texture->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC texDesc;
					reinterpret_cast<ID3D11Texture2D*>(texture)->GetDesc(&texDesc);
					res.description.width = texDesc.Width;
					res.description.height = texDesc.Height;
					res.description.mipCount = texDesc.MipLevels;
					res.description.format = ffxGetSurfaceFormatDX11(texDesc.Format);
					res.description.flags = FFX_RESOURCE_FLAGS_NONE;
				}
			}
		}
	}

	FidelityFX::~FidelityFX()
	{
		if (fsrScratchBuffer) {
			ffxFsr3ContextDestroy(&fsrContext);
			std::free(fsrScratchBuffer);
			fsrScratchBuffer = nullptr;
		}
	}

	bool FidelityFX::CreateFSRResources(
		ID3D11Device* device,
		uint32_t backBufferWidth,
		uint32_t backBufferHeight,
		DXGI_FORMAT backBufferFormat)
	{
		if (fsrScratchBuffer) {
			REX::LogWarning("CreateFSRResources: already created, skipping");
			return true;
		}

		size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
		fsrScratchBuffer = std::calloc(scratchSize, 1);
		if (!fsrScratchBuffer) {
			REX::LogError("CreateFSRResources: calloc failed");
			return false;
		}

		FfxErrorCode err = ffxGetInterfaceDX11(
			&backendInterface,
			reinterpret_cast<FfxDevice>(device),
			fsrScratchBuffer,
			scratchSize,
			FFX_FSR3UPSCALER_CONTEXT_COUNT);
		if (err != FFX_OK) {
			REX::LogError("CreateFSRResources: ffxGetInterfaceDX11 failed error={}", static_cast<int>(err));
			std::free(fsrScratchBuffer);
			fsrScratchBuffer = nullptr;
			return false;
		}

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (rendererData) {
			auto* rt4 = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[RenderTarget::kMainTemp].texture);
			if (rt4) {
				D3D11_TEXTURE2D_DESC rt4Desc = {};
				rt4->GetDesc(&rt4Desc);
				D3D11_TEXTURE2D_DESC texDesc = {};
				texDesc.Width = rt4Desc.Width;
				texDesc.Height = rt4Desc.Height;
				texDesc.MipLevels = 1;
				texDesc.ArraySize = 1;
				texDesc.Format = rt4Desc.Format;
				texDesc.SampleDesc.Count = 1;
				texDesc.Usage = D3D11_USAGE_DEFAULT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				colorOpaqueOnlyTexture = std::make_unique<Texture2D>();
				if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &colorOpaqueOnlyTexture->resource))) {
					REX::LogError("CreateFSRResources: colorOpaqueOnly failed");
					colorOpaqueOnlyTexture.reset();
				} else {
					REX::LogDebug("CreateFSRResources: colorOpaqueOnly {}x{} fmt={}",
						texDesc.Width, texDesc.Height, static_cast<int>(texDesc.Format));
				}
			}

			{
				D3D11_TEXTURE2D_DESC texDesc = {};
				texDesc.Width = backBufferWidth;
				texDesc.Height = backBufferHeight;
				texDesc.MipLevels = 1;
				texDesc.ArraySize = 1;
				texDesc.Format = DXGI_FORMAT_R8_UNORM;
				texDesc.SampleDesc.Count = 1;
				texDesc.Usage = D3D11_USAGE_DEFAULT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				reactiveMaskTexture = std::make_unique<Texture2D>();
				if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &reactiveMaskTexture->resource))) {
					REX::LogError("CreateFSRResources: reactiveMask failed");
					reactiveMaskTexture.reset();
				} else {
					REX::LogDebug("CreateFSRResources: reactiveMask {}x{}", texDesc.Width, texDesc.Height);
				}
			}
		}

		FfxFsr3ContextDescription contextDesc = {};
		contextDesc.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY;
		contextDesc.maxRenderSize = { backBufferWidth, backBufferHeight };
		contextDesc.maxUpscaleSize = { backBufferWidth, backBufferHeight };
		contextDesc.displaySize = { backBufferWidth, backBufferHeight };
		contextDesc.backendInterfaceSharedResources = backendInterface;
		contextDesc.backendInterfaceUpscaling = backendInterface;
		contextDesc.backendInterfaceFrameInterpolation = backendInterface;
		contextDesc.fpMessage = nullptr;
		contextDesc.backBufferFormat = ffxGetSurfaceFormatDX11(backBufferFormat);

		REX::LogDebug("CreateFSRResources: flags=0x{:x} size={}x{} fmt={}",
			contextDesc.flags, backBufferWidth, backBufferHeight, static_cast<int>(backBufferFormat));

		err = ffxFsr3ContextCreate(&fsrContext, &contextDesc);
		if (err != FFX_OK) {
			REX::LogError("CreateFSRResources: ffxFsr3ContextCreate failed error={} (0x{:x})",
				static_cast<int>(err), static_cast<int>(err));
			std::free(fsrScratchBuffer);
			fsrScratchBuffer = nullptr;
			return false;
		}

		REX::LogDebug("FSR3 context created ({}x{})", backBufferWidth, backBufferHeight);

		const std::pair<FfxFsr3UpscalerConfigureKey, float> upscalerConstants[] = {
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, 0.333f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, -0.333f },
		};
		for (const auto& [key, value] : upscalerConstants) {
			ffxFsr3SetUpscalerConstant(&fsrContext, key, const_cast<float*>(&value));
		}

		return true;
	}

	void FidelityFX::Apply(
		ID3D11Texture2D* texture,
		float jitterX,
		float jitterY,
		uint32_t renderWidth,
		uint32_t renderHeight)
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto& aa = Upscaling::GetSingleton();

		if (!s_frameTimerInitialized) {
			QueryPerformanceFrequency(&s_frequency);
			QueryPerformanceCounter(&s_lastFrameTime);
			s_frameTimerInitialized = true;
		}

		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);
		float frameTimeDelta = static_cast<float>(currentTime.QuadPart - s_lastFrameTime.QuadPart)
			/ static_cast<float>(s_frequency.QuadPart) * 1000.0f;
		s_lastFrameTime = currentTime;
		if (frameTimeDelta < 0.0f || frameTimeDelta > 1000.0f) {
			frameTimeDelta = 16.666f;
		} else if (frameTimeDelta > 50.0f) {
			frameTimeDelta = 33.333f;
		}

		FfxResource colorResource;
		FfxResourceFromDX11Texture(colorResource, reinterpret_cast<ID3D11Resource*>(texture));

		FfxResource depthResource = {};
		auto* depthTexture = reinterpret_cast<ID3D11Resource*>(rendererData->depthStencilTargets[DepthStencil::kMain].texture);
		if (depthTexture) {
			FfxResourceFromDX11Texture(depthResource, depthTexture);
			if (depthResource.description.format == FFX_SURFACE_FORMAT_R32_UINT) {
				depthResource.description.format = FFX_SURFACE_FORMAT_R32_FLOAT;
			}
		} else {
			auto* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);
			if (depthSRV) {
				ID3D11Resource* depthRes = nullptr;
				depthSRV->GetResource(&depthRes);
				if (depthRes) {
					FfxResourceFromDX11Texture(depthResource, depthRes);
					if (depthResource.description.format == FFX_SURFACE_FORMAT_R32_UINT) {
						depthResource.description.format = FFX_SURFACE_FORMAT_R32_FLOAT;
					}
					depthRes->Release();
				}
			}
			if (!depthResource.resource) {
				std::memset(&depthResource, 0, sizeof(FfxResource));
				depthResource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
				depthResource.description.usage = static_cast<FfxResourceUsage>(FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			}
		}

		FfxResource mvResource = {};
		auto* mvTexture = reinterpret_cast<ID3D11Resource*>(rendererData->renderTargets[RenderTarget::kMotionVectors].texture);
		if (mvTexture) {
			FfxResourceFromDX11Texture(mvResource, mvTexture);
		} else {
			auto* mvSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kMotionVectors].srView);
			if (mvSRV) {
				ID3D11Resource* mvRes = nullptr;
				mvSRV->GetResource(&mvRes);
				if (mvRes) {
					FfxResourceFromDX11Texture(mvResource, mvRes);
					mvRes->Release();
				}
			}
			if (!mvResource.resource) {
				std::memset(&mvResource, 0, sizeof(FfxResource));
				mvResource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
				mvResource.description.usage = static_cast<FfxResourceUsage>(FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			}
		}

		FfxResource reactiveResource = {};
		if (reactiveMaskTexture && reactiveMaskTexture->resource) {
			FfxResourceFromDX11Texture(reactiveResource, reactiveMaskTexture->resource);
		}

		FfxResource nullResource;
		std::memset(&nullResource, 0, sizeof(FfxResource));
		nullResource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		nullResource.description.usage = static_cast<FfxResourceUsage>(FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

		float cameraNear = 0.1f;
		float cameraFar = 1000.0f;
		GetCameraNearFar(cameraNear, cameraFar);

		float cameraFovAngleVertical = 1.0f;
		{
			auto& state = RE::BSGraphics::State::GetSingleton();
			const auto& camView = state.cameraState.camViewData;
			float proj11 = camView.projMat[1].m128_f32[1];
			if (proj11 != 0.0f && std::isfinite(proj11)) {
				cameraFovAngleVertical = 2.0f * std::atan(1.0f / proj11);
			}
		}

		FfxFsr3DispatchUpscaleDescription dispatch = {};
		dispatch.commandList = reinterpret_cast<FfxCommandList>(ctx);
		dispatch.color = colorResource;
		dispatch.depth = depthResource;
		dispatch.motionVectors = mvResource;
		dispatch.exposure = nullResource;
		dispatch.reactive = reactiveResource;
		dispatch.transparencyAndComposition = nullResource;
		dispatch.upscaleOutput = colorResource;
		dispatch.jitterOffset.x = -jitterX;
		dispatch.jitterOffset.y = -jitterY;
		dispatch.motionVectorScale.x = static_cast<float>(renderWidth);
		dispatch.motionVectorScale.y = static_cast<float>(renderHeight);
		dispatch.enableSharpening = true;
		dispatch.sharpness = aa.settings.fSharpness;
		dispatch.frameTimeDelta = frameTimeDelta;
		dispatch.preExposure = 1.0f;
		dispatch.reset = aa.resetHistory;
		dispatch.cameraNear = cameraNear;
		dispatch.cameraFar = cameraFar;
		dispatch.cameraFovAngleVertical = cameraFovAngleVertical;
		dispatch.viewSpaceToMetersFactor = 1.828125f / 128.0f;
		auto& fsrState = RE::BSGraphics::State::GetSingleton();
		dispatch.renderSize.width = renderWidth;
		dispatch.renderSize.height = renderHeight;
		dispatch.upscaleSize.width = fsrState.screenWidth;
		dispatch.upscaleSize.height = fsrState.screenHeight;
		dispatch.flags = 0;
		dispatch.frameID = static_cast<uint64_t>(RE::BSGraphics::State::GetSingleton().frameCount);

		if (s_dispatchCount < 3) {
			REX::LogDebug("Dispatch frame={} {}x{} j=({:.4f},{:.4f}) dt={:.2f}ms depthFmt={} mvFmt={}",
				s_dispatchCount, renderWidth, renderHeight,
				dispatch.jitterOffset.x, dispatch.jitterOffset.y,
				frameTimeDelta,
				static_cast<int>(depthResource.description.format),
				static_cast<int>(mvResource.description.format));
		}
		s_dispatchCount++;

		FfxErrorCode err = ffxFsr3ContextDispatchUpscale(&fsrContext, &dispatch);
		if (err != FFX_OK) {
			REX::LogError("ffxFsr3ContextDispatchUpscale failed error={} (0x{:x})",
				static_cast<int>(err), static_cast<int>(err));
		}

		ID3D11Buffer* nullCB = nullptr;
		ID3D11UnorderedAccessView* nullUAVs[8] = {};
		ID3D11ShaderResourceView* nullSRVs[16] = {};
		ctx->CSSetConstantBuffers(0, 1, &nullCB);
		ctx->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ctx->CSSetShaderResources(0, 16, nullSRVs);
		ctx->CSSetShader(nullptr, nullptr, 0);

		aa.resetHistory = false;
	}

	void FidelityFX::GenerateReactiveMask()
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		if (!colorOpaqueOnlyTexture || !colorOpaqueOnlyTexture->resource ||
			!reactiveMaskTexture || !reactiveMaskTexture->resource)
			return;

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		ctx->OMSetRenderTargets(0, nullptr, nullptr);

		auto* rt4 = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[RenderTarget::kMainTemp].texture);
		if (!rt4) return;

		FfxResource opaqueResource;
		FfxResourceFromDX11Texture(opaqueResource, colorOpaqueOnlyTexture->resource);

		FfxResource fullResource;
		FfxResourceFromDX11Texture(fullResource, rt4);

		FfxResource reactiveResource;
		FfxResourceFromDX11Texture(reactiveResource, reactiveMaskTexture->resource);

		uint32_t renderW = static_cast<uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(rtMgr) + 0.5f);
		uint32_t renderH = static_cast<uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(rtMgr) + 0.5f);

		FfxFsr3GenerateReactiveDescription desc = {};
		desc.commandList = reinterpret_cast<FfxCommandList>(ctx);
		desc.colorOpaqueOnly = opaqueResource;
		desc.colorPreUpscale = fullResource;
		desc.outReactive = reactiveResource;
		desc.renderSize = { renderW, renderH };
		desc.flags = 8;
		desc.scale = 0.5f;
		desc.cutoffThreshold = 0.0f;

		FfxErrorCode err = ffxFsr3ContextGenerateReactiveMask(&fsrContext, &desc);
		if (err != FFX_OK) {
			REX::LogError("GenerateReactiveMask failed error={}", static_cast<int>(err));
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