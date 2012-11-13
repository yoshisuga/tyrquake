/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_main.c

#include "console.h"
#include "gl_model.h"
#include "glquake.h"
#include "mathlib.h"
#include "quakedef.h"
#include "render.h"
#include "screen.h"
#include "sound.h"
#include "sys.h"
#include "vid.h"
#include "view.h"

entity_t r_worldentity;
qboolean r_cache_thrash;	// compatability

vec3_t modelorg, r_entorigin;

int r_visframecount;		// bumped when going to a new PVS
int r_framecount;		// used for dlight push checking

static mplane_t frustum[4];

int c_lightmaps_uploaded;
int c_brush_polys;
static int c_alias_polys;

qboolean envmap;		// true during envmap command capture

GLuint currenttexture = -1;	// to avoid unnecessary texture sets
GLuint playertextures[MAX_CLIENTS];// up to 16 color translated skins

int mirrortexturenum;		// quake texturenum, not gltexturenum
qboolean mirror;
mplane_t *mirror_plane;

//
// view origin
//
vec3_t vup;
vec3_t vpn;
vec3_t vright;
vec3_t r_origin;

float r_world_matrix[16];

#ifdef NQ_HACK /* Mirrors disabled for now in QW */
static float r_base_world_matrix[16];
#endif

//
// screen size info
//
refdef_t r_refdef;

mleaf_t *r_viewleaf, *r_oldviewleaf;
texture_t *r_notexture_mip;
int d_lightstylevalue[256];	// 8.8 fraction of base light value

cvar_t r_norefresh = { "r_norefresh", "0" };
cvar_t r_drawentities = { "r_drawentities", "1" };
cvar_t r_drawviewmodel = { "r_drawviewmodel", "1" };
cvar_t r_speeds = { "r_speeds", "0" };
cvar_t r_lightmap = { "r_lightmap", "0" };
cvar_t r_shadows = { "r_shadows", "0" };
cvar_t r_mirroralpha = { "r_mirroralpha", "1" };
cvar_t r_wateralpha = { "r_wateralpha", "1", true };
cvar_t r_dynamic = { "r_dynamic", "1" };
cvar_t r_novis = { "r_novis", "0" };
#ifdef QW_HACK
cvar_t r_netgraph = { "r_netgraph", "0" };
#endif
cvar_t r_waterwarp = { "r_waterwarp", "1" };

cvar_t r_fullbright = {
    .name = "r_fullbright",
    .string = "0",
    .flags = CVAR_DEVELOPER
};
cvar_t gl_keeptjunctions = {
    .name = "gl_keeptjunctions",
    .string = "1",
    .flags = CVAR_OBSOLETE
};
cvar_t gl_reporttjunctions = {
    .name = "gl_reporttjunctions",
    .string = "0",
    .flags = CVAR_OBSOLETE
};
cvar_t gl_texsort = {
    .name = "gl_texsort",
    .string = "1",
    .flags = CVAR_OBSOLETE
};

cvar_t gl_finish = { "gl_finish", "0" };
cvar_t gl_clear = { "gl_clear", "0" };
cvar_t gl_cull = { "gl_cull", "1" };
cvar_t gl_smoothmodels = { "gl_smoothmodels", "1" };
cvar_t gl_affinemodels = { "gl_affinemodels", "0" };
cvar_t gl_polyblend = { "gl_polyblend", "1" };
cvar_t gl_flashblend = { "gl_flashblend", "1" };
cvar_t gl_playermip = { "gl_playermip", "0" };
cvar_t gl_nocolors = { "gl_nocolors", "0" };
#ifdef NQ_HACK
cvar_t gl_doubleeyes = { "gl_doubleeyes", "1" };
#endif

cvar_t _gl_allowgammafallback = { "_gl_allowgammafallback", "1" };

/*
=================
R_CullBox

Returns true if the box is completely outside the frustum
=================
*/
qboolean
R_CullBox(vec3_t mins, vec3_t maxs)
{
    int i;

    for (i = 0; i < 4; i++)
	if (BoxOnPlaneSide(mins, maxs, &frustum[i]) == 2)
	    return true;
    return false;
}


void
R_RotateForEntity(entity_t *e)
{
    glTranslatef(e->origin[0], e->origin[1], e->origin[2]);

    glRotatef(e->angles[1], 0, 0, 1);
    glRotatef(-e->angles[0], 0, 1, 0);
    glRotatef(e->angles[2], 1, 0, 0);
}

/*
=============================================================

  SPRITE MODELS

=============================================================
*/

int R_SpriteDataSize(int numpixels)
{
    return sizeof(GLuint);
}

void R_SpriteDataStore(mspriteframe_t *frame, const char *modelname,
		       int framenum, byte *pixels)
{
    char name[MAX_QPATH];
    GLuint *gl_texturenum = (GLuint *)&frame->rdata[0];

    snprintf(name, sizeof(name), "%s_%i", modelname, framenum);
    *gl_texturenum = GL_LoadTexture(name, frame->width, frame->height, pixels,
				    true, true);
}

/*
================
R_GetSpriteFrame
================
*/
static mspriteframe_t *
R_GetSpriteFrame(entity_t *e)
{
    msprite_t *psprite;
    mspritegroup_t *pspritegroup;
    mspriteframe_t *pspriteframe;
    int i, numframes, frame;
    float *pintervals, fullinterval, targettime, time;

    psprite = e->model->cache.data;
    frame = e->frame;

    if ((frame >= psprite->numframes) || (frame < 0)) {
	Con_Printf("R_DrawSprite: no such frame %d\n", frame);
	frame = 0;
    }

    if (psprite->frames[frame].type == SPR_SINGLE) {
	pspriteframe = psprite->frames[frame].frameptr;
    } else {
	pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
	pintervals = pspritegroup->intervals;
	numframes = pspritegroup->numframes;
	fullinterval = pintervals[numframes - 1];

	time = cl.time + e->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval
	// values are positive, so we don't have to worry about division by 0
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i = 0; i < (numframes - 1); i++) {
	    if (pintervals[i] > targettime)
		break;
	}
	pspriteframe = pspritegroup->frames[i];
    }

    return pspriteframe;
}


/*
=================
R_DrawSpriteModel

=================
*/
static void
R_DrawSpriteModel(entity_t *e)
{
    vec3_t point;
    mspriteframe_t *frame;
    float *up, *right;
    vec3_t v_forward, v_right, v_up;
    msprite_t *psprite;

    // don't even bother culling, because it's just a single
    // polygon without a surface cache
    frame = R_GetSpriteFrame(e);
    psprite = e->model->cache.data;

    if (psprite->type == SPR_ORIENTED) {	// bullet marks on walls
	AngleVectors(e->angles, v_forward, v_right, v_up);
	up = v_up;
	right = v_right;
    } else {			// normal sprite
	up = vup;
	right = vright;
    }

    glColor3f(1, 1, 1);

    GL_DisableMultitexture();
    GL_Bind(*(GLuint *)&frame->rdata[0]);

    glEnable(GL_ALPHA_TEST);
    glBegin(GL_QUADS);

    glTexCoord2f(0, 1);
    VectorMA(e->origin, frame->down, up, point);
    VectorMA(point, frame->left, right, point);
    glVertex3fv(point);

    glTexCoord2f(0, 0);
    VectorMA(e->origin, frame->up, up, point);
    VectorMA(point, frame->left, right, point);
    glVertex3fv(point);

    glTexCoord2f(1, 0);
    VectorMA(e->origin, frame->up, up, point);
    VectorMA(point, frame->right, right, point);
    glVertex3fv(point);

    glTexCoord2f(1, 1);
    VectorMA(e->origin, frame->down, up, point);
    VectorMA(point, frame->right, right, point);
    glVertex3fv(point);

    glEnd();

    glDisable(GL_ALPHA_TEST);
}

/*
=============================================================

  ALIAS MODELS

=============================================================
*/

#define NUMVERTEXNORMALS 162

float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

static vec3_t shadevector;
static float shadelight, ambientlight;

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16
static float r_avertexnormal_dots[SHADEDOT_QUANT][256] =
#include "anorm_dots.h"
     ;

static float *shadedots = r_avertexnormal_dots[0];

static int lastposenum;

/*
=============
GL_DrawAliasFrame
=============
*/
static void
GL_DrawAliasFrame(aliashdr_t *paliashdr, int posenum)
{
    float l;
    trivertx_t *verts;
    int *order;
    int count;

    lastposenum = posenum;

    verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
    verts += posenum * paliashdr->poseverts;
    order = (int *)((byte *)paliashdr + paliashdr->commands);

    while (1) {
	// get the vertex count and primitive type
	count = *order++;
	if (!count)
	    break;		// done
	if (count < 0) {
	    count = -count;
	    glBegin(GL_TRIANGLE_FAN);
	} else
	    glBegin(GL_TRIANGLE_STRIP);

	do {
	    // texture coordinates come from the draw list
	    glTexCoord2f(((float *)order)[0], ((float *)order)[1]);
	    order += 2;

	    // normals and vertexes come from the frame list
	    l = shadedots[verts->lightnormalindex] * shadelight;
	    glColor3f(l, l, l);
	    glVertex3f(verts->v[0], verts->v[1], verts->v[2]);
	    verts++;
	} while (--count);

	glEnd();
    }
}

/*
=============
GL_DrawAliasShadow
=============
*/
static void
GL_DrawAliasShadow(entity_t *e, aliashdr_t *paliashdr, int posenum)
{
    trivertx_t *verts;
    int *order;
    vec3_t point;
    float height, lheight;
    int count;

    lheight = e->origin[2] - lightspot[2];
    height = -lheight + 1.0;

    verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
    verts += posenum * paliashdr->poseverts;
    order = (int *)((byte *)paliashdr + paliashdr->commands);

    while (1) {
	// get the vertex count and primitive type
	count = *order++;
	if (!count)
	    break;		// done
	if (count < 0) {
	    count = -count;
	    glBegin(GL_TRIANGLE_FAN);
	} else
	    glBegin(GL_TRIANGLE_STRIP);

	do {
	    // texture coordinates come from the draw list
	    // (skipped for shadows) glTexCoord2fv ((float *)order);
	    order += 2;

	    // normals and vertexes come from the frame list
	    point[0] =
		verts->v[0] * paliashdr->scale[0] +
		paliashdr->scale_origin[0];
	    point[1] =
		verts->v[1] * paliashdr->scale[1] +
		paliashdr->scale_origin[1];
	    point[2] =
		verts->v[2] * paliashdr->scale[2] +
		paliashdr->scale_origin[2];

	    point[0] -= shadevector[0] * (point[2] + lheight);
	    point[1] -= shadevector[1] * (point[2] + lheight);
	    point[2] = height;
//                      height -= 0.001;
	    glVertex3fv(point);

	    verts++;
	} while (--count);

	glEnd();
    }
}



/*
=================
R_SetupAliasFrame

=================
*/
static void
R_SetupAliasFrame(int frame, aliashdr_t *paliashdr)
{
    int pose, numposes;
    float interval;

    if ((frame >= paliashdr->numframes) || (frame < 0)) {
	Con_DPrintf("R_AliasSetupFrame: no such frame %d\n", frame);
	frame = 0;
    }

    pose = paliashdr->frames[frame].firstpose;
    numposes = paliashdr->frames[frame].numposes;

    if (numposes > 1) {
	interval = paliashdr->frames[frame].interval;
	pose += (int)(cl.time / interval) % numposes;
    }

    GL_DrawAliasFrame(paliashdr, pose);
}



/*
=================
R_DrawAliasModel
=================
*/
static void
R_DrawAliasModel(entity_t *e)
{
    int i;
    int lnum;
    vec3_t dist;
    float add;
    model_t *clmodel;
    vec3_t mins, maxs;
    aliashdr_t *paliashdr;
    float an;
    int anim;

    clmodel = e->model;

    VectorAdd(e->origin, clmodel->mins, mins);
    VectorAdd(e->origin, clmodel->maxs, maxs);

    if (R_CullBox(mins, maxs))
	return;

    VectorCopy(e->origin, r_entorigin);
    VectorSubtract(r_origin, r_entorigin, modelorg);

    //
    // get lighting information
    //
    ambientlight = shadelight = R_LightPoint(e->origin);

    // allways give the gun some light
    if (e == &cl.viewent && ambientlight < 24)
	ambientlight = shadelight = 24;

    for (lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
	if (cl_dlights[lnum].die >= cl.time) {
	    VectorSubtract(e->origin, cl_dlights[lnum].origin, dist);
	    add = cl_dlights[lnum].radius - Length(dist);

	    if (add > 0) {
		ambientlight += add;
		/* ZOID: models should be affected by dlights as well */
		shadelight += add;
	    }
	}
    }

    // clamp lighting so it doesn't overbright as much
    if (ambientlight > 128)
	ambientlight = 128;
    if (ambientlight + shadelight > 192)
	shadelight = 192 - ambientlight;

    // ZOID: never allow players to go totally black
#ifdef NQ_HACK
    if (CL_PlayerEntity(e)) {
#endif
#ifdef QW_HACK
    if (!strcmp(clmodel->name, "progs/player.mdl")) {
#endif
	if (ambientlight < 8)
	    ambientlight = shadelight = 8;
    } else if (!strcmp(clmodel->name, "progs/flame.mdl")
	       || !strcmp(clmodel->name, "progs/flame2.mdl")) {
	// HACK HACK HACK -- no fullbright colors, so make torches full light
	ambientlight = shadelight = 256;
    }

    shadedots =
	r_avertexnormal_dots[((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0)))
			     & (SHADEDOT_QUANT - 1)];
    shadelight = shadelight / 200.0;

    an = e->angles[1] / 180 * M_PI;
    shadevector[0] = cos(-an);
    shadevector[1] = sin(-an);
    shadevector[2] = 1;
    VectorNormalize(shadevector);

    //
    // locate the proper data
    //
    paliashdr = (aliashdr_t *)Mod_Extradata(e->model);

    c_alias_polys += paliashdr->numtris;

    //
    // draw all the triangles
    //
    GL_DisableMultitexture();
    glPushMatrix();
    R_RotateForEntity(e);

#ifdef NQ_HACK
    if (!strcmp(clmodel->name, "progs/eyes.mdl") && gl_doubleeyes.value) {
#endif
#ifdef QW_HACK
    if (!strcmp(clmodel->name, "progs/eyes.mdl")) {
#endif
	glTranslatef(paliashdr->scale_origin[0], paliashdr->scale_origin[1],
		     paliashdr->scale_origin[2] - (22 + 8));
	// double size of eyes, since they are really hard to see in gl
	glScalef(paliashdr->scale[0] * 2, paliashdr->scale[1] * 2,
		 paliashdr->scale[2] * 2);
    } else {
	glTranslatef(paliashdr->scale_origin[0], paliashdr->scale_origin[1],
		     paliashdr->scale_origin[2]);
	glScalef(paliashdr->scale[0], paliashdr->scale[1],
		 paliashdr->scale[2]);
    }

    anim = (int)(cl.time * 10) & 3;
    GL_Bind(paliashdr->gl_texturenum[e->skinnum][anim]);

    // we can't dynamically colormap textures, so they are cached
    // seperately for the players.  Heads are just uncolored.
#ifdef NQ_HACK
    if (e->colormap != vid.colormap && !gl_nocolors.value) {
	i = CL_PlayerEntity(e);
	if (i)
	    GL_Bind(playertextures[i - 1]);
    }
#endif
#ifdef QW_HACK
    if (e->scoreboard && !gl_nocolors.value) {
	i = e->scoreboard - cl.players;
	if (!e->scoreboard->skin) {
	    Skin_Find(e->scoreboard);
	    R_TranslatePlayerSkin(i);
	}
	if (i >= 0 && i < MAX_CLIENTS)
	    GL_Bind(playertextures[i]);
    }
#endif

    if (gl_smoothmodels.value)
	glShadeModel(GL_SMOOTH);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    if (gl_affinemodels.value)
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);

    R_SetupAliasFrame(e->frame, paliashdr);

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glShadeModel(GL_FLAT);
    if (gl_affinemodels.value)
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

    glPopMatrix();

    if (r_shadows.value) {
	glPushMatrix();
	R_RotateForEntity(e);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glColor4f(0, 0, 0, 0.5);
	GL_DrawAliasShadow(e, paliashdr, lastposenum);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);
	glPopMatrix();
    }
}

//=============================================================================

/*
===============
R_MarkLeaves
===============
*/
static void
R_MarkLeaves(void)
{
    byte *vis;
    mnode_t *node;
    int i;
    byte solid[4096];

    if (r_oldviewleaf == r_viewleaf && !r_novis.value)
	return;

    if (mirror)
	return;

    r_visframecount++;
    r_oldviewleaf = r_viewleaf;

    if (r_novis.value) {
	vis = solid;
	memset(solid, 0xff, (cl.worldmodel->numleafs + 7) >> 3);
    } else
	vis = Mod_LeafPVS(r_viewleaf, cl.worldmodel);

    for (i = 0; i < cl.worldmodel->numleafs; i++) {
	if (vis[i >> 3] & (1 << (i & 7))) {
	    node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
	    do {
		if (node->visframe == r_visframecount)
		    break;
		node->visframe = r_visframecount;
		node = node->parent;
	    } while (node);
	}
    }
}

/*
====================
R_DrawEntitiesOnList
====================
*/
static void
R_DrawEntitiesOnList(void)
{
    entity_t *e;
    int i;

    if (!r_drawentities.value)
	return;

    // draw sprites seperately, because of alpha blending
    for (i = 0; i < cl_numvisedicts; i++) {
	e = &cl_visedicts[i];
	switch (e->model->type) {
	case mod_alias:
	    R_DrawAliasModel(e);
	    break;
	case mod_brush:
	    R_DrawBrushModel(e);
	    break;
	default:
	    break;
	}
    }

    for (i = 0; i < cl_numvisedicts; i++) {
	e = &cl_visedicts[i];
	switch (e->model->type) {
	case mod_sprite:
	    R_DrawSpriteModel(e);
	    break;
	default:
	    break;
	}
    }
}

/*
=============
R_DrawViewModel
=============
*/
static void
R_DrawViewModel(void)
{
    float ambient[4], diffuse[4];
    int j;
    int lnum;
    vec3_t dist;
    float add;
    dlight_t *dl;
    int ambientlight, shadelight;
    entity_t *e;

#ifdef NQ_HACK
    if (!r_drawviewmodel.value)
	return;

    if (chase_active.value)
	return;
#endif
#ifdef QW_HACK
    if (!r_drawviewmodel.value || !Cam_DrawViewModel())
	return;
#endif

    if (envmap)
	return;

    if (!r_drawentities.value)
	return;

    if (cl.stats[STAT_ITEMS] & IT_INVISIBILITY)
	return;

    if (cl.stats[STAT_HEALTH] <= 0)
	return;

    e = &cl.viewent;
    if (!e->model)
	return;

    j = R_LightPoint(e->origin);

    if (j < 24)
	j = 24;			// allways give some light on gun
    ambientlight = j;
    shadelight = j;

// add dynamic lights
    for (lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
	dl = &cl_dlights[lnum];
	if (!dl->radius)
	    continue;
	if (!dl->radius)
	    continue;
	if (dl->die < cl.time)
	    continue;

	VectorSubtract(e->origin, dl->origin, dist);
	add = dl->radius - Length(dist);
	if (add > 0)
	    ambientlight += add;
    }

    ambient[0] = ambient[1] = ambient[2] = ambient[3] =
	(float)ambientlight / 128;
    diffuse[0] = diffuse[1] = diffuse[2] = diffuse[3] =
	(float)shadelight / 128;

    // hack the depth range to prevent view model from poking into walls
    glDepthRange(gldepthmin, gldepthmin + 0.3 * (gldepthmax - gldepthmin));
    R_DrawAliasModel(e);
    glDepthRange(gldepthmin, gldepthmax);
}

/*
 * GL_DrawBlendPoly
 * - Render a polygon covering the whole screen
 * - Used for full-screen color blending and approximated gamma correction
 */
static void
GL_DrawBlendPoly(void)
{
    glBegin(GL_QUADS);
    glVertex3f(10, 100, 100);
    glVertex3f(10, -100, 100);
    glVertex3f(10, -100, -100);
    glVertex3f(10, 100, -100);
    glEnd();
}

/*
============
R_PolyBlend
============
*/
static void
R_PolyBlend(void)
{
    float gamma = 1.0;

    if (!VID_IsFullScreen() || (!VID_SetGammaRamp &&
				_gl_allowgammafallback.value)) {
	gamma = v_gamma.value * v_gamma.value;
	if (gamma < 0.25)
	    gamma = 0.25;
	else if (gamma > 1.0)
	    gamma = 1.0;
    }

    if ((gl_polyblend.value && v_blend[3]) || gamma < 1.0) {
	GL_DisableMultitexture();

	glDisable(GL_ALPHA_TEST);
	glEnable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);

	glLoadIdentity();
	glRotatef(-90, 1, 0, 0);	// put Z going up
	glRotatef(90, 0, 0, 1);		// put Z going up

	if (gl_polyblend.value && v_blend[3]) {
	    glColor4fv(v_blend);
	    GL_DrawBlendPoly();
	}
	if (gamma < 1.0) {
	    glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
	    glColor4f(1, 1, 1, gamma);
	    GL_DrawBlendPoly();
	    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_ALPHA_TEST);
    }
}


static int
SignbitsForPlane(mplane_t *out)
{
    int bits, j;

    // for fast box on planeside test
    bits = 0;
    for (j = 0; j < 3; j++) {
	if (out->normal[j] < 0)
	    bits |= 1 << j;
    }
    return bits;
}


static void
R_SetFrustum(void)
{
    int i;

    // FIXME - organise better?
    if (r_lockfrustum.value)
	return;

    if (r_refdef.fov_x == 90) {
	// front side is visible

	VectorAdd(vpn, vright, frustum[0].normal);
	VectorSubtract(vpn, vright, frustum[1].normal);

	VectorAdd(vpn, vup, frustum[2].normal);
	VectorSubtract(vpn, vup, frustum[3].normal);
    } else {
	// rotate VPN right by FOV_X/2 degrees
	RotatePointAroundVector(frustum[0].normal, vup, vpn,
				-(90 - r_refdef.fov_x / 2));
	// rotate VPN left by FOV_X/2 degrees
	RotatePointAroundVector(frustum[1].normal, vup, vpn,
				90 - r_refdef.fov_x / 2);
	// rotate VPN up by FOV_X/2 degrees
	RotatePointAroundVector(frustum[2].normal, vright, vpn,
				90 - r_refdef.fov_y / 2);
	// rotate VPN down by FOV_X/2 degrees
	RotatePointAroundVector(frustum[3].normal, vright, vpn,
				-(90 - r_refdef.fov_y / 2));
    }

    for (i = 0; i < 4; i++) {
	frustum[i].type = PLANE_ANYZ;	// FIXME - true for all angles?
	frustum[i].dist = DotProduct(r_origin, frustum[i].normal);
	frustum[i].signbits = SignbitsForPlane(&frustum[i]);
    }
}



/*
===============
R_SetupFrame
===============
*/
void
R_SetupFrame(void)
{
// don't allow cheats in multiplayer
#ifdef NQ_HACK
    if (cl.maxclients > 1)
	Cvar_Set("r_fullbright", "0");
#endif
#ifdef QW_HACK
    r_fullbright.value = 0;
    r_lightmap.value = 0;
    if (!atoi(Info_ValueForKey(cl.serverinfo, "watervis")))
	r_wateralpha.value = 1;
#endif

    R_AnimateLight();

    r_framecount++;

// build the transformation matrix for the given view angles
    VectorCopy(r_refdef.vieworg, r_origin);

    AngleVectors(r_refdef.viewangles, vpn, vright, vup);

// current viewleaf
    r_oldviewleaf = r_viewleaf;
    if (!r_viewleaf || !r_lockpvs.value)
	r_viewleaf = Mod_PointInLeaf(r_origin, cl.worldmodel);

// color shifting for water, etc.
    V_SetContentsColor(r_viewleaf->contents);
    V_CalcBlend();

// surface cache isn't thrashing (don't have one in GL?)
    r_cache_thrash = false;

// reset count of polys for this frame
    c_brush_polys = 0;
    c_alias_polys = 0;
    c_lightmaps_uploaded = 0;
}


static void
MYgluPerspective(GLdouble fovy, GLdouble aspect,
		 GLdouble zNear, GLdouble zFar)
{
    GLdouble xmin, xmax, ymin, ymax;

    ymax = zNear * tan(fovy * M_PI / 360.0);
    ymin = -ymax;

    xmin = ymin * aspect;
    xmax = ymax * aspect;

    glFrustum(xmin, xmax, ymin, ymax, zNear, zFar);
}


/*
=============
R_SetupGL
=============
*/
static void
R_SetupGL(void)
{
    float screenaspect;
    int x, x2, y2, y, w, h;

    //
    // set up viewpoint
    //
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    x = r_refdef.vrect.x * glwidth / vid.width;
    x2 = (r_refdef.vrect.x + r_refdef.vrect.width) * glwidth / vid.width;
    y = (vid.height - r_refdef.vrect.y) * glheight / vid.height;
    y2 = (vid.height -
	  (r_refdef.vrect.y + r_refdef.vrect.height)) * glheight / vid.height;

    // fudge around because of frac screen scale
    // FIXME: well not fix, but figure out why this is done...
    if (x > 0)
	x--;
    if (x2 < glwidth)
	x2++;
    if (y2 < 0)
	y2--;
    if (y < glheight)
	y++;

    w = x2 - x;
    h = y - y2;

    // FIXME: Skybox? Regular Quake sky?
    if (envmap) {
	x = y2 = 0;
	w = h = 256;
    }

    glViewport(glx + x, gly + y2, w, h);
    screenaspect = (float)r_refdef.vrect.width / r_refdef.vrect.height;

    // FIXME - wtf is all this about?
    //yfov = 2*atan((float)r_refdef.vrect.height/r_refdef.vrect.width)*180/M_PI;
    //yfov = (2.0 * tan (scr_fov.value/360*M_PI)) / screenaspect;
    //yfov = 2*atan((float)r_refdef.vrect.height/r_refdef.vrect.width)*(scr_fov.value*2)/M_PI;
    //MYgluPerspective (yfov,  screenaspect,  4,  4096);

    // FIXME - set depth dynamically for improved depth precision in smaller
    //         spaces
    //MYgluPerspective (r_refdef.fov_y, screenaspect, 4, 4096);
    MYgluPerspective(r_refdef.fov_y, screenaspect, 4, 6144);

    if (mirror) {
	if (mirror_plane->normal[2])
	    glScalef(1, -1, 1);
	else
	    glScalef(-1, 1, 1);
	glCullFace(GL_BACK);
    } else
	glCullFace(GL_FRONT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glRotatef(-90, 1, 0, 0);	// put Z going up
    glRotatef(90, 0, 0, 1);	// put Z going up
    glRotatef(-r_refdef.viewangles[2], 1, 0, 0);
    glRotatef(-r_refdef.viewangles[0], 0, 1, 0);
    glRotatef(-r_refdef.viewangles[1], 0, 0, 1);
    glTranslatef(-r_refdef.vieworg[0], -r_refdef.vieworg[1],
		 -r_refdef.vieworg[2]);

    glGetFloatv(GL_MODELVIEW_MATRIX, r_world_matrix);

    //
    // set drawing parms
    //
    if (gl_cull.value)
	glEnable(GL_CULL_FACE);
    else
	glDisable(GL_CULL_FACE);

    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_DEPTH_TEST);
}

/*
================
R_RenderScene

r_refdef must be set before the first call
================
*/
static void
R_RenderScene(void)
{
    R_SetupFrame();
    R_SetFrustum();
    R_SetupGL();
    R_MarkLeaves();		// done here so we know if we're in water
    R_DrawWorld();		// adds static entities to the list
    S_ExtraUpdate();		// don't let sound get messed up if going slow
    R_DrawEntitiesOnList();
    GL_DisableMultitexture();
    R_RenderDlights();
    R_DrawParticles();
}


/*
=============
R_Clear
=============
*/
static void
R_Clear(void)
{
    if (r_mirroralpha.value != 1.0) {
	if (gl_clear.value)
	    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	else
	    glClear(GL_DEPTH_BUFFER_BIT);
	gldepthmin = 0;
	gldepthmax = 0.5;
	glDepthFunc(GL_LEQUAL);
    } else if (gl_ztrick.value) {
	static unsigned int trickframe = 0;

	if (gl_clear.value)
	    glClear(GL_COLOR_BUFFER_BIT);

	trickframe++;
	if (trickframe & 1) {
	    gldepthmin = 0;
	    gldepthmax = 0.49999;
	    glDepthFunc(GL_LEQUAL);
	} else {
	    gldepthmin = 1;
	    gldepthmax = 0.5;
	    glDepthFunc(GL_GEQUAL);
	}
    } else {
	if (gl_clear.value)
	    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	else
	    glClear(GL_DEPTH_BUFFER_BIT);
	gldepthmin = 0;
	gldepthmax = 1;
	glDepthFunc(GL_LEQUAL);
    }

    glDepthRange(gldepthmin, gldepthmax);
}

#ifdef NQ_HACK /* Mirrors disabled for now in QW */
/*
=============
R_Mirror
=============
*/
static void
R_Mirror(void)
{
    float d;
    msurface_t *s;
    entity_t *ent;

    if (!mirror)
	return;

    memcpy(r_base_world_matrix, r_world_matrix, sizeof(r_base_world_matrix));

    d = DotProduct(r_refdef.vieworg,
		   mirror_plane->normal) - mirror_plane->dist;
    VectorMA(r_refdef.vieworg, -2 * d, mirror_plane->normal,
	     r_refdef.vieworg);

    d = DotProduct(vpn, mirror_plane->normal);
    VectorMA(vpn, -2 * d, mirror_plane->normal, vpn);

    r_refdef.viewangles[0] = -asin(vpn[2]) / M_PI * 180;
    r_refdef.viewangles[1] = atan2(vpn[1], vpn[0]) / M_PI * 180;
    r_refdef.viewangles[2] = -r_refdef.viewangles[2];

    ent = &cl_entities[cl.viewentity];
    if (cl_numvisedicts < MAX_VISEDICTS) {
	cl_visedicts[cl_numvisedicts] = *ent;
	cl_numvisedicts++;
    }

    gldepthmin = 0.5;
    gldepthmax = 1;
    glDepthRange(gldepthmin, gldepthmax);
    glDepthFunc(GL_LEQUAL);

    R_RenderScene();
    R_DrawWaterSurfaces();

    gldepthmin = 0;
    gldepthmax = 0.5;
    glDepthRange(gldepthmin, gldepthmax);
    glDepthFunc(GL_LEQUAL);

    // blend on top
    glEnable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    if (mirror_plane->normal[2])
	glScalef(1, -1, 1);
    else
	glScalef(-1, 1, 1);
    glCullFace(GL_FRONT);
    glMatrixMode(GL_MODELVIEW);

    glLoadMatrixf(r_base_world_matrix);

    glColor4f(1, 1, 1, r_mirroralpha.value);
    s = cl.worldmodel->textures[mirrortexturenum]->texturechain;
    for (; s; s = s->texturechain)
	R_RenderBrushPoly(&r_worldentity, s);
    cl.worldmodel->textures[mirrortexturenum]->texturechain = NULL;
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
}
#endif

/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
void
R_RenderView(void)
{
    double time1 = 0, time2;

    if (r_norefresh.value)
	return;

    if (!r_worldentity.model || !cl.worldmodel)
	Sys_Error("%s: NULL worldmodel", __func__);

    if (gl_finish.value || r_speeds.value)
	glFinish();

    if (r_speeds.value) {
	time1 = Sys_DoubleTime();
	c_brush_polys = 0;
	c_alias_polys = 0;
	c_lightmaps_uploaded = 0;
    }

    mirror = false;

    R_Clear();

    // render normal view
    R_RenderScene();
    R_DrawViewModel();
    R_DrawWaterSurfaces();

#ifdef NQ_HACK /* Mirrors disabled for now in QW */
    // render mirror view
    R_Mirror();
#endif

    R_PolyBlend();

    if (r_speeds.value) {
//              glFinish ();
	time2 = Sys_DoubleTime();
	Con_Printf("%3i ms  %4i wpoly %4i epoly %4i dlit\n",
		   (int)((time2 - time1) * 1000), c_brush_polys,
		   c_alias_polys, c_lightmaps_uploaded);
    }
}
