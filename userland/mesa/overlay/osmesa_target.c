/*
 * OSMesa's screen: the Mali (Panfrost, through the DRM stand-in src/s8-drm) where the kernel has one,
 * else softpipe. Based on Mesa 25.0.7 src/gallium/targets/osmesa/osmesa_target.c (Brian Paul, MIT) and the
 * HakiOS S9+ approach (MIT). OSMESA_SOFTWARE=1 forces softpipe.
 */

#include "target-helpers/inline_sw_helper.h"
#include "target-helpers/inline_debug_helper.h"

#include "sw/null/null_sw_winsys.h"

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/u_debug.h"
#include "panfrost/drm/panfrost_drm_public.h"
#include <xf86drm.h>

static int gpu_fd = -1;

static const driOptionDescription panfrost_options[] = {
#include "panfrost/driinfo_panfrost.h"
};

static struct pipe_screen *
osmesa_create_gpu_screen(void)
{
   static driOptionCache info, cache;
   static struct pipe_screen_config config;
   struct pipe_screen *screen;
   int fd;

   if (debug_get_bool_option("OSMESA_SOFTWARE", false))
      return NULL;
   fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return NULL;
   driParseOptionInfo(&info, panfrost_options, ARRAY_SIZE(panfrost_options));
   driParseConfigFiles(&cache, &info, 0, "panfrost", NULL, NULL, NULL, 0, NULL, 0);
   config.options = &cache;
   config.options_info = &info;
   screen = panfrost_drm_screen_create_renderonly(fd, NULL, &config);
   if (screen)
      gpu_fd = fd;
   return screen ? debug_screen_wrap(screen) : NULL;
}

struct pipe_screen *
osmesa_create_screen(void);

struct pipe_screen *
osmesa_create_screen(void)
{
   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   screen = osmesa_create_gpu_screen();
   if (screen)
      return screen;

   /* We use a null software winsys since we always just render to ordinary
    * driver resources.
    */
   winsys = null_sw_create();
   if (!winsys)
      return NULL;

   /* Create llvmpipe or softpipe screen */
   screen = sw_screen_create(winsys);
   if (!screen) {
      winsys->destroy(winsys);
      return NULL;
   }

   /* Inject optional trace, debug, etc. wrappers */
   return debug_screen_wrap(screen);
}
