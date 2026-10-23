#include "Config.hpp"
#include "PCH.hpp"
#include "Platform.hpp"
#include "Upscaling.hpp"
#include "WorldTransformFix.hpp"
#if F4R_HAS_STREAMLINE
#include "Streamline.hpp"
#endif
#if F4R_HAS_XESS
#include "XeSS.hpp"
#endif
#include <Detours.h>

#include <filesystem>

bool g_enbLoaded = false;
bool g_enbExtractionFailed = false;
ID3D11Device* g_realDevice = nullptr;
ID3D11DeviceContext* g_realContext = nullptr;

void ExtractRealD3D11()
{
	auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
	if (!rendererData) {
		return;
	}

	auto* wrappedDev = reinterpret_cast<ID3D11Device*>(rendererData->device);
	auto* wrappedCtx = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	if (!wrappedDev || !wrappedCtx) {
		return;
	}

	const F4R_Upscaling::Platform::ModuleHandle d3d11Module = F4R_Upscaling::Platform::ModuleByName(L"d3d11.dll");
	if (!d3d11Module) {
		return;
	}

	void* vtable = *reinterpret_cast<void**>(wrappedDev);
	if (F4R_Upscaling::Platform::ModuleFromAddress(vtable) != d3d11Module) {
		return;
	}

	constexpr std::ptrdiff_t devOffset = F4R_Upscaling::Config::kENBDeviceOffset;
	constexpr std::ptrdiff_t ctxOffset = F4R_Upscaling::Config::kENBContextOffset;

	g_realDevice = *reinterpret_cast<ID3D11Device**>(reinterpret_cast<std::uint8_t*>(wrappedDev) + devOffset);
	g_realContext = *reinterpret_cast<ID3D11DeviceContext**>(reinterpret_cast<std::uint8_t*>(wrappedCtx) + ctxOffset);

	if (g_realDevice && g_realContext && g_realDevice != wrappedDev) {
		g_realDevice->AddRef();
		g_realDevice->Release();
		g_realContext->AddRef();
		g_realContext->Release();
		REX::LogInformation("ENB D3D11 proxy bypassed (dev+0x{:X}, ctx+0x{:X})", devOffset, ctxOffset);
	}
	else {
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
		using ENBGetSDKVersionFunc = long (*)();

		for (const auto module : F4R_Upscaling::Platform::ProcessModules()) {
			const auto proc = F4R_Upscaling::Platform::ResolveExport<ENBGetSDKVersionFunc>(module, "ENBGetSDKVersion");
			if (proc) {
				const long version = proc();
				if ((version / 1000) % 10 == 1) {
					REX::LogInformation("ENB detected (SDK v{}.{})",
						version / 1000, version % 1000);
					return true;
				}
			}
		}

		return false;
	}

	F4R_Upscaling::Method GetConfiguredMode()
	{
		if constexpr (F4R_Upscaling::kSoleBackend != F4R_Upscaling::Method::Off) {
			return F4R_Upscaling::kSoleBackend;
		}
		else {
			return static_cast<F4R_Upscaling::Method>(F4R_Upscaling::Config::GetSingleton().GetInt("Settings", "iMethod", static_cast<std::int32_t>(F4R_Upscaling::kDefaultMethod)));
		}
	}

	void LoadConfig()
	{
		auto& config = F4R_Upscaling::Config::GetSingleton();
		const std::filesystem::path iniPath = F4R_Upscaling::Platform::PluginINIPath();
		if (config.Load(iniPath)) {
			return;
		}

		config.WriteDefaults(iniPath);
		if (!config.Load(iniPath)) {
			REX::LogCritical("Could not load {}", iniPath.string());
		}
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->GetType()) {
			case F4SE::MessagingInterface::MessageType::kPostLoad: {
				g_enbLoaded = IsENBLoaded();
				F4R_Upscaling::Upscaling::GetSingleton().LoadSettings();
				F4R_Upscaling::Upscaling::GetSingleton().Init();
				F4R_Upscaling::WorldTransformFix::InstallHooks();
				break;
			}
			case F4SE::MessagingInterface::MessageType::kPostLoadGame:
			case F4SE::MessagingInterface::MessageType::kNewGame: {
				F4R_Upscaling::Upscaling::GetSingleton().InvalidateFlareDepth();
				F4R_Upscaling::Upscaling::GetSingleton().RequestReset();
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
			if (streamline.interposer.IsValid()) {
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
		const auto module = F4R_Upscaling::Platform::ExecutableModule();
		auto result = Detours::IATHook(
			module, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
			reinterpret_cast<std::uintptr_t>(&Hook_D3D11CreateDeviceAndSwapChain));
		g_originalD3D11CreateDeviceAndSwapChain = reinterpret_cast<D3D11CreateDeviceAndSwapChainFunc>(result);

		if (!result) {
			REX::LogWarning("D3D11CreateDeviceAndSwapChain IAT hook FAILED");
		}
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
	if (GetConfiguredMode() == F4R_Upscaling::Method::XeSS) {
		F4R_Upscaling::XeSS::GetSingleton().Load();
	}
#endif

	return true;
}
