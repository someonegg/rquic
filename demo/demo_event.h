#ifndef DEMO_EVENT_H
#define DEMO_EVENT_H

#include <rquic/rquic.h>

typedef void (*demo_event_cb_pt)(void *arg);

typedef struct demo_event_watch_s {
    int fd;
    int armed;
    demo_event_cb_pt cb;
    void *arg;
    struct demo_event_watch_s *next;
} demo_event_watch_t;

typedef struct demo_event_runtime_s {
    rqc_engine_t *engine;
    int stop;
    int read_fd;
    demo_event_cb_pt read_cb;
    void *read_arg;
    int timer_set;
    rqc_usec_t timer_at;
    demo_event_watch_t *write_watches;
} demo_event_runtime_t;

int demo_event_runtime_init(demo_event_runtime_t *runtime);
void demo_event_runtime_cleanup(demo_event_runtime_t *runtime);
void demo_event_runtime_set_engine(demo_event_runtime_t *runtime,
    rqc_engine_t *engine);
void demo_event_runtime_set_read(demo_event_runtime_t *runtime, int fd,
    demo_event_cb_pt cb, void *arg);
void demo_event_runtime_arm_write(demo_event_runtime_t *runtime,
    demo_event_watch_t *watch, int fd, demo_event_cb_pt cb, void *arg);
void demo_event_runtime_disarm_write(demo_event_runtime_t *runtime,
    demo_event_watch_t *watch);
void demo_event_runtime_stop(demo_event_runtime_t *runtime);
int demo_event_runtime_run(demo_event_runtime_t *runtime);

void demo_set_event_timer(rqc_usec_t wake_after, void *engine_user_data);

#endif
