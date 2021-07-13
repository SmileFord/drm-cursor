/*
 *  Copyright (c) 2021, Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>

#include "drm_egl.h"

#define LIBDRM_CURSOR_VERSION "1.1.1~20210713"

#define DRM_LOG(tag, ...) { \
  fprintf(g_log_fp ?: stderr, tag ": %s(%d) ", __func__, __LINE__); \
  fprintf(g_log_fp ?: stderr, __VA_ARGS__); fflush(g_log_fp ?: stderr); }

#define DRM_DEBUG(...) \
  if (g_drm_debug) DRM_LOG("DRM_DEBUG", __VA_ARGS__)

#define DRM_INFO(...) DRM_LOG("DRM_INFO", __VA_ARGS__)

#define DRM_ERROR(...) DRM_LOG("DRM_ERROR", __VA_ARGS__)

#ifndef DRM_FORMAT_MOD_VENDOR_ARM
#define DRM_FORMAT_MOD_VENDOR_ARM 0x08
#endif

#ifndef DRM_FORMAT_MOD_ARM_AFBC
#define DRM_FORMAT_MOD_ARM_AFBC(__afbc_mode) fourcc_mod_code(ARM, __afbc_mode)
#endif

#ifndef AFBC_FORMAT_MOD_BLOCK_SIZE_16x16
#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 (1ULL)
#endif

#ifndef AFBC_FORMAT_MOD_SPARSE
#define AFBC_FORMAT_MOD_SPARSE (((__u64)1) << 6)
#endif

#define DRM_AFBC_MODIFIER \
  (DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE) | \
   DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16))

#define DRM_CURSOR_CONFIG_FILE "/etc/drm-cursor.conf"
#define OPT_DEBUG "debug="
#define OPT_LOG_FILE "log-file="
#define OPT_ALLOW_OVERLAY "allow-overlay="
#define OPT_PREFER_AFBC "prefer-afbc="
#define OPT_PREFER_PLANE "prefer-plane="
#define OPT_PREFER_PLANES "prefer-planes="
#define OPT_CRTC_BLOCKLIST "crtc-blocklist="
#define OPT_NUM_SURFACES "num-surfaces="

#define DRM_MAX_CRTCS 8

typedef struct {
  uint32_t plane_id;
  drmModePlane *plane;
  drmModeObjectProperties *props;
} drm_plane;

typedef struct {
  uint32_t handle;
  uint32_t fb;

  int width;
  int height;

  int x;
  int y;

  int off_x;
  int off_y;

  int reload;
} drm_cursor_state;

typedef enum {
  IDLE = 0,
  BUSY,
  ERROR,
  PENDING,
} drm_thread_state;

typedef struct {
  uint32_t crtc_id;
  uint32_t crtc_pipe;

  uint32_t plane_id;
  uint32_t prefer_plane_id;

  drm_cursor_state cursor_next;
  drm_cursor_state cursor_curr;

  pthread_t thread;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  drm_thread_state state;

  void *egl_ctx;

  int use_afbc_modifier;
  int blocked;
} drm_crtc;

typedef struct {
  int fd;

  drm_crtc crtcs[DRM_MAX_CRTCS];
  int num_crtcs;

  drmModePlaneResPtr pres;
  drmModeRes *res;

  int prefer_afbc_modifier;
  int allow_overlay;
  int num_surfaces;
  int inited;

  char *configs;
} drm_ctx;

static drm_ctx g_drm_ctx = { 0, };
static int g_drm_debug = 0;
static FILE *g_log_fp = NULL;

static int drm_plane_get_prop(drm_ctx *ctx, drm_plane *plane, const char *name)
{
  drmModePropertyPtr prop;
  int i;

  for (i = 0; i < plane->props->count_props; i++) {
    prop = drmModeGetProperty(ctx->fd, plane->props->props[i]);
    if (prop && !strcmp(prop->name, name)) {
      drmModeFreeProperty(prop);
      return i;
    }
    drmModeFreeProperty(prop);
  }

  return -1;
}

static int drm_plane_get_prop_value(drm_ctx *ctx, drm_plane *plane,
                                    const char *name, uint64_t *value)
{
  int prop_idx = drm_plane_get_prop(ctx, plane, name);
  if (prop_idx < 0)
    return -1;

  *value = plane->props->prop_values[prop_idx];
  return 0;
}

static int drm_plane_set_prop_max(drm_ctx *ctx, drm_plane *plane,
                                  const char *name)
{
  drmModePropertyPtr prop;
  int prop_idx = drm_plane_get_prop(ctx, plane, name);
  if (prop_idx < 0)
    return -1;

  prop = drmModeGetProperty(ctx->fd, plane->props->props[prop_idx]);
  drmModeObjectSetProperty (ctx->fd, plane->plane_id,
                            DRM_MODE_OBJECT_PLANE,
                            plane->props->props[prop_idx],
                            prop->values[prop->count_values - 1]);
  DRM_DEBUG("set plane %d prop: %s to max: %"PRIu64"\n",
            plane->plane_id, name, prop->values[prop->count_values - 1]);
  drmModeFreeProperty(prop);
  return 0;
}

static void drm_free_plane(drm_plane *plane)
{
  drmModeFreeObjectProperties(plane->props);
  drmModeFreePlane(plane->plane);
  free(plane);
}

static drm_plane *drm_get_plane(drm_ctx *ctx, uint32_t plane_id)
{
  drm_plane *plane = calloc(1, sizeof(*plane));
  if (!plane)
    return NULL;

  plane->plane_id = plane_id;
  plane->plane = drmModeGetPlane(ctx->fd, plane_id);
  if (!plane->plane)
    goto err;

  plane->props = drmModeObjectGetProperties(ctx->fd, plane_id,
                                            DRM_MODE_OBJECT_PLANE);
  if (!plane->props)
    goto err;

  return plane;
err:
  drm_free_plane(plane);
  return NULL;
}

static int drm_plane_has_afbc(drm_ctx *ctx, drm_plane *plane)
{
  drmModePropertyBlobPtr blob;
  struct drm_format_modifier_blob *header;
  struct drm_format_modifier *modifiers;
  uint64_t value;
  int i;

  if (drm_plane_get_prop_value(ctx, plane, "IN_FORMATS", &value) < 0)
    return -1;

  blob = drmModeGetPropertyBlob(ctx->fd, value);
  if (!blob)
    return -1;

  header = blob->data;
  modifiers = (struct drm_format_modifier *)
    ((char *) header + header->modifiers_offset);

  for (i = 0; i < header->count_modifiers; i++) {
    if (modifiers[i].modifier == DRM_AFBC_MODIFIER) {
      drmModeFreePropertyBlob(blob);
      return 0;
    }
  }

  drmModeFreePropertyBlob(blob);
  return -1;
}

static void drm_load_configs(drm_ctx *ctx)
{
  struct stat st;
  const char *file = DRM_CURSOR_CONFIG_FILE;
  char *ptr, *tmp;
  int fd;

  if (stat(file, &st) < 0)
    return;

  fd = open(file, O_RDONLY);
  if (fd < 0)
    return;

  ptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (ptr == MAP_FAILED)
    goto out_close_fd;

  ctx->configs = malloc(st.st_size + 1);
  if (!ctx->configs)
    goto out_unmap;

  memcpy(ctx->configs, ptr, st.st_size);
  ctx->configs[st.st_size] = '\0';

  tmp = ctx->configs;
  while ((tmp = strchr(tmp, '#'))) {
    while (*tmp != '\n' && *tmp != '\0')
      *tmp++ = '\n';
  }

out_unmap:
  munmap(ptr, st.st_size);
out_close_fd:
  close(fd);
}

static const char *drm_get_config(drm_ctx *ctx, const char *name)
{
  static char buf[4096];
  const char *config;

  if (!ctx->configs)
    return NULL;

  config = strstr(ctx->configs, name);
  if (!config)
    return NULL;

  sscanf(config + strlen(name), "%4095s", buf);
  return buf;
}

static int drm_get_config_int(drm_ctx *ctx, const char *name, int def)
{
  const char *config = drm_get_config(ctx, name);

  if (config)
    return atoi(config);

  return def;
}

static drm_ctx *drm_get_ctx(int fd)
{
  drm_ctx *ctx = &g_drm_ctx;
  uint32_t prefer_planes[DRM_MAX_CRTCS] = { 0, };
  uint32_t prefer_plane = 0;
  const char *config;
  int i;

  if (ctx->inited)
    return ctx;

  /* Failed already */
  if (ctx->fd < 0)
    return NULL;

  ctx->fd = dup(fd);
  if (ctx->fd < 0)
    return NULL;

  drm_load_configs(ctx);

  g_drm_debug = drm_get_config_int(ctx, OPT_DEBUG, 0);

  if (getenv("DRM_DEBUG") || !access("/tmp/.drm_cursor_debug", F_OK))
    g_drm_debug = 1;

  if (!(config = getenv("DRM_CURSOR_LOG_FILE")))
    config = drm_get_config(ctx, OPT_LOG_FILE);

  g_log_fp = fopen(config ?: "/var/log/drm-cursor.log", "wb+");

#ifdef PREFER_AFBC_MODIFIER
  ctx->prefer_afbc_modifier = 1;
#endif

  ctx->prefer_afbc_modifier =
    drm_get_config_int(ctx, OPT_PREFER_AFBC, ctx->prefer_afbc_modifier);

  if (ctx->prefer_afbc_modifier)
    DRM_DEBUG("prefer ARM AFBC modifier\n");

  ctx->allow_overlay = drm_get_config_int(ctx, OPT_ALLOW_OVERLAY, 0);

  if (ctx->allow_overlay)
    DRM_DEBUG("allow overlay planes\n");

  drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1);
  drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  ctx->num_surfaces = drm_get_config_int(ctx, OPT_NUM_SURFACES, 8);

  ctx->res = drmModeGetResources(ctx->fd);
  if (!ctx->res)
    goto err_free_configs;

  ctx->pres = drmModeGetPlaneResources(ctx->fd);
  if (!ctx->pres)
    goto err_free_res;

  /* Allow specifying prefer plane */
  if ((config = getenv("DRM_CURSOR_PREFER_PLANE")))
    prefer_plane = atoi(config);
  else
    prefer_plane = drm_get_config_int(ctx, OPT_PREFER_PLANE, 0);

  /* Allow specifying prefer planes */
  if (!(config = getenv("DRM_CURSOR_PREFER_PLANES")))
    config = drm_get_config(ctx, OPT_PREFER_PLANES);
  for (i = 0; config && i < ctx->res->count_crtcs; i++) {
    prefer_planes[i] = atoi(config);

    config = strchr(config, ',');
    if (config)
      config++;
  }

  /* Fetch all CRTCs */
  for (i = 0; i < ctx->res->count_crtcs; i++) {
    drmModeCrtcPtr c = drmModeGetCrtc(ctx->fd, ctx->res->crtcs[i]);
    drm_crtc *crtc = &ctx->crtcs[ctx->num_crtcs];

    if (!c)
      continue;

    crtc->crtc_id = c->crtc_id;
    crtc->crtc_pipe = i;
    crtc->prefer_plane_id = prefer_planes[i] ?: prefer_plane;

    DRM_DEBUG("found %d CRTC: %d(%d) (%dx%d) prefer plane: %d\n",
              ctx->num_crtcs, c->crtc_id, i, c->width, c->height,
              crtc->prefer_plane_id);

    ctx->num_crtcs++;
    drmModeFreeCrtc(c);
  }

  DRM_DEBUG("found %d CRTCs\n", ctx->num_crtcs);

  if (!ctx->num_crtcs)
    goto err_free_pres;

  config = drm_get_config(ctx, OPT_CRTC_BLOCKLIST);
  for (i = 0; config && i < ctx->res->count_crtcs; i++) {
    int crtc_id = atoi(config);

    for (int j = 0; j < ctx->num_crtcs; j++) {
      drm_crtc *crtc = &ctx->crtcs[j];
      if (crtc->crtc_id != crtc_id)
        continue;

      DRM_DEBUG("CRTC: %d blocked\n", crtc_id);
      crtc->blocked = 1;
    }

    config = strchr(config, ',');
    if (config)
      config++;
  }

  if (g_drm_debug) {
    /* Dump planes for debugging */
    for (i = 0; i < ctx->pres->count_planes; i++) {
      drm_plane *plane = drm_get_plane(ctx, ctx->pres->planes[i]);
      char *type;
      uint64_t value = 0;
      int has_afbc;

      if (!plane)
        continue;

      has_afbc = !(drm_plane_has_afbc(ctx, plane) < 0);

      drm_plane_get_prop_value(ctx, plane, "type", &value);
      switch (value) {
      case DRM_PLANE_TYPE_PRIMARY:
        type = "primary";
        break;
      case DRM_PLANE_TYPE_OVERLAY:
        type = "overlay";
        break;
      case DRM_PLANE_TYPE_CURSOR:
        type = "cursor ";
        break;
      default:
        type = "unknown";
        break;
      }

      DRM_DEBUG("found plane: %d[%s] crtcs: 0x%x %s\n",
                plane->plane_id, type, plane->plane->possible_crtcs,
                has_afbc ? "(AFBC)" : "");

      drm_free_plane(plane);
    }
  }

  DRM_INFO("using libdrm-cursor (%s)\n", LIBDRM_CURSOR_VERSION);

  ctx->inited = 1;
  return ctx;

err_free_pres:
  drmModeFreePlaneResources(ctx->pres);
err_free_res:
  drmModeFreeResources(ctx->res);
err_free_configs:
  free(ctx->configs);
  close(ctx->fd);
  ctx->fd = -1;
  return NULL;
}

#define drm_crtc_bind_plane_force(ctx, crtc, plane_id) \
  drm_crtc_bind_plane(ctx, crtc, plane_id, 0, 1)

#define drm_crtc_bind_plane_cursor(ctx, crtc, plane_id) \
  drm_crtc_bind_plane(ctx, crtc, plane_id, 0, 0)

#define drm_crtc_bind_plane_afbc(ctx, crtc, plane_id) \
  drm_crtc_bind_plane(ctx, crtc, plane_id, 1, 0)

static int drm_crtc_bind_plane(drm_ctx *ctx, drm_crtc *crtc, uint32_t plane_id,
                               int use_afbc, int allow_overlay)
{
  drm_plane *plane;
  uint64_t value;
  int i, ret = -1;

  /* CRTC already assigned */
  if (crtc->plane_id)
    return 1;

  /* Plane already assigned */
  for (i = 0; i < ctx->num_crtcs; i++) {
    if (ctx->crtcs[i].plane_id == plane_id)
      return -1;
  }

  plane = drm_get_plane(ctx, plane_id);
  if (!plane)
    return -1;

  /* Not for this CRTC */
  if (!(plane->plane->possible_crtcs & (1 << crtc->crtc_pipe)))
    goto out;

  /* Not using primary planes */
  if (drm_plane_get_prop_value(ctx, plane, "type", &value) < 0)
    goto out;

  if (value == DRM_PLANE_TYPE_PRIMARY)
    goto out;

  /* Check for overlay plane */
  if (!allow_overlay && value == DRM_PLANE_TYPE_OVERLAY)
    goto out;

  /* Check for AFBC modifier */
  if (drm_plane_has_afbc(ctx, plane) >= 0)
    crtc->use_afbc_modifier = 1;
  else if (use_afbc)
    goto out;

  DRM_DEBUG("CRTC[%d]: bind plane: %d%s\n", crtc->crtc_id, plane->plane_id,
            crtc->use_afbc_modifier ? "(AFBC)" : "");

  crtc->plane_id = plane->plane_id;

  /* Set maximum ZPOS */
  drm_plane_set_prop_max(ctx, plane, "zpos");
  drm_plane_set_prop_max(ctx, plane, "ZPOS");

  ret = 0;
out:
  drm_free_plane(plane);
  return ret;
}

#define drm_crtc_disable_cursor(ctx, crtc) \
  drm_crtc_update_cursor(ctx, crtc, NULL)

static int drm_crtc_update_cursor(drm_ctx *ctx, drm_crtc *crtc,
                                  drm_cursor_state *cursor_state)
{
  uint32_t old_fb = crtc->cursor_curr.fb;
  uint32_t fb;
  int x, y, w, h, ret;

  /* Disable */
  if (!cursor_state) {
    if (old_fb) {
      DRM_DEBUG("CRTC[%d]: disabling cursor\n", crtc->crtc_id);
      drmModeSetPlane(ctx->fd, crtc->plane_id, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0);

      drmModeRmFB(ctx->fd, old_fb);
    }

    memset(&crtc->cursor_curr, 0, sizeof(drm_cursor_state));
    return 0;
  }

  /* Unchanged */
  if (crtc->cursor_curr.fb == cursor_state->fb &&
      crtc->cursor_curr.x == cursor_state->x &&
      crtc->cursor_curr.y == cursor_state->y) {
    crtc->cursor_curr = *cursor_state;
    return 0;
  }

  fb = cursor_state->fb;
  x = cursor_state->x;
  y = cursor_state->y;
  w = crtc->cursor_curr.width;
  h = crtc->cursor_curr.height;

  DRM_DEBUG("CRTC[%d]: setting fb: %d (%dx%d) on plane: %d at (%d,%d)\n",
            crtc->crtc_id, fb, w, h, crtc->plane_id, x, y);

  ret = drmModeSetPlane(ctx->fd, crtc->plane_id, crtc->crtc_id, fb, 0,
                        x, y, w, h, 0, 0, w << 16, h << 16);
  if (ret)
    DRM_ERROR("CRTC[%d]: failed to set plane (%d)\n", crtc->crtc_id, errno);

  if (old_fb && old_fb != fb) {
    DRM_DEBUG("CRTC[%d]: remove FB: %d\n", crtc->crtc_id, old_fb);
    drmModeRmFB(ctx->fd, old_fb);
  }

  crtc->cursor_curr = *cursor_state;
  return ret;
}

static int drm_crtc_create_fb(drm_ctx *ctx, drm_crtc *crtc,
                              drm_cursor_state *cursor_state)
{
  uint32_t handle = cursor_state->handle;
  int width = cursor_state->width;
  int height = cursor_state->height;
  int off_x = cursor_state->off_x;
  int off_y = cursor_state->off_y;

  DRM_DEBUG("CRTC[%d]: convert FB from %d (%dx%d) offset:(%d,%d)\n",
            crtc->crtc_id, handle, width, height, off_x, off_y);

  if (!crtc->egl_ctx) {
    uint64_t modifier;
    int format;

    if (crtc->use_afbc_modifier) {
      /* Mali only support AFBC with BGR formats now */
      format = GBM_FORMAT_ABGR8888;
      modifier = DRM_AFBC_MODIFIER;
    } else {
      format = GBM_FORMAT_ARGB8888;
      modifier = 0;
    }

    crtc->egl_ctx = egl_init_ctx(ctx->fd, ctx->num_surfaces,
                                 width, height, format, modifier);
    if (!crtc->egl_ctx) {
      DRM_ERROR("CRTC[%d]: failed to init egl ctx\n", crtc->crtc_id);
      return -1;
    }
  }

  cursor_state->fb =
    egl_convert_fb(crtc->egl_ctx, handle, width, height, off_x, off_y);
  if (!cursor_state->fb) {
    DRM_ERROR("CRTC[%d]: failed to create FB\n", crtc->crtc_id);
    return -1;
  }

  DRM_DEBUG("CRTC[%d]: created FB: %d\n", crtc->crtc_id, cursor_state->fb);
  return 0;
}

static void *drm_crtc_thread_fn(void *data)
{
  drm_ctx *ctx = drm_get_ctx(-1);
  drm_crtc *crtc = data;
  drm_cursor_state cursor_state;

  DRM_DEBUG("CRTC[%d]: thread started\n", crtc->crtc_id);

  while (1) {
    /* Wait for new cursor state */
    pthread_mutex_lock(&crtc->mutex);
    while (crtc->state != PENDING)
      pthread_cond_wait(&crtc->cond, &crtc->mutex);

    cursor_state = crtc->cursor_next;
    crtc->state = BUSY;
    pthread_mutex_unlock(&crtc->mutex);

    if (cursor_state.reload) {
      /* Handle set-cursor */
      DRM_DEBUG("CRTC[%d]: set new cursor %d (%dx%d)\n",
                crtc->crtc_id, cursor_state.handle,
                cursor_state.width, cursor_state.height);

      if (!cursor_state.handle) {
        drm_crtc_disable_cursor(ctx, crtc);
        goto next;
      }

      if (drm_crtc_create_fb(ctx, crtc, &cursor_state) < 0)
        goto error;

      if (drm_crtc_update_cursor(ctx, crtc, &cursor_state) < 0) {
        DRM_ERROR("CRTC[%d]: failed to set cursor\n", crtc->crtc_id);
        goto error;
      }
    } else {
      /* Handle move-cursor */
      DRM_DEBUG("CRTC[%d]: move cursor to (%d+%d,%d+%d)\n",
                crtc->crtc_id, cursor_state.x, cursor_state.off_x,
                cursor_state.y, cursor_state.off_y);

      if (crtc->cursor_curr.off_x != cursor_state.off_x ||
          crtc->cursor_curr.off_y != cursor_state.off_y) {
        /* Edge moving */
        if (drm_crtc_create_fb(ctx, crtc, &cursor_state) < 0)
          goto error;
      } else if (!crtc->cursor_curr.fb) {
        /* Pre-moving */
        crtc->cursor_curr = cursor_state;
        goto next;
      } else {
        /* Normal moving */
        cursor_state.fb = crtc->cursor_curr.fb;
      }

      if (drm_crtc_update_cursor(ctx, crtc, &cursor_state) < 0) {
        DRM_ERROR("CRTC[%d]: failed to move cursor\n", crtc->crtc_id);
        goto error;
      }
    }

next:
    pthread_mutex_lock(&crtc->mutex);
    if (crtc->state != PENDING) {
      crtc->state = IDLE;
      pthread_cond_signal(&crtc->cond);
    }
    pthread_mutex_unlock(&crtc->mutex);
  }

error:
  if (crtc->egl_ctx)
    egl_free_ctx(crtc->egl_ctx);

  drm_crtc_disable_cursor(ctx, crtc);

  pthread_mutex_lock(&crtc->mutex);
  DRM_DEBUG("CRTC[%d]: thread error\n", crtc->crtc_id);
  crtc->state = ERROR;
  pthread_cond_signal(&crtc->cond);
  pthread_mutex_unlock(&crtc->mutex);

  return NULL;
}

static int drm_crtc_prepare(drm_ctx *ctx, drm_crtc *crtc)
{
  int i;

  /* CRTC already assigned */
  if (crtc->plane_id)
    return 1;

  /* Try specific plane */
  if (crtc->prefer_plane_id)
    drm_crtc_bind_plane_force(ctx, crtc, crtc->prefer_plane_id);

  /* Try cursor plane */
  for (i = 0; !crtc->plane_id && i < ctx->pres->count_planes; i++)
    drm_crtc_bind_plane_cursor(ctx, crtc, ctx->pres->planes[i]);

  /* Try AFBC plane */
  if (ctx->prefer_afbc_modifier) {
    for (i = 0; !crtc->plane_id && i < ctx->pres->count_planes; i++)
      drm_crtc_bind_plane_afbc(ctx, crtc, ctx->pres->planes[i]);
  }

  if (ctx->allow_overlay) {
    /* Fallback to any available overlay plane */
    for (i = ctx->pres->count_planes - 1; !crtc->plane_id && i; i--)
      drm_crtc_bind_plane_force(ctx, crtc, ctx->pres->planes[i]);
  }

  if (!crtc->plane_id) {
    DRM_ERROR("CRTC[%d]: failed to find any plane\n", crtc->crtc_id);
    return -1;
  }

  pthread_cond_init(&crtc->cond, NULL);
  pthread_mutex_init(&crtc->mutex, NULL);
  pthread_create(&crtc->thread, NULL, drm_crtc_thread_fn, crtc);

  return 0;
}

static int drm_crtc_size(drm_ctx *ctx, uint32_t crtc_id,
                         int *width, int *height)
{
  drmModeCrtcPtr c;
  int ret;

  c = drmModeGetCrtc(ctx->fd, crtc_id);
  if (!c)
    return -1;

  if (width)
    *width = c->width;
  if (height)
    *height = c->height;

  ret = (c->width && c->height) ? 0 : -1;

  drmModeFreeCrtc(c);
  return ret;
}

static drm_crtc *drm_get_crtc(drm_ctx *ctx, uint32_t crtc_id)
{
  drm_crtc *crtc = NULL;
  int i;

  for (i = 0; i < ctx->num_crtcs; i++) {
    crtc = &ctx->crtcs[i];
    if (!crtc_id && drm_crtc_size(ctx, crtc->crtc_id, NULL, NULL) < 0)
      continue;

    if (crtc->blocked)
      continue;

    if (!crtc_id || crtc->crtc_id == crtc_id)
      break;
  }

  if (i == ctx->num_crtcs) {
    DRM_ERROR("CRTC[%d]: not available\n", crtc_id);
    return NULL;
  }

  return crtc;
}

static int drm_set_cursor(int fd, uint32_t crtc_id, uint32_t handle,
                          uint32_t width, uint32_t height)
{
  drm_crtc *crtc;
  drm_ctx *ctx;
  drm_cursor_state *cursor_next;

  ctx = drm_get_ctx(fd);
  if (!ctx)
    return -1;

  crtc = drm_get_crtc(ctx, crtc_id);
  if (!crtc)
    return -1;

  if (drm_crtc_prepare(ctx, crtc) < 0)
    return -1;

  DRM_DEBUG("CRTC[%d]: request setting new cursor %d (%dx%d)\n",
            crtc->crtc_id, handle, width, height);

  pthread_mutex_lock(&crtc->mutex);
  if (crtc->state == ERROR) {
    pthread_mutex_unlock(&crtc->mutex);
    DRM_ERROR("CRTC[%d]: failed to set cursor\n", crtc->crtc_id);
    return -1;
  }

  /* Update next cursor state and notify the thread */
  cursor_next = &crtc->cursor_next;

  cursor_next->reload = 1;
  cursor_next->fb = 0;
  cursor_next->handle = handle;
  cursor_next->width = width;
  cursor_next->height = height;
  crtc->state = PENDING;
  pthread_cond_signal(&crtc->cond);

  /* Wait for result (success or failed) */
  while (crtc->state != IDLE && crtc->state != ERROR)
    pthread_cond_wait(&crtc->cond, &crtc->mutex);

  pthread_mutex_unlock(&crtc->mutex);

  if (crtc->state == ERROR) {
    DRM_ERROR("CRTC[%d]: failed to set cursor\n", crtc->crtc_id);
    return -1;
  }

  return 0;
}

static int drm_move_cursor(int fd, uint32_t crtc_id, int x, int y)
{
  drm_ctx *ctx;
  drm_crtc *crtc;
  drm_cursor_state *cursor_next;
  int width, height, off_x, off_y;

  ctx = drm_get_ctx(fd);
  if (!ctx)
    return -1;

  crtc = drm_get_crtc(ctx, crtc_id);
  if (!crtc)
    return -1;

  if (drm_crtc_prepare(ctx, crtc) < 0)
    return -1;

  if (drm_crtc_size(ctx, crtc->crtc_id, &width, &height) < 0)
    return -1;

  DRM_DEBUG("CRTC[%d]: request moving cursor to (%d,%d) in (%dx%d)\n",
            crtc->crtc_id, x, y, width, height);

  off_x = off_y = 0;

  /* For edge moving */
  width -= crtc->cursor_curr.width;
  height -= crtc->cursor_curr.height;

  if (x < 0) {
    off_x = x;
    x = 0;
  }
  if (y < 0) {
    off_y = y;
    y = 0;
  }
  if (x > width) {
    off_x = x - width;
    x = width;
  }
  if (y > height) {
    off_y = y - height;
    y = height;
  }

  pthread_mutex_lock(&crtc->mutex);
  if (crtc->state == ERROR) {
    pthread_mutex_unlock(&crtc->mutex);
    return -1;
  }

  /* Update next cursor state and notify the thread */
  cursor_next = &crtc->cursor_next;

  cursor_next->reload = 0;
  cursor_next->fb = 0;
  cursor_next->x = x;
  cursor_next->y = y;
  cursor_next->off_x = off_x;
  cursor_next->off_y = off_y;
  crtc->state = PENDING;
  pthread_cond_signal(&crtc->cond);
  pthread_mutex_unlock(&crtc->mutex);

  return 0;
}

/* Hook functions */

int drmModeSetCursor(int fd, uint32_t crtcId, uint32_t bo_handle,
                     uint32_t width, uint32_t height)
{
  /* Init log file */
  drm_get_ctx(fd);

  DRM_DEBUG("fd: %d crtc: %d handle: %d size: %dx%d\n",
            fd, crtcId, bo_handle, width, height);
  return drm_set_cursor(fd, crtcId, bo_handle, width, height);
}

int drmModeMoveCursor(int fd, uint32_t crtcId, int x, int y)
{
  DRM_DEBUG("fd: %d crtc: %d position: %d,%d\n", fd, crtcId, x, y);
  return drm_move_cursor(fd, crtcId, x, y);
}

int drmModeSetCursor2(int fd, uint32_t crtcId, uint32_t bo_handle,
                      uint32_t width, uint32_t height,
                      int32_t hot_x, int32_t hot_y)
{
  DRM_DEBUG("fd: %d crtc: %d handle: %d size: %dx%d\n",
            fd, crtcId, bo_handle, width, height);
  return -EINVAL;
}
