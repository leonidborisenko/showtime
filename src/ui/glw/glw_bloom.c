/*
 *  GL Widgets - Bloom filter
 *  Copyright (C) 2009 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "glw.h"
#include "glw_bloom.h"

#define EDGE_SIZE 16.0

/**
 *
 */
static void
bloom_destroy_rtt(glw_root_t *gr, glw_bloom_t *b)
{
  int i;
  for(i = 0; i < BLOOM_COUNT; i++)
    glw_rtt_destroy(gr, &b->b_rtt[i]);
  b->b_width = 0;
  b->b_height = 0;
}


/**
 *
 */
static void
glw_bloom_dtor(glw_t *w)
{
  glw_bloom_t *b = (void *)w;

  glw_gf_unregister(&b->b_flushctrl);

  if(b->b_width || b->b_height)
    bloom_destroy_rtt(w->glw_root, b);

  if(b->b_render_initialized)
    glw_render_free(&b->b_render);
}

#if 0
static void
ll(void)
{
  glDisable(GL_TEXTURE_2D);
  glBegin(GL_LINE_LOOP);
  glColor4f(1,1,1,1);
  glVertex3f(-1.0, -1.0, 0.0);
  glVertex3f( 1.0, -1.0, 0.0);
  glVertex3f( 1.0,  1.0, 0.0);
  glVertex3f(-1.0,  1.0, 0.0);
  glEnd();
  glEnable(GL_TEXTURE_2D);
}
#endif


/**
 *
 */
static void 
glw_bloom_render(glw_t *w, glw_rctx_t *rc)
{
  glw_bloom_t *b = (void *)w;
  float a = rc->rc_alpha * w->glw_alpha;
  glw_rctx_t rc0;
  glw_t *c;

  rc0 = *rc;
  rc0.rc_alpha = a;
  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link)
    glw_render0(c, &rc0);

  if(b->b_glow < 0.01)
    return;

  b->b_need_render = a > 0.01;

  if(!b->b_need_render)
    return;
  
  if(glw_is_focusable(w))
    glw_store_matrix(w, rc);
  
  rc0 = *rc;

  glw_PushMatrix(&rc0, rc);
  

  glw_Scalef(&rc0, 
	     1.0 + EDGE_SIZE / rc->rc_size_x, 
	     1.0 + EDGE_SIZE / rc->rc_size_y, 
	     1.0);
#if 0
  glw_render(&b->b_render, w->glw_root, &rc0, 
	     GLW_RENDER_MODE_QUADS, GLW_RENDER_ATTRIBS_TEX,
	     &glw_rtt_texture(&b->b_rtt[0]), 1, 1, 1, a);
#endif

  a *= b->b_glow;

  glw_blendmode(GLW_BLEND_ADDITIVE);
  glw_render(&b->b_render, w->glw_root, &rc0, 
	     GLW_RENDER_MODE_QUADS, GLW_RENDER_ATTRIBS_TEX,
	     &glw_rtt_texture(&b->b_rtt[0]), 1, 1, 1, a * 0.50);


  glw_render(&b->b_render, w->glw_root, &rc0, 
	     GLW_RENDER_MODE_QUADS, GLW_RENDER_ATTRIBS_TEX,
	     &glw_rtt_texture(&b->b_rtt[1]), 1, 1, 1, a * 0.44);


  glw_render(&b->b_render, w->glw_root, &rc0, 
	     GLW_RENDER_MODE_QUADS, GLW_RENDER_ATTRIBS_TEX,
	     &glw_rtt_texture(&b->b_rtt[2]), 1, 1, 1, a * 0.33);
 
  glw_blendmode(GLW_BLEND_NORMAL);

  glw_PopMatrix();

}


/**
 *
 */
static void 
glw_bloom_layout(glw_t *w, glw_rctx_t *rc)
{
  glw_bloom_t *b = (void *)w;
  glw_root_t *gr = w->glw_root;
  glw_rctx_t rc0;
  glw_t *c;
  int x, y, i, sizx, sizy;

  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link)
    glw_layout0(c, rc);
  
  if(b->b_glow < 0.01) {

    if(b->b_width || b->b_height)
      bloom_destroy_rtt(gr, b);
    return;
  }


  sizx = rc->rc_size_x + EDGE_SIZE;
  sizy = rc->rc_size_y + EDGE_SIZE;

  if(b->b_width != sizx || b->b_height != sizy) {
    if(b->b_width || b->b_height)
      bloom_destroy_rtt(gr, b);

    b->b_width  = sizx;
    b->b_height = sizy;

    if(b->b_width || b->b_height) {

      for(i = 0; i < BLOOM_COUNT; i++) {
	x = b->b_width  / (2 << i);
	y = b->b_height / (2 << i);
	glw_rtt_init(gr, &b->b_rtt[i], x, y, 1);
      }
    }
  }

  // Initialize output texture
  if(!b->b_render_initialized) {
    float xs = gr->gr_normalized_texture_coords ? 1.0 : b->b_width;
    float ys = gr->gr_normalized_texture_coords ? 1.0 : b->b_height;

    glw_render_init(&b->b_render, 4, GLW_RENDER_ATTRIBS_TEX);
    b->b_render_initialized = 1;

    glw_render_vtx_pos(&b->b_render, 0, -1.0, -1.0, 0.0);
    glw_render_vtx_st (&b->b_render, 0,  0.0,  0);

    glw_render_vtx_pos(&b->b_render, 1,  1.0, -1.0, 0.0);
    glw_render_vtx_st (&b->b_render, 1,  xs,   0);

    glw_render_vtx_pos(&b->b_render, 2,  1.0,  1.0, 0.0);
    glw_render_vtx_st (&b->b_render, 2,  xs,   ys);

    glw_render_vtx_pos(&b->b_render, 3, -1.0,  1.0, 0.0);
    glw_render_vtx_st (&b->b_render, 3,  0.0,  ys);
  }

  memset(&rc0, 0, sizeof(glw_rctx_t));
  rc0.rc_alpha  = 1;
  rc0.rc_size_x = b->b_width  - EDGE_SIZE;
  rc0.rc_size_y = b->b_height - EDGE_SIZE;


  if(!b->b_need_render)
    return;

  for(i = 0; i < BLOOM_COUNT; i++) {

    glw_rtt_enter(gr, &b->b_rtt[i], &rc0);
    
    rc0.rc_size_x = b->b_width  - EDGE_SIZE;
    rc0.rc_size_y = b->b_height - EDGE_SIZE;

    glw_Scalef(&rc0, 
	       1.0 - EDGE_SIZE / b->b_width,
	       1.0 - EDGE_SIZE / b->b_height, 
	       1.0);
    TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link)
      glw_render0(c, &rc0);
    glw_rtt_restore(gr, &b->b_rtt[i]);
  }
}

/**
 *
 */
static int
glw_bloom_callback(glw_t *w, void *opaque, glw_signal_t signal,
		    void *extra)
{
  switch(signal) {
  default:
    break;
  case GLW_SIGNAL_LAYOUT:
    glw_bloom_layout(w, extra);
    break;
  case GLW_SIGNAL_RENDER:
    glw_bloom_render(w, extra);
    break;
  case GLW_SIGNAL_DTOR:
    glw_bloom_dtor(w);
    break;
  case GLW_SIGNAL_CHILD_CONSTRAINTS_CHANGED:
    glw_copy_constraints(w, extra);
    return 1;
  }
  return 0;
}


/**
 *
 */
static void
bflush(void *aux)
{
  glw_bloom_t *b = aux;

  if(b->b_width || b->b_height)
    bloom_destroy_rtt(b->w.glw_root, b);
}


/*
 *
 */
void 
glw_bloom_ctor(glw_t *w, int init, va_list ap)
{
  glw_bloom_t *b = (void *)w;
  glw_attribute_t attrib;

  if(init) {
    glw_signal_handler_int(w, glw_bloom_callback);

    /* Flush due to opengl shutdown */
    b->b_flushctrl.opaque = b;
    b->b_flushctrl.flush = bflush;
    glw_gf_register(&b->b_flushctrl);
  }

  do {
    attrib = va_arg(ap, int);
    switch(attrib) {
    case GLW_ATTRIB_VALUE:
      b->b_glow = va_arg(ap, double);
      break;

    default:
      GLW_ATTRIB_CHEW(attrib, ap);
      break;
    }
  } while(attrib);
}
