#pragma once

#if F4R_HAS_XESS

#include <d3d11_4.h>
#include <d3d12.h>
#include <xess/xess.h>
#include <xess/xess_d3d12.h>

namespace F4R_Upscaling
{
	struct XeSS
	{
		using PFun_xessD3D12CreateContext = xess_result_t(*)(ID3D12Device*, xess_context_handle_t*);
		using PFun_xessD3D12Init = xess_result_t(*)(xess_context_handle_t, const xess_d3d12_init_params_t*);
		using PFun_xessD3D12Execute = xess_result_t(*)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*);
		using PFun_xessDestroyContext = xess_result_t(*)(xess_context_handle_t);
		using PFun_xessSetJitterScale = xess_result_t(*)(xess_context_handle_t, float, float);
		using PFun_xessSetVelocityScale = xess_result_t(*)(xess_context_handle_t, float, float);
		using PFun_xessIsOptimalDriver = xess_result_t(*)(xess_context_handle_t);

		HMODULE module = nullptr;

		PFun_xessD3D12CreateContext xessD3D12CreateContext = nullptr;
		PFun_xessD3D12Init xessD3D12Init = nullptr;
		PFun_xessD3D12Execute xessD3D12Execute = nullptr;
		PFun_xessDestroyContext xessDestroyContext = nullptr;
		PFun_xessSetJitterScale xessSetJitterScale = nullptr;
		PFun_xessSetVelocityScale xessSetVelocityScale = nullptr;
		PFun_xessIsOptimalDriver xessIsOptimalDriver = nullptr;

		xess_context_handle_t context = nullptr;

		ID3D11Device* device11 = nullptr;
		ID3D11Device5* device5 = nullptr;
		ID3D11DeviceContext4* context4 = nullptr;

		ID3D12Device* device = nullptr;
		ID3D12CommandQueue* commandQueue = nullptr;
		ID3D12CommandAllocator* commandAllocators[2] = { nullptr, nullptr };
		ID3D12GraphicsCommandList* commandLists[2] = { nullptr, nullptr };
		ID3D12Fence* fence = nullptr;
		HANDLE fenceEvent = nullptr;
		ID3D11Fence* d3d11Fence = nullptr;

		UINT64 fenceValue = 0;
		UINT32 frameIndex = 0;
		UINT64 allocatorFenceValue[2] = { 0, 0 };

		bool loaded = false;
		bool initialized = false;
		bool failed = false;
		bool disabled = false;

		float velocityScaleX = 0.0f;
		float velocityScaleY = 0.0f;

		[[nodiscard]] static XeSS& GetSingleton();

		XeSS(const XeSS&) = delete;
		XeSS& operator=(const XeSS&) = delete;

		bool Load();
		bool CreateD3D12(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
		bool CreateContext(uint32_t a_width, uint32_t a_height, int a_qualityMode = 0);
		void TeardownD3D12();

		bool CreateSharedTexture(
			SharedTexture2D* a_out,
			uint32_t a_width,
			uint32_t a_height,
			DXGI_FORMAT a_format,
			uint32_t a_bindFlags,
			DXGI_FORMAT a_srvFormat = DXGI_FORMAT_UNKNOWN,
			DXGI_FORMAT a_uavFormat = DXGI_FORMAT_UNKNOWN);

		void Execute(
			SharedTexture2D* a_color,
			SharedTexture2D* a_motionVectors,
			SharedTexture2D* a_depth,
			SharedTexture2D* a_output,
			float a_jitterX,
			float a_jitterY,
			uint32_t a_width,
			uint32_t a_height,
			uint32_t a_resetHistory);

	private:
		XeSS() = default;
	};
}
#endif
