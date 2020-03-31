/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/*
// ========================================================================== //
   This file contains declarations used by all path tracer shaders, including 
   rgen and all the hit shaders.

General notes about the Q2RTX path tracer.

The path tracer is separated into 3 stages for performance reasons:

  1. `primary_rays.rgen` - responsible for shooting primary rays from the 
     camera. It can work with different projections, see `projection.glsl` for
     more information. The primary rays stage will find the primary visible 
     surface or test the visibility of a gradient sample. For the visible 
     surface, it will compute motion vectors and texture gradients. 
     This stage will also collect transparent objects such as sprites and 
     particles between the camera and the primary surface, and store them in 
     the transparency channel. Surface information is stored as a visibility
     buffer, which is enough to reconstruct the position and material parameters
     of the surface in a later shader stage.

     The primary rays stage can potentially be replaced with a rasterization 
     pass, but that pass would have to process checkerboarding and per-pixel
     offsets for temporal AA using programmable sample positions. Also, a 
	 rasterization pass will not be able to handle custom projections like 
	 the cylindrical projection.

  2. `direct_lighting.rgen` - takes the primary surface produced by the first
     stage. If that surface is a special material like water, glass, or chrome,
     it will trace a single reflection or refraction ray, ignoring subsequent
     intersections with transparent geometry. For the first opaque surface, 
     this stage computes direct lighting from local polygonal and sphere lights
	 and sun light.

	 This stage also writes out some parameters of the first opaque surface,
	 like world position and normal, into G-buffer channels. Even though the 
	 next stage can extract this information using the visibility buffer, it is
	 faster to write them to a G-buffer because accessing the vertex buffers 
	 and textures and reconstructing materials from them is relatively 
	 inefficient.

  3. `indirect_lighting.rgen` - takes the opaque surface from the G-buffer,
     as produced by stage (2) or previous iteration of stage (3). From that
     surface, it traces a single bounce ray - either diffuse or specular,
     depending on whether it's the first or second bounce (second bounce 
     doesn't do specular) and depending on material roughness and incident
     ray direction. For the surface hit by the bounce ray, this stage will
     compute direct lighting in the same way as stage (2) does, including 
     local lights and sun light. 

     Stage (3) can be invoked multiple times, currently that number is limited
     to 2. First invocation computes the first lighting bounce and replaces 
     the surface parameters in the G-buffer with the surface hit by the bounce
     ray. Second invocation computes the second lighting bounce.

     Second bounce does not include local lights because they are very 
     expensive, yet their contribution from the second bounce is barely 
     noticeable, if at all. 

     If the denoiser is disabled, the last invocation of stage (3) will also
     perform compositing of all lighting channels into final color.


Also note that "local lights" in this path tracer includes skybox triangles in
some cases. Generally, if there is an area with a relatively small opening 
that exposes the sky, that opening will be marked as an analytic local light.
Quake 2 geometry includes these skyboxes that are often placed right where
we would need a portal light. But in many cases, they are also big and complex
meshes that enclose large outdoor areas, and we don't want to create analytic
light from those. So specific places in the game where portal lights make sense
are marked in the "sky_clusters.txt" file in the game directory. Same conversion
is applied for lava geometry in many places in the game, and is guided by 
the same file.

Converting skyboxes to local lights provides two benefits:

  - Reducing noise that comes from small openings. For example, if there is a 
    room with a window, and that window is visible as a 0.3 steradian opening
    from a wall on the other side (which corresponds to a roughly 36 degree 
    cone), that window covers only 5% of the hemisphere above the surface,
    so the probability of a diffuse bounce ray of hitting that window is also 
    about 5%. If we place a portal light, and there are no other lights in the 
    room, that probability becomes 100%.

  - Adding a free bounce of sky light. Without portal lights, the first bounce
    stage will compute direct lighting from the sky, and the second bounce
    stage will compute bounce lighting from the sky. With portal lights, the
    direct lighting stage will compute direct sky lighting, and the first 
    bounce stage will compute bounce lighting. For this reason, you might
    notice that some areas lose sky lighting when you disable all bounce passes,
    and some do not.

// ========================================================================== //
*/

#extension GL_NV_ray_tracing : require
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier    : enable

#define INSTANCE_DYNAMIC_FLAG        (1u << 31)
#define INSTANCE_SKY_FLAG            (1u << 30)
#define PRIM_ID_MASK (~(INSTANCE_DYNAMIC_FLAG | INSTANCE_SKY_FLAG))

#define GLOBAL_UBO_DESC_SET_IDX 1
#include "global_ubo.h"


layout (push_constant) uniform push_constant_block {
	int gpu_index;
    int bounce_index;
} push_constants;

struct RayPayload {
	vec2 barycentric;
	uint instance_prim;
	float hit_distance;
	uvec2 transparency; // half4x16
	float max_transparent_distance;
};

struct RayPayloadShadow {
	int missed;
};

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
