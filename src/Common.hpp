#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <d3d12.h>

#ifndef F4R_HAS_DLSS
	#define F4R_HAS_DLSS 1
#endif
#ifndef F4R_HAS_FSR3
	#define F4R_HAS_FSR3 1
#endif
#ifndef F4R_HAS_XESS
	#define F4R_HAS_XESS 1
#endif
#ifndef F4R_HAS_STREAMLINE
	#define F4R_HAS_STREAMLINE 1
#endif
#ifndef F4R_DEFAULT_Method
	#define F4R_DEFAULT_Method 1
#endif

#if F4R_HAS_DLSS && F4R_HAS_FSR3 && F4R_HAS_XESS
	#define F4R_HAS_MULTI 1
#else
	#define F4R_HAS_MULTI 0
#endif

namespace RE::BSGraphics
{
	class RenderTargetManager;
}

namespace F4R_Upscaling
{
	inline bool IsAE()
	{
		return F4SE::IsRuntimeOnlyAE();
	}

	inline bool IsNG()
	{
		return F4SE::IsRuntimeOnlyNG();
	}

	inline bool IsOG()
	{
		return !IsAE() && !IsNG();
	}

	inline void GetCameraNearFar(float& a_near, float& a_far)
	{
		if (IsOG()) {
			a_near = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0xe281 } }.GetAddress());
			a_far = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0xea19d } }.GetAddress());
		} else {
			a_near = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x296532 } }.GetAddress());
			a_far = *reinterpret_cast<float*>(
				REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x296533 } }.GetAddress());
		}
	}

	inline float& GetDynWidthRatio(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static const std::ptrdiff_t OG_OFFSET = 0xF88;
		static const std::ptrdiff_t AE_OFFSET = 0xFB8;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline float& GetDynHeightRatio(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static const std::ptrdiff_t OG_OFFSET = 0xF8C;
		static const std::ptrdiff_t AE_OFFSET = 0xFBC;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	inline bool& GetDynResActivated(RE::BSGraphics::RenderTargetManager& a_rtMgr)
	{
		static const std::ptrdiff_t OG_OFFSET = 0xFA8;
		static const std::ptrdiff_t AE_OFFSET = 0xFD8;
		std::ptrdiff_t off = (IsAE() || IsNG()) ? AE_OFFSET : OG_OFFSET;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(&a_rtMgr) + off);
	}

	namespace RenderTarget
	{
		enum : std::uint32_t
		{
			kFrameBuffer = 0,
			kMainTemp = 4,
			kSSRRaw = 7,
			kSSRBlurred = 8,
			kSSRBlurredExtra = 9,
			kSSRDirection = 10,
			kSSRMask = 11,
			kGbufferNormal = 20,
			kMotionVectors = 29,
			kSSAOFinal = 25,
		};
	}

	namespace DepthStencil
	{
		enum : std::uint32_t
		{
			kMain = 2,
		};
	}

	enum class Method: int32_t
	{
		Off = 0,
		FSR3 = 1,
		DLSS = 2,
		XeSS = 3
	};

	struct Settings
	{
		int32_t iMethod = F4R_DEFAULT_Method;

		float fSharpness = 0.5f;
		float fAnisotropicMipBias = -0.0001f;
		int32_t iQualityMode = 0;

		float fQualityScale = 0.65f;
		float fBalancedScale = 0.59f;
		float fPerformanceScale = 0.5f;

		bool bEnableReflex = false;
		bool bReflexBoost = false;
		bool bReflexUseFPSLimit = false;
		float fReflexFPSLimit = 60.0f;
	};

	struct Texture2D
	{
		ID3D11Texture2D* resource = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;

		~Texture2D()
		{
			if (uav) uav->Release();
			if (srv) srv->Release();
			if (rtv) rtv->Release();
			if (resource) resource->Release();
		}
	};

	struct SamplerStates
	{
		ID3D11SamplerState* a[320];
	};

	struct SharedTexture2D
	{
		ID3D11Texture2D* resource = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D12Resource* resource12 = nullptr;

		~SharedTexture2D()
		{
			if (uav) uav->Release();
			if (srv) srv->Release();
			if (resource) resource->Release();
			if (resource12) resource12->Release();
		}
	};
}

extern bool g_enbLoaded;
extern bool g_enbExtractionFailed;
extern ID3D11Device* g_realDevice;
extern ID3D11DeviceContext* g_realContext;

void ExtractRealD3D11();

namespace F4R_Upscaling
{
	inline std::string GetModuleDirectory()
	{
		char buf[MAX_PATH] = {};
		GetModuleFileNameA(GetModuleHandleA(F4R_MODULE_NAME), buf, MAX_PATH);
		std::string path(buf);
		return path.substr(0, path.rfind('\\') + 1);
	}

	inline std::string GetPluginINIPath()
	{
		return GetModuleDirectory() + F4R_MODULE_NAME + ".ini";
	}

	inline std::uint64_t GetFileWriteTime(const std::string& a_path)
	{
		WIN32_FILE_ATTRIBUTE_DATA attrs{};
		if (!GetFileAttributesExA(a_path.c_str(), GetFileExInfoStandard, &attrs)) return 0;
		return (static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwHighDateTime) << 32) |
			static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwLowDateTime);
	}

	inline SamplerStates* GetGlobalSamplers()
	{
		std::uintptr_t id = IsOG() ? 0xad18ull : 0x294447ull;
		return reinterpret_cast<SamplerStates*>(
			REL::Relocation<std::uintptr_t>{ REL::Id<>{ id } }.GetAddress());
	}

	inline ID3D11Device* GetRenderer()
	{
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) return nullptr;
		return (g_enbLoaded && g_realDevice) ? g_realDevice : reinterpret_cast<ID3D11Device*>(rd->device);
	}

	inline ID3D11DeviceContext* GetImmediateContext()
	{
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) return nullptr;
		return (g_enbLoaded && g_realContext) ? g_realContext : reinterpret_cast<ID3D11DeviceContext*>(rd->context);
	}
}
