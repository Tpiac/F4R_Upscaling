#include "FidelityFX.hpp"
#include "PCH.hpp"
#include "Upscaling.hpp"

#include <cmath>
#include <cstring>
#include <utility>

#if F4R_HAS_FSR3

namespace F4R_Upscaling
{
	namespace
	{
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
		if (!fsrScratchBuffer.empty()) {
			ffxFsr3ContextDestroy(&fsrContext);
			fsrScratchBuffer.clear();
			fsrScratchBuffer.shrink_to_fit();
		}
	}

	bool FidelityFX::CreateFSRResources(
		ID3D11Device* device,
		std::uint32_t backBufferWidth,
		std::uint32_t backBufferHeight,
		DXGI_FORMAT backBufferFormat)
	{
		if (!fsrScratchBuffer.empty()) {
			REX::LogWarning("CreateFSRResources: already created, skipping");
			return true;
		}

		size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
		fsrScratchBuffer.assign(scratchSize, std::byte{ 0 });
		if (fsrScratchBuffer.empty() && scratchSize > 0) {
			REX::LogError("CreateFSRResources: allocation failed");
			return false;
		}

		FfxErrorCode err = ffxGetInterfaceDX11(
			&backendInterface,
			reinterpret_cast<FfxDevice>(device),
			fsrScratchBuffer.data(),
			scratchSize,
			FFX_FSR3UPSCALER_CONTEXT_COUNT);
		if (err != FFX_OK) {
			REX::LogError("CreateFSRResources: ffxGetInterfaceDX11 failed error={}", static_cast<int>(err));
			fsrScratchBuffer.clear();
			fsrScratchBuffer.shrink_to_fit();
			return false;
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

		err = ffxFsr3ContextCreate(&fsrContext, &contextDesc);
		if (err != FFX_OK) {
			REX::LogError("CreateFSRResources: ffxFsr3ContextCreate failed error={} (0x{:x})",
				static_cast<int>(err), static_cast<int>(err));
			fsrScratchBuffer.clear();
			fsrScratchBuffer.shrink_to_fit();
			return false;
		}

		const std::array<std::pair<FfxFsr3UpscalerConfigureKey, float>, 5> upscalerConstants = { {
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, 1.0f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, 0.333f },
			{ FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, -0.333f },
		} };
		for (const auto& [key, value] : upscalerConstants) {
			ffxFsr3SetUpscalerConstant(&fsrContext, key, const_cast<float*>(&value));
		}

		return true;
	}

	void FidelityFX::Apply(
		ID3D11Texture2D* texture,
		float jitterX,
		float jitterY,
		std::uint32_t renderWidth,
		std::uint32_t renderHeight)
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) {
			return;
		}

		auto* ctx = GetImmediateContext();
		if (!ctx) {
			return;
		}

		auto& aa = Upscaling::GetSingleton();

		const auto* timer = RE::BSTimer::GetSingleton();
		const float frameTimeDelta = timer ? timer->realTimeDelta * 1000.0f : 0.0f;

		FfxResource colorResource;
		FfxResourceFromDX11Texture(colorResource, reinterpret_cast<ID3D11Resource*>(texture));

		FfxResource depthResource = {};
		auto* depthTexture = reinterpret_cast<ID3D11Resource*>(rendererData->depthStencilTargets[DepthStencil::kMain].texture);
		if (depthTexture) {
			FfxResourceFromDX11Texture(depthResource, depthTexture);
			if (depthResource.description.format == FFX_SURFACE_FORMAT_R32_UINT) {
				depthResource.description.format = FFX_SURFACE_FORMAT_R32_FLOAT;
			}
		}
		else {
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
		}
		else {
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

		float cameraNear = 0.0f;
		float cameraFar = 1.0f;
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
		dispatch.frameID = static_cast<std::uint64_t>(RE::BSGraphics::State::GetSingleton().frameCount);

		FfxErrorCode err = ffxFsr3ContextDispatchUpscale(&fsrContext, &dispatch);
		if (err != FFX_OK) {
			static bool dispatchLogged = false;
			if (!dispatchLogged) {
				dispatchLogged = true;
				REX::LogError("ffxFsr3ContextDispatchUpscale failed error={} (0x{:x})",
					static_cast<int>(err), static_cast<int>(err));
			}
		}

		ID3D11Buffer* nullCB = nullptr;
		std::array<ID3D11UnorderedAccessView*, 8> nullUAVs{};
		std::array<ID3D11ShaderResourceView*, 16> nullSRVs{};
		ctx->CSSetConstantBuffers(0, 1, &nullCB);
		ctx->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr);
		ctx->CSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
		ctx->CSSetShader(nullptr, nullptr, 0);

		aa.resetHistory = false;
	}

}
#endif
