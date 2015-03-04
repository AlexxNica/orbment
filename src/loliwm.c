#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <dirent.h>
#include <wlc/wlc.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>
#include <chck/math/math.h>
#include <chck/pool/pool.h>
#include <chck/lut/lut.h>
#include <chck/xdg/xdg.h>
#include "plugin.h"
#include "config.h"

#define DEFAULT_TERMINAL "weston-terminal"
#define DEFAULT_MENU "bemenu-run"

// XXX: hack
enum {
   BIT_BEMENU = 1<<5,
};

enum direction {
   NEXT,
   PREV,
};

typedef void (*layout_fun_t)(wlc_handle output, const wlc_handle *views, size_t memb);
struct layout {
   const char *name;
   layout_fun_t function;
};

typedef void (*keybind_fun_t)(wlc_handle view, uint32_t time, intptr_t arg);
struct keybind {
   const char *name;
   struct chck_string syntax;
   keybind_fun_t function;
   intptr_t arg;
};

static struct {
   struct {
      // simplest data structure to cycle
      // there usually isn't many layouts so linear search is fast enough.
      // contigous arrays are very fast.
      struct chck_iter_pool pool;
      size_t index;
   } layouts;

   struct {
      struct chck_pool pool;
      struct chck_hash_table table;
   } keybinds;

   struct {
      const struct layout *layout;
      wlc_handle view;
   } active;

   uint32_t prefix;
   struct chck_string terminal;
} loliwm = {
   .prefix = WLC_BIT_MOD_ALT,
};

static void
next_or_prev_layout(bool direction)
{
   loliwm.layouts.index = (loliwm.layouts.index + (direction ? 1 : -1)) % loliwm.layouts.pool.items.count;
   loliwm.active.layout = chck_iter_pool_get(&loliwm.layouts.pool, loliwm.layouts.index);
}

static bool
layout_exists(const char *name)
{
   const struct layout *l;
   chck_iter_pool_for_each(&loliwm.layouts.pool, l)
      if (chck_cstreq(name, l->name))
         return true;
   return false;
}

static bool
add_layout(const char *name, layout_fun_t function)
{
   if (!name)
      return false;

   if (layout_exists(name)) {
      wlc_log(WLC_LOG_WARN, "Layout with name '%s' already exists", name);
      return false;
   }

   struct layout l = {
      .name = name,
      .function = function,
   };

   if (!chck_iter_pool_push_back(&loliwm.layouts.pool, &l))
      return false;

   wlc_log(WLC_LOG_INFO, "Added layout: %s", name);

   if (!loliwm.active.layout)
      next_or_prev_layout(true);

   return true;
}

static void
remove_layout(const char *name)
{
   const struct layout *l;
   chck_iter_pool_for_each(&loliwm.layouts.pool, l) {
      if (!chck_cstreq(name, l->name))
         continue;

      chck_iter_pool_remove(&loliwm.layouts.pool, _I - 1);
      wlc_log(WLC_LOG_INFO, "Removed layout: %s", name);

      if (loliwm.layouts.index >= _I - 1)
         next_or_prev_layout(false);

      break;
   }
}

static bool
keybind_exists(const char *name)
{
   const struct keybind *k;
   chck_pool_for_each(&loliwm.keybinds.pool, k)
      if (chck_cstreq(name, k->name))
         return true;
   return false;
}

static const struct keybind*
keybind_for_syntax(const char *syntax)
{
   size_t *index;
   if (!(index = chck_hash_table_str_get(&loliwm.keybinds.table, syntax, strlen(syntax))) || *index == (size_t)-1)
      return NULL;

   return chck_pool_get(&loliwm.keybinds.pool, *index);
}

static bool
add_keybind(const char *name, const char *syntax, keybind_fun_t function, intptr_t arg)
{
   if (!name)
      return false;

   if (keybind_exists(name)) {
      wlc_log(WLC_LOG_WARN, "Keybind with name '%s' already exists", name);
      return false;
   }

   struct keybind k = {
      .name = name,
      .function = function,
      .arg = arg,
   };

   chck_string_set_cstr(&k.syntax, syntax, false);

   size_t index;
   if (!chck_pool_add(&loliwm.keybinds.pool, &k, &index))
      return false;

   const struct keybind *o;
   if (!(o = keybind_for_syntax(k.syntax.data))) {
      chck_hash_table_str_set(&loliwm.keybinds.table, k.syntax.data, k.syntax.size, &index);
   } else {
      wlc_log(WLC_LOG_WARN, "'%s' is already mapped to keybind '%s'", syntax, o->name);
   }

   wlc_log(WLC_LOG_INFO, "Added keybind: %s (%s)", name, syntax);
   return true;
}

static void
remove_keybind(const char *name)
{
   const struct keybind *k;
   chck_pool_for_each(&loliwm.keybinds.pool, k) {
      if (!chck_cstreq(name, k->name))
         continue;

      chck_hash_table_str_set(&loliwm.keybinds.table, k->syntax.data, k->syntax.size, NULL);
      wlc_log(WLC_LOG_INFO, "Removed keybind: %s", name);
      break;
   };
}

static wlc_handle
get_next_view(wlc_handle output, size_t offset, enum direction dir)
{
   size_t memb;
   const wlc_handle *views;
   views = wlc_output_get_views(output, &memb);
   return (memb > 0 ? views[(dir == PREV ? memb - (offset + 1) : memb + (offset + 1)) % memb] : 0);
}

static wlc_handle
get_next_output(size_t offset, enum direction dir)
{
   wlc_handle focused;
   if (!(focused = wlc_get_focused_output()))
      return 0;

   size_t memb, i;
   const wlc_handle *outputs = wlc_get_outputs(&memb);
   for (i = 0; i < memb && outputs[i] != focused; ++i);
   return (memb > 0 && i < memb ? outputs[(dir == PREV ? i - offset : i + offset) % memb] : 0);
}

static void
layout_parent(wlc_handle view, wlc_handle parent, const struct wlc_size *size)
{
   assert(view && parent);

   // Size to fit the undermost parent
   // TODO: Use surface height as base instead of current
   wlc_handle under;
   for (under = parent; under && wlc_view_get_parent(under); under = wlc_view_get_parent(under));

   // Undermost view and parent view geometry
   const struct wlc_geometry *u = wlc_view_get_geometry(under);
   const struct wlc_geometry *p = wlc_view_get_geometry(parent);

   // Current constrained size
   float cw = chck_maxf(size->w, u->size.w * 0.6);
   float ch = chck_maxf(size->h, u->size.h * 0.6);

   struct wlc_geometry g;
   g.size.w = chck_minf(cw, u->size.w * 0.8);
   g.size.h = chck_minf(ch, u->size.h * 0.8);
   g.origin.x = p->size.w * 0.5 - g.size.w * 0.5;
   g.origin.y = p->size.h * 0.5 - g.size.h * 0.5;
   wlc_view_set_geometry(view, &g);
}

static bool
should_focus_on_create(wlc_handle view)
{
   // Do not allow unmanaged views to steal focus (tooltips, dnds, etc..)
   // Do not allow parented windows to steal focus, if current window wasn't parent.
   uint32_t type = wlc_view_get_type(view);
   wlc_handle parent = wlc_view_get_parent(view);
   return (!(type & WLC_BIT_UNMANAGED) && (!loliwm.active.view || !parent || parent == loliwm.active.view));
}

static bool
is_or(wlc_handle view)
{
   return (wlc_view_get_type(view) & WLC_BIT_OVERRIDE_REDIRECT) || (wlc_view_get_state(view) & BIT_BEMENU);
}

static bool
is_managed(wlc_handle view)
{
   uint32_t type = wlc_view_get_type(view);
   return !(type & WLC_BIT_UNMANAGED) && !(type & WLC_BIT_POPUP) && !(type & WLC_BIT_SPLASH);
}

static bool
is_modal(wlc_handle view)
{
   uint32_t type = wlc_view_get_type(view);
   return (type & WLC_BIT_MODAL);
}

static bool
is_tiled(wlc_handle view)
{
   uint32_t state = wlc_view_get_state(view);
   return !(state & WLC_BIT_FULLSCREEN) && !wlc_view_get_parent(view) && is_managed(view) && !is_or(view) && !is_modal(view);
}

static void
relayout(wlc_handle output)
{
   const struct wlc_size *r;
   if (!(r = wlc_output_get_resolution(output)))
      return;

   size_t memb;
   const wlc_handle *views;
   views = wlc_output_get_views(output, &memb);

   struct chck_iter_pool tiled;
   if (loliwm.active.layout && !chck_iter_pool(&tiled, memb, memb, sizeof(wlc_handle)))
      return;

   for (size_t i = 0; i < memb; ++i) {
      if (wlc_output_get_mask(output) != wlc_view_get_mask(views[i]))
         continue;

      if (wlc_view_get_state(views[i]) & WLC_BIT_FULLSCREEN)
         wlc_view_set_geometry(views[i], &(struct wlc_geometry){ { 0, 0 }, *r });

      if (wlc_view_get_type(views[i]) & WLC_BIT_SPLASH) {
         struct wlc_geometry g = *wlc_view_get_geometry(views[i]);
         g.origin = (struct wlc_origin){ r->w * 0.5 - g.size.w * 0.5, r->h * 0.5 - g.size.h * 0.5 };
         wlc_view_set_geometry(views[i], &g);
      }

      wlc_handle parent;
      if (is_managed(views[i]) && !is_or(views[i]) && (parent = wlc_view_get_parent(views[i])))
         layout_parent(views[i], parent, &wlc_view_get_geometry(views[i])->size);

      if (!is_tiled(views[i]) || !loliwm.active.layout)
         continue;

      chck_iter_pool_push_back(&tiled, &views[i]);
   }

   if (!loliwm.active.layout)
      return;

   loliwm.active.layout->function(output, tiled.items.buffer, tiled.items.count);
   chck_iter_pool_release(&tiled);
}

static void
raise_all(wlc_handle view)
{
   assert(view);

   // Raise view and all related views to top honoring the stacking order.
   wlc_handle parent;
   if ((parent = wlc_view_get_parent(view))) {
      raise_all(parent);

      size_t memb;
      const wlc_handle *views = wlc_output_get_views(wlc_view_get_output(view), &memb);
      for (size_t i = 0; i < memb; ++i) {
         if (views[i] == view || wlc_view_get_parent(views[i]) != parent)
            continue;

         wlc_view_bring_to_front(views[i]);
      }
   }

   wlc_view_bring_to_front(view);
}

static void
focus_view(wlc_handle view)
{
   if (loliwm.active.view == view)
      return;

   // Bemenu should always have focus when open.
   if (loliwm.active.view && (wlc_view_get_state(loliwm.active.view) & BIT_BEMENU)) {
      wlc_view_bring_to_front(loliwm.active.view);
      return;
   }

   if (view) {
      {
         size_t memb;
         const wlc_handle *views = wlc_output_get_views(wlc_view_get_output(view), &memb);
         for (size_t i = (memb > 0 ? memb - 1 : 0); i > 0; --i) {
            if (wlc_view_get_parent(views[i]) == view) {
               // If window has parent, focus it instead of this.
               // By reverse searching views list, we get the topmost parent.
               focus_view(views[i]);
               return;
            }
         }
      }

      // Only raise fullscreen views when focused view is managed
      if (is_managed(view) && !is_or(view)) {
         size_t memb;
         const wlc_handle *views = wlc_output_get_views(wlc_view_get_output(view), &memb);
         for (size_t i = (memb > 0 ? memb - 1 : 0); i > 0; --i) {
            if (wlc_view_get_state(views[i]) & WLC_BIT_FULLSCREEN) {
               // Bring the first topmost found fullscreen wlc_view to front.
               // This way we get a "peek" effect when we cycle other views.
               // Meaning the active view is always over fullscreen view,
               // but fullscreen view is on top of the other views.
               wlc_view_bring_to_front(views[i]);
               break;
            }
         }
      }

      raise_all(view);

      {
         size_t memb;
         const wlc_handle *views = wlc_output_get_views(wlc_view_get_output(view), &memb);
         for (size_t i = (memb > 0 ? memb - 1 : 0); i > 0; --i) {
            if ((wlc_view_get_state(views[i]) & BIT_BEMENU)) {
               // Always bring bemenu to front when exists.
               wlc_view_bring_to_front(views[i]);
               break;
            }
         }
      }
   }

   wlc_view_focus(view);
   loliwm.active.view = view;
}

static void
focus_next_or_previous_view(wlc_handle view, enum direction direction)
{
   focus_view(get_next_view(wlc_view_get_output(view), 1, direction));
}

static void
focus_space(uint32_t index)
{
   wlc_output_set_mask(wlc_get_focused_output(), (1<<index));
}

static void
move_to_space(wlc_handle view, uint32_t index)
{
   wlc_view_set_mask(view, (1<<index));
   relayout(wlc_view_get_output(view));
}

static wlc_handle
output_for_index(uint32_t index)
{
   size_t memb;
   const wlc_handle *outputs = wlc_get_outputs(&memb);
   return (index < memb ? outputs[index] : 0);
}

static void
cycle_output(wlc_handle output)
{
   wlc_view_send_to_back(get_next_view(output, 1, PREV));
   relayout(output);
}

static void
focus_output(wlc_handle output)
{
   wlc_output_focus(output);
   relayout(output);
}

static void
move_to_output(wlc_handle view, uint32_t index)
{
   wlc_handle output;
   if (!(output = output_for_index(index)))
      return;

   wlc_handle old = wlc_view_get_output(view);
   wlc_view_set_output(view, output);
   relayout(old);
   focus_output(output);
}

static void
focus_next_or_previous_output(enum direction direction)
{
   wlc_output_focus(get_next_output(1, direction));
}

static bool
view_created(wlc_handle view)
{
   if (wlc_view_get_class(view) && !strcmp(wlc_view_get_class(view), "bemenu")) {
      // Do not allow more than one bemenu instance
      if (loliwm.active.view && wlc_view_get_state(loliwm.active.view) & BIT_BEMENU)
         return false;

      wlc_view_set_state(view, BIT_BEMENU, true); // XXX: Hack
   }

   if (should_focus_on_create(view))
      focus_view(view);

   relayout(wlc_view_get_output(view));
   wlc_log(WLC_LOG_INFO, "new view: %zu (%zu)", view, wlc_view_get_parent(view));
   return true;
}

static void
view_destroyed(wlc_handle view)
{
   if (loliwm.active.view == view) {
      loliwm.active.view = 0;

      wlc_handle v;
      if ((v = wlc_view_get_parent(view))) {
         // Focus the parent view, if there was one
         // Set parent 0 before this to avoid focusing back to dying view
         wlc_view_set_parent(view, 0);
         focus_view(v);
      } else {
         // Otherwise focus previous one (stacking order).
         focus_next_or_previous_view(view, PREV);
      }
   }

   relayout(wlc_view_get_output(view));
   wlc_log(WLC_LOG_INFO, "view destroyed: %zu", view);
}

static void
view_focus(wlc_handle view, bool focus)
{
   wlc_view_set_state(view, WLC_BIT_ACTIVATED, focus);
}

static void
view_move_to_output(wlc_handle view, wlc_handle from, wlc_handle to)
{
   (void)view;

   relayout(from);
   relayout(to);

   if (from == to)
      focus_space(wlc_output_get_mask(from));
}

static void
view_geometry_request(wlc_handle view, const struct wlc_geometry *geometry)
{
   uint32_t type = wlc_view_get_type(view);
   uint32_t state = wlc_view_get_state(view);
   bool tiled = is_tiled(view);
   bool action = ((state & WLC_BIT_RESIZING) || (state & WLC_BIT_MOVING));

   if (tiled && !action)
      return;

   if (tiled)
      wlc_view_set_state(view, WLC_BIT_MAXIMIZED, false);

   if ((state & WLC_BIT_FULLSCREEN) || (type & WLC_BIT_SPLASH))
      return;

   wlc_handle parent;
   if (is_managed(view) && !is_or(view) && (parent = wlc_view_get_parent(view))) {
      layout_parent(view, parent, &geometry->size);
   } else {
      wlc_view_set_geometry(view, geometry);
   }
}

static void
view_state_request(wlc_handle view, const enum wlc_view_state_bit state, const bool toggle)
{
   wlc_log(WLC_LOG_INFO, "STATE: %d (%d)", state, toggle);
   wlc_view_set_state(view, state, toggle);

   switch (state) {
      case WLC_BIT_MAXIMIZED:
         if (toggle)
            relayout(wlc_view_get_output(view));
      break;
      case WLC_BIT_FULLSCREEN:
         relayout(wlc_view_get_output(view));
      break;
      default:break;
   }
}

static bool
pointer_button(wlc_handle view, uint32_t time, const struct wlc_modifiers *modifiers, uint32_t button, enum wlc_button_state state)
{
   (void)time, (void)modifiers, (void)button;

   if (state == WLC_BUTTON_STATE_PRESSED)
      focus_view(view);

   return true;
}

static void
store_rgba(const struct wlc_size *size, uint8_t *rgba, void *arg)
{
   (void)arg;

   FILE *f;

   time_t now;
   time(&now);
   char buf[sizeof("loliwm-0000-00-00T00:00:00Z.ppm")];
   strftime(buf, sizeof(buf), "loliwm-%FT%TZ.ppm", gmtime(&now));

   uint8_t *rgb;
   if (!(rgb = calloc(1, size->w * size->h * 3)))
      return;

   if (!(f = fopen(buf, "wb"))) {
      free(rgb);
      return;
   }

   for (uint32_t i = 0, c = 0; i < size->w * size->h * 4; i += 4, c += 3)
      memcpy(rgb + c, rgba + i, 3);

   for (uint32_t i = 0; i * 2 < size->h; ++i) {
      uint32_t o = i * size->w * 3;
      uint32_t r = (size->h - 1 - i) * size->w * 3;
      for (uint32_t i2 = size->w * 3; i2 > 0; --i2, ++o, ++r) {
         uint8_t temp = rgb[o];
         rgb[o] = rgb[r];
         rgb[r] = temp;
      }
   }

   fprintf(f, "P6\n%d %d\n255\n", size->w, size->h);
   fwrite(rgb, 1, size->w * size->h * 3, f);
   free(rgb);
   fclose(f);
}

static void
screenshot(wlc_handle output)
{
   wlc_output_get_pixels(output, store_rgba, NULL);
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
syntax_append(struct chck_string *syntax, const char *cstr, bool is_heap)
{
   if (syntax->size > 0)
      return chck_string_set_format(syntax, "%s-%s", syntax->data, cstr);
   return chck_string_set_cstr(syntax, cstr, is_heap);
}

static bool
append_mods(struct chck_string *syntax, struct chck_string *prefixed, uint32_t mods)
{
   if (mods == loliwm.prefix && !syntax_append(prefixed, "P", false))
      return false;

   static const struct {
      const char *name;
      enum wlc_modifier_bit mod;
   } map[] = {
      { "S", WLC_BIT_MOD_SHIFT },
      { "C", WLC_BIT_MOD_CTRL },
      { "M", WLC_BIT_MOD_ALT },
      { "L", WLC_BIT_MOD_LOGO },
      { "M2", WLC_BIT_MOD_MOD2 },
      { "M3", WLC_BIT_MOD_MOD3 },
      { "M5", WLC_BIT_MOD_MOD5 },
      { NULL, 0 },
   };

   for (uint32_t i = 0; map[i].name; ++i) {
      if (!(mods & map[i].mod))
         continue;

      if (!syntax_append(syntax, map[i].name, false))
         return false;
   }

   return true;
}

static bool
keyboard_key(wlc_handle view, uint32_t time, const struct wlc_modifiers *modifiers, uint32_t key, uint32_t sym, enum wlc_key_state state)
{
   (void)time, (void)key;

   bool pass = true;

   struct chck_string syntax = {0}, prefixed = {0};
   if (!append_mods(&syntax, &prefixed, modifiers->mods))
      goto out;

   char name[64];
   if (xkb_keysym_get_name(sym, name, sizeof(name)) == -1)
      goto out;

   syntax_append(&syntax, name, true);
   syntax_append(&prefixed, name, true);
   chck_string_set_format(&syntax, "<%s>", syntax.data);
   chck_string_set_format(&prefixed, "<%s>", prefixed.data);

   const struct keybind *k;
   if (!(k = keybind_for_syntax(prefixed.data)) &&
       !(k = keybind_for_syntax(syntax.data)))
       goto out;

   if (state == WLC_KEY_STATE_PRESSED)
      k->function(view, time, k->arg);
   pass = false;

out:
   chck_string_release(&syntax);
   chck_string_release(&prefixed);
   return pass;
}

static void
output_resolution(wlc_handle output, const struct wlc_size *from, const struct wlc_size *to)
{
   (void)output, (void)from, (void)to;
   relayout(output);
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

static void
key_cb_exit(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time, (void)arg;
   wlc_terminate();
}

static void
key_cb_close_client(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time, (void)arg;

   if (!view)
      return;

   wlc_view_close(view);
}

static void
key_cb_spawn(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time;
   spawn((const char*)arg);
}

static void
key_cb_toggle_fullscreen(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time, (void)arg;

   if (!view)
      return;

   wlc_view_set_state(view, WLC_BIT_FULLSCREEN, !(wlc_view_get_state(view) & WLC_BIT_FULLSCREEN));
   relayout(wlc_view_get_output(view));
}

static void
key_cb_cycle_clients(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time, (void)arg;
   cycle_output(wlc_get_focused_output());
}

static void key_cb_focus_space(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time;
   focus_space((uint32_t)arg);
}

static void key_cb_move_to_output(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time;

   if (!view)
      return;

   move_to_output(view, (uint32_t)arg);
}

static void key_cb_move_to_space(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time;

   if (!view)
      return;

   move_to_space(view, (uint32_t)arg);
}

static void
key_cb_focus_next_output(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time, (void)arg;
   focus_next_or_previous_output(NEXT);
}

static void
key_cb_focus_previous_client(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time, (void)arg;

   if (!view)
      return;

   focus_next_or_previous_view(view, PREV);
}

static void
key_cb_focus_next_client(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)time, (void)arg;

   if (!view)
      return;

   focus_next_or_previous_view(view, NEXT);
}

static void
key_cb_take_screenshot(wlc_handle view, uint32_t time, intptr_t arg)
{
   (void)view, (void)time, (void)arg;
   screenshot(wlc_get_focused_output());
}

static bool
setup_default_keybinds(void)
{
   const char *terminal = getenv("TERMINAL");
   chck_string_set_cstr(&loliwm.terminal, (terminal && strlen(terminal) ? terminal : DEFAULT_TERMINAL), true);

   return (add_keybind("exit", "<P-Escape>", key_cb_exit, 0) &&
           add_keybind("close client", "<P-q>", key_cb_close_client, 0) &&
           add_keybind("spawn terminal", "<P-Return>", key_cb_spawn, (intptr_t)loliwm.terminal.data) &&
           add_keybind("spawn bemenu", "<P-p>", key_cb_spawn, (intptr_t)DEFAULT_MENU) &&
           add_keybind("toggle fullscreen", "<P-f>", key_cb_toggle_fullscreen, 0) &&
           add_keybind("cycle clients", "<P-h>", key_cb_cycle_clients, 0) &&
           add_keybind("focus next output", "<P-l>", key_cb_focus_next_output, 0) &&
           add_keybind("focus next client", "<P-k>", key_cb_focus_next_client, 0) &&
           add_keybind("focus previous client", "<P-j>", key_cb_focus_previous_client, 0) &&
           add_keybind("take screenshot", "<P-SunPrint_Screen>", key_cb_take_screenshot, 0) &&
           add_keybind("focus space 0", "<P-0>", key_cb_focus_space, 0) &&
           add_keybind("focus space 1", "<P-1>", key_cb_focus_space, 1) &&
           add_keybind("focus space 2", "<P-2>", key_cb_focus_space, 2) &&
           add_keybind("focus space 3", "<P-3>", key_cb_focus_space, 3) &&
           add_keybind("focus space 4", "<P-4>", key_cb_focus_space, 4) &&
           add_keybind("focus space 5", "<P-5>", key_cb_focus_space, 5) &&
           add_keybind("focus space 6", "<P-6>", key_cb_focus_space, 6) &&
           add_keybind("focus space 7", "<P-7>", key_cb_focus_space, 7) &&
           add_keybind("focus space 8", "<P-8>", key_cb_focus_space, 8) &&
           add_keybind("focus space 9", "<P-9>", key_cb_focus_space, 9) &&
           add_keybind("move to space 0", "<P-F0>", key_cb_move_to_space, 0) &&
           add_keybind("move to space 1", "<P-F1>", key_cb_move_to_space, 1) &&
           add_keybind("move to space 2", "<P-F2>", key_cb_move_to_space, 2) &&
           add_keybind("move to space 3", "<P-F3>", key_cb_move_to_space, 3) &&
           add_keybind("move to space 4", "<P-F4>", key_cb_move_to_space, 4) &&
           add_keybind("move to space 5", "<P-F5>", key_cb_move_to_space, 5) &&
           add_keybind("move to space 6", "<P-F6>", key_cb_move_to_space, 6) &&
           add_keybind("move to space 7", "<P-F7>", key_cb_move_to_space, 7) &&
           add_keybind("move to space 8", "<P-F8>", key_cb_move_to_space, 8) &&
           add_keybind("move to space 9", "<P-F9>", key_cb_move_to_space, 9) &&
           add_keybind("move to output 0", "<P-z>", key_cb_move_to_output, 0) &&
           add_keybind("move to output 1", "<P-x>", key_cb_move_to_output, 1) &&
           add_keybind("move to output 2", "<P-c>", key_cb_move_to_output, 2));
}

static bool
plugins_init(void)
{
   {
      static const struct method methods[] = {
         REGISTER_METHOD(relayout, "v(h)|1"),
         REGISTER_METHOD(add_layout, "b(c[],p)|1"),
         REGISTER_METHOD(remove_layout, "v(c[])|1"),
         REGISTER_METHOD(add_keybind, "b(c[],c[],p,ip)|1"),
         REGISTER_METHOD(remove_keybind, "v(c[])|1"),
         {0},
      };

      struct plugin core = {
         .info = {
            .name = "loliwm",
            .version = "1.0.0",
            .methods = methods,
      }, {0}};

      if (!register_plugin(&core, NULL))
         return false;
   }

   if (chck_cstr_is_empty(PLUGINS_PATH)) {
      wlc_log(WLC_LOG_ERROR, "Could not find plugins path. PLUGINS_PATH was not set during compile.");
      return true;
   }

   {
      struct chck_string xdg = {0};
      {
         char *tmp = xdg_get_path("XDG_DATA_HOME", ".local/share");
         chck_string_set_cstr(&xdg, tmp, true);
         free(tmp);
      }

      chck_string_set_format(&xdg, "%s/loliwm/plugins", xdg.data);

#ifndef NDEBUG
      // allows running without install, as long as you build in debug mode
      const char *paths[] = { PLUGINS_PATH, "plugins", xdg.data, NULL };
#else
      const char *paths[] = { PLUGINS_PATH, xdg.data, NULL };
#endif

      // FIXME: add portable directory code to chck/fs/fs.c
      for (uint32_t i = 0; paths[i]; ++i) {
         DIR *d;
         if (!(d = opendir(paths[i]))) {
            wlc_log(WLC_LOG_WARN, "Could not open plugins directory: %s", paths[i]);
            continue;
         }

         struct dirent *dir;
         while ((dir = readdir(d))) {
            if (!chck_cstr_starts_with(dir->d_name, "loliwm-plugin-"))
               continue;

            struct chck_string tmp = {0};
            if (chck_string_set_format(&tmp, "%s/%s", paths[i], dir->d_name))
               register_plugin_from_path(tmp.data);
            chck_string_release(&tmp);
         }

         closedir(d);
      }

      chck_string_release(&xdg);
   }
   return true;
}

int
main(int argc, char *argv[])
{
   (void)argc, (void)argv;

   if (!chck_iter_pool(&loliwm.layouts.pool, 32, 0, sizeof(struct layout)) ||
       !chck_pool(&loliwm.keybinds.pool, 32, 0, sizeof(struct keybind)) ||
       !chck_hash_table(&loliwm.keybinds.table, -1, 256, sizeof(size_t)))
      return EXIT_FAILURE;

   static const struct wlc_interface interface = {
      .output = {
         .resolution = output_resolution,
      },

      .view = {
         .created = view_created,
         .destroyed = view_destroyed,
         .focus = view_focus,
         .move_to_output = view_move_to_output,

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
   };

   if (!wlc_init(&interface, argc, argv))
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

   if (!setup_default_keybinds())
      return EXIT_FAILURE;

   if (!plugins_init())
      return EXIT_FAILURE;

   wlc_log(WLC_LOG_INFO, "loliwm started");
   wlc_run();

   memset(&loliwm, 0, sizeof(loliwm));
   wlc_log(WLC_LOG_INFO, "-!- loliwm is gone, bye bye!");
   return EXIT_SUCCESS;
}
