#include "PCH.hpp"
#include "XeSS.hpp"

#include <dxgi1_2.h>

#if F4R_HAS_XESS

namespace F4R_Upscaling
{
	namespace
	{
		void XeSSResourceBarrier(
			ID3D12GraphicsCommandList* a_list,
			ID3D12Resource* a_resource,
			D3D12_RESOURCE_STATES a_before,
			D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			a_list->ResourceBarrier(1, &barrier);
		}

		const char* XeSSResultToString(xess_result_t a_result)
		{
			switch (a_result) {
			case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "WARNING_NONEXISTING_FOLDER";
			case XESS_RESULT_WARNING_OLD_DRIVER: return "WARNING_OLD_DRIVER";
			case XESS_RESULT_SUCCESS: return "SUCCESS";
			case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "ERROR_UNSUPPORTED_DEVICE";
			case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "ERROR_UNSUPPORTED_DRIVER";
			case XESS_RESULT_ERROR_UNINITIALIZED: return "ERROR_UNINITIALIZED";
			case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "ERROR_INVALID_ARGUMENT";
			case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "ERROR_DEVICE_OUT_OF_MEMORY";
			case XESS_RESULT_ERROR_DEVICE: return "ERROR_DEVICE";
			case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "ERROR_NOT_IMPLEMENTED";
			case XESS_RESULT_ERROR_INVALID_CONTEXT: return "ERROR_INVALID_CONTEXT";
			case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "ERROR_OPERATION_IN_PROGRESS";
			case XESS_RESULT_ERROR_UNSUPPORTED: return "ERROR_UNSUPPORTED";
			case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "ERROR_CANT_LOAD_LIBRARY";
			case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "ERROR_WRONG_CALL_ORDER";
			case XESS_RESULT_ERROR_UNKNOWN: return "ERROR_UNKNOWN";
			default: return "UNKNOWN";
			}
		}
	}

	XeSS& XeSS::GetSingleton()
	{
		static XeSS instance;
		return instance;
	}

	bool XeSS::Load()
	{
		if (module) return true;
		if (failed) return false;

		char buf[MAX_PATH];
		if (!GetModuleFileNameA(GetModuleHandleA(F4R_MODULE_NAME), buf, sizeof(buf))) {
			REX::LogCritical("Failed to resolve plugin path");
			failed = true;
			return false;
		}

		std::string dir = buf;
		dir = dir.substr(0, dir.rfind('\\') + 1);
		std::wstring dllPath(dir.begin(), dir.end());
		dllPath += L"XeSS\\libxess.dll";

		module = LoadLibraryW(dllPath.c_str());
		if (!module) {
			REX::LogCritical("Failed to load libxess.dll");
			failed = true;
			return false;
		}

		auto resolve = [&](const char* a_name) -> void* {
			return GetProcAddress(module, a_name);
		};

		xessD3D12CreateContext = reinterpret_cast<PFun_xessD3D12CreateContext>(resolve("xessD3D12CreateContext"));
		xessD3D12Init = reinterpret_cast<PFun_xessD3D12Init>(resolve("xessD3D12Init"));
		xessD3D12Execute = reinterpret_cast<PFun_xessD3D12Execute>(resolve("xessD3D12Execute"));
		xessDestroyContext = reinterpret_cast<PFun_xessDestroyContext>(resolve("xessDestroyContext"));
		xessGetInputResolution = reinterpret_cast<PFun_xessGetInputResolution>(resolve("xessGetInputResolution"));
		xessSetJitterScale = reinterpret_cast<PFun_xessSetJitterScale>(resolve("xessSetJitterScale"));
		xessSetVelocityScale = reinterpret_cast<PFun_xessSetVelocityScale>(resolve("xessSetVelocityScale"));
		xessIsOptimalDriver = reinterpret_cast<PFun_xessIsOptimalDriver>(resolve("xessIsOptimalDriver"));
		xessGetVersion = reinterpret_cast<PFun_xessGetVersion>(resolve("xessGetVersion"));

		if (!xessD3D12CreateContext || !xessD3D12Init || !xessD3D12Execute || !xessDestroyContext) {
			REX::LogCritical("Failed to resolve XeSS exports");
			FreeLibrary(module);
			module = nullptr;
			failed = true;
			return false;
		}

		loaded = true;
		REX::LogInformation("libxess.dll loaded");
		return true;
	}

	bool XeSS::CreateD3D12(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (!a_device || !a_context) {
			REX::LogWarning("CreateD3D12: null device/context");
			return false;
		}

		device11 = a_device;

		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(&device5)))) {
			REX::LogWarning("ID3D11Device5 not available (ENB or old driver?)");
			return false;
		}

		if (FAILED(a_context->QueryInterface(IID_PPV_ARGS(&context4)))) {
			REX::LogWarning("ID3D11DeviceContext4 not available");
			return false;
		}

		IDXGIDevice* dxgiDevice = nullptr;
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
			REX::LogWarning("IDXGIDevice not available");
			return false;
		}

		IDXGIAdapter* adapter = nullptr;
		HRESULT hr = dxgiDevice->GetAdapter(&adapter);
		dxgiDevice->Release();
		if (FAILED(hr) || !adapter) {
			REX::LogWarning("Failed to get adapter");
			return false;
		}

		hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
		adapter->Release();
		if (FAILED(hr)) {
			REX::LogWarning("D3D12CreateDevice failed hr=0x{:x}", static_cast<uint32_t>(hr));
			return false;
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queueDesc.NodeMask = 0;
		if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)))) {
			REX::LogWarning("CreateCommandQueue failed");
			return false;
		}

		for (uint32_t i = 0; i < 2; i++) {
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])))) {
				REX::LogWarning("CreateCommandAllocator failed");
				return false;
			}

			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i], nullptr, IID_PPV_ARGS(&commandLists[i])))) {
				REX::LogWarning("CreateCommandList failed");
				return false;
			}
			commandLists[i]->Close();
		}

		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
			REX::LogWarning("CreateFence failed");
			return false;
		}

		HANDLE sharedFenceHandle = nullptr;
		if (FAILED(device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle))) {
			REX::LogWarning("CreateSharedHandle(fence) failed");
			return false;
		}

		hr = device5->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence));
		CloseHandle(sharedFenceHandle);
		if (FAILED(hr)) {
			REX::LogWarning("OpenSharedFence failed hr=0x{:x}", static_cast<uint32_t>(hr));
			return false;
		}

		fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!fenceEvent) {
			REX::LogWarning("CreateEvent failed");
			return false;
		}

		REX::LogInformation("D3D12 interop initialized");
		return true;
	}

	bool XeSS::CreateContext(uint32_t a_width, uint32_t a_height, int a_qualityMode)
	{
		if (!device) return false;

		xess_result_t r = xessD3D12CreateContext(device, &context);
		if (r != XESS_RESULT_SUCCESS) {
			REX::LogError("xessD3D12CreateContext failed ({})", XeSSResultToString(r));
			context = nullptr;
			return false;
		}

		if (xessIsOptimalDriver && xessIsOptimalDriver(context) == XESS_RESULT_WARNING_OLD_DRIVER) {
			REX::LogWarning("Old driver detected: XeSS quality may degrade");
		}

		xess_quality_settings_t quality = XESS_QUALITY_SETTING_AA;
		if (a_qualityMode == 1) quality = XESS_QUALITY_SETTING_QUALITY;
		else if (a_qualityMode == 2) quality = XESS_QUALITY_SETTING_BALANCED;
		else if (a_qualityMode == 3) quality = XESS_QUALITY_SETTING_PERFORMANCE;

		float inputScale = 1.0f;
		if (quality == XESS_QUALITY_SETTING_QUALITY) inputScale = 0.6666667f;
		else if (quality == XESS_QUALITY_SETTING_BALANCED) inputScale = 0.5882353f;
		else if (quality == XESS_QUALITY_SETTING_PERFORMANCE) inputScale = 0.5f;

		xess_d3d12_init_params_t params{};
		params.outputResolution = { a_width, a_height };
		params.qualitySetting = quality;
		params.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
		params.creationNodeMask = 0;
		params.visibleNodeMask = 0;
		params.pTempBufferHeap = nullptr;
		params.pTempTextureHeap = nullptr;
		params.pPipelineLibrary = nullptr;

		r = xessD3D12Init(context, &params);
		if (r != XESS_RESULT_SUCCESS) {
			REX::LogError("xessD3D12Init failed ({})", XeSSResultToString(r));
			xessDestroyContext(context);
			context = nullptr;
			return false;
		}

		if (xessSetVelocityScale) {
			float inputWidthF = static_cast<float>(a_width) * inputScale;
			float inputHeightF = static_cast<float>(a_height) * inputScale;
			xessSetVelocityScale(context, inputWidthF, inputHeightF);
			velocityScaleX = inputWidthF;
			velocityScaleY = inputHeightF;
		}
		if (xessSetJitterScale) {
			xessSetJitterScale(context, 1.0f, 1.0f);
		}

		const char* qname = "Native";
		if (quality == XESS_QUALITY_SETTING_QUALITY) qname = "Quality";
		else if (quality == XESS_QUALITY_SETTING_BALANCED) qname = "Balanced";
		else if (quality == XESS_QUALITY_SETTING_PERFORMANCE) qname = "Performance";
		initialized = true;
		REX::LogDebug("XeSS context created ({}x{} {})", a_width, a_height, qname);
		return true;
	}

	bool XeSS::CreateSharedTexture(
		SharedTexture2D* a_out,
		uint32_t a_width,
		uint32_t a_height,
		DXGI_FORMAT a_format,
		uint32_t a_bindFlags,
		DXGI_FORMAT a_srvFormat,
		DXGI_FORMAT a_uavFormat)
	{
		if (!a_out || !device11 || !device) return false;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		if (FAILED(device11->CreateTexture2D(&desc, nullptr, &a_out->resource))) {
			REX::LogError("CreateTexture2D(shared) failed fmt={} flags=0x{:x}", static_cast<int>(a_format), a_bindFlags);
			return false;
		}

		IDXGIResource1* dxgiResource = nullptr;
		if (SUCCEEDED(a_out->resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)))) {
			HANDLE sharedHandle = nullptr;
			if (SUCCEEDED(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle))) {
				device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&a_out->resource12));
				CloseHandle(sharedHandle);
			}
			dxgiResource->Release();
		}

		if (!a_out->resource12) {
			REX::LogError("Failed to open shared handle for fmt={}", static_cast<int>(a_format));
			return false;
		}

		if (a_bindFlags & D3D11_BIND_SHADER_RESOURCE) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = (a_srvFormat != DXGI_FORMAT_UNKNOWN) ? a_srvFormat : a_format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(device11->CreateShaderResourceView(a_out->resource, &srvDesc, &a_out->srv))) {
				REX::LogError("CreateShaderResourceView(shared) failed");
			}
		}

		if (a_bindFlags & D3D11_BIND_UNORDERED_ACCESS) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = (a_uavFormat != DXGI_FORMAT_UNKNOWN) ? a_uavFormat : a_format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			if (FAILED(device11->CreateUnorderedAccessView(a_out->resource, &uavDesc, &a_out->uav))) {
				REX::LogError("CreateUnorderedAccessView(shared) failed");
			}
		}

		REX::LogDebug("shared texture {}x{} fmt={} bind=0x{:x}", a_width, a_height, static_cast<int>(a_format), a_bindFlags);
		return true;
	}

	void XeSS::Execute(
		SharedTexture2D* a_color,
		SharedTexture2D* a_motionVectors,
		SharedTexture2D* a_depth,
		SharedTexture2D* a_output,
		float a_jitterX,
		float a_jitterY,
		uint32_t a_width,
		uint32_t a_height,
		uint32_t a_resetHistory)
	{
		if (!initialized || !a_color || !a_motionVectors || !a_output) return;
		if (!a_color->resource12 || !a_motionVectors->resource12 || !a_output->resource12) return;
		if (!device || !commandQueue || !context4 || !d3d11Fence || !fence) return;
		if (!commandAllocators[0] || !commandAllocators[1] || !commandLists[0] || !commandLists[1]) return;
		if (!xessD3D12Execute) return;

		if (xessSetVelocityScale) {
			float wantX = static_cast<float>(a_width);
			float wantY = static_cast<float>(a_height);
			if (wantX != velocityScaleX || wantY != velocityScaleY) {
				xessSetVelocityScale(context, wantX, wantY);
				velocityScaleX = wantX;
				velocityScaleY = wantY;
			}
		}

		uint32_t idx = frameIndex;
		frameIndex ^= 1;

		if (fence && fenceEvent && fence->GetCompletedValue() < allocatorFenceValue[idx]) {
			fence->SetEventOnCompletion(allocatorFenceValue[idx], fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		context4->Signal(d3d11Fence, fenceValue);
		commandQueue->Wait(fence, fenceValue);
		fenceValue++;

		ID3D12CommandAllocator* allocator = commandAllocators[idx];
		ID3D12GraphicsCommandList* list = commandLists[idx];
		allocator->Reset();
		list->Reset(allocator, nullptr);

		XeSSResourceBarrier(list, a_color->resource12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		XeSSResourceBarrier(list, a_motionVectors->resource12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (a_depth && a_depth->resource12) {
			XeSSResourceBarrier(list, a_depth->resource12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		XeSSResourceBarrier(list, a_output->resource12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		xess_d3d12_execute_params_t exec{};
		exec.pColorTexture = a_color->resource12;
		exec.pVelocityTexture = a_motionVectors->resource12;
		exec.pDepthTexture = (a_depth && a_depth->resource12) ? a_depth->resource12 : nullptr;
		exec.pExposureScaleTexture = nullptr;
		exec.pResponsivePixelMaskTexture = nullptr;
		exec.pOutputTexture = a_output->resource12;
		exec.jitterOffsetX = -a_jitterX;
		exec.jitterOffsetY = -a_jitterY;
		exec.exposureScale = 1.0f;
		exec.resetHistory = a_resetHistory;
		exec.inputWidth = a_width;
		exec.inputHeight = a_height;
		exec.pDescriptorHeap = nullptr;
		exec.descriptorHeapOffset = 0;

		xess_result_t r = xessD3D12Execute(context, list, &exec);
		if (r != XESS_RESULT_SUCCESS) {
			REX::LogError("xessD3D12Execute failed ({})", XeSSResultToString(r));
		}

		XeSSResourceBarrier(list, a_output->resource12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		if (a_depth && a_depth->resource12) {
			XeSSResourceBarrier(list, a_depth->resource12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		XeSSResourceBarrier(list, a_motionVectors->resource12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		XeSSResourceBarrier(list, a_color->resource12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

		list->Close();

		ID3D12CommandList* lists[] = { list };
		commandQueue->ExecuteCommandLists(1, lists);

		commandQueue->Signal(fence, fenceValue);
		context4->Wait(d3d11Fence, fenceValue);
		allocatorFenceValue[idx] = fenceValue;
		fenceValue++;
	}

	void XeSS::TeardownD3D12()
	{
		if (fenceEvent) {
			CloseHandle(fenceEvent);
			fenceEvent = nullptr;
		}
		if (d3d11Fence) { d3d11Fence->Release(); d3d11Fence = nullptr; }
		if (fence) { fence->Release(); fence = nullptr; }
		for (uint32_t i = 0; i < 2; i++) {
			if (commandLists[i]) { commandLists[i]->Release(); commandLists[i] = nullptr; }
			if (commandAllocators[i]) { commandAllocators[i]->Release(); commandAllocators[i] = nullptr; }
		}
		if (commandQueue) { commandQueue->Release(); commandQueue = nullptr; }
		if (device) { device->Release(); device = nullptr; }
		if (context4) { context4->Release(); context4 = nullptr; }
		if (device5) { device5->Release(); device5 = nullptr; }
		device11 = nullptr;
	}

	void XeSS::Destroy()
	{
		if (context && xessDestroyContext) {
			xessDestroyContext(context);
			context = nullptr;
		}

		if (fenceEvent) {
			CloseHandle(fenceEvent);
			fenceEvent = nullptr;
		}

		if (d3d11Fence) { d3d11Fence->Release(); d3d11Fence = nullptr; }
		if (fence) { fence->Release(); fence = nullptr; }
		for (uint32_t i = 0; i < 2; i++) {
			if (commandLists[i]) { commandLists[i]->Release(); commandLists[i] = nullptr; }
			if (commandAllocators[i]) { commandAllocators[i]->Release(); commandAllocators[i] = nullptr; }
		}
		if (commandQueue) { commandQueue->Release(); commandQueue = nullptr; }
		if (device) { device->Release(); device = nullptr; }
		if (context4) { context4->Release(); context4 = nullptr; }
		if (device5) { device5->Release(); device5 = nullptr; }
		device11 = nullptr;

		if (module) {
			FreeLibrary(module);
			module = nullptr;
		}

		initialized = false;
		loaded = false;
		disabled = false;
		velocityScaleX = 0.0f;
		velocityScaleY = 0.0f;
	}
}
#endif
