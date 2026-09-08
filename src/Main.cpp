#include "PCH.hpp"
#include "Upscaling.hpp"
#if F4R_HAS_DLSS
#include "Streamline.hpp"
#endif
#if F4R_HAS_XESS
#include "XeSS.hpp"
#endif
#include <Detours.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")

bool g_enbLoaded = false;
bool g_enbExtractionFailed = false;
ID3D11Device* g_realDevice = nullptr;
ID3D11DeviceContext* g_realContext = nullptr;

namespace { std::string GetPluginINIPath(); }

void ExtractRealD3D11()
{
	auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
	if (!rendererData) return;

	auto* wrappedDev = reinterpret_cast<ID3D11Device*>(rendererData->device);
	auto* wrappedCtx = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	if (!wrappedDev || !wrappedCtx) return;

	HMODULE enbMod = GetModuleHandleA("d3d11.dll");
	if (!enbMod) return;

	MODULEINFO enbInfo;
	if (!GetModuleInformation(GetCurrentProcess(), enbMod, &enbInfo, sizeof(enbInfo))) return;

	void* vtable = *(void**)wrappedDev;
	uintptr_t enbStart = (uintptr_t)enbMod;
	uintptr_t enbEnd = enbStart + enbInfo.SizeOfImage;
	if ((uintptr_t)vtable < enbStart || (uintptr_t)vtable >= enbEnd) return;

	const std::string iniPath = GetPluginINIPath();
	const std::ptrdiff_t devOffset = GetPrivateProfileIntA("ENB", "DeviceOffset", 0x28, iniPath.c_str());
	const std::ptrdiff_t ctxOffset = GetPrivateProfileIntA("ENB", "ContextOffset", 0x6C20, iniPath.c_str());

	g_realDevice = *(ID3D11Device**)((char*)wrappedDev + devOffset);
	g_realContext = *(ID3D11DeviceContext**)((char*)wrappedCtx + ctxOffset);

	if (g_realDevice && g_realContext && g_realDevice != wrappedDev) {
		g_realDevice->AddRef();
		g_realDevice->Release();
		g_realContext->AddRef();
		g_realContext->Release();
		REX::LogInformation("ENB D3D11 proxy bypassed (dev+0x{:X}, ctx+0x{:X})", devOffset, ctxOffset);
	} else {
		REX::LogWarning("ENB bypass failed: offsets may be wrong for this ENB version");
		g_realDevice = nullptr;
		g_realContext = nullptr;
		g_enbExtractionFailed = true;
	}
}

namespace
{
	bool IsENBLoaded()
	{
		HANDLE process = GetCurrentProcess();
		HMODULE modules[1024];
		DWORD needed = 0;

		if (!K32EnumProcessModules(process, modules, sizeof(modules), &needed))
			return false;

		DWORD count = needed / sizeof(HMODULE);
		for (DWORD i = 0; i < count; i++) {
			auto proc = GetProcAddress(modules[i], "ENBGetSDKVersion");
			if (proc) {
				using ENBGetSDKVersionFunc = long (*)();
				long version = reinterpret_cast<ENBGetSDKVersionFunc>(proc)();
				if ((version / 1000) % 10 == 1) {
					REX::LogInformation("ENB detected (SDK v{}.{})",
						version / 1000, version % 1000);
					return true;
				}
			}
		}

		return false;
	}

	std::string GetPluginINIPath()
	{
		static std::string path;
		if (path.empty()) {
			char buf[MAX_PATH];
			GetModuleFileNameA(GetModuleHandleA(F4R_MODULE_NAME), buf, sizeof(buf));
			path = buf;
			path = path.substr(0, path.rfind('\\') + 1);
			path += F4R_MODULE_NAME;
			path += ".ini";
		}
		return path;
	}

	int32_t GetConfiguredMode()
	{
#if F4R_HAS_DLSS && !F4R_HAS_FSR3 && !F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::DLSS);
#elif !F4R_HAS_DLSS && F4R_HAS_FSR3 && !F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::FSR3);
#elif !F4R_HAS_DLSS && !F4R_HAS_FSR3 && F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::XeSS);
#else
		return GetPrivateProfileIntA("Settings", "iMethod", F4R_DEFAULT_Method, GetPluginINIPath().c_str());
#endif
	}

	void CreateDefaultINI()
	{
		std::string path = GetPluginINIPath();
		if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;

#if !F4R_HAS_FSR3 && F4R_HAS_DLSS

		std::string content =
			"[Settings]\n"
			"; Settings apply in-game instantly without restarting.\n"
			"\n"
			"; RCAS sharpness - 0.0 = no sharpening, 1.0 = max\n"
			"fSharpness=0.5\n"
			"\n"
			"; NVIDIA Reflex to reduce game latency\n"
			"bEnableReflex=0\n"
			"bReflexBoost=0\n"
			"\n"
			"; 0 = Native (DLAA, default), 1 = Quality DLSS (0.66x, 1.5x), 2 = Balanced (0.58x, 1.7x), 3 = Performance (0.5x, 2x)\n"
			"; ENB forces Native\n"
			"iQualityMode=0\n"
			"\n"
			"[Advanced]\n"
			"; Reflex FPS limiter\n"
			"bReflexUseFPSLimit=0\n"
			"fReflexFPSLimit=60\n"
			"\n"
			"; (*) -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; 0.0 = allows other mods to override samplers (ranging from -2.0 to 0.0)\n"
			"fAnisotropicMipBias=-0.0001\n";

#elif !F4R_HAS_DLSS && F4R_HAS_FSR3

		std::string content =
			"[Settings]\n"
			"; Settings apply in-game instantly without restarting.\n"
			"\n"
			"; RCAS sharpness - 0.0 = no sharpening, 1.0 = max\n"
			"fSharpness=0.5\n"
			"\n"
			"; 0 = Native (FSR 1.0x), 1 = Quality (0.66x, 1.5x), 2 = Balanced (0.58x, 1.7x), 3 = Performance (0.5x, 2x)\n"
			"; ENB forces Native\n"
			"iQualityMode=0\n"
			"\n"
			"[Advanced]\n"
			"; (*) -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; 0.0 = allows other mods to override samplers (ranging from -2.0 to 0.0)\n"
			"fAnisotropicMipBias=-0.0001\n"
			"\n"
			"[ENB]\n"
			"; ENB D3D11 proxy bypass offsets\n"
			"DeviceOffset=0x28\n"
			"ContextOffset=0x6C20\n";

#elif !F4R_HAS_DLSS && !F4R_HAS_FSR3 && F4R_HAS_XESS

		std::string content =
			"[Settings]\n"
			"; Settings marked with (*) apply in-game instantly without restarting.\n"
			"\n"
			"; RCAS sharpness - 0.0 = no sharpening, 1.0 = max\n"
			"fSharpness=0.5\n"
			"\n"
			"; 0 = Native (XeSS 1.0x, default), 1 = Quality (0.66x, 1.5x), 2 = Balanced (0.58x, 1.7x), 3 = Performance (0.5x, 2x)\n"
			"; ENB forces Native\n"
			"iQualityMode=0\n"
			"\n"
			"[Advanced]\n"
			"; (*) -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; 0.0 = allows other mods to override samplers (ranging from -2.0 to 0.0)\n"
			"fAnisotropicMipBias=-0.0001\n"
			"\n"
			"[ENB]\n"
			"; ENB D3D11 proxy bypass offsets\n"
			"DeviceOffset=0x28\n"
			"ContextOffset=0x6C20\n";

#else

		std::string content =
			"[Settings]\n"
			"; Settings marked with (*) apply in-game instantly without restarting.\n"
			"; (*) Applies to DLSS & FSR3 only. For XeSS, only fAnisotropicMipBias works in realtime.\n"
			"\n"
			"; FSR3 - Nvidia & AMD GPU, DLSS - RTX Only, XeSS - Intel & any GPU\n"
			"; 1 - FSR3, 2 - DLSS, 3 - XeSS, 0 - Off\n"
			"iMethod=1\n"
			"\n"
			"; (*) RCAS sharpness - 0.0 = no sharpening, 1.0 = max\n"
			"fSharpness=0.5\n"
			"\n"
			"; (*) NVIDIA Reflex to reduce game latency (DLSS mode only)\n"
			"bEnableReflex=0\n"
			"bReflexBoost=0\n"
			"\n"
			"; (*) 0 = Native (default), 1 = Quality (0.66x, 1.5x), 2 = Balanced (0.58x, 1.7x), 3 = Performance (0.5x, 2x)\n"
			"; ENB forces Native\n"
			"iQualityMode=0\n"
			"\n"
			"[Advanced]\n"
			"; (*) Reflex FPS limiter (DLSS mode only)\n"
			"bReflexUseFPSLimit=0\n"
			"fReflexFPSLimit=60\n"
			"\n"
			"; (*) -0.0001 = default safety net to preserve samplers from being overridden\n"
			"; 0.0 = allows other mods to override samplers (ranging from -2.0 to 0.0)\n"
			"fAnisotropicMipBias=-0.0001\n"
			"\n"
			"[ENB]\n"
			"; ENB D3D11 proxy bypass offsets (FSR3 & XeSS)\n"
			"DeviceOffset=0x28\n"
			"ContextOffset=0x6C20\n";

#endif

		HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			DWORD written;
			WriteFile(file, content.c_str(), static_cast<DWORD>(content.size()), &written, nullptr);
			CloseHandle(file);
		}
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->GetType()) {
		case F4SE::MessagingInterface::MessageType::kPostLoad:
			{
				g_enbLoaded = IsENBLoaded();
				CreateDefaultINI();
				F4R_Upscaling::Upscaling::GetSingleton().LoadSettings(GetPluginINIPath());
				F4R_Upscaling::Upscaling::GetSingleton().Init();
				break;
			}
		case F4SE::MessagingInterface::MessageType::kPostLoadGame:
		case F4SE::MessagingInterface::MessageType::kNewGame:
			{
				F4R_Upscaling::Upscaling::GetSingleton().InvalidateFlareDepth();
				F4R_Upscaling::Upscaling::GetSingleton().RequestReset();
				F4R_Upscaling::Upscaling::GetSingleton().PollRuntimeSettings();
				break;
			}
		default:
			break;
		}
	}
}

namespace F4R_Upscaling
{
	using D3D11CreateDeviceAndSwapChainFunc = HRESULT(WINAPI*)(
		IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
		const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
		D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

	static D3D11CreateDeviceAndSwapChainFunc g_originalD3D11CreateDeviceAndSwapChain = nullptr;

	HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(
		IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
		const D3D_FEATURE_LEVEL* a_featureLevels, UINT a_featureLevelsCount, UINT a_sdkVersion,
		const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
		D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_immediateContext)
	{
		HRESULT hr = g_originalD3D11CreateDeviceAndSwapChain(
			a_adapter, a_driverType, a_software, a_flags,
			a_featureLevels, a_featureLevelsCount, a_sdkVersion,
			a_swapChainDesc, a_swapChain, a_device,
			a_featureLevel, a_immediateContext);

		if (SUCCEEDED(hr)) {
#if F4R_HAS_DLSS
			auto& streamline = Streamline::GetSingleton();
			if (streamline.interposer) {
				streamline.Initialize();

				if (!g_enbLoaded && streamline.slUpgradeInterface && a_swapChain) {
					streamline.slUpgradeInterface(reinterpret_cast<void**>(a_swapChain));
				}

				if (streamline.slSetD3DDevice && a_device && *a_device) {
					streamline.slSetD3DDevice(*a_device);
				}

				streamline.CheckFeatures(a_adapter);

				if (streamline.featureDLSS) {
					streamline.PostDevice();
				}
			}
#endif
		}

		return hr;
	}

	void InstallD3D11Hook()
	{
		uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
		auto result = Detours::IATHook(
			module, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
			reinterpret_cast<uintptr_t>(&Hook_D3D11CreateDeviceAndSwapChain));
		g_originalD3D11CreateDeviceAndSwapChain = reinterpret_cast<D3D11CreateDeviceAndSwapChainFunc>(result);

		if (result)
			REX::LogDebug("D3D11CreateDeviceAndSwapChain IAT hook installed");
		else
			REX::LogWarning("D3D11CreateDeviceAndSwapChain IAT hook FAILED");
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::InitInfo initInfo;
	initInfo.logFormat = "%v";
	F4SE::Init(a_f4se, initInfo);

	REX::LogInformation("F4SE {} & {}", F4SE::GetF4SEVersion(), F4SE::GetRuntimeVersion());

	auto messaging = F4SE::GetMessagingInterface();
	messaging->RegisterListener(REX::NotNull{ &OnF4SEMessage });

#if F4R_HAS_DLSS
	{
		int32_t mode = GetConfiguredMode();
		if (mode == static_cast<int32_t>(F4R_Upscaling::Method::DLSS)) {
			F4R_Upscaling::Streamline::GetSingleton().LoadInterposer();
			F4R_Upscaling::InstallD3D11Hook();
		}
	}
#endif
#if F4R_HAS_XESS
	if (GetConfiguredMode() == static_cast<int32_t>(F4R_Upscaling::Method::XeSS)) {
		F4R_Upscaling::XeSS::GetSingleton().Load();
	}
#endif

	return true;
}
