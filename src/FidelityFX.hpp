#pragma once

#include "Common.hpp"

#if F4R_HAS_FSR3

namespace F4R_Upscaling
{
	struct FidelityFX
	{
		FfxFsr3Context fsrContext{};
		void* fsrScratchBuffer = nullptr;
		FfxInterface backendInterface{};
		std::unique_ptr<Texture2D> colorOpaqueOnlyTexture;
		std::unique_ptr<Texture2D> reactiveMaskTexture;

		FidelityFX() = default;
		~FidelityFX();

		FidelityFX(const FidelityFX&) = delete;
		FidelityFX& operator=(const FidelityFX&) = delete;

		bool CreateFSRResources(
			ID3D11Device* device,
			uint32_t backBufferWidth,
			uint32_t backBufferHeight,
			DXGI_FORMAT backBufferFormat);

		void Apply(
			ID3D11Texture2D* texture,
			float jitterX,
			float jitterY,
			uint32_t renderWidth,
			uint32_t renderHeight);

		// no-op, reserved.
		//void GenerateReactiveMask();
	};
}
#endif
