#include "stdafx.h"
#include "r4_fsr_bridge.h"
#include "../xrRenderDX10/StateManager/dx10SamplerStateCache.h"

// AMD FSR, run through the D3D12 interop bridge. Uses SSFX's motion vectors and jitter injection,
// so it needs the Screen Space Shaders motion vector headers (o.ssfx_motionvectors).
//
// r__fsr: 0 off, 1 native AA, 2 quality (1.5x), 3 balanced (1.7x), 4 performance (2x), 5 ultra performance (3x)
// Modes 2+ render the scene at a lower resolution (see CRenderTarget::fsr_begin_scene) and need vid_restart.

static const float g_fsr_ratio[] = {1.0f, 1.0f, 1.5f, 1.7f, 2.0f, 3.0f};

static bool fsr_prerequisites()
{
	return RImplementation.o.ssfx_motionvectors && !RImplementation.o.dx10_msaa && !RImplementation.o.dx11_hdr10;
}

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
	if (!T || !T->fsr_usable())
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
	else if (ps_r__fsr && !RImplementation.o.ssfx_motionvectors)
		reason = 3;
	else
	{
		u32 rw, rh;
		fsr_render_size(T->fsr_display_w, T->fsr_display_h, rw, rh);
		if (fsr_scale_wanted() != T->fsr_scaled || rw != T->fsr_render_w || rh != T->fsr_render_h)
			reason = 4;
	}

	if (reason == last_reason)
		return;
	last_reason = reason;

	switch (reason)
	{
	case 1: Msg("! [FSR] not active: MSAA is on. Set r3_msaa st_opt_off and run vid_restart"); break;
	case 2: Msg("! [FSR] not active: HDR10 output is not supported"); break;
	case 3: Msg("! [FSR] not active: Screen Space Shaders motion vectors missing (shaders\\r3\\screenspace_mvectors.h not found)"); break;
	case 4: Msg("! [FSR] render resolution changed, run vid_restart to apply"); break;
	}
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

void CRenderTarget::fsr_begin_scene()
{
	FsrReport(this);
	FsrUpdateMipBias(this);

	if (fsr_scene_active)
		return;

	fsr_hud_begin_frame(fsr_scaled && fsr_usable());

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
	in.motion_vectors = rt_ssfx_motion_vectors->pSurface;
	in.output = output;
	in.hdr = false;

	FsrJitterPixels(this, in.jitter_x, in.jitter_y);

	// SSFX stores (current - previous) in UV units, FSR wants (previous - current) in pixels
	in.mv_scale_x = -float(fsr_render_w);
	in.mv_scale_y = -float(fsr_render_h);

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
