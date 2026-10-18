#pragma once

// AMD FSR (ffx_api, D3D12 only) driven from the D3D11 renderer.
//
// The bridge owns a side D3D12 device on the same adapter as the engine's D3D11 device.
// Inputs/outputs live in textures created by D3D11 with NT shared handles and opened in D3D12.
// Both queues are ordered through one shared fence, so no CPU stalls happen per frame.
//
// This header intentionally pulls in only D3D11 types so engine code does not see d3d12.h.

#include <d3d11.h>

class FsrBridge
{
public:
	typedef void (*LogFn)(const char* msg);

	// Separate FSR history per camera: 0 = main view, 1 = 3D scope (second viewport)
	enum { kViews = 2 };

	struct Inputs
	{
		int view;                       // 0..kViews-1
		ID3D11Resource* color;          // render-res color, same format as the output target
		ID3D11Resource* depth;          // D24S8 depth-stencil texture (copied, never bound)
		ID3D11Resource* motion_vectors; // RGBA16F, xy = motion; unused when generate_mv
		ID3D11Resource* output;         // display-res target the upscaled image is copied into

		// No per-object motion vectors available: build camera motion vectors from depth instead
		bool  generate_mv;
		float reproject[16];            // jittered current clip -> previous clip, row-vector, row-major
		float hud_depth;                // depth at or below this is HUD (moves with the camera): no motion

		float jitter_x, jitter_y;       // pixels, as applied to the projection this frame
		float mv_scale_x, mv_scale_y;   // multiplies motion_vectors.xy to get previous-current in pixels
		float frame_time_ms;
		float camera_near, camera_far;
		float camera_fov_y;             // radians
		float sharpness;                // 0 disables sharpening
		bool  hdr;                      // linear HDR color; false = tonemapped sRGB-like color
		bool  reset;
		bool  debug_view;
	};

	FsrBridge();
	~FsrBridge();

	// Creates the D3D12 device, queue and fence and loads amd_fidelityfx_loader_dx12.dll.
	// Returns false if anything is unavailable; the bridge is then unusable and should be deleted.
	bool Init(ID3D11Device* device, ID3D11DeviceContext* context, LogFn log);

	// Upscale inputs->color into inputs->output. Creates or recreates shared textures and the FSR context
	// when sizes or formats change. Returns false on failure.
	bool Dispatch(const Inputs& in);

	// Name of the FSR provider that the runtime picked (e.g. "FSR 4.1.1"), empty before first dispatch.
	const char* ProviderName() const { return m_provider_name; }

	// Halton(2,3) jitter in pixels, same sequence as ffxQueryDescUpscaleGetJitterOffset.
	static int  JitterPhaseCount(unsigned render_width, unsigned display_width);
	static void JitterOffset(int index, int phase_count, float& out_x, float& out_y);

private:
	struct Impl;
	Impl* m;
	char  m_provider_name[64];
};
