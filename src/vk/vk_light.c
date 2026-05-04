/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2018-2019 Krzysztof Kondrak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Lightmaps and dynamic lighting
 *
 * =======================================================================
 */

#include "header/local.h"

int r_dlightframecount;

uint32_t            vk_dlightUboOffset;
VkDescriptorSet     vk_dlightUboDescriptorSet;
uint32_t            vk_numDynLights;

static void
R_RenderDlight(dlight_t *light)
{
	VkDescriptorSet	uboDescriptorSet;
	uint8_t	*vertData, *uboData;
	VkDeviceSize	vboOffset;
	uint32_t	uboOffset;
	VkBuffer	vbo;
	int		i, j;
	float	rad;

	rad = light->intensity * 0.35;

	struct {
		vec3_t verts;
		float color[3];
	} lightVerts[18];

	for (i = 0; i < 3; i++)
	{
		lightVerts[0].verts[i] = light->origin[i] - vpn[i] * rad;
	}

	lightVerts[0].color[0] = light->color[0] * 0.2;
	lightVerts[0].color[1] = light->color[1] * 0.2;
	lightVerts[0].color[2] = light->color[2] * 0.2;

	for (i = 16; i >= 0; i--)
	{
		float	a;

		a = i / 16.0 * M_PI * 2;
		for (j = 0; j < 3; j++)
		{
			lightVerts[i+1].verts[j] = light->origin[j] + vright[j] * cos(a)*rad
				+ vup[j] * sin(a)*rad;
			lightVerts[i+1].color[j] = 0.f;
		}
	}

	QVk_BindPipeline(&vk_drawDLightPipeline);

	vertData = QVk_GetVertexBuffer(sizeof(lightVerts), &vbo, &vboOffset);
	uboData = QVk_GetUniformBuffer(sizeof(r_viewproj_matrix), &uboOffset, &uboDescriptorSet);
	memcpy(vertData, lightVerts, sizeof(lightVerts));
	memcpy(uboData,  r_viewproj_matrix, sizeof(r_viewproj_matrix));

	vkCmdBindVertexBuffers(vk_activeCmdbuffer, 0, 1, &vbo, &vboOffset);
	vkCmdBindDescriptorSets(vk_activeCmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_drawDLightPipeline.layout, 0, 1, &uboDescriptorSet, 1, &uboOffset);
	vkCmdBindIndexBuffer(vk_activeCmdbuffer, QVk_GetTriangleFanIbo(48), 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(vk_activeCmdbuffer, 48, 1, 0, 0, 0);
}

void
R_RenderDlights(void)
{
	int i;
	dlight_t *l;

	if (!vk_flashblend->value)
	{
		return;
	}

	/* because the count hasn't advanced yet for this frame */
	r_dlightframecount = r_framecount + 1;

	l = r_newrefdef.dlights;

	for (i = 0; i < r_newrefdef.num_dlights; i++, l++)
	{
		R_RenderDlight(l);
	}
}


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

void
R_MarkSurfaceLights(dlight_t *light, int bit, mnode_t *node, int r_dlightframecount)
{
	msurface_t	*surf;
	int			i;

	/* mark the polygons */
	surf = r_worldmodel->surfaces + node->firstsurface;

	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}
		surf->dlightbits |= bit;
	}
}

/*
=============
R_PushDlights

Marks affected world surfaces and uploads the per-frame dynamic-light UBO that
the lightmapped fragment shader samples for per-pixel dynamic lighting.
=============
*/
void
R_PushDlights(void)
{
	dlight_t	*l;
	int		i, num;
	vk_dlight_ubo_t ubo;
	uint8_t		*uboData;

	vk_numDynLights = 0;

	if (vk_flashblend->value || !vk_dynamic->value)
	{
		// still allocate a zeroed UBO so the lmap pipeline always has a valid
		// descriptor binding for set=3
		memset(&ubo, 0, sizeof(ubo));
		uboData = QVk_GetUniformBuffer(sizeof(ubo), &vk_dlightUboOffset, &vk_dlightUboDescriptorSet);
		memcpy(uboData, &ubo, sizeof(ubo));
		return;
	}

	r_dlightframecount = r_framecount + 1;	// because the count hasn't
											//  advanced yet for this frame

	num = r_newrefdef.num_dlights;
	if (num > MAX_VK_DLIGHTS)
		num = MAX_VK_DLIGHTS;

	memset(&ubo, 0, sizeof(ubo));

	l = r_newrefdef.dlights;
	for (i=0 ; i<num ; i++, l++)
	{
		R_MarkLights(l, 1<<i, r_worldmodel->nodes, r_dlightframecount,
			R_MarkSurfaceLights);

		ubo.dlights[i].origin[0] = l->origin[0];
		ubo.dlights[i].origin[1] = l->origin[1];
		ubo.dlights[i].origin[2] = l->origin[2];
		ubo.dlights[i].color[0]  = l->color[0];
		ubo.dlights[i].color[1]  = l->color[1];
		ubo.dlights[i].color[2]  = l->color[2];
		ubo.dlights[i].color[3]  = l->intensity;
	}

	vk_numDynLights = (uint32_t)num;

	uboData = QVk_GetUniformBuffer(sizeof(ubo), &vk_dlightUboOffset, &vk_dlightUboDescriptorSet);
	memcpy(uboData, &ubo, sizeof(ubo));
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/


vec3_t	lightspot;

static int
RecursiveLightPoint(mnode_t *node, vec3_t start, vec3_t end, vec3_t pointcolor)
{
	float		front, back, frac;
	int			side;
	cplane_t	*plane;
	vec3_t		mid;
	msurface_t	*surf;
	int			s, t, ds, dt;
	int			i;
	mtexinfo_t	*tex;
	byte		*lightmap;
	int			maps;
	int			r;

	if (node->contents != CONTENTS_NODE)
	{
		return -1;		// didn't hit anything
	}

// calculate mid point

// FIXME: optimize for axial
	plane = node->plane;
	front = DotProduct(start, plane->normal) - plane->dist;
	back = DotProduct(end, plane->normal) - plane->dist;
	side = front < 0;

	if ((back < 0) == side)
	{
		return RecursiveLightPoint(node->children[side], start, end, pointcolor);
	}

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	/* go down front side */
	r = RecursiveLightPoint(node->children[side], start, mid, pointcolor);
	if (r >= 0)
	{
		return r;		// hit something
	}

	/* check for impact on this node */
	VectorCopy(mid, lightspot);

	surf = r_worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		vec3_t scale;

		if (surf->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
		{
			continue;	// no lightmaps
		}

		tex = surf->texinfo;

		s = DotProduct(mid, tex->vecs[0]) + tex->vecs[0][3];
		t = DotProduct(mid, tex->vecs[1]) + tex->vecs[1][3];

		if (s < surf->texturemins[0] || t < surf->texturemins[1])
		{
			continue;
		}

		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];

		if (ds > surf->extents[0] || dt > surf->extents[1])
		{
			continue;
		}

		if (!surf->samples)
		{
			return 0;
		}

		ds >>= surf->lmshift;
		dt >>= surf->lmshift;

		lightmap = surf->samples;
		VectorCopy(vec3_origin, pointcolor);

		lightmap += 3 * (dt * ((surf->extents[0] >> surf->lmshift) + 1) + ds);

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			int j;

			for (j = 0; j < 3; j++)
			{
				scale[j] = r_modulate->value * r_newrefdef.lightstyles[surf->styles[maps]].rgb[j];
			}

			pointcolor[0] += lightmap[0] * scale[0] * (1.0/255);
			pointcolor[1] += lightmap[1] * scale[1] * (1.0/255);
			pointcolor[2] += lightmap[2] * scale[2] * (1.0/255);
			lightmap += 3 * ((surf->extents[0] >> surf->lmshift) + 1) *
					((surf->extents[1] >> surf->lmshift) + 1);
		}

		return 1;
	}

	/* go down back side */
	return RecursiveLightPoint(node->children[!side], mid, end, pointcolor);
}

static int
BSPX_LightGridSingleValue(const bspxlightgrid_t *grid, const lightstyle_t *lightstyles, int x, int y, int z, vec3_t res_diffuse)
{
	unsigned int node;

	node = grid->rootnode;
	while (!(node & LGNODE_LEAF))
	{
		struct bspxlgnode_s *n;
		if (node & LGNODE_MISSING)
			return 0;	//failure
		n = grid->nodes + node;
		node = n->child[
				((x>=n->mid[0])<<2)|
				((y>=n->mid[1])<<1)|
				((z>=n->mid[2])<<0)];
	}

	{
		struct bspxlgleaf_s *leaf = &grid->leafs[node & ~LGNODE_LEAF];
		struct bspxlgsamp_s *samp;
		int i;

		x -= leaf->mins[0];
		y -= leaf->mins[1];
		z -= leaf->mins[2];
		if (x >= leaf->size[0] ||
			y >= leaf->size[1] ||
			z >= leaf->size[2])
			return 0;	//sample we're after is out of bounds...

		i = x + leaf->size[0]*(y + leaf->size[1]*z);
		samp = leaf->rgbvalues + i;

		//no hdr support
		for (i = 0; i < 4; i++)
		{
			if (samp->map[i].style == ((byte)(~0u)))
				break;	//no more
			res_diffuse[0] += samp->map[i].rgb[0] * lightstyles[samp->map[i].style].rgb[0] / 255.0;
			res_diffuse[1] += samp->map[i].rgb[1] * lightstyles[samp->map[i].style].rgb[1] / 255.0;
			res_diffuse[2] += samp->map[i].rgb[2] * lightstyles[samp->map[i].style].rgb[2] / 255.0;
		}
	}
	return 1;
}

static void
BSPX_LightGridValue(const bspxlightgrid_t *grid, const lightstyle_t *lightstyles,
	const vec3_t point, vec3_t res_diffuse)
{
	int tile[3];
	int i;
	int s;

	VectorSet(res_diffuse, 0, 0, 0);	//assume worst

	for (i = 0; i < 3; i++)
		tile[i] = (point[i] - grid->mins[i]) * grid->gridscale[i];

	for (i = 0, s = 0; i < 8; i++)
		s += BSPX_LightGridSingleValue(grid, lightstyles,
			tile[0]+!!(i&1),
			tile[1]+!!(i&2),
			tile[2]+!!(i&4), res_diffuse);

	VectorScale(res_diffuse, 1.0/s, res_diffuse);	//average the successful ones
}

void
R_LightPoint(const bspxlightgrid_t *grid, vec3_t p, vec3_t color, entity_t *currententity)
{
	vec3_t		end, pointcolor, dist;
	float		r;
	int			lnum;
	dlight_t	*dl;

	if (!r_worldmodel->lightdata || !currententity)
	{
		color[0] = color[1] = color[2] = 1.0;
		return;
	}

	if (grid)
	{
		BSPX_LightGridValue(grid, r_newrefdef.lightstyles,
			currententity->origin, color);
		return;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;

	r = RecursiveLightPoint(r_worldmodel->nodes, p, end, pointcolor);

	if (r == -1)
	{
		VectorCopy(vec3_origin, color);
	}
	else
	{
		VectorCopy(pointcolor, color);
	}

	/* add dynamic lights */
	dl = r_newrefdef.dlights;

	for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++, dl++)
	{
		float	add;

		VectorSubtract(currententity->origin,
				dl->origin, dist);
		add = dl->intensity - VectorLength(dist);
		add *= (1.0 / 256);

		if (add > 0)
		{
			VectorMA(color, add, dl->color, color);
		}
	}

	VectorScale(color, r_modulate->value, color);
}


//===================================================================

/*
 * Dynamic-light contribution is computed per-pixel in the lightmapped
 * fragment shader. The CPU lightmap baking path used by GL1 / the soft
 * renderer is no longer compiled in here.
 */

/*
** R_SetCacheState
*/
void R_SetCacheState( msurface_t *surf )
{
	int maps;

	for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
		 maps++)
	{
		surf->cached_light[maps] = r_newrefdef.lightstyles[surf->styles[maps]].white;
	}
}

float *s_blocklights = NULL, *s_blocklights_max = NULL;

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the floating format in blocklights
===============
*/
void
R_BuildLightMap(msurface_t *surf, byte *dest, int stride)
{
	int			smax, tmax;
	int			r, g, b, a, max;
	int			i, j, size;
	byte		*lightmap;
	float		scale[4];
	int			mapscount;
	float		*bl;

	if (surf->texinfo->flags & (SURF_SKY|SURF_TRANS33|SURF_TRANS66|SURF_WARP))
	{
		ri.Sys_Error (ERR_DROP, "R_BuildLightMap called for non-lit surface");
	}

	smax = (surf->extents[0] >> surf->lmshift) + 1;
	tmax = (surf->extents[1] >> surf->lmshift) + 1;
	size = smax * tmax;

	if (!s_blocklights || (s_blocklights + (size * 3) >= s_blocklights_max))
	{
		int new_size = ROUNDUP(size * 3, 1024);

		if (new_size < 4096)
		{
			new_size = 4096;
		}

		if (s_blocklights)
		{
			free(s_blocklights);
		}

		s_blocklights = malloc(new_size * sizeof(float));
		s_blocklights_max = s_blocklights + new_size;

		if (!s_blocklights)
		{
			ri.Sys_Error (ERR_DROP, "Can't alloc s_blocklights");
		}
	}

	/* set to full bright if no light data */
	if (!surf->samples)
	{
		for (i = 0; i < size * 3; i++)
		{
			s_blocklights[i] = 255;
		}

		goto store;
	}

	// count the # of maps
	for ( mapscount = 0 ; mapscount < MAXLIGHTMAPS && surf->styles[mapscount] != 255 ;
		 mapscount++)
	{
	}

	lightmap = surf->samples;

	// add all the lightmaps
	if ( mapscount == 1 )
	{
		int maps;

		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			bl = s_blocklights;

			for (i=0 ; i<3 ; i++)
			{
				scale[i] = r_modulate->value*r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ( scale[0] == 1.0F &&
				 scale[1] == 1.0F &&
				 scale[2] == 1.0F )
			{
				for (i=0 ; i<size ; i++, bl+=3)
				{
					bl[0] = lightmap[i*3+0];
					bl[1] = lightmap[i*3+1];
					bl[2] = lightmap[i*3+2];
				}
			}
			else
			{
				for (i=0 ; i<size ; i++, bl+=3)
				{
					bl[0] = lightmap[i*3+0] * scale[0];
					bl[1] = lightmap[i*3+1] * scale[1];
					bl[2] = lightmap[i*3+2] * scale[2];
				}
			}
			lightmap += size*3;		// skip to next lightmap
		}
	}
	else
	{
		int maps;

		memset(s_blocklights, 0, sizeof(s_blocklights[0]) * size * 3);

		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			bl = s_blocklights;

			for (i=0 ; i<3 ; i++)
			{
				scale[i] = r_modulate->value*r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ( scale[0] == 1.0F &&
				 scale[1] == 1.0F &&
				 scale[2] == 1.0F )
			{
				for (i=0 ; i<size ; i++, bl+=3 )
				{
					bl[0] += lightmap[i*3+0];
					bl[1] += lightmap[i*3+1];
					bl[2] += lightmap[i*3+2];
				}
			}
			else
			{
				for (i=0 ; i<size ; i++, bl+=3)
				{
					bl[0] += lightmap[i*3+0] * scale[0];
					bl[1] += lightmap[i*3+1] * scale[1];
					bl[2] += lightmap[i*3+2] * scale[2];
				}
			}
			lightmap += size * 3;		// skip to next lightmap
		}
	}

store:
	// put into texture format
	stride -= (smax<<2);
	bl = s_blocklights;

	for (i = 0; i < tmax; i++, dest += stride)
	{
		for (j = 0; j < smax; j++)
		{
			r = Q_ftol(bl[0]);
			g = Q_ftol(bl[1]);
			b = Q_ftol(bl[2]);

			/* catch negative lights */
			if (r < 0)
			{
				r = 0;
			}

			if (g < 0)
			{
				g = 0;
			}

			if (b < 0)
			{
				b = 0;
			}

			/* determine the brightest of the three color components */
			if (r > g)
			{
				max = r;
			}
			else
			{
				max = g;
			}

			if (b > max)
			{
				max = b;
			}

			/* alpha is ONLY used for the mono lightmap case. For this
			   reason we set it to the brightest of the color components
			   so that things don't get too dim. */
			a = max;

			/* rescale all the color components if the
			   intensity of the greatest channel exceeds
			   1.0 */
			if (max > 255)
			{
				float t = 255.0F / max;

				r = r * t;
				g = g * t;
				b = b * t;
				a = a * t;
			}

			dest[0] = r;
			dest[1] = g;
			dest[2] = b;
			dest[3] = a;

			bl += 3;
			dest += 4;
		}
	}
}
