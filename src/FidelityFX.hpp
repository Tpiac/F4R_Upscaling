#pragma once

#include "Common.hpp"

#include <vector>

#if F4R_HAS_FSR3

namespace F4R_Upscaling
{
	struct FidelityFX
	{
		FfxFsr3Context fsrContext{};
		std::vector<std::byte> fsrScratchBuffer;
		FfxInterface backendInterface{};

		FidelityFX() = default;
		~FidelityFX();

		FidelityFX(const FidelityFX&) = delete;
		FidelityFX& operator=(const FidelityFX&) = delete;

		bool CreateFSRResources(
			ID3D11Device* device,
			std::uint32_t backBufferWidth,
			std::uint32_t backBufferHeight,
			DXGI_FORMAT backBufferFormat);

		void Apply(
			ID3D11Texture2D* texture,
			float jitterX,
			float jitterY,
			std::uint32_t renderWidth,
			std::uint32_t renderHeight);
	};
}
#endif
