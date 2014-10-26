#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <wlc.h>
#include <wayland-util.h>

// XXX: hack
enum {
   BIT_BEMENU = 1<<5,
};

static struct {
   struct wlc_compositor *compositor;
   struct wlc_view *active;
   uint32_t prefix;
} loliwm = {
   .prefix = WLC_BIT_MOD_ALT,
};

static bool
is_tiled(struct wlc_view *view)
{
   uint32_t state = wlc_view_get_state(view);
   return !(state & WLC_BIT_FULLSCREEN) && !(state & BIT_BEMENU) && !wlc_view_get_parent(view);
}

static void
relayout(struct wlc_space *space)
{
   struct wl_list *views;
   if (!(views = wlc_space_get_userdata(space)))
      return;

   uint32_t rwidth, rheight;
   struct wlc_output *output = wlc_space_get_output(space);
   wlc_output_get_resolution(output, &rwidth, &rheight);

   struct wlc_view *v;
   uint32_t count = 0;
   wlc_view_for_each_user(v, views)
      if (is_tiled(v)) ++count;

   bool toggle = false;
   uint32_t y = 0, height = rheight / (count > 1 ? count - 1 : 1);
   wlc_view_for_each_user(v, views) {
      if (wlc_view_get_state(v) & BIT_BEMENU) {
         wlc_view_resize(v, rwidth, wlc_view_get_height(v));
         wlc_view_position(v, 0, 0);
      }

      if ((wlc_view_get_state(v) & WLC_BIT_FULLSCREEN)) {
         wlc_view_resize(v, rwidth, rheight);
         wlc_view_position(v, 0, 0);
      }

      if (!is_tiled(v))
         continue;

      wlc_view_set_state(v, WLC_BIT_MAXIMIZED, true);
      wlc_view_resize(v, (count > 1 ? rwidth / 2 : rwidth), (toggle ? height : rheight));
      wlc_view_position(v, (toggle ? rwidth / 2 : 0), y);

      if (toggle)
         y += height;

      toggle = true;
   }
}

static void
cycle(struct wlc_compositor *compositor)
{
   struct wl_list *l = wlc_space_get_userdata(wlc_compositor_get_focused_space(compositor));

   if (!l)
      return;

   struct wlc_view *v;
   uint32_t count = 0;
   wlc_view_for_each_user(v, l)
      if (is_tiled(v)) ++count;

   // Check that we have at least two tiled views
   // so we don't get in infinite loop.
   if (count <= 1)
      return;

   // Cycle until we hit next tiled view.
   struct wl_list *p;
   do {
      p = l->prev;
      wl_list_remove(l->prev);
      wl_list_insert(l, p);
   } while (!is_tiled(wlc_view_from_user_link(p)));

   relayout(wlc_compositor_get_focused_space(compositor));
}

static void
set_active(struct wlc_compositor *compositor, struct wlc_view *view)
{
   if (loliwm.active == view)
      return;

   if (loliwm.active && (wlc_view_get_state(loliwm.active) & BIT_BEMENU)) {
      wlc_view_bring_to_front(loliwm.active);
      return;
   }

   if (loliwm.active)
      wlc_view_set_state(loliwm.active, WLC_BIT_ACTIVATED, false);

   if (view) {
      struct wlc_view *v;
      struct wl_list *views = wlc_space_get_views(wlc_view_get_space(view));
      wlc_view_for_each_reverse(v, views) {
         if ((wlc_view_get_state(v) & WLC_BIT_FULLSCREEN)) {
            // Bring the first topmost found fullscreen wlc_view to front.
            // This way we get a "peek" effect when we cycle other views.
            // Meaning the active view is always over fullscreen view,
            // but fullscreen view is on top of the other views.
            wlc_view_bring_to_front(v);
            break;
         }
      }

      wlc_view_set_state(view, WLC_BIT_ACTIVATED, true);
      wlc_view_bring_to_front(view);

      wlc_view_for_each_reverse(v, views) {
         if ((wlc_view_get_state(v) & BIT_BEMENU)) {
            // Always bring bemenu to front when exists.
            wlc_view_bring_to_front(v);
            break;
         }
      }
   }

   wlc_compositor_focus_view(compositor, view);
   loliwm.active = view;
}

static void
focus_next_view(struct wlc_compositor *compositor, struct wlc_view *view)
{
   struct wl_list *l = wlc_view_get_user_link(view)->next;
   struct wl_list *views = wlc_space_get_userdata(wlc_view_get_space(view));
   if (!l || wl_list_empty(views) || (l == views && !(l = l->next)))
      return;

   struct wlc_view *v;
   if (!(v = wlc_view_from_user_link(l)))
      return;

   set_active(compositor, v);
}

static struct wlc_space*
space_for_index(struct wl_list *spaces, int index)
{
   int i = 0;
   struct wlc_space *s;
   wlc_space_for_each(s, spaces) {
      if (index == i)
         return s;
      ++i;
   }
   return NULL;
}

static void
focus_space(struct wlc_compositor *compositor, int index)
{
   struct wlc_space *active = wlc_compositor_get_focused_space(compositor);
   struct wl_list *spaces = wlc_output_get_spaces(wlc_space_get_output(active));
   struct wlc_space *s = space_for_index(spaces, index);

   if (s)
      wlc_output_focus_space(wlc_space_get_output(s), s);
}

static struct wlc_output*
output_for_index(struct wl_list *outputs, int index)
{
   int i = 0;
   struct wlc_output *o;
   wlc_output_for_each(o, outputs) {
      if (index == i)
         return o;
      ++i;
   }
   return NULL;
}

static void
move_to_output(struct wlc_compositor *compositor, struct wlc_view *view, int index)
{
   struct wl_list *outputs = wlc_compositor_get_outputs(compositor);
   struct wlc_output *o = output_for_index(outputs, index);

   if (o)
      wlc_view_set_space(view, wlc_output_get_active_space(o));
}

static void
move_to_space(struct wlc_compositor *compositor, struct wlc_view *view, int index)
{
   struct wlc_space *active = wlc_compositor_get_focused_space(compositor);
   struct wl_list *spaces = wlc_output_get_spaces(wlc_space_get_output(active));
   struct wlc_space *s = space_for_index(spaces, index);

   if (s)
      wlc_view_set_space(view, s);
}

static void
focus_next_output(struct wlc_compositor *compositor)
{
   struct wlc_output *active = wlc_compositor_get_focused_output(compositor);
   struct wl_list *l = wlc_output_get_link(active)->next;
   struct wl_list *outputs = wlc_compositor_get_outputs(compositor);
   if (!l || wl_list_empty(outputs) || (l == outputs && !(l = l->next)))
      return;

   struct wlc_output *o;
   if (!(o = wlc_output_from_link(l)))
      return;

   wlc_compositor_focus_output(compositor, o);
}

static bool
view_created(struct wlc_compositor *compositor, struct wlc_view *view, struct wlc_space *space)
{
   (void)compositor;

   struct wl_list *views;
   if (!(views = wlc_space_get_userdata(space))) {
      if (!(views = calloc(1, sizeof(struct wl_list))))
         return false;

      wl_list_init(views);
      wlc_space_set_userdata(space, views);
   }

   if (wlc_view_get_class(view) && !strcmp(wlc_view_get_class(view), "bemenu"))
      wlc_view_set_state(view, BIT_BEMENU, true); // XXX: Hack

   wl_list_insert(views->prev, wlc_view_get_user_link(view));
   set_active(compositor, view);
   relayout(space);
   wlc_log(WLC_LOG_INFO, "new view: %p", view);
   return true;
}

static void
view_destroyed(struct wlc_compositor *compositor, struct wlc_view *view)
{
   struct wl_list *views = wlc_space_get_userdata(wlc_view_get_space(view));
   wl_list_remove(wlc_view_get_user_link(view));

   if (loliwm.active == view) {
      loliwm.active = NULL;

      struct wlc_view *v = wlc_view_get_parent(view);
      if (v) {
         // Focus the parent view, if there was one
         set_active(compositor, v);
      } else if (!wl_list_empty(views) && (v = wlc_view_from_user_link(views->prev))) {
         // Otherwise focus previous one.
         set_active(compositor, v);
      }
   }

   relayout(wlc_view_get_space(view));

   if (wl_list_empty(views)) {
      free(views);
      wlc_space_set_userdata(wlc_view_get_space(view), NULL);
   }

   wlc_log(WLC_LOG_INFO, "view destroyed: %p", view);
}

static void
view_switch_space(struct wlc_compositor *compositor, struct wlc_view *view, struct wlc_space *from, struct wlc_space *to)
{
   wl_list_remove(wlc_view_get_user_link(view));
   relayout(from);
   view_created(compositor, view, to);
}

static void
view_geometry_request(struct wlc_compositor *compositor, struct wlc_view *view, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
   (void)compositor;

   bool tiled = is_tiled(view);

   uint32_t state = wlc_view_get_state(view);
   if (!tiled || (state & WLC_BIT_RESIZING) || (state & WLC_BIT_MOVING)) {
      wlc_view_position(view, x, y);
      wlc_view_resize(view, w, h);

      if (tiled)
         wlc_view_set_state(view, WLC_BIT_MAXIMIZED, false);
   }
}

static void
view_state_request(struct wlc_compositor *compositor, struct wlc_view *view, const enum wlc_view_state_bit state, const bool toggle)
{
   (void)compositor;
   wlc_view_set_state(view, state, toggle);

   wlc_log(WLC_LOG_INFO, "STATE: %d (%d)", state, toggle);
   switch (state) {
      case WLC_BIT_MAXIMIZED:
         if (toggle)
            relayout(wlc_view_get_space(view));
      break;
      case WLC_BIT_FULLSCREEN:
         relayout(wlc_view_get_space(view));
      break;
      default:break;
   }
}

static bool
pointer_button(struct wlc_compositor *compositor, struct wlc_view *view, uint32_t button, enum wlc_button_state state)
{
   (void)button;

   if (state == WLC_BUTTON_STATE_PRESSED)
      set_active(compositor, view);

   return true;
}

static void
spawn(const char *bin)
{
   if (fork() == 0) {
      setsid();
      freopen("/dev/null", "w", stdout);
      freopen("/dev/null", "w", stderr);
      execlp(bin, bin, NULL);
      _exit(EXIT_SUCCESS);
   }
}

static bool
keyboard_key(struct wlc_compositor *compositor, struct wlc_view *view, uint32_t leds, uint32_t mods, uint32_t key, enum wlc_key_state state)
{
   (void)leds;

   bool pass = true;
   if (mods == loliwm.prefix) {
      if (key == 1) {
         if (state == WLC_KEY_STATE_RELEASED)
            exit(EXIT_SUCCESS);
      } else if (view && key == 16) {
         if (state == WLC_KEY_STATE_RELEASED)
            wlc_view_close(view);
         pass = false;
      } else if (key == 28) {
         if (state == WLC_KEY_STATE_RELEASED) {
            const char *terminal = getenv("TERMINAL");
            terminal = (terminal ? terminal : "weston-terminal");
            spawn(terminal);
         }
         pass = false;
      } else if (key == 25) {
         if (state == WLC_KEY_STATE_RELEASED)
            spawn("bemenu-run");
         pass = false;
      } else if (key == 35) {
         if (state == WLC_KEY_STATE_RELEASED)
            cycle(compositor);
         pass = false;
      } else if (key >= 2 && key <= 11) {
         if (state == WLC_KEY_STATE_RELEASED)
            focus_space(compositor, key - 2);
         pass = false;
      } else if (view && key >= 44 && key <= 46) {
         if (state == WLC_KEY_STATE_RELEASED)
            move_to_output(compositor, view, key - 44);
         pass = false;
      } else if (view && key >= 59 && key <= 68) {
         if (state == WLC_KEY_STATE_RELEASED)
            move_to_space(compositor, view, key - 59);
         pass = false;
      } else if (key == 37) {
         if (state == WLC_KEY_STATE_RELEASED)
            focus_next_output(compositor);
         pass = false;
      } else if (view && key == 38) {
         if (state == WLC_KEY_STATE_RELEASED)
            focus_next_view(compositor, view);
         pass = false;
      }
   }

   if (pass)
      wlc_log(WLC_LOG_INFO, "(%p) KEY: %u", view, key);

   return pass;
}

static void
resolution_notify(struct wlc_compositor *compositor, struct wlc_output *output, uint32_t width, uint32_t height)
{
   (void)compositor, (void)output, (void)width, (void)height;
   relayout(wlc_output_get_active_space(output));
}

static void
output_notify(struct wlc_compositor *compositor, struct wlc_output *output)
{
   struct wl_list *views = wlc_space_get_views(wlc_output_get_active_space(output));

   if (!wl_list_empty(views)) {
      set_active(compositor, wlc_view_from_link(views->prev));
   } else {
      set_active(compositor, NULL);
   }
}

static void
space_notify(struct wlc_compositor *compositor, struct wlc_space *space)
{
   struct wl_list *views = wlc_space_get_views(space);

   if (!wl_list_empty(views)) {
      set_active(compositor, wlc_view_from_link(views->prev));
   } else {
      set_active(compositor, NULL);
   }
}

static bool
output_created(struct wlc_compositor *compositor, struct wlc_output *output)
{
   (void)compositor;

   // Add some spaces
   for (int i = 1; i < 10; ++i)
      if (!wlc_space_add(output))
         return false;

   return true;
}

static void
terminate(void)
{
   if (loliwm.compositor)
      wlc_compositor_free(loliwm.compositor);

   memset(&loliwm, 0, sizeof(loliwm));
}

static bool
initialize(void)
{
   struct wlc_interface interface = {
      .view = {
         .created = view_created,
         .destroyed = view_destroyed,
         .switch_space = view_switch_space,

         .request = {
            .geometry = view_geometry_request,
            .state = view_state_request,
         },
      },

      .pointer = {
         .button = pointer_button,
      },

      .keyboard = {
         .key = keyboard_key,
      },

      .output = {
         .created = output_created,
         .activated = output_notify,
         .resolution = resolution_notify,
      },

      .space = {
         .activated = space_notify,
      },
   };

   if (!(loliwm.compositor = wlc_compositor_new(&interface)))
      goto fail;

   return true;

fail:
   terminate();
   return false;
}

static void
run(void)
{
   wlc_log(WLC_LOG_INFO, "loliwm started");
   wlc_compositor_run(loliwm.compositor);
}

static void
die(const char *format, ...)
{
   va_list vargs;
   va_start(vargs, format);
   wlc_vlog(WLC_LOG_ERROR, format, vargs);
   va_end(vargs);
   fflush(stderr);
   exit(EXIT_FAILURE);
}

static uint32_t
parse_prefix(const char *str)
{
   static const struct {
      const char *name;
      enum wlc_modifier_bit mod;
   } map[] = {
      { "shift", WLC_BIT_MOD_SHIFT },
      { "caps", WLC_BIT_MOD_CAPS },
      { "ctrl", WLC_BIT_MOD_CTRL },
      { "alt", WLC_BIT_MOD_ALT },
      { "mod2", WLC_BIT_MOD_MOD2 },
      { "mod3", WLC_BIT_MOD_MOD3 },
      { "logo", WLC_BIT_MOD_LOGO },
      { "mod5", WLC_BIT_MOD_MOD5 },
      { NULL, 0 },
   };

   uint32_t prefix = 0;
   const char *s = str;
   for (int i = 0; map[i].name && *s; ++i) {
      if ((prefix & map[i].mod) || strncmp(map[i].name, s, strlen(map[i].name)))
         continue;

      prefix |= map[i].mod;
      s += strlen(map[i].name) + 1;
      if (*(s - 1) != ',')
         break;
      i = 0;
   }

   return (prefix ? prefix : WLC_BIT_MOD_ALT);
}

int
main(int argc, char *argv[])
{
   (void)argc, (void)argv;

   if (!wlc_init(argc, argv))
      return EXIT_FAILURE;

   struct sigaction action = {
      .sa_handler = SIG_DFL,
      .sa_flags = SA_NOCLDWAIT
   };

   // do not care about childs
   sigaction(SIGCHLD, &action, NULL);

   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--prefix")) {
         if (i + 1 >= argc)
            die("--prefix takes an argument (shift,caps,ctrl,alt,logo,mod2,mod3,mod5)");
         loliwm.prefix = parse_prefix(argv[++i]);
      }
   }

   if (!initialize())
      return EXIT_FAILURE;

   run();
   terminate();
   return EXIT_SUCCESS;
}
