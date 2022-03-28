/*
 * Copyright (c) 2022, Jeffy Chen <jeffy.chen@rock-chips.com>
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define LIBDRM_MIRROR_VERSION "1.0.0~20220513"

static int drm_debug = 0;
static FILE *log_fp = NULL;

#define LOG_FILE (log_fp ? log_fp : stderr)
#define DRM_LOG(tag, ...) { \
  struct timeval tv; gettimeofday(&tv, NULL); \
  fprintf(LOG_FILE, "[%05ld.%03ld] " tag ": %s(%d) ", \
          tv.tv_sec % 100000, tv.tv_usec / 1000, __func__, __LINE__); \
  fprintf(LOG_FILE, __VA_ARGS__); fflush(LOG_FILE); }

#define DRM_DEBUG(...) if (drm_debug) DRM_LOG("DRM_DEBUG", __VA_ARGS__)
#define DRM_INFO(...) DRM_LOG("DRM_INFO", __VA_ARGS__)
#define DRM_ERROR(...) DRM_LOG("DRM_ERROR", __VA_ARGS__)

#undef ARRAY_SIZE
#define ARRAY_SIZE(x) (int)(sizeof(x)/sizeof(x[0]))

/* Load libdrm symbols */

static drmModeResPtr (* _drmModeGetResources)(int fd) = NULL;
static drmModePlaneResPtr (* _drmModeGetPlaneResources)(int fd) = NULL;
static int (* _drmModeSetCrtc)(int fd, uint32_t crtcId, uint32_t bufferId,
                               uint32_t x, uint32_t y, uint32_t *connectors,
                               int count, drmModeModeInfoPtr mode) = NULL;
static drmModeEncoderPtr (* _drmModeGetEncoder)(int fd,
                                                uint32_t encoder_id) = NULL;
static int (* _drmModePageFlip)(int fd, uint32_t crtc_id, uint32_t fb_id,
                                uint32_t flags, void *user_data) = NULL;
static int (* _drmModePageFlipTarget)(int fd, uint32_t crtc_id, uint32_t fb_id,
                                      uint32_t flags, void *user_data,
                                      uint32_t target_vblank) = NULL;
static drmModePlanePtr (* _drmModeGetPlane)(int fd, uint32_t plane_id) = NULL;
static int (* _drmModeSetPlane)(int fd, uint32_t plane_id, uint32_t crtc_id,
                                uint32_t fb_id, uint32_t flags,
                                int32_t crtc_x, int32_t crtc_y,
                                uint32_t crtc_w, uint32_t crtc_h,
                                uint32_t src_x, uint32_t src_y,
                                uint32_t src_w, uint32_t src_h) = NULL;

#define DRM_SYMBOL(func) { #func, (void **)(&_ ## func), }
static struct {
  const char *func;
  void **symbol;
} drm_symbols[] = {
  DRM_SYMBOL(drmModeGetResources),
  DRM_SYMBOL(drmModeGetPlaneResources),
  DRM_SYMBOL(drmModeSetCrtc),
  DRM_SYMBOL(drmModeGetEncoder),
  DRM_SYMBOL(drmModePageFlip),
  DRM_SYMBOL(drmModePageFlipTarget),
  DRM_SYMBOL(drmModeGetPlane),
  DRM_SYMBOL(drmModeSetPlane),
};

__attribute__((constructor)) static void load_drm_symbols(void)
{
  void *handle, *symbol;
  int i;

#define LIBDRM_SO "libdrm.so.2"

  /* The libdrm should be already loaded */
  handle = dlopen(LIBDRM_SO, RTLD_LAZY | RTLD_NOLOAD);
  if (!handle) {
    /* Should not reach here */
    fprintf(stderr, "FATAL: dlopen(" LIBDRM_SO ") failed(%s)\n", dlerror());
    exit(-1);
  }

  for (i = 0; i < ARRAY_SIZE(drm_symbols); i++) {
    const char *func = drm_symbols[i].func;

    /* Clear error */
    dlerror();

    symbol = dlsym(handle, func);
    if (!symbol) {
      /* Should not reach here */
      fprintf(stderr, "FATAL: " LIBDRM_SO " dlsym(%s) failed(%s)\n",
              func, dlerror());
      dlclose(handle);
      exit(-1);
    }

    *drm_symbols[i].symbol = symbol;
  }

  dlclose(handle);
}

/* Parsing drm-mirror config */

#define DRM_MIRROR_CONFIG_FILE "/etc/drm-mirror.conf"
#define OPT_DEBUG "debug="
#define OPT_LOG_FILE "log-file="
#define OPT_PRIMARY "primary="
#define OPT_FILL_MODE "fill-mode="
#define OPT_MODE "mode="
#define OPT_DST "dst="

enum drm_fill_mode {
  DRM_FILL_MODE_NONE = 0,
  DRM_FILL_MODE_STRETCH,
  DRM_FILL_MODE_FIT,
  DRM_FILL_MODE_CROP,
};

struct drm_rect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

struct drm_connector {
  uint32_t connector_id;
  const char *name;

  int crtc_pipe;
  uint32_t crtc_id;
  uint32_t plane_id;
  uint32_t fb_id;

  enum drm_fill_mode fill_mode;
  const char *modeline;
  struct drm_rect dst;

  drmModeModeInfo mode;
};

static struct {
  uint32_t count_connectors;
  struct drm_connector *connectors;
  struct drm_connector *primary;

  uint32_t src_bpp;
  struct drm_rect src;

  const char *configs;

  const char *primary_name;
  enum drm_fill_mode fill_mode;

  bool inited;
} drm_ctx = {0,};

static const char* drm_fill_mode_names[] = {
  [DRM_FILL_MODE_NONE] = "none",
  [DRM_FILL_MODE_STRETCH] = "stretch",
  [DRM_FILL_MODE_FIT] = "fit",
  [DRM_FILL_MODE_CROP] = "crop",
};

static enum drm_fill_mode drm_parse_fill_mode(const char *name,
                                              enum drm_fill_mode def)
{
  int i;

  for (i = 0; i < ARRAY_SIZE(drm_fill_mode_names); i++) {
    if (!strcmp(drm_fill_mode_names[i], name))
      return i;
  }

  return def;
}

/* based on weston-10: libweston/backend-drm/drm.c */
static const char *const connector_type_names[] = {
  [DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
  [DRM_MODE_CONNECTOR_VGA]         = "VGA",
  [DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
  [DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
  [DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
  [DRM_MODE_CONNECTOR_Composite]   = "Composite",
  [DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
  [DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
  [DRM_MODE_CONNECTOR_Component]   = "Component",
  [DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
  [DRM_MODE_CONNECTOR_DisplayPort] = "DP",
  [DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
  [DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
  [DRM_MODE_CONNECTOR_TV]          = "TV",
  [DRM_MODE_CONNECTOR_eDP]         = "eDP",
  [DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
  [DRM_MODE_CONNECTOR_DSI]         = "DSI",
  [DRM_MODE_CONNECTOR_DPI]         = "DPI",
};

static inline bool drm_connector_is_external(const drmModeConnector *conn)
{
  switch (conn->connector_type) {
    case DRM_MODE_CONNECTOR_LVDS:
    case DRM_MODE_CONNECTOR_eDP:
#ifdef DRM_MODE_CONNECTOR_DSI
    case DRM_MODE_CONNECTOR_DSI:
#endif
      return false;
    default:
      return true;
  }
}

static char *make_connector_name(const drmModeConnector *con)
{
  char *name;
  const char *type_name = NULL;
  int ret;

  if (con->connector_type < ARRAY_SIZE(connector_type_names))
    type_name = connector_type_names[con->connector_type];

  if (!type_name)
    type_name = "UNNAMED";

  ret = asprintf(&name, "%s-%d", type_name, con->connector_type_id);
  if (ret < 0)
    return NULL;

  return name;
}

static void drm_load_config(const char *file)
{
  struct stat st;
  char *configs = NULL, *ptr, *tmp;
  int fd;

  if (stat(file, &st) < 0)
    return;

  fd = open(file, O_RDONLY);
  if (fd < 0)
    return;

  ptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (ptr == MAP_FAILED)
    goto out_close_fd;

  configs = malloc(st.st_size + 1);
  if (!configs)
    goto out_unmap;

  memcpy(configs, ptr, st.st_size);
  configs[st.st_size] = '\0';

  tmp = configs;
  while ((tmp = strchr(tmp, '#'))) {
    while (*tmp != '\n' && *tmp != '\0')
      *tmp++ = '\n';
  }

  drm_ctx.configs = configs;
out_unmap:
  munmap(ptr, st.st_size);
out_close_fd:
  close(fd);
}

static const char *drm_get_config(const char *name, const char *def)
{
  static char buf[4096];
  const char *config;

  if (!drm_ctx.configs)
    return def;

  config = strstr(drm_ctx.configs, name);
  if (!config)
    return def;

  config += strlen(name);
  if (config[0] == '\0' || config[0] == ' ' || config[0] == '\n')
    return def;

  sscanf(config, "%4095s", buf);
  return buf;
}

static int drm_get_config_int(const char *name, int def)
{
  const char *config = drm_get_config(name, NULL);

  if (config)
    return atoi(config);

  return def;
}

static const char *drm_get_connector_config(struct drm_connector *connector,
                                            const char *name, const char *def)
{
  char key[1024];
  snprintf(key, sizeof(key), "%s:%s", connector->name, name);
  return drm_get_config(key, def);
}

static bool drm_plane_is_primary(int fd, int plane_id) {
  drmModeObjectPropertiesPtr props;
  drmModePropertyPtr prop;
  bool found;
  uint32_t i;

  props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (!props)
    return false;

  for (i = 0, found = false; i < props->count_props && !found; i++) {
    prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;

    if (!strcmp(prop->name, "type") &&
        props->prop_values[i] == DRM_PLANE_TYPE_PRIMARY)
      found = true;

    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return found;
}

static void drm_init_drm(int fd)
{
  drmModePlaneResPtr pres;
  drmModeResPtr res;
  drmModeConnectorPtr conn;
  drmModeEncoderPtr encoder;

  struct drm_connector *first_connector = NULL;
  uint32_t primary_planes[16] = {0,};
  uint32_t available_crtcs;
  uint32_t x1, y1, x2, y2;
  const char *config;
  int i, j;

  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  res = _drmModeGetResources(fd);
  assert(res);

  pres = _drmModeGetPlaneResources(fd);
  assert(pres);

  for (i = 0, j = 0; i < (int)pres->count_planes; i++) {
    if (!drm_plane_is_primary(fd, pres->planes[i]))
      continue;

    primary_planes[j++] = pres->planes[i];
  }

  drm_ctx.count_connectors = res->count_connectors;
  drm_ctx.connectors =
    calloc(sizeof(struct drm_connector), res->count_connectors);
  assert(drm_ctx.connectors);

  available_crtcs = (1 << res->count_crtcs) - 1;
  for (i = 0; i < res->count_connectors; i++) {
    struct drm_connector *connector = &drm_ctx.connectors[i];
    connector->connector_id = res->connectors[i];

    conn = drmModeGetConnector(fd, connector->connector_id);
    assert(conn);

    connector->name = make_connector_name(conn);
    assert(connector->name);

    if (!strcmp(drm_ctx.primary_name, connector->name)) {
      /* Got a match */
      drm_ctx.primary = connector;
    } else if (!drm_ctx.primary && conn->connection == DRM_MODE_CONNECTED) {
      if (!first_connector)
        first_connector = connector;

      /* Fallback to first internal connector */
      if (!drm_connector_is_external(conn))
        drm_ctx.primary = connector;
    }

    for (j = 0; j < conn->count_encoders; j++) {
      encoder = _drmModeGetEncoder(fd, conn->encoders[j]);
      assert(encoder);

      connector->crtc_pipe = ffs(encoder->possible_crtcs & available_crtcs) - 1;
      if (connector->crtc_pipe >= 0) {
        connector->crtc_id = res->crtcs[connector->crtc_pipe];
        connector->plane_id = primary_planes[connector->crtc_pipe];
        available_crtcs &= ~(1 << connector->crtc_pipe);
        break;
      }
    }

    config = drm_get_connector_config(connector, OPT_FILL_MODE, "auto");
    connector->fill_mode = drm_parse_fill_mode(config, drm_ctx.fill_mode);

    config = drm_get_connector_config(connector, OPT_MODE, "");
    connector->modeline = strdup(config);

    config = drm_get_connector_config(connector, OPT_DST, "");
    if (sscanf(config, "<%u,%u,%u,%u>", &x1, &y1, &x2, &y2) == 4) {
      if (x2 > x1 && y2 > y1) {
        connector->dst.x = x1;
        connector->dst.y = y1;
        connector->dst.w = x2 - x1;
        connector->dst.h = y2 - y1;
      }
    }

    DRM_INFO("connector(%d):\n\tname=%s crtc=%d(%d) plane=%d\n"
             "\tfill=%s modeline=%s dst=<%d,%d,%d,%d>\n",
             connector->connector_id, connector->name,
             connector->crtc_id, connector->crtc_pipe, connector->plane_id,
             drm_fill_mode_names[connector->fill_mode], connector->modeline,
             connector->dst.x, connector->dst.y,
             connector->dst.x + connector->dst.w,
             connector->dst.y + connector->dst.h);
  }

  /* Fallback to first connector */
  if (!drm_ctx.primary)
    drm_ctx.primary = first_connector;

  assert(drm_ctx.primary);
  assert(drm_ctx.primary->crtc_id);
  DRM_INFO("found primary connector(%s) for %s\n",
           drm_ctx.primary->name, drm_ctx.primary_name);
}

static void drm_init(int fd)
{
  const char *config;
  const char *primary;

  if (drm_ctx.inited)
    return;

  drm_load_config(DRM_MIRROR_CONFIG_FILE);

  drm_debug = drm_get_config_int(OPT_DEBUG, 0);

  if (getenv("DRM_DEBUG") || !access("/tmp/.drm_mirror_debug", F_OK))
    drm_debug = 1;

  if (!(config = getenv("DRM_MIRROR_LOG_FILE")))
    config = drm_get_config(OPT_LOG_FILE, "/var/log/drm-mirror.log");

  log_fp = fopen(config, "wb+");

  config = drm_get_config(OPT_FILL_MODE, "auto");
  drm_ctx.fill_mode = drm_parse_fill_mode(config, DRM_FILL_MODE_FIT);

  if (!(primary = getenv("DRM_MIRROR_PRIMARY")))
    primary = drm_get_config(OPT_PRIMARY, "");

  drm_ctx.primary_name = strdup(primary);

  drm_init_drm(fd);

  drm_ctx.inited = true;
  DRM_INFO("using libdrm-mirror (%s)\n", LIBDRM_MIRROR_VERSION);
}

/* Update mirror connectors */

static uint32_t drm_create_fb(int fd, uint32_t width, uint32_t height)
{
  struct drm_mode_create_dumb create_arg;
  struct drm_mode_destroy_dumb destroy_arg;
  uint32_t fb_id;
  int ret;

  memset(&create_arg, 0, sizeof create_arg);
  create_arg.bpp = drm_ctx.src_bpp;
  create_arg.width = width;
  create_arg.height = height;

  ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
  if (ret < 0)
    return 0;

  ret = drmModeAddFB(fd, create_arg.width, create_arg.height,
                     create_arg.bpp, create_arg.bpp,
                     create_arg.pitch, create_arg.handle, &fb_id);

  memset(&destroy_arg, 0, sizeof(destroy_arg));
  destroy_arg.handle = create_arg.handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

  return fb_id;
}

static int drm_connector_enable(int fd, struct drm_connector *connector)
{
  drmModeModeInfoPtr mode, preferred = NULL, configured = NULL;
  drmModeConnectorPtr conn;
  uint32_t width, height, refresh = 0, fb_id;
  char name[16] = {0};
  int i, ret = -1;

  conn = drmModeGetConnector(fd, connector->connector_id);
  if (!conn)
    return -1;

  if (conn->connection != DRM_MODE_CONNECTED)
    goto err;

  /* Already enabled */
  if (connector->fb_id)
    goto out;

  sscanf(connector->modeline, "%12[^@pP]", name);
  sscanf(connector->modeline, "%ux%u%*[^0-9]%u", &width, &height, &refresh);

  for (i = 0; i < conn->count_modes; i++) {
    mode = &conn->modes[i];

    if (mode->type & DRM_MODE_TYPE_PREFERRED)
      preferred = mode;

    if (!strcmp(mode->name, name) && (!refresh || refresh == mode->vrefresh))
      configured = mode;
  }

  if (configured)
    preferred = configured;

  if (!preferred)
    goto err;

  connector->mode = *preferred;

  /* Do modeset with a dummy FB */

  fb_id = drm_create_fb(fd, connector->mode.hdisplay, connector->mode.vdisplay);
  if (!fb_id)
    goto err;

  ret = _drmModeSetCrtc(fd, connector->crtc_id, fb_id, 0, 0,
                        &connector->connector_id, 1, &connector->mode);
  drmModeRmFB(fd, fb_id);
  if (ret < 0)
    goto err;

  connector->fb_id = fb_id;

  DRM_INFO("%s enabled with mode(%s)\n",
           connector->name, connector->mode.name);
out:
  drmModeFreeConnector(conn);
  return 0;
err:
  drmModeFreeConnector(conn);
  return -1;
}

static void drm_update_mirror(int fd,
                              struct drm_connector *connector, bool check)
{
  double src_ratio, dst_ratio;
  int src_x, src_y, src_w, src_h;
  int dst_x, dst_y, dst_w, dst_h;
  uint32_t fb_id = drm_ctx.primary->fb_id;
  int ret;

  if (drm_ctx.primary == connector)
    return; 

  if (!fb_id)
    goto disable_connector;

  if (!connector->fb_id || check) {
    if (drm_connector_enable(fd, connector) < 0)
      goto disable_connector;
  }

  /* Already done */
  if (fb_id == connector->fb_id)
    return;

  if (connector->dst.w && connector->dst.h) {
    dst_x = connector->dst.x;
    dst_y = connector->dst.y;
    dst_w = connector->dst.w;
    dst_h = connector->dst.h;
  } else {
    dst_x = dst_y = 0;
    dst_w = connector->mode.hdisplay;
    dst_h = connector->mode.vdisplay;
  }

  src_x = drm_ctx.src.x;
  src_y = drm_ctx.src.y;
  src_w = drm_ctx.src.w;
  src_h = drm_ctx.src.h;

  src_ratio = (double) src_w / src_h;
  dst_ratio = (double) dst_w / dst_h;

  switch (connector->fill_mode) {
  case DRM_FILL_MODE_NONE:
    dst_w = src_w = (src_w > dst_w) ? dst_w : src_w;
    dst_h = src_h = (src_h > dst_h) ? dst_h : src_h;
    break;
  case DRM_FILL_MODE_STRETCH:
    break;
  case DRM_FILL_MODE_FIT:
    if (src_ratio < dst_ratio) {
      ret = dst_h * src_ratio;
      dst_x += (dst_w - ret) / 2;
      dst_w = ret;
    } else {
      ret = dst_w / src_ratio;
      dst_y += (dst_h - ret) / 2;
      dst_h = ret;
    }
    break;
  case DRM_FILL_MODE_CROP:
    if (src_ratio > dst_ratio) {
      ret = src_h * dst_ratio;
      src_x += (src_w - ret) / 2;
      src_w = ret;
    } else {
      ret = src_w / dst_ratio;
      src_y += (src_h - ret) / 2;
      src_h = ret;
    }
    break;
  }

  DRM_DEBUG("%s set FB(%d) from %d,%d(%dx%d) to %d,%d(%dx%d)\n",
            connector->name, fb_id, src_x, src_y, src_w, src_h,
            dst_x, dst_y, dst_w, dst_h);

  ret = _drmModeSetPlane(fd, connector->plane_id, connector->crtc_id,
                         fb_id, 0, dst_x, dst_y, dst_w, dst_h,
                         src_x << 16, src_y << 16, src_w << 16, src_h << 16);
  if (ret < 0)
    goto disable_connector;

  connector->fb_id = fb_id;
  return;

disable_connector:
  if (!connector->fb_id)
    return;

  _drmModeSetCrtc(fd, connector->crtc_id, 0, 0, 0, NULL, 0, NULL);
  connector->fb_id = 0;

  DRM_INFO("%s disabled\n", connector->name);
}

static void drm_update_mirrors(int fd, bool check)
{
  uint32_t i;

  for (i = 0; i < drm_ctx.count_connectors; i++)
    drm_update_mirror(fd, &drm_ctx.connectors[i], check);
}

/* Override libdrm APIs */

#define _MIRROR_ADD_RES(res, type, count, value) { \
  (res)->type = realloc((res)->type, sizeof(int) * ((res)->count + 1)); \
  (res)->type[(res)->count] = value; \
  (res)->count++; \
}
#define MIRROR_ADD_RES(res, type, value) \
  _MIRROR_ADD_RES(res, type, count_ ## type, value)

drmModeResPtr drmModeGetResources(int fd)
{
  drmModeResPtr res;
  struct drm_connector *primary;

  res = _drmModeGetResources(fd);
  if (!res)
    return NULL;

  drm_init(fd);
  primary = drm_ctx.primary;

  /* Hide non-primary CRTCs */
  res->crtcs = realloc(res->crtcs, sizeof(int));
  res->crtcs[0] = primary->crtc_id;
  res->count_crtcs = 1;

  /* Hide non-primary connectors */
  res->connectors = realloc(res->connectors, sizeof(int));
  res->connectors[0] = primary->connector_id;
  res->count_connectors = 1;

  /* Force updating connectors' status here */
  drm_update_mirrors(fd, true);
  return res;
}

drmModePlaneResPtr drmModeGetPlaneResources(int fd)
{
  drmModePlaneResPtr pres;
  struct drm_connector *primary;

  pres = _drmModeGetPlaneResources(fd);
  if (!pres)
    return NULL;

  drm_init(fd);
  primary = drm_ctx.primary;

  /* Hide non-primary planes */
  pres->planes = realloc(pres->planes, sizeof(int));
  pres->planes[0] = primary->plane_id;
  pres->count_planes = 1;

  return pres;
}

static int drm_fb_parse(int fd, uint32_t fb_id, uint32_t *bpp,
                        uint32_t *width, uint32_t *height)
{
  drmModeFBPtr fb;

  if (!fb_id)
    return -1;

  fb = drmModeGetFB(fd, fb_id);
  if (!fb)
    return -1;

  *bpp = fb->bpp;
  *width = fb->width;
  *height = fb->height;

  drmModeFreeFB(fb);
  return 0;
}

int drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t fb_id,
                   uint32_t x, uint32_t y, uint32_t *connectors, int count,
                   drmModeModeInfoPtr mode)
{
  int ret;

  if (crtc_id != drm_ctx.primary->crtc_id)
    return -EINVAL;

  ret = _drmModeSetCrtc(fd, crtc_id, fb_id, x, y, connectors, count, mode);
  if (ret < 0) {
    drm_ctx.primary->fb_id = 0;
  } else {
    drm_ctx.primary->fb_id = fb_id;
    drm_ctx.src.x = drm_ctx.src.y = 0;
    drm_fb_parse(fd, fb_id, &drm_ctx.src_bpp, &drm_ctx.src.w, &drm_ctx.src.h);
  }

  DRM_DEBUG("CRTC(%d) set mode(%s) with FB(%d) at %d,%d\n",
            crtc_id, mode ? mode->name : "null", fb_id, x, y);
  drm_update_mirrors(fd, false);
  return ret;
}

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id)
{
  drmModeEncoderPtr encoder;
  struct drm_connector *primary;

  encoder = _drmModeGetEncoder(fd, encoder_id);
  if (!encoder)
    return NULL;

  drm_init(fd);
  primary = drm_ctx.primary;

  /* Hide non-primary CRTCs */
  if (encoder->possible_crtcs & 1 << primary->crtc_pipe)
    encoder->possible_crtcs = 1;
  else
    encoder->possible_crtcs = 0;

  if (encoder->crtc_id != primary->crtc_id)
    encoder->crtc_id = 0;

  return encoder;
}

int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id,
                    uint32_t flags, void *user_data)
{
  int ret;

  if (crtc_id != drm_ctx.primary->crtc_id)
    return -EINVAL;

  ret = _drmModePageFlip(fd, crtc_id, fb_id, flags, user_data);
  if (ret < 0) {
    drm_ctx.primary->fb_id = 0;
  } else {
    if (drm_ctx.primary->fb_id == fb_id)
      return 0;

    drm_ctx.primary->fb_id = fb_id;
    drm_ctx.src.x = drm_ctx.src.y = 0;
    drm_fb_parse(fd, fb_id, &drm_ctx.src_bpp, &drm_ctx.src.w, &drm_ctx.src.h);
  }

  DRM_DEBUG("CRTC(%d) flip FB(%d)\n", crtc_id, fb_id);
  drm_update_mirrors(fd, false);
  return ret;
}

int drmModePageFlipTarget(int fd, uint32_t crtc_id, uint32_t fb_id,
                          uint32_t flags, void *user_data,
                          uint32_t target_vblank)
{
  int ret;

  if (crtc_id != drm_ctx.primary->crtc_id)
    return -EINVAL;

  ret = _drmModePageFlipTarget(fd, crtc_id, fb_id, flags,
                               user_data, target_vblank);
  if (ret < 0) {
    drm_ctx.primary->fb_id = 0;
  } else {
    if (drm_ctx.primary->fb_id == fb_id)
      return 0;

    drm_ctx.primary->fb_id = fb_id;
    drm_ctx.src.x = drm_ctx.src.y = 0;
    drm_fb_parse(fd, fb_id, &drm_ctx.src_bpp, &drm_ctx.src.w, &drm_ctx.src.h);
  }

  DRM_DEBUG("CRTC(%d) flip FB(%d)\n", crtc_id, fb_id);
  drm_update_mirrors(fd, false);
  return ret;
}

drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id)
{
  drmModePlanePtr plane;
  struct drm_connector *primary;

  plane = _drmModeGetPlane(fd, plane_id);
  if (!plane)
    return NULL;

  drm_init(fd);
  primary = drm_ctx.primary;

  /* Hide non-primary CRTCs */
  if (plane->possible_crtcs & 1 << primary->crtc_pipe)
    plane->possible_crtcs = 1;
  else
    plane->possible_crtcs = 0;

  if (plane->crtc_id != primary->crtc_id)
    plane->crtc_id = 0;

  return plane;
}

int drmModeSetPlane(int fd, uint32_t plane_id, uint32_t crtc_id,
                    uint32_t fb_id, uint32_t flags,
                    int32_t crtc_x, int32_t crtc_y,
                    uint32_t crtc_w, uint32_t crtc_h,
                    uint32_t src_x, uint32_t src_y,
                    uint32_t src_w, uint32_t src_h)
{
  int ret = 0;

  if (crtc_id != drm_ctx.primary->crtc_id)
    return -EINVAL;

  if (plane_id != drm_ctx.primary->plane_id)
    return -EINVAL;

  ret = _drmModeSetPlane(fd, plane_id, crtc_id, fb_id, flags,
                         crtc_x, crtc_y, crtc_w, crtc_h,
                         src_x, src_y, src_w, src_h);
  if (ret < 0) {
    drm_ctx.primary->fb_id = 0;
  } else {
    drm_ctx.primary->fb_id = fb_id;
    drm_ctx.src.x = src_x >> 16;
    drm_ctx.src.y = src_y >> 16;
    drm_ctx.src.w = src_w >> 16;
    drm_ctx.src.h = src_h >> 16;
  }

  DRM_DEBUG("CRTC(%d) set FB(%d) on plane(%d) at %d,%d(%dx%d)\n",
            crtc_id, fb_id, plane_id, crtc_x, crtc_y, crtc_w, crtc_h);
  drm_update_mirrors(fd, false);
  return ret;
}

/* Force disabling HW cursor since we are not going to mirror it */

int drmModeMoveCursor(int fd, uint32_t crtcId, int x, int y)
{
  (void) fd;
  (void) crtcId;
  (void) x;
  (void) y;
  return -ENOSYS;
}

int drmModeSetCursor2(int fd, uint32_t crtcId, uint32_t bo_handle,
                      uint32_t width, uint32_t height,
                      int32_t hot_x, int32_t hot_y)
{
  (void) fd;
  (void) crtcId;
  (void) bo_handle;
  (void) width;
  (void) height;
  (void) hot_x;
  (void) hot_y;
  return -ENOSYS;
}
