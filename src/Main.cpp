#include "PCH.hpp"
#include "Config.hpp"
#include "Upscaling.hpp"
#if F4R_HAS_STREAMLINE
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

	constexpr std::ptrdiff_t devOffset = F4R_Upscaling::Config::kENBDeviceOffset;
	constexpr std::ptrdiff_t ctxOffset = F4R_Upscaling::Config::kENBContextOffset;

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

	int32_t GetConfiguredMode()
	{
#if F4R_HAS_DLSS && !F4R_HAS_FSR3 && !F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::DLSS);
#elif !F4R_HAS_DLSS && F4R_HAS_FSR3 && !F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::FSR3);
#elif !F4R_HAS_DLSS && !F4R_HAS_FSR3 && F4R_HAS_XESS
		return static_cast<int32_t>(F4R_Upscaling::Method::XeSS);
#else
		return F4R_Upscaling::Config::GetSingleton().GetInt("Settings", "iMethod", F4R_DEFAULT_Method);
#endif
	}

	void LoadConfig()
	{
		auto& config = F4R_Upscaling::Config::GetSingleton();
		const std::string iniPath = F4R_Upscaling::GetPluginINIPath();
		if (config.Load(iniPath)) return;

		config.WriteDefaults(iniPath);
		if (!config.Load(iniPath)) {
			REX::LogCritical("Could not load {}", iniPath);
		}
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->GetType()) {
		case F4SE::MessagingInterface::MessageType::kPostLoad:
			{
				g_enbLoaded = IsENBLoaded();
				F4R_Upscaling::Upscaling::GetSingleton().LoadSettings();
				F4R_Upscaling::Upscaling::GetSingleton().Init();
				break;
			}
		case F4SE::MessagingInterface::MessageType::kPostLoadGame:
		case F4SE::MessagingInterface::MessageType::kNewGame:
			{
				F4R_Upscaling::Upscaling::GetSingleton().InvalidateFlareDepth();
				F4R_Upscaling::Upscaling::GetSingleton().RequestReset();
				F4R_Upscaling::Upscaling::GetSingleton().PollSettingsChanged();
				break;
			}
		default:
			break;
		}
	}
}

#if F4R_HAS_STREAMLINE
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

				streamline.PostDevice();
			}
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
#endif

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::InitInfo initInfo;
	initInfo.logFormat = "%v";
	F4SE::Init(a_f4se, initInfo);

	REX::LogInformation("F4SE {} & {}", F4SE::GetF4SEVersion(), F4SE::GetRuntimeVersion());

	LoadConfig();

	auto messaging = F4SE::GetMessagingInterface();
	messaging->RegisterListener(REX::NotNull{ &OnF4SEMessage });

#if F4R_HAS_STREAMLINE
	F4R_Upscaling::Streamline::GetSingleton().LoadInterposer();
	F4R_Upscaling::InstallD3D11Hook();
#endif

#if F4R_HAS_XESS
	if (GetConfiguredMode() == static_cast<int32_t>(F4R_Upscaling::Method::XeSS)) {
		F4R_Upscaling::XeSS::GetSingleton().Load();
	}
#endif

	return true;
}
