#include "stdafx.h"
#include "r4_fsr_bridge.h"
#include "../xrRenderDX10/StateManager/dx10SamplerStateCache.h"
#include "../xrRender/dxRenderDeviceRender.h"
#include "../../xrEngine/ShadersExternalData.h"

// AMD FSR, run through the D3D12 interop bridge.
// With Screen Space Shaders (o.ssfx_motionvectors) FSR uses SSFX's per-object motion vectors and its shader
// jitter (ssfx_jitter). Without it (vanilla Anomaly shaders) the engine jitters the projection matrices itself
// and the bridge builds camera motion vectors from depth.
//
// r__fsr: 0 off, 1 native AA, 2 quality (1.5x), 3 balanced (1.7x), 4 performance (2x), 5 ultra performance (3x)
// Modes 2+ render the scene at a lower resolution (see CRenderTarget::fsr_begin_scene) and need vid_restart.

static const float g_fsr_ratio[] = {1.0f, 1.0f, 1.5f, 1.7f, 2.0f, 3.0f};

static bool fsr_prerequisites()
{
	return !RImplementation.o.dx10_msaa && !RImplementation.o.dx11_hdr10;
}

// No SSFX motion vectors: engine-side jitter and depth-based camera motion vectors
static bool FsrEngineMotion()
{
	return !RImplementation.o.ssfx_motionvectors;
}

// HUD geometry is drawn with rmNear (depth range 0..0.02)
static const float kFsrHudDepth = 0.02f;

bool fsr_scale_wanted()
{
	return ps_r__fsr >= 2 && fsr_prerequisites();
}

void fsr_render_size(u32 display_w, u32 display_h, u32& render_w, u32& render_h)
{
	const float ratio = fsr_scale_wanted() ? g_fsr_ratio[_min(ps_r__fsr, 5)] : 1.0f;
	render_w = _max(1u, u32(float(display_w) / ratio));
	render_h = _max(1u, u32(float(display_h) / ratio));
}

static void FsrLog(const char* msg)
{
	Msg("%s", msg);
}

// 3D scope frames come from a second camera; they get their own FSR history (the engine keeps matching
// per-viewport previous matrices, see CRenderDevice::mView_saved_vp)
static int FsrView()
{
	return Device.m_SecondViewport.IsSVPFrame() ? 1 : 0;
}

static void FsrJitterPixels(const CRenderTarget* T, float& x, float& y)
{
	const int phases = FsrBridge::JitterPhaseCount(T->fsr_render_w, T->fsr_display_w);
	FsrBridge::JitterOffset((int)(T->fsr_jitter_index[FsrView()] % (u32)phases), phases, x, y);
}

// r__dsgraph_render.cpp: 3D PDA screen replay at display resolution
void fsr_hud_begin_frame(bool capture);
void fsr_hud_replay();
bool fsr_hud_replaying();

bool fsr_jitter_ndc(Fvector2& ndc)
{
	const CRenderTarget* T = RImplementation.Target;
	if (!T || !T->fsr_usable() || FsrEngineMotion())
		return false;

	if (fsr_hud_replaying())
	{
		ndc.set(0.f, 0.f);
		return true;
	}

	float x, y;
	FsrJitterPixels(T, x, y);
	ndc.set(2.0f * x / float(T->fsr_render_w), -2.0f * y / float(T->fsr_render_h));
	return true;
}

bool CRenderTarget::fsr_usable() const
{
	return ps_r__fsr && !fsr_failed && fsr_prerequisites();
}

// Says once why r__fsr has no effect (or needs a restart), instead of silently using the old path
static void FsrReport(const CRenderTarget* T)
{
	static int last_reason = 0;
	int reason = 0;
	if (ps_r__fsr && RImplementation.o.dx10_msaa)
		reason = 1;
	else if (ps_r__fsr && RImplementation.o.dx11_hdr10)
		reason = 2;
	else
	{
		u32 rw, rh;
		fsr_render_size(T->fsr_display_w, T->fsr_display_h, rw, rh);
		if (fsr_scale_wanted() != T->fsr_scaled || rw != T->fsr_base_render_w || rh != T->fsr_base_render_h)
			reason = 4;
	}

	if (reason == last_reason)
		return;
	last_reason = reason;

	switch (reason)
	{
	case 1: Msg("! [FSR] not active: MSAA is on. Set r3_msaa st_opt_off and run vid_restart"); break;
	case 2: Msg("! [FSR] not active: HDR10 output is not supported"); break;
	case 4: Msg("! [FSR] render resolution changed, run vid_restart to apply"); break;
	}

	static bool engine_motion_reported = false;
	if (ps_r__fsr && FsrEngineMotion() && !engine_motion_reported)
	{
		engine_motion_reported = true;
		Msg("[FSR] Screen Space Shaders not found: using engine jitter and camera-only motion vectors "
			"(moving objects may ghost; install Screen Space Shaders for best quality)");
	}
}

// Engine jitter (no SSFX): offset every projection used this frame by the FSR jitter in NDC, and record the
// reprojection (jittered current clip -> previous clip) for the depth-based motion vectors. Restored at the
// end of the scene so game code (picking, UI projection) never sees jittered matrices.
static void FsrJitterProjection(Fmatrix& p, float ox, float oy)
{
	// Row-vector perspective projection: clip.w = view z, so adding o * w to clip.xy is o in row 3
	p._31 += ox;
	p._32 += oy;
}

void CRenderTarget::fsr_apply_engine_jitter()
{
	float jx, jy;
	FsrJitterPixels(this, jx, jy);
	fsr_engine_jitter_px.set(jx, jy);
	const float ox = 2.0f * jx / float(fsr_render_w), oy = -2.0f * jy / float(fsr_render_h);

	fsr_saved_proj[0] = Device.mProject;
	fsr_saved_proj[1] = Device.mProjectHud;
	fsr_saved_proj[2] = Device.mProjectCam;
	fsr_saved_full[0] = Device.mFullTransform;
	fsr_saved_full[1] = Device.mFullTransformHud;
	fsr_saved_full[2] = Device.mFullTransformCam;

	FsrJitterProjection(Device.mProject, ox, oy);
	FsrJitterProjection(Device.mProjectHud, ox, oy);
	FsrJitterProjection(Device.mProjectCam, ox, oy);
	Device.mFullTransform.mul(Device.mProject, Device.mView);
	Device.mFullTransformHud.mul(Device.mProjectHud, Device.mViewHud);
	Device.mFullTransformCam.mul(Device.mProjectCam, Device.mViewCam);
	Device.m_pRender->SetCacheXform(Device.mView, Device.mProject);

	// Previous matrices are saved unjittered (and per viewport while a 3D scope alternates cameras)
	Fmatrix inv_current, prev;
	D3DXMatrixInverse((D3DXMATRIX*)&inv_current, nullptr, (D3DXMATRIX*)&Device.mFullTransform);
	prev.mul(Device.mProject_prev, Device.mView_prev);
	fsr_engine_reproject.mul(prev, inv_current);

	fsr_engine_jitter_applied = true;
}

void CRenderTarget::fsr_restore_engine_jitter()
{
	if (!fsr_engine_jitter_applied)
		return;

	Device.mProject = fsr_saved_proj[0];
	Device.mProjectHud = fsr_saved_proj[1];
	Device.mProjectCam = fsr_saved_proj[2];
	Device.mFullTransform = fsr_saved_full[0];
	Device.mFullTransformHud = fsr_saved_full[1];
	Device.mFullTransformCam = fsr_saved_full[2];
	Device.m_pRender->SetCacheXform(Device.mView, Device.mProject);
	fsr_engine_jitter_applied = false;
}

// AMD recommends log2(render / display) - 1 on top of the user's bias. Only rebuilds samplers when it changes.
static void FsrUpdateMipBias(const CRenderTarget* T)
{
	float bias = ps_r__tf_Mipbias;
	if (T->fsr_usable())
		bias += log2f(float(T->fsr_render_w) / float(T->fsr_display_w)) - 1.0f;
	SSManager.SetMipLODBias(bias);
}

// u_setrt() never touches the viewport (everything used to be one size), so switch it explicitly
static void FsrSetViewport(u32 w, u32 h)
{
	D3D_VIEWPORT VP = {0, 0, float(w), float(h), 0.f, 1.f};
	HW.pContext->RSSetViewports(1, &VP);
}

// r__dsgraph_render.cpp
extern u32 fsr_scope_lens_frame;

// Native resolution while aiming down a 3DSS scope: the lens zooms into the frame, so it needs the pixels.
// The lens being drawn is known one frame late, hidden by the aim-in animation.
static bool FsrWantNative(const CRenderTarget* T)
{
	if (!ps_r__fsr_scope_native || !T->fsr_usable() || !scope_3D_fake_enabled || !g_pGamePersistent)
		return false;
	if (Device.dwFrame - fsr_scope_lens_frame > 2)
		return false;
	return g_pGamePersistent->m_pGShaderConstants->hud_params.x > 0.5f; // aim-in progress
}

void CRenderTarget::fsr_begin_scene()
{
	if (fsr_scene_active)
		return;

	if (fsr_scaled)
		fsr_set_native(FsrWantNative(this));

	FsrReport(this);
	FsrUpdateMipBias(this);

	// The PDA screen replay only matters below display resolution
	fsr_hud_begin_frame(fsr_scaled && fsr_usable() && !fsr_native_active);

	if (fsr_usable() && FsrEngineMotion())
		fsr_apply_engine_jitter();

	if (!fsr_scaled)
		return;

	Device.dwWidth = fsr_render_w;
	Device.dwHeight = fsr_render_h;
	fsr_saved_zb = HW.pBaseZB;
	HW.pBaseZB = rt_fsr_zb->pZRT;
	dwWidth = fsr_render_w;
	dwHeight = fsr_render_h;
	FsrSetViewport(fsr_render_w, fsr_render_h);
	fsr_scene_active = true;
}

void CRenderTarget::fsr_end_scene()
{
	fsr_restore_engine_jitter();

	if (!fsr_scene_active)
		return;

	Device.dwWidth = fsr_display_w;
	Device.dwHeight = fsr_display_h;
	HW.pBaseZB = fsr_saved_zb;
	fsr_saved_zb = nullptr;
	dwWidth = fsr_display_w;
	dwHeight = fsr_display_h;
	FsrSetViewport(fsr_display_w, fsr_display_h);
	fsr_scene_active = false;
}

bool CRenderTarget::phase_fsr(ID3DTexture2D* output)
{
	if (!fsr_usable())
		return false;

	PIX_EVENT(phase_fsr);

	if (!fsr_bridge)
	{
		Msg("[FSR] enabled, creating D3D12 bridge...");
		fsr_bridge = xr_new<FsrBridge>();
		if (!fsr_bridge->Init(HW.pDevice, HW.pContext, &FsrLog))
		{
			xr_delete(fsr_bridge);
			fsr_failed = true;
			Msg("! [FSR] disabled, falling back to the regular AA path");
			return false;
		}
	}

	ID3D11Resource* depth = nullptr;
	HW.pBaseZB->GetResource(&depth);

	// Input is combine_2's output: tonemapped, graded and with lens flares, at render resolution
	const int view = FsrView();

	FsrBridge::Inputs in = {};
	in.view = view;
	in.color = rt_Color->pSurface;
	in.depth = depth;
	in.output = output;
	in.hdr = false;

	if (FsrEngineMotion())
	{
		// Jitter applied to the projections in fsr_begin_scene; the bridge writes (previous - current) in UV
		in.generate_mv = true;
		in.jitter_x = fsr_engine_jitter_px.x;
		in.jitter_y = fsr_engine_jitter_px.y;
		CopyMemory(in.reproject, &fsr_engine_reproject, sizeof(in.reproject));
		in.hud_depth = kFsrHudDepth;
		in.mv_scale_x = float(fsr_render_w);
		in.mv_scale_y = float(fsr_render_h);
	}
	else
	{
		in.motion_vectors = rt_ssfx_motion_vectors->pSurface;
		FsrJitterPixels(this, in.jitter_x, in.jitter_y);

		// SSFX stores (current - previous) in UV units, FSR wants (previous - current) in pixels
		in.mv_scale_x = -float(fsr_render_w);
		in.mv_scale_y = -float(fsr_render_h);
	}

	// While a 3D scope is up each view only renders every few frames
	const u32 max_gap = Device.m_SecondViewport.IsSVPActive() ? Device.m_SecondViewport.GetSVPFrameDelay() : 1;
	const u32 gap = Device.dwFrame - fsr_last_frame[view];

	in.frame_time_ms = Device.fTimeDelta * 1000.0f * float(_max(1u, _min(gap, max_gap)));
	in.camera_near = Device.ViewportNear;
	in.camera_far = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	in.camera_fov_y = deg2rad(Device.fFOV);
	in.sharpness = ps_r__fsr_sharpness;
	in.debug_view = ps_r__fsr_debug != 0;

	// History is useless after skipped frames (loading, menus, scope just raised) or a teleport
	in.reset = !fsr_last_frame[view] || gap > max_gap || fsr_last_camera[view].distance_to(Device.vCameraPosition) > 10.0f;
	fsr_last_frame[view] = Device.dwFrame;
	fsr_last_camera[view].set(Device.vCameraPosition);

	const bool ok = fsr_bridge->Dispatch(in);
	_RELEASE(depth);
	fsr_jitter_index[view]++;

	if (!ok)
	{
		xr_delete(fsr_bridge);
		fsr_failed = true;
		Msg("! [FSR] dispatch failed, disabled until the render target is recreated");
	}
	return ok;
}

// Unbinds every cached texture stage so the next set_Element rebinds through the new SRV
static void FsrFlushTextureCache()
{
	// Never freed: ~STextureList calls into the resource manager, which may be gone at static destruction
	static STextureList* empty = xr_new<STextureList>();
	RCache.set_Textures(empty);
}

void CRenderTarget::phase_upscale()
{
	const bool upscaled = phase_fsr(fsr_scaled ? rt_fsr_output->pSurface : rt_Color->pSurface);

	fsr_end_scene();

	// phase_pp samples $user$albedo (rt_Color) with UVs. Point it at the display-res result; without FSR it
	// samples the render-res rt_Color directly, which is a plain bilinear stretch.
	if (upscaled && fsr_scaled)
	{
		// Redraw the 3D PDA screen at display resolution over the upscaled image (fresh display-res depth)
		u_setrt(rt_fsr_output, nullptr, nullptr, HW.pBaseZB);
		HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);
		fsr_hud_replay();

		rt_Color->pTexture->surface_set(rt_fsr_output->pSurface);
		FsrFlushTextureCache();
		fsr_output_bound = true;
	}
	else
		fsr_hud_begin_frame(false);
}

void CRenderTarget::phase_upscale_finish()
{
	if (!fsr_output_bound)
		return;

	rt_Color->pTexture->surface_set(rt_Color->pSurface);
	FsrFlushTextureCache();
	fsr_output_bound = false;
}

void CRenderTarget::fsr_menu_bind(bool display_res)
{
	if (!fsr_scaled)
		return;

	rt_Generic_0->pTexture->surface_set(display_res ? rt_fsr_output->pSurface : rt_Generic_0->pSurface);
	rt_Generic_1->pTexture->surface_set(display_res ? rt_fsr_menu_distort->pSurface : rt_Generic_1->pSurface);
	FsrFlushTextureCache();
}

//////////////////////////////////////////////////////////////////////////
// Dynamic resolution: display-size twins of the render-res RTs

struct FsrAltRT
{
	CRT* rt;
	ID3DTexture2D* surface;
	ID3DRenderTargetView* rtv;
	ID3DDepthStencilView* dsv;
	ID3D11UnorderedAccessView* uav;
	u32 w, h;
};

struct FsrNativeSet
{
	xr_vector<FsrAltRT> rts;
};

static void FsrReleaseAlt(FsrAltRT& a)
{
	_RELEASE(a.uav);
	_RELEASE(a.dsv);
	_RELEASE(a.rtv);
	_RELEASE(a.surface);
}

// Same texture and views as rt, at w x h
static bool FsrCreateAlt(CRT* rt, u32 w, u32 h, FsrAltRT& a)
{
	a = {};
	a.rt = rt;
	a.w = w;
	a.h = h;

	D3D11_TEXTURE2D_DESC td;
	rt->pSurface->GetDesc(&td);
	td.Width = w;
	td.Height = h;
	if (FAILED(HW.pDevice->CreateTexture2D(&td, nullptr, &a.surface)))
		return false;

	bool ok = true;
	if (rt->pRT)
	{
		D3D11_RENDER_TARGET_VIEW_DESC vd;
		rt->pRT->GetDesc(&vd);
		ok = ok && SUCCEEDED(HW.pDevice->CreateRenderTargetView(a.surface, &vd, &a.rtv));
	}
	if (rt->pZRT)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC vd;
		rt->pZRT->GetDesc(&vd);
		ok = ok && SUCCEEDED(HW.pDevice->CreateDepthStencilView(a.surface, &vd, &a.dsv));
	}
	if (rt->pUAView)
	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC vd;
		rt->pUAView->GetDesc(&vd);
		ok = ok && SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(a.surface, &vd, &a.uav));
	}

	if (!ok)
		FsrReleaseAlt(a);
	return ok;
}

// Exchanges the RT's D3D resources with its twin; code and shaders keep using the same CRT / texture name
static void FsrSwap(FsrAltRT& a)
{
	CRT* rt = a.rt;
	std::swap(rt->pSurface, a.surface);
	std::swap(rt->pRT, a.rtv);
	std::swap(rt->pZRT, a.dsv);
	std::swap(rt->pUAView, a.uav);
	std::swap(rt->dwWidth, a.w);
	std::swap(rt->dwHeight, a.h);
	if (rt->pTexture._get())
		rt->pTexture->surface_set(rt->pSurface);
}

static bool FsrRegistered(const CResourceManager::map_RT& registry, const CRT* rt)
{
	for (const auto& it : registry)
		if (it.second == rt)
			return true;
	return false;
}

// Drops twins of RTs that were destroyed, then twins every RT sized render/n that is not yet covered.
// Display-size and fixed-size RTs (swapchain copies, shadow maps, luminance) are left alone.
static void FsrScanNativeSet(CRenderTarget* T)
{
	const CResourceManager::map_RT& registry = dxRenderDeviceRender::Instance().Resources->_GetRTs();
	xr_vector<FsrAltRT>& rts = T->fsr_native_set->rts;

	for (u32 i = 0; i < rts.size();)
	{
		if (FsrRegistered(registry, rts[i].rt))
		{
			++i;
			continue;
		}
		FsrReleaseAlt(rts[i]);
		rts.erase(rts.begin() + i);
	}

	const u32 bw = T->fsr_base_render_w, bh = T->fsr_base_render_h;
	u32 created = 0;
	for (const auto& it : registry)
	{
		CRT* rt = it.second;
		if (!rt || !rt->pSurface || (!rt->pRT && !rt->pZRT))
			continue;

		bool twinned = false;
		for (const FsrAltRT& a : rts)
			twinned = twinned || a.rt == rt;
		if (twinned)
			continue;

		D3D11_TEXTURE2D_DESC td;
		rt->pSurface->GetDesc(&td);
		if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || td.MipLevels != 1)
			continue;

		// Only exact divisions of the render size (the renderer uses w/1 ... w/64); anything else, like the
		// fluid renderer's capped buffers, is not screen-proportional even if it happens to look close
		const u32 n = (u32)iFloor(float(bw) / float(rt->dwWidth) + 0.5f);
		if (n < 1 || n > 64 || rt->dwWidth != bw / n || rt->dwHeight != bh / n)
			continue;

		const u32 w = T->fsr_display_w / n, h = T->fsr_display_h / n;

		FsrAltRT a;
		if (FsrCreateAlt(rt, _max(1u, w), _max(1u, h), a))
		{
			rts.push_back(a);
			++created;
		}
		else
			Msg("! [FSR] could not create a native-resolution copy of %s", it.first);
	}

	if (created)
		Msg("[FSR] native-resolution set: %u render targets (%u new)", u32(rts.size()), created);
}

void CRenderTarget::fsr_set_native(bool native)
{
	if (!fsr_scaled || native == fsr_native_active)
		return;

	if (native)
	{
		if (!fsr_native_set)
			fsr_native_set = xr_new<FsrNativeSet>();
		FsrScanNativeSet(this); // also picks up RTs created since the last switch
	}

	// Nothing bound may keep pointing at the views being swapped out
	for (u32 i = 0; i < 4; ++i)
		RCache.set_RT(nullptr, i);
	RCache.set_ZB(nullptr);
	FsrFlushTextureCache();

	for (FsrAltRT& a : fsr_native_set->rts)
		FsrSwap(a);

	fsr_native_active = native;
	fsr_render_w = native ? fsr_display_w : fsr_base_render_w;
	fsr_render_h = native ? fsr_display_h : fsr_base_render_h;
}

void CRenderTarget::fsr_release_native_set()
{
	if (!fsr_native_set)
		return;

	if (fsr_native_active)
	{
		// Only swap back RTs that still exist
		const CResourceManager::map_RT& registry = dxRenderDeviceRender::Instance().Resources->_GetRTs();
		for (FsrAltRT& a : fsr_native_set->rts)
			if (FsrRegistered(registry, a.rt))
				FsrSwap(a);
		fsr_native_active = false;
		fsr_render_w = fsr_base_render_w;
		fsr_render_h = fsr_base_render_h;
	}

	for (FsrAltRT& a : fsr_native_set->rts)
		FsrReleaseAlt(a);
	xr_delete(fsr_native_set);
}
