#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client-core.h>

#include "presentation-time-client-protocol.h"

#define EXPORT __attribute__((visibility("default")))
#define MAX_ARGS 20
#define MAX_SURFACES 64
#define WL_DISPLAY_GET_REGISTRY_OPCODE 1
#define WL_REGISTRY_BIND_OPCODE 0
#define WL_SURFACE_DESTROY_OPCODE 0
#define WL_SURFACE_ATTACH_OPCODE 1
#define WL_SURFACE_COMMIT_OPCODE 6
#define WP_PRESENTATION_FEEDBACK_OPCODE 1
#define FLUSH_INTERVAL_NS 200000000ll

extern const struct wl_interface wl_registry_interface;

typedef struct wl_proxy* (*array_flags_fn)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t,
                                           union wl_argument*);
typedef void (*array_fn)(struct wl_proxy*, uint32_t, union wl_argument*);
typedef const struct wl_interface* (*get_interface_fn)(struct wl_proxy*);
typedef struct wl_display* (*get_display_fn)(struct wl_proxy*);

struct surface {
  uint32_t id;
  uint32_t attached;
  uint64_t commits;
};

struct pending {
  uint32_t surface_id;
  uint64_t commit_no;
};

static array_flags_fn real_array_flags;
static array_fn real_array;
static get_interface_fn real_get_interface;
static get_display_fn real_get_display;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char* log_path;
static FILE* log_file;
static int64_t last_flush_ns;
static int setup_state;
static struct wl_display* display;
static struct wl_event_queue* queue;
static struct wl_proxy* presentation;
static struct surface surfaces[MAX_SURFACES];
static _Atomic uint64_t mark;
static uint64_t last_arrival;

static int64_t now_ns(void);

EXPORT void pacing_mark(uint64_t value) {
  atomic_store_explicit(&mark, value, memory_order_relaxed);
}

EXPORT void pacing_arrival(uint64_t seq) {
  pthread_mutex_lock(&lock);

  if (setup_state == 1 && seq > last_arrival) {
    last_arrival = seq;
    fprintf(log_file, "A %lld %llu\n", (long long)now_ns(), (unsigned long long)seq);
  }

  pthread_mutex_unlock(&lock);
}

static void resolve(void) {
  real_array_flags = (array_flags_fn)dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
  real_array = (array_fn)dlsym(RTLD_NEXT, "wl_proxy_marshal_array");
  real_get_interface = (get_interface_fn)dlsym(RTLD_NEXT, "wl_proxy_get_interface");
  real_get_display = (get_display_fn)dlsym(RTLD_NEXT, "wl_proxy_get_display");
  log_path = getenv("PACING_LOG");

  if (log_path != NULL && log_path[0] == '\0') {
    log_path = NULL;
  }
}

// libwayland before 1.24 and 1.23 has no accessor for these two fields; struct wl_proxy starts with the interface pointer and holds the display pointer after its 24-byte wl_object.
static const struct wl_interface* interface_of(struct wl_proxy* proxy) {
  if (real_get_interface != NULL) {
    return real_get_interface(proxy);
  }

  return *(const struct wl_interface**)proxy;
}

static struct wl_display* display_of(struct wl_proxy* proxy) {
  if (real_get_display != NULL) {
    return real_get_display(proxy);
  }

  return *(struct wl_display**)((char*)proxy + 24);
}

static int64_t now_ns(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static int args_from_va(const char* signature, union wl_argument* args, va_list ap) {
  int count = 0;

  for (const char* p = signature; *p != '\0' && count < MAX_ARGS; p++) {
    switch (*p) {
      case 'i':
        args[count++].i = va_arg(ap, int32_t);
        break;
      case 'u':
        args[count++].u = va_arg(ap, uint32_t);
        break;
      case 'f':
        args[count++].f = va_arg(ap, wl_fixed_t);
        break;
      case 's':
        args[count++].s = va_arg(ap, const char*);
        break;
      case 'o':
      case 'n':
        args[count++].o = va_arg(ap, struct wl_object*);
        break;
      case 'a':
        args[count++].a = va_arg(ap, struct wl_array*);
        break;
      case 'h':
        args[count++].h = va_arg(ap, int32_t);
        break;
      default:
        break;
    }
  }

  return count;
}

static struct surface* surface_for(uint32_t id) {
  struct surface* free_slot = NULL;

  for (int i = 0; i < MAX_SURFACES; i++) {
    if (surfaces[i].id == id) {
      return &surfaces[i];
    }

    if (surfaces[i].id == 0 && free_slot == NULL) {
      free_slot = &surfaces[i];
    }
  }

  if (free_slot != NULL) {
    free_slot->id = id;
    free_slot->attached = 0;
    free_slot->commits = 0;
  }

  return free_slot;
}

static void on_clock_id(void* data, struct wp_presentation* object, uint32_t clock_id) {
  fprintf(log_file, "K %u\n", clock_id);
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = on_clock_id,
};

static void on_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
  if (strcmp(interface, wp_presentation_interface.name) != 0 || presentation != NULL) {
    return;
  }

  union wl_argument args[4] = {{.u = name}, {.s = wp_presentation_interface.name}, {.u = 1}, {.n = 0}};

  presentation = real_array_flags((struct wl_proxy*)registry, WL_REGISTRY_BIND_OPCODE, &wp_presentation_interface, 1, 0, args);

  if (presentation != NULL) {
    wl_proxy_add_listener(presentation, (void (**)(void)) & presentation_listener, NULL);
  }
}

static void on_global_remove(void* data, struct wl_registry* registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static void setup(struct wl_proxy* surface) {
  setup_state = -1;
  log_file = fopen(log_path, "w");

  if (log_file == NULL) {
    return;
  }

  setvbuf(log_file, NULL, _IOFBF, 1 << 20);
  fprintf(log_file, "V 1 pid %d\n", getpid());

  display = display_of(surface);
  queue = wl_display_create_queue(display);

  struct wl_proxy* wrapper = wl_proxy_create_wrapper(display);

  wl_proxy_set_queue(wrapper, queue);

  union wl_argument args[1] = {{.n = 0}};
  struct wl_proxy* registry =
      real_array_flags(wrapper, WL_DISPLAY_GET_REGISTRY_OPCODE, &wl_registry_interface, wl_proxy_get_version(wrapper), 0, args);

  wl_proxy_wrapper_destroy(wrapper);

  if (registry == NULL) {
    return;
  }

  wl_proxy_add_listener(registry, (void (**)(void)) & registry_listener, NULL);
  wl_display_roundtrip_queue(display, queue);
  wl_display_roundtrip_queue(display, queue);
  wl_proxy_destroy(registry);

  if (presentation != NULL) {
    setup_state = 1;
  } else {
    fprintf(log_file, "E no wp_presentation\n");
    fflush(log_file);
  }
}

static void on_sync_output(void* data, struct wp_presentation_feedback* feedback, struct wl_output* output) {}

static void on_presented(void* data, struct wp_presentation_feedback* feedback, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                         uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
  struct pending* p = data;
  uint64_t sec = ((uint64_t)sec_hi << 32) | sec_lo;
  uint64_t msc = ((uint64_t)seq_hi << 32) | seq_lo;

  fprintf(log_file, "P %u %llu %llu %u %llu %u\n", p->surface_id, (unsigned long long)p->commit_no,
          (unsigned long long)(sec * 1000000000ull + nsec), refresh, (unsigned long long)msc, flags);
  free(p);
  wl_proxy_destroy((struct wl_proxy*)feedback);
}

static void on_discarded(void* data, struct wp_presentation_feedback* feedback) {
  struct pending* p = data;

  fprintf(log_file, "D %u %llu\n", p->surface_id, (unsigned long long)p->commit_no);
  free(p);
  wl_proxy_destroy((struct wl_proxy*)feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = on_sync_output,
    .presented = on_presented,
    .discarded = on_discarded,
};

static void on_commit(struct wl_proxy* proxy) {
  if (setup_state == 0) {
    setup(proxy);
  }

  if (setup_state != 1) {
    return;
  }

  uint32_t id = wl_proxy_get_id(proxy);
  struct surface* s = surface_for(id);

  if (s == NULL) {
    return;
  }

  int64_t now = now_ns();

  s->commits++;
  fprintf(log_file, "C %lld %u %llu %u %llu %ld\n", (long long)now, id, (unsigned long long)s->commits, s->attached,
          (unsigned long long)atomic_load_explicit(&mark, memory_order_relaxed), (long)syscall(SYS_gettid));

  struct pending* p = malloc(sizeof(*p));
  union wl_argument args[2] = {{.o = (struct wl_object*)proxy}, {.n = 0}};
  struct wl_proxy* feedback =
      real_array_flags(presentation, WP_PRESENTATION_FEEDBACK_OPCODE, &wp_presentation_feedback_interface, 1, 0, args);

  p->surface_id = id;
  p->commit_no = s->commits;
  s->attached = 0;

  if (feedback != NULL) {
    wl_proxy_add_listener(feedback, (void (**)(void)) & feedback_listener, p);
  } else {
    free(p);
  }

  wl_display_dispatch_queue_pending(display, queue);

  if (now - last_flush_ns > FLUSH_INTERVAL_NS) {
    fflush(log_file);
    last_flush_ns = now;
  }
}

static void on_request(struct wl_proxy* proxy, uint32_t opcode, uint32_t flags, const union wl_argument* args) {
  if (log_path == NULL || (opcode != WL_SURFACE_COMMIT_OPCODE && opcode != WL_SURFACE_ATTACH_OPCODE && opcode != WL_SURFACE_DESTROY_OPCODE)) {
    return;
  }

  if (strcmp(wl_proxy_get_class(proxy), "wl_surface") != 0) {
    return;
  }

  pthread_mutex_lock(&lock);

  if (opcode == WL_SURFACE_COMMIT_OPCODE) {
    on_commit(proxy);
  } else if (setup_state == 1) {
    struct surface* s = surface_for(wl_proxy_get_id(proxy));

    if (s != NULL && opcode == WL_SURFACE_ATTACH_OPCODE) {
      s->attached = args[0].o != NULL ? 1 : 2;
    } else if (s != NULL) {
      s->id = 0;
    }
  }

  pthread_mutex_unlock(&lock);
}

EXPORT struct wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy, uint32_t opcode, const struct wl_interface* interface,
                                                     uint32_t version, uint32_t flags, union wl_argument* args) {
  pthread_once(&resolve_once, resolve);
  on_request(proxy, opcode, flags, args);

  return real_array_flags(proxy, opcode, interface, version, flags, args);
}

EXPORT struct wl_proxy* wl_proxy_marshal_flags(struct wl_proxy* proxy, uint32_t opcode, const struct wl_interface* interface,
                                               uint32_t version, uint32_t flags, ...) {
  union wl_argument args[MAX_ARGS];
  va_list ap;

  pthread_once(&resolve_once, resolve);

  va_start(ap, flags);
  args_from_va(interface_of(proxy)->methods[opcode].signature, args, ap);
  va_end(ap);

  on_request(proxy, opcode, flags, args);

  return real_array_flags(proxy, opcode, interface, version, flags, args);
}

EXPORT void wl_proxy_marshal(struct wl_proxy* proxy, uint32_t opcode, ...) {
  union wl_argument args[MAX_ARGS];
  va_list ap;

  pthread_once(&resolve_once, resolve);

  va_start(ap, opcode);
  args_from_va(interface_of(proxy)->methods[opcode].signature, args, ap);
  va_end(ap);

  on_request(proxy, opcode, 0, args);
  real_array(proxy, opcode, args);
}
