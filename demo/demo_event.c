#include "demo_event.h"

#include "demo_common.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>

int
demo_event_runtime_init(demo_event_runtime_t *runtime)
{
    if (runtime == NULL) {
        return -1;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->read_fd = -1;
    return 0;
}

void
demo_event_runtime_cleanup(demo_event_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->read_fd = -1;
}

void
demo_event_runtime_set_engine(demo_event_runtime_t *runtime, rqc_engine_t *engine)
{
    if (runtime != NULL) {
        runtime->engine = engine;
    }
}

void
demo_event_runtime_set_read(demo_event_runtime_t *runtime, int fd,
    demo_event_cb_pt cb, void *arg)
{
    if (runtime == NULL) {
        return;
    }

    runtime->read_fd = fd;
    runtime->read_cb = cb;
    runtime->read_arg = arg;
}

void
demo_event_runtime_disarm_write(demo_event_runtime_t *runtime,
    demo_event_watch_t *watch)
{
    demo_event_watch_t **cur;

    if (runtime == NULL || watch == NULL || !watch->armed) {
        return;
    }

    for (cur = &runtime->write_watches; *cur != NULL; cur = &(*cur)->next) {
        if (*cur == watch) {
            *cur = watch->next;
            break;
        }
    }

    watch->fd = -1;
    watch->armed = 0;
    watch->cb = NULL;
    watch->arg = NULL;
    watch->next = NULL;
}

void
demo_event_runtime_arm_write(demo_event_runtime_t *runtime,
    demo_event_watch_t *watch, int fd, demo_event_cb_pt cb, void *arg)
{
    if (runtime == NULL || watch == NULL || fd < 0 || cb == NULL) {
        return;
    }

    if (watch->armed) {
        demo_event_runtime_disarm_write(runtime, watch);
    }

    watch->fd = fd;
    watch->armed = 1;
    watch->cb = cb;
    watch->arg = arg;
    watch->next = runtime->write_watches;
    runtime->write_watches = watch;
}

void
demo_event_runtime_stop(demo_event_runtime_t *runtime)
{
    if (runtime != NULL) {
        runtime->stop = 1;
    }
}

void
demo_set_event_timer(rqc_usec_t wake_after, void *engine_user_data)
{
    demo_event_runtime_t *runtime = (demo_event_runtime_t *)engine_user_data;

    if (runtime == NULL) {
        return;
    }

    runtime->timer_at = demo_now() + wake_after;
    runtime->timer_set = 1;
}

static int
demo_event_timeout_ms(demo_event_runtime_t *runtime)
{
    rqc_usec_t now;
    rqc_usec_t diff;

    if (runtime == NULL || !runtime->timer_set) {
        return -1;
    }

    now = demo_now();
    if (runtime->timer_at <= now) {
        return 0;
    }

    diff = runtime->timer_at - now;
    diff = (diff + 999) / 1000;
    return diff > INT_MAX ? INT_MAX : (int)diff;
}

static nfds_t
demo_event_fill_pollfds(demo_event_runtime_t *runtime, struct pollfd *fds,
    demo_event_watch_t **write_watches, nfds_t max_fds)
{
    nfds_t nfds = 0;
    demo_event_watch_t *watch;

    if (runtime->read_fd >= 0 && runtime->read_cb != NULL && nfds < max_fds) {
        fds[nfds].fd = runtime->read_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        write_watches[nfds] = NULL;
        nfds++;
    }

    for (watch = runtime->write_watches; watch != NULL && nfds < max_fds;
         watch = watch->next)
    {
        if (!watch->armed || watch->fd < 0 || watch->cb == NULL) {
            continue;
        }
        fds[nfds].fd = watch->fd;
        fds[nfds].events = POLLOUT;
        fds[nfds].revents = 0;
        write_watches[nfds] = watch;
        nfds++;
    }

    return nfds;
}

static demo_event_watch_t *
demo_event_find_armed_write(demo_event_runtime_t *runtime,
    demo_event_watch_t *target, int fd)
{
    demo_event_watch_t *watch;

    for (watch = runtime->write_watches; watch != NULL; watch = watch->next) {
        if (watch == target && watch->armed && watch->fd == fd
            && watch->cb != NULL)
        {
            return watch;
        }
    }

    return NULL;
}

static void
demo_event_dispatch_pollfds(demo_event_runtime_t *runtime, struct pollfd *fds,
    demo_event_watch_t **write_watches, nfds_t nfds)
{
    nfds_t i;

    for (i = 0; i < nfds && !runtime->stop; i++) {
        if (fds[i].revents == 0) {
            continue;
        }

        if (write_watches[i] == NULL) {
            if (runtime->read_cb != NULL
                && (fds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
            {
                runtime->read_cb(runtime->read_arg);
            }
            continue;
        }

        if (fds[i].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) {
            demo_event_watch_t *watch;
            demo_event_cb_pt cb;
            void *arg;

            watch = demo_event_find_armed_write(runtime, write_watches[i],
                fds[i].fd);
            if (watch == NULL) {
                continue;
            }

            cb = watch->cb;
            arg = watch->arg;

            demo_event_runtime_disarm_write(runtime, watch);
            if (cb != NULL) {
                cb(arg);
            }
        }
    }
}

static void
demo_event_run_timer(demo_event_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->timer_set || runtime->timer_at > demo_now()) {
        return;
    }

    runtime->timer_set = 0;
    if (runtime->engine != NULL) {
        rqc_engine_main_logic(runtime->engine);
    }
}

int
demo_event_runtime_run(demo_event_runtime_t *runtime)
{
    while (runtime != NULL && !runtime->stop) {
        struct pollfd fds[64];
        demo_event_watch_t *write_watches[64];
        nfds_t nfds;
        int ret;

        demo_event_run_timer(runtime);
        if (runtime->stop) {
            break;
        }

        nfds = demo_event_fill_pollfds(runtime, fds, write_watches,
            (nfds_t)(sizeof(fds) / sizeof(fds[0])));
        ret = poll(fds, nfds, demo_event_timeout_ms(runtime));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            demo_event_run_timer(runtime);
            continue;
        }

        demo_event_dispatch_pollfds(runtime, fds, write_watches, nfds);
    }

    return 0;
}
