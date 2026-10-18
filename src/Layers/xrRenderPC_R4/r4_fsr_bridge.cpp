// Built without the precompiled header: this file only needs Windows SDK D3D headers and ffx_api,
// and keeping engine headers out avoids macro clashes with d3d12.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FidelityFX/upscalers/include/ffx_upscale.h"
#include "FidelityFX/api/include/dx12/ffx_api_dx12.h"

#include "r4_fsr_bridge.h"

using Microsoft::WRL::ComPtr;

namespace
{
FsrBridge::LogFn g_log = nullptr;

void Log(const char* fmt, ...)
{
	if (!g_log)
		return;

	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	g_log(buf);
}

void FfxMessage(uint32_t type, const wchar_t* message)
{
	char buf[512];
	WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf), nullptr, nullptr);
	Log("[FSR] ffx %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

// Engine RTs may be typeless; FSR needs a typed format to build its views.
DXGI_FORMAT TypedFormat(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return fmt;
	}
}

float Halton(int index, int base)
{
	float f = 1.0f, result = 0.0f;
	for (int cur = index; cur > 0;)
	{
		f /= (float)base;
		result += f * (float)(cur % base);
		cur = cur / base;
	}
	return result;
}

// D24S8 -> R32F, so the depth can cross into D3D12 (depth formats cannot be shared). The destination may be
// larger than the source (dynamic resolution); only the source-sized top-left region is written.
const char g_depth_cs[] =
	"Texture2D<float> src : register(t0);\n"
	"RWTexture2D<float> dst : register(u0);\n"
	"[numthreads(8, 8, 1)]\n"
	"void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"	uint w, h;\n"
	"	src.GetDimensions(w, h);\n"
	"	if (id.x >= w || id.y >= h) return;\n"
	"	dst[id.xy] = src.Load(int3(id.xy, 0));\n"
	"}\n";

// Camera motion vectors from depth, for games without per-object motion vectors (no Screen Space Shaders).
// Reprojects each pixel with (jittered current clip -> previous clip) and writes previous - current in UV,
// with the current position unjittered. HUD depth (rmNear range) moves with the camera: zero motion.
const char g_mv_cs[] =
	"Texture2D<float> depth : register(t0);\n"
	"RWTexture2D<float2> mv : register(u0);\n"
	"cbuffer params : register(b0)\n"
	"{\n"
	"	row_major float4x4 reproject;\n"
	"	float2 inv_size;\n"
	"	float2 jitter_uv;\n"
	"	float hud_depth;\n"
	"	float3 pad;\n"
	"};\n"
	"[numthreads(8, 8, 1)]\n"
	"void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"	uint w, h;\n"
	"	depth.GetDimensions(w, h);\n"
	"	if (id.x >= w || id.y >= h) return;\n"
	"	float d = depth.Load(int3(id.xy, 0));\n"
	"	float2 uv = (float2(id.xy) + 0.5) * inv_size;\n"
	"	float2 m = 0;\n"
	"	if (d > hud_depth)\n"
	"	{\n"
	"		float4 p = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1), reproject);\n"
	"		p.xy /= p.w;\n"
	"		m = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) - (uv - jitter_uv);\n"
	"	}\n"
	"	mv[id.xy] = m;\n"
	"}\n";

struct MvParams
{
	float reproject[16];
	float inv_size[2];
	float jitter_uv[2];
	float hud_depth;
	float pad[3];
};

const UINT kFrames = 3;

struct SharedTex
{
	ComPtr<ID3D11Texture2D> t11;
	ComPtr<ID3D12Resource> t12;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	bool Matches(UINT _w, UINT _h, DXGI_FORMAT _fmt) const { return t11 && w == _w && h == _h && fmt == _fmt; }
	void Reset() { t11.Reset(); t12.Reset(); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }
};
}

struct FsrBridge::Impl
{
	HMODULE d3d12_dll = nullptr;
	HMODULE ffx_dll = nullptr;

	PfnFfxCreateContext ffxCreateContext = nullptr;
	PfnFfxDestroyContext ffxDestroyContext = nullptr;
	PfnFfxQuery ffxQuery = nullptr;
	PfnFfxDispatch ffxDispatch = nullptr;

	ComPtr<ID3D11Device5> dev11;
	ComPtr<ID3D11DeviceContext4> ctx11;
	ComPtr<ID3D11Fence> fence11;

	ComPtr<ID3D12Device> dev12;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> alloc[kFrames];
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence12;
	UINT64 fence_value = 0;
	UINT64 alloc_fence[kFrames] = {};
	UINT frame = 0;
	HANDLE event = nullptr;

	SharedTex color, depth, mv, output;

	ComPtr<ID3D11ComputeShader> depth_cs;
	ComPtr<ID3D11ComputeShader> mv_cs;
	ComPtr<ID3D11Buffer> mv_cb;
	ComPtr<ID3D11UnorderedAccessView> mv_uav;
	ComPtr<ID3D11Texture2D> depth_copy;
	ComPtr<ID3D11ShaderResourceView> depth_copy_srv;
	ComPtr<ID3D11UnorderedAccessView> depth_uav;

	// One FSR context (and so one history) per view: the main camera and the 3D scope camera
	struct UpscaleContext
	{
		ffxContext ctx = nullptr;
		UINT render_w = 0, render_h = 0, display_w = 0, display_h = 0;
		uint32_t flags = 0;
	};
	UpscaleContext views[FsrBridge::kViews];

	void WaitGpuIdle()
	{
		if (!fence12 || fence12->GetCompletedValue() >= fence_value)
			return;
		fence12->SetEventOnCompletion(fence_value, event);
		WaitForSingleObject(event, INFINITE);
	}

	void DestroyContext(UpscaleContext& v)
	{
		if (v.ctx)
		{
			WaitGpuIdle();
			ffxDestroyContext(&v.ctx, nullptr);
			v.ctx = nullptr;
		}
	}

	void DestroyContexts()
	{
		for (UpscaleContext& v : views)
			DestroyContext(v);
	}

	bool CreateShared(SharedTex& s, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind)
	{
		s.Reset();

		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = bind;
		d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = dev11->CreateTexture2D(&d, nullptr, &s.t11);
		if (FAILED(hr))
		{
			Log("[FSR] ! CreateTexture2D(shared %ux%u fmt %d) failed: 0x%08x", w, h, (int)fmt, (unsigned)hr);
			return false;
		}

		ComPtr<IDXGIResource1> dxgi_res;
		HANDLE handle = nullptr;
		if (FAILED(s.t11.As(&dxgi_res)) ||
			FAILED(dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
		{
			Log("[FSR] ! CreateSharedHandle failed");
			s.Reset();
			return false;
		}

		hr = dev12->OpenSharedHandle(handle, IID_PPV_ARGS(&s.t12));
		CloseHandle(handle);
		if (FAILED(hr))
		{
			Log("[FSR] ! D3D12 OpenSharedHandle(texture) failed: 0x%08x", (unsigned)hr);
			s.Reset();
			return false;
		}

		s.w = w;
		s.h = h;
		s.fmt = fmt;
		return true;
	}

	// Inputs are sized for the largest render size (the display size), so the render resolution can change
	// every frame (FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION): each frame's inputs go to the top-left corner.
	bool EnsureResources(UINT rw, UINT rh, DXGI_FORMAT color_fmt, DXGI_FORMAT mv_fmt, UINT dw, UINT dh, DXGI_FORMAT out_fmt)
	{
		const UINT rt_srv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		const UINT rt_srv_uav = rt_srv | D3D11_BIND_UNORDERED_ACCESS;

		const bool shared_changed = !color.Matches(dw, dh, color_fmt) || !mv.Matches(dw, dh, mv_fmt) ||
			!depth.Matches(dw, dh, DXGI_FORMAT_R32_FLOAT) || !output.Matches(dw, dh, out_fmt);
		if (shared_changed)
		{
			// D3D12 may still be reading the old textures.
			WaitGpuIdle();
			depth_uav.Reset();
			mv_uav.Reset();

			if (!CreateShared(color, dw, dh, color_fmt, rt_srv) ||
				!CreateShared(mv, dw, dh, mv_fmt, rt_srv_uav) ||
				!CreateShared(depth, dw, dh, DXGI_FORMAT_R32_FLOAT, rt_srv_uav) ||
				!CreateShared(output, dw, dh, out_fmt, rt_srv_uav))
				return false;

			if (FAILED(dev11->CreateUnorderedAccessView(depth.t11.Get(), nullptr, &depth_uav)) ||
				FAILED(dev11->CreateUnorderedAccessView(mv.t11.Get(), nullptr, &mv_uav)))
			{
				Log("[FSR] ! failed to create depth / motion vector UAVs");
				return false;
			}
		}

		// Depth-stencil resources can only be copied whole, so this one follows the render size
		D3D11_TEXTURE2D_DESC cur = {};
		if (depth_copy)
			depth_copy->GetDesc(&cur);
		if (depth_copy && cur.Width == rw && cur.Height == rh)
			return true;

		depth_copy_srv.Reset();
		depth_copy.Reset();

		D3D11_TEXTURE2D_DESC d = {};
		d.Width = rw;
		d.Height = rh;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R24G8_TYPELESS;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev11->CreateTexture2D(&d, nullptr, &depth_copy)))
		{
			Log("[FSR] ! failed to create depth copy texture");
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		if (FAILED(dev11->CreateShaderResourceView(depth_copy.Get(), &srv, &depth_copy_srv)))
		{
			Log("[FSR] ! failed to create depth conversion SRV");
			return false;
		}

		return true;
	}

	bool EnsureContext(UpscaleContext& v, UINT rw, UINT rh, UINT dw, UINT dh, uint32_t flags, char* provider_name, size_t provider_name_size)
	{
		if (v.ctx && v.render_w == rw && v.render_h == rh && v.display_w == dw && v.display_h == dh && v.flags == flags)
			return true;

		DestroyContext(v);

		ffxCreateContextDescUpscale desc = {};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		desc.flags = flags;
		desc.maxRenderSize.width = rw;
		desc.maxRenderSize.height = rh;
		desc.maxUpscaleSize.width = dw;
		desc.maxUpscaleSize.height = dh;
		desc.fpMessage = &FfxMessage;

		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = dev12.Get();

		ffxCreateContextDescUpscaleVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.version = FFX_UPSCALER_VERSION;

		// No version override: the runtime picks the newest provider the driver supports (FSR 4 on RDNA4).
		desc.header.pNext = &backend.header;
		backend.header.pNext = &version.header;

		ffxReturnCode_t rc = ffxCreateContext(&v.ctx, &desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK)
		{
			Log("[FSR] ! ffxCreateContext failed: %u", rc);
			v.ctx = nullptr;
			return false;
		}

		ffxQueryGetProviderVersion pv = {};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (ffxQuery(&v.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
		{
			strncpy_s(provider_name, provider_name_size, pv.versionName, _TRUNCATE);
			Log("[FSR] upscaler context created (%s view): %s, max render %ux%u -> display %ux%u",
				&v == &views[0] ? "main" : "scope", pv.versionName, rw, rh, dw, dh);
		}

		v.render_w = rw;
		v.render_h = rh;
		v.display_w = dw;
		v.display_h = dh;
		v.flags = flags;
		return true;
	}

	void ConvertDepth(ID3D11Resource* src_depth)
	{
		ID3D11DeviceContext* c = ctx11.Get();

		c->CopyResource(depth_copy.Get(), src_depth);

		// The engine caches compute bindings in RCache, so put back whatever was bound.
		ComPtr<ID3D11ComputeShader> old_cs;
		ComPtr<ID3D11ShaderResourceView> old_srv;
		ComPtr<ID3D11UnorderedAccessView> old_uav;
		c->CSGetShader(&old_cs, nullptr, nullptr);
		c->CSGetShaderResources(0, 1, &old_srv);
		c->CSGetUnorderedAccessViews(0, 1, &old_uav);

		ID3D11ShaderResourceView* srv = depth_copy_srv.Get();
		ID3D11UnorderedAccessView* uav = depth_uav.Get();
		c->CSSetShader(depth_cs.Get(), nullptr, 0);
		c->CSSetShaderResources(0, 1, &srv);
		c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		D3D11_TEXTURE2D_DESC src_desc;
		depth_copy->GetDesc(&src_desc);
		c->Dispatch((src_desc.Width + 7) / 8, (src_desc.Height + 7) / 8, 1);

		srv = old_srv.Get();
		uav = old_uav.Get();
		c->CSSetShader(old_cs.Get(), nullptr, 0);
		c->CSSetShaderResources(0, 1, &srv);
		c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	}

	void GenerateMotionVectors(const FsrBridge::Inputs& in, UINT rw, UINT rh)
	{
		ID3D11DeviceContext* c = ctx11.Get();

		D3D11_MAPPED_SUBRESOURCE mapped;
		if (FAILED(c->Map(mv_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		MvParams* p = (MvParams*)mapped.pData;
		memcpy(p->reproject, in.reproject, sizeof(p->reproject));
		p->inv_size[0] = 1.0f / float(rw);
		p->inv_size[1] = 1.0f / float(rh);
		p->jitter_uv[0] = in.jitter_x / float(rw);
		p->jitter_uv[1] = in.jitter_y / float(rh);
		p->hud_depth = in.hud_depth;
		c->Unmap(mv_cb.Get(), 0);

		ComPtr<ID3D11ComputeShader> old_cs;
		ComPtr<ID3D11ShaderResourceView> old_srv;
		ComPtr<ID3D11UnorderedAccessView> old_uav;
		ComPtr<ID3D11Buffer> old_cb;
		c->CSGetShader(&old_cs, nullptr, nullptr);
		c->CSGetShaderResources(0, 1, &old_srv);
		c->CSGetUnorderedAccessViews(0, 1, &old_uav);
		c->CSGetConstantBuffers(0, 1, &old_cb);

		ID3D11ShaderResourceView* srv = depth_copy_srv.Get();
		ID3D11UnorderedAccessView* uav = mv_uav.Get();
		ID3D11Buffer* cb = mv_cb.Get();
		c->CSSetShader(mv_cs.Get(), nullptr, 0);
		c->CSSetShaderResources(0, 1, &srv);
		c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		c->CSSetConstantBuffers(0, 1, &cb);
		c->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);

		srv = old_srv.Get();
		uav = old_uav.Get();
		cb = old_cb.Get();
		c->CSSetShader(old_cs.Get(), nullptr, 0);
		c->CSSetShaderResources(0, 1, &srv);
		c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		c->CSSetConstantBuffers(0, 1, &cb);
	}
};

FsrBridge::FsrBridge() : m(new Impl())
{
	m_provider_name[0] = 0;
}

FsrBridge::~FsrBridge()
{
	m->DestroyContexts();
	m->WaitGpuIdle();

	if (m->event)
		CloseHandle(m->event);

	HMODULE ffx_dll = m->ffx_dll;
	HMODULE d3d12_dll = m->d3d12_dll;
	delete m;

	// Release every D3D12 object before unloading the DLLs that own their code.
	if (ffx_dll)
		FreeLibrary(ffx_dll);
	if (d3d12_dll)
		FreeLibrary(d3d12_dll);
}

bool FsrBridge::Init(ID3D11Device* device, ID3D11DeviceContext* context, LogFn log)
{
	g_log = log;

	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m->dev11))) ||
		FAILED(context->QueryInterface(IID_PPV_ARGS(&m->ctx11))))
	{
		Log("[FSR] ! ID3D11Device5 / ID3D11DeviceContext4 unavailable (needs Windows 10 1703+)");
		return false;
	}

	m->ffx_dll = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
	if (!m->ffx_dll)
	{
		Log("[FSR] ! amd_fidelityfx_loader_dx12.dll not found next to the exe");
		return false;
	}
	m->ffxCreateContext = (PfnFfxCreateContext)GetProcAddress(m->ffx_dll, "ffxCreateContext");
	m->ffxDestroyContext = (PfnFfxDestroyContext)GetProcAddress(m->ffx_dll, "ffxDestroyContext");
	m->ffxQuery = (PfnFfxQuery)GetProcAddress(m->ffx_dll, "ffxQuery");
	m->ffxDispatch = (PfnFfxDispatch)GetProcAddress(m->ffx_dll, "ffxDispatch");
	if (!m->ffxCreateContext || !m->ffxDestroyContext || !m->ffxQuery || !m->ffxDispatch)
	{
		Log("[FSR] ! amd_fidelityfx_loader_dx12.dll is missing exports");
		return false;
	}

	m->d3d12_dll = LoadLibraryW(L"d3d12.dll");
	PFN_D3D12_CREATE_DEVICE create_device = m->d3d12_dll ? (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m->d3d12_dll, "D3D12CreateDevice") : nullptr;
	if (!create_device)
	{
		Log("[FSR] ! d3d12.dll unavailable");
		return false;
	}

	// The D3D12 device must live on the same adapter as the D3D11 one for sharing to work.
	ComPtr<IDXGIDevice> dxgi_device;
	ComPtr<IDXGIAdapter> adapter11;
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter1> adapter;
	DXGI_ADAPTER_DESC adesc = {};
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
		FAILED(dxgi_device->GetAdapter(&adapter11)) ||
		FAILED(adapter11->GetDesc(&adesc)) ||
		FAILED(adapter11->GetParent(IID_PPV_ARGS(&factory))) ||
		FAILED(factory->EnumAdapterByLuid(adesc.AdapterLuid, IID_PPV_ARGS(&adapter))))
	{
		Log("[FSR] ! could not resolve the D3D11 adapter");
		return false;
	}

	HRESULT hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m->dev12));
	if (FAILED(hr))
	{
		Log("[FSR] ! D3D12CreateDevice failed: 0x%08x", (unsigned)hr);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(m->dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m->queue))))
	{
		Log("[FSR] ! CreateCommandQueue failed");
		return false;
	}
	for (UINT i = 0; i < kFrames; ++i)
	{
		if (FAILED(m->dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m->alloc[i]))))
		{
			Log("[FSR] ! CreateCommandAllocator failed");
			return false;
		}
	}
	if (FAILED(m->dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m->alloc[0].Get(), nullptr, IID_PPV_ARGS(&m->list))))
	{
		Log("[FSR] ! CreateCommandList failed");
		return false;
	}
	m->list->Close();

	// One fence shared by both APIs; values only ever increase.
	HANDLE fence_handle = nullptr;
	if (FAILED(m->dev11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m->fence11))) ||
		FAILED(m->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle)))
	{
		Log("[FSR] ! could not create shared D3D11 fence");
		return false;
	}
	hr = m->dev12->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&m->fence12));
	CloseHandle(fence_handle);
	if (FAILED(hr))
	{
		Log("[FSR] ! D3D12 OpenSharedHandle(fence) failed: 0x%08x", (unsigned)hr);
		return false;
	}

	m->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	ComPtr<ID3DBlob> code, errors;
	hr = D3DCompile(g_depth_cs, sizeof(g_depth_cs) - 1, "fsr_depth_cs", nullptr, nullptr, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr) || FAILED(m->dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m->depth_cs)))
	{
		Log("[FSR] ! depth conversion shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "");
		return false;
	}

	code.Reset();
	errors.Reset();
	hr = D3DCompile(g_mv_cs, sizeof(g_mv_cs) - 1, "fsr_mv_cs", nullptr, nullptr, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr) || FAILED(m->dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m->mv_cs)))
	{
		Log("[FSR] ! motion vector shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "");
		return false;
	}

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(MvParams);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m->dev11->CreateBuffer(&cbd, nullptr, &m->mv_cb)))
	{
		Log("[FSR] ! motion vector constant buffer failed");
		return false;
	}

	char name[256];
	WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, name, sizeof(name), nullptr, nullptr);
	Log("[FSR] D3D12 bridge ready on %s", name);
	return true;
}

bool FsrBridge::Dispatch(const Inputs& in)
{
	ComPtr<ID3D11Texture2D> color_tex, mv_tex, out_tex;
	if (FAILED(in.color->QueryInterface(IID_PPV_ARGS(&color_tex))) ||
		(!in.generate_mv && FAILED(in.motion_vectors->QueryInterface(IID_PPV_ARGS(&mv_tex)))) ||
		FAILED(in.output->QueryInterface(IID_PPV_ARGS(&out_tex))))
		return false;

	D3D11_TEXTURE2D_DESC cd, md, od;
	color_tex->GetDesc(&cd);
	out_tex->GetDesc(&od);
	if (in.generate_mv)
	{
		md = cd;
		md.Format = DXGI_FORMAT_R16G16_FLOAT;
	}
	else
		mv_tex->GetDesc(&md);

	if (cd.SampleDesc.Count != 1 || md.SampleDesc.Count != 1)
	{
		Log("[FSR] ! MSAA inputs are not supported");
		return false;
	}

	const DXGI_FORMAT color_fmt = TypedFormat(cd.Format);
	const DXGI_FORMAT out_fmt = TypedFormat(od.Format);
	if (!m->EnsureResources(cd.Width, cd.Height, color_fmt, md.Format, od.Width, od.Height, out_fmt))
		return false;

	const bool hdr = in.hdr;
	uint32_t flags = hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
	flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION; // render size may switch (e.g. native while aiming a scope)
	if (in.debug_view)
		flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;

	Impl::UpscaleContext& view = m->views[in.view == 1 ? 1 : 0];
	const bool context_reset = !view.ctx;
	// Max render size = display size, so switching render resolution keeps the context and its history
	if (!m->EnsureContext(view, od.Width, od.Height, od.Width, od.Height, flags, m_provider_name, sizeof(m_provider_name)))
		return false;

	// --- D3D11: stage inputs into shared textures
	ID3D11DeviceContext4* c11 = m->ctx11.Get();
	const D3D11_BOX box = {0, 0, 0, cd.Width, cd.Height, 1};
	c11->CopySubresourceRegion(m->color.t11.Get(), 0, 0, 0, 0, in.color, 0, &box);
	m->ConvertDepth(in.depth);
	if (in.generate_mv)
		m->GenerateMotionVectors(in, cd.Width, cd.Height);
	else
		c11->CopySubresourceRegion(m->mv.t11.Get(), 0, 0, 0, 0, in.motion_vectors, 0, &box);

	const UINT64 inputs_ready = ++m->fence_value;
	c11->Signal(m->fence11.Get(), inputs_ready);
	c11->Flush();

	// --- D3D12: wait for D3D11, then run FSR
	m->queue->Wait(m->fence12.Get(), inputs_ready);

	const UINT slot = m->frame++ % kFrames;
	if (m->fence12->GetCompletedValue() < m->alloc_fence[slot])
	{
		m->fence12->SetEventOnCompletion(m->alloc_fence[slot], m->event);
		WaitForSingleObject(m->event, INFINITE);
	}
	m->alloc[slot]->Reset();
	m->list->Reset(m->alloc[slot].Get(), nullptr);

	ID3D12Resource* inputs[] = {m->color.t12.Get(), m->depth.t12.Get(), m->mv.t12.Get()};
	D3D12_RESOURCE_BARRIER barriers[4] = {};
	for (UINT i = 0; i < 4; ++i)
	{
		barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[i].Transition.pResource = i < 3 ? inputs[i] : m->output.t12.Get();
		barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[i].Transition.StateAfter = i < 3 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	m->list->ResourceBarrier(4, barriers);

	ffxDispatchDescUpscale d = {};
	d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	d.commandList = m->list.Get();
	d.color = ffxApiGetResourceDX12(m->color.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.depth = ffxApiGetResourceDX12(m->depth.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = ffxApiGetResourceDX12(m->mv.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.output = ffxApiGetResourceDX12(m->output.t12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	d.jitterOffset.x = in.jitter_x;
	d.jitterOffset.y = in.jitter_y;
	d.motionVectorScale.x = in.mv_scale_x;
	d.motionVectorScale.y = in.mv_scale_y;
	d.renderSize.width = cd.Width;
	d.renderSize.height = cd.Height;
	d.upscaleSize.width = od.Width;
	d.upscaleSize.height = od.Height;
	d.enableSharpening = in.sharpness > 0.0f;
	d.sharpness = in.sharpness;
	d.frameTimeDelta = in.frame_time_ms;
	d.preExposure = 1.0f;
	d.reset = in.reset || context_reset;
	d.cameraNear = in.camera_near;
	d.cameraFar = in.camera_far;
	d.cameraFovAngleVertical = in.camera_fov_y;
	d.viewSpaceToMetersFactor = 1.0f;
	d.flags = (hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) | (in.debug_view ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0);

	ffxReturnCode_t rc = m->ffxDispatch(&view.ctx, &d.header);

	for (UINT i = 0; i < 4; ++i)
	{
		D3D12_RESOURCE_STATES after = barriers[i].Transition.StateAfter;
		barriers[i].Transition.StateAfter = barriers[i].Transition.StateBefore;
		barriers[i].Transition.StateBefore = after;
	}
	m->list->ResourceBarrier(4, barriers);
	m->list->Close();

	// Always submit and signal, even if FSR failed, so the D3D11 wait below can never hang the GPU.
	ID3D12CommandList* lists[] = {m->list.Get()};
	m->queue->ExecuteCommandLists(1, lists);
	const UINT64 fsr_done = ++m->fence_value;
	m->queue->Signal(m->fence12.Get(), fsr_done);
	m->alloc_fence[slot] = fsr_done;

	// --- D3D11: wait for D3D12, then take the result
	c11->Wait(m->fence11.Get(), fsr_done);

	if (rc != FFX_API_RETURN_OK)
	{
		Log("[FSR] ! ffxDispatch failed: %u", rc);
		return false;
	}

	c11->CopyResource(in.output, m->output.t11.Get());
	return true;
}

int FsrBridge::JitterPhaseCount(unsigned render_width, unsigned display_width)
{
	const float ratio = (float)display_width / (float)render_width;
	return (int)(8.0f * ratio * ratio);
}

void FsrBridge::JitterOffset(int index, int phase_count, float& out_x, float& out_y)
{
	const int base = (index % phase_count) + 1;
	out_x = Halton(base, 2) - 0.5f;
	out_y = Halton(base, 3) - 0.5f;
}
