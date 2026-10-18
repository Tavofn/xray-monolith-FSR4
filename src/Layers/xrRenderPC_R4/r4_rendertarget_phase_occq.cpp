#include "stdafx.h"

void CRenderTarget::phase_occq()
{
	// Under FSR scaling HW.pBaseZB is render-res and cannot pair with the display-res backbuffer; colour
	// writes are off anyway, so bind depth only
	if (fsr_scene_active)
		u_setrt(Device.dwWidth, Device.dwHeight, NULL, NULL, NULL, HW.pBaseZB);
	else if (!RImplementation.o.dx10_msaa)
		u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
	else
		u_setrt(Device.dwWidth, Device.dwHeight,NULL,NULL,NULL, rt_MSAADepth->pZRT);
	RCache.set_Shader(s_occq);
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
	RCache.set_ColorWriteEnable(FALSE);
}
