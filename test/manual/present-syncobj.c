/* SPDX-License-Identifier: MIT
 *
 * Manual test for Present explicit sync (PresentPixmapSynced, DRI3 1.4).
 * Not built by meson or autotools. Build and run it on a running X server:
 *
 *   cc -o present-syncobj present-syncobj.c \
 *      $(pkg-config --cflags --libs xcb xcb-dri3 xcb-present libdrm)
 *   ./present-syncobj [--fullscreen] [--plain] [--device /dev/dri/renderD128]
 *
 * Windowed, the present should be a copy; its release point must stay
 * unsignalled until the acquire point is signalled, then signal.
 * With --fullscreen the present should be a flip; its release point must
 * signal only after a second present replaces it on the screen.
 * --plain uses PresentPixmap without sync objects instead and checks
 * IdleNotify the same way: after the copy, or once a flip is replaced.
 * The exit status is 0 only if every check passed.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xf86drm.h>

#define MAX_SERIAL 3

static xcb_connection_t *conn;
static uint8_t present_opcode;
static int complete_mode[MAX_SERIAL];
static int idle_serial[MAX_SERIAL];
static int idle_notifies;
static int failures;

static void
check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static uint64_t
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static const char *
mode_name(int mode)
{
    switch (mode) {
    case XCB_PRESENT_COMPLETE_MODE_COPY: return "Copy";
    case XCB_PRESENT_COMPLETE_MODE_FLIP: return "Flip";
    case XCB_PRESENT_COMPLETE_MODE_SKIP: return "Skip";
    case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY: return "SuboptimalCopy";
    default: return "none";
    }
}

static void
handle_event(xcb_generic_event_t *ev)
{
    xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *) ev;

    if ((ev->response_type & 0x7f) != XCB_GE_GENERIC ||
        ge->extension != present_opcode)
        return;

    if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
        xcb_present_complete_notify_event_t *c = (void *) ev;

        if (c->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP &&
            c->serial < MAX_SERIAL)
            complete_mode[c->serial] = c->mode;
    } else if (ge->event_type == XCB_PRESENT_IDLE_NOTIFY) {
        xcb_present_idle_notify_event_t *i = (void *) ev;

        idle_notifies++;
        if (i->serial < MAX_SERIAL)
            idle_serial[i->serial]++;
    }
}

/* Process events until serial's CompleteNotify (or, with idle, its
 * IdleNotify) arrives or timeout_ms ends.
 */
static int
wait_event(uint32_t serial, int idle, int timeout_ms)
{
    uint64_t end = now_ns() + (uint64_t) timeout_ms * 1000000ull;
    struct pollfd pfd = { .fd = xcb_get_file_descriptor(conn), .events = POLLIN };

    xcb_flush(conn);
    for (;;) {
        xcb_generic_event_t *ev;
        uint64_t now;

        while ((ev = xcb_poll_for_event(conn))) {
            handle_event(ev);
            free(ev);
        }
        if (idle ? idle_serial[serial] > 0 : complete_mode[serial] >= 0)
            return 1;

        now = now_ns();
        if (now >= end)
            return 0;
        poll(&pfd, 1, (int) ((end - now) / 1000000ull) + 1);
    }
}

static int
wait_complete(uint32_t serial, int timeout_ms)
{
    return wait_event(serial, 0, timeout_ms);
}

static int
wait_idle(uint32_t serial, int timeout_ms)
{
    return wait_event(serial, 1, timeout_ms);
}

/* Drain events without blocking. */
static void
drain_events(void)
{
    xcb_generic_event_t *ev;

    xcb_flush(conn);
    while ((ev = xcb_poll_for_event(conn))) {
        handle_event(ev);
        free(ev);
    }
}

/* 1 if the point has signalled within timeout_ms (0: just check). */
static int
point_signalled(int fd, uint32_t handle, uint64_t point, int timeout_ms)
{
    uint64_t deadline = timeout_ms ? now_ns() + (uint64_t) timeout_ms * 1000000ull : 0;

    if (drmSyncobjTimelineWait(fd, &handle, &point, 1, (int64_t) deadline,
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == 0)
        return 1;
    if (errno != ETIME)
        fprintf(stderr, "drmSyncobjTimelineWait: %s\n", strerror(errno));
    return 0;
}

static void
signal_point(int fd, uint32_t handle, uint64_t point)
{
    if (drmSyncobjTimelineSignal(fd, &handle, &point, 1))
        fprintf(stderr, "drmSyncobjTimelineSignal: %s\n", strerror(errno));
}

/* Create a timeline syncobj and import it into the server as id. */
static int
import_syncobj(int fd, xcb_window_t window, uint32_t *handle, xcb_dri3_syncobj_t *id)
{
    xcb_generic_error_t *err;
    int obj_fd;

    if (drmSyncobjCreate(fd, 0, handle) ||
        drmSyncobjHandleToFD(fd, *handle, &obj_fd)) {
        fprintf(stderr, "creating a syncobj failed: %s\n", strerror(errno));
        return 0;
    }

    *id = xcb_generate_id(conn);
    /* libxcb closes obj_fd once it is sent */
    err = xcb_request_check(conn, xcb_dri3_import_syncobj_checked(conn, *id, window, obj_fd));
    if (err) {
        fprintf(stderr, "DRI3ImportSyncobj: X error %d\n", err->error_code);
        free(err);
        return 0;
    }
    return 1;
}

static xcb_pixmap_t
filled_pixmap(xcb_screen_t *screen, xcb_window_t window, uint16_t w, uint16_t h,
              uint32_t color)
{
    xcb_pixmap_t pixmap = xcb_generate_id(conn);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_rectangle_t rect = { 0, 0, w, h };

    xcb_create_pixmap(conn, screen->root_depth, pixmap, window, w, h);
    xcb_create_gc(conn, gc, pixmap, XCB_GC_FOREGROUND, &color);
    xcb_poly_fill_rectangle(conn, pixmap, gc, 1, &rect);
    xcb_free_gc(conn, gc);
    return pixmap;
}

static int
present_synced(xcb_window_t window, xcb_pixmap_t pixmap, uint32_t serial,
               xcb_dri3_syncobj_t acquire, xcb_dri3_syncobj_t release,
               uint64_t point)
{
    xcb_generic_error_t *err;

    err = xcb_request_check(conn,
        xcb_present_pixmap_synced_checked(conn, window, pixmap, serial,
                                          XCB_NONE, XCB_NONE, 0, 0, XCB_NONE,
                                          acquire, release, point, point,
                                          XCB_PRESENT_OPTION_NONE, 0, 0, 0,
                                          0, NULL));
    if (err) {
        fprintf(stderr, "PresentPixmapSynced: X error %d\n", err->error_code);
        free(err);
        return 0;
    }
    return 1;
}

static int
present_plain(xcb_window_t window, xcb_pixmap_t pixmap, uint32_t serial)
{
    xcb_generic_error_t *err;

    err = xcb_request_check(conn,
        xcb_present_pixmap_checked(conn, window, pixmap, serial,
                                   XCB_NONE, XCB_NONE, 0, 0, XCB_NONE,
                                   XCB_NONE, XCB_NONE,
                                   XCB_PRESENT_OPTION_NONE, 0, 0, 0,
                                   0, NULL));
    if (err) {
        fprintf(stderr, "PresentPixmap: X error %d\n", err->error_code);
        free(err);
        return 0;
    }
    return 1;
}

/* PresentPixmap without sync objects: IdleNotify must follow the copy, or
 * the replacement of a flipped buffer.
 */
static void
run_plain(xcb_screen_t *screen, xcb_window_t window, uint16_t w, uint16_t h,
          int fullscreen)
{
    xcb_pixmap_t pix1, pix2 = XCB_NONE;

    pix1 = filled_pixmap(screen, window, w, h, 0x00c04040);
    check(present_plain(window, pix1, 1), "PresentPixmap accepted");
    check(wait_complete(1, 2000), "CompleteNotify");
    printf("present 1 mode: %s\n", mode_name(complete_mode[1]));

    if (complete_mode[1] == XCB_PRESENT_COMPLETE_MODE_FLIP) {
        drain_events();
        check(idle_serial[1] == 0, "no IdleNotify while the flipped buffer is on screen");

        pix2 = filled_pixmap(screen, window, w, h, 0x004040c0);
        check(present_plain(window, pix2, 2), "second PresentPixmap accepted");
        check(wait_complete(2, 2000), "second CompleteNotify");
        printf("present 2 mode: %s\n", mode_name(complete_mode[2]));
        check(wait_idle(1, 1000), "IdleNotify for present 1 after the next present");
    } else {
        if (fullscreen)
            printf("note: present 1 was not flipped (a compositor or another "
                   "window may prevent flipping)\n");
        check(wait_idle(1, 1000), "IdleNotify after the copy");
    }

    xcb_destroy_window(conn, window);
    xcb_free_pixmap(conn, pix1);
    if (pix2 != XCB_NONE)
        xcb_free_pixmap(conn, pix2);
    xcb_flush(conn);
}

int
main(int argc, char **argv)
{
    const char *device = "/dev/dri/renderD128";
    int fullscreen = 0;
    const xcb_query_extension_reply_t *ext;
    xcb_dri3_query_version_reply_t *dri3_ver;
    xcb_present_query_version_reply_t *present_ver;
    xcb_present_query_capabilities_reply_t *caps;
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_pixmap_t pix1, pix2 = XCB_NONE;
    uint32_t acquire_handle, release_handle;
    xcb_dri3_syncobj_t acquire_id, release_id;
    uint16_t w = 256, h = 256;
    xcb_generic_event_t *ev;
    int plain = 0;
    int fd = -1, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen"))
            fullscreen = 1;
        else if (!strcmp(argv[i], "--plain"))
            plain = 1;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            device = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--fullscreen] [--plain] [--device PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    for (i = 0; i < MAX_SERIAL; i++)
        complete_mode[i] = -1;

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "cannot connect to the X server\n");
        return 2;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

    ext = xcb_get_extension_data(conn, &xcb_present_id);
    if (!ext || !ext->present) {
        fprintf(stderr, "no Present extension\n");
        return 2;
    }
    present_opcode = ext->major_opcode;

    dri3_ver = xcb_dri3_query_version_reply(conn, xcb_dri3_query_version(conn, 1, 4), NULL);
    present_ver = xcb_present_query_version_reply(conn, xcb_present_query_version(conn, 1, 4), NULL);
    if (!dri3_ver || !present_ver) {
        fprintf(stderr, "DRI3 or Present QueryVersion failed\n");
        return 2;
    }
    printf("DRI3 %u.%u, Present %u.%u\n", dri3_ver->major_version, dri3_ver->minor_version,
           present_ver->major_version, present_ver->minor_version);
    if (!plain)
        check(dri3_ver->major_version > 1 ||
              (dri3_ver->major_version == 1 && dri3_ver->minor_version >= 4),
              "DRI3 1.4");
    free(dri3_ver);
    free(present_ver);

    if (!plain) {
        fd = open(device, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", device, strerror(errno));
            return 2;
        }
    }

    if (fullscreen) {
        w = screen->width_in_pixels;
        h = screen->height_in_pixels;
    }

    window = xcb_generate_id(conn);
    {
        uint32_t values[] = { screen->black_pixel, fullscreen ? 1 : 0,
                              XCB_EVENT_MASK_EXPOSURE };

        xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root,
                          0, 0, w, h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                          values);
    }
    xcb_present_select_input(conn, xcb_generate_id(conn), window,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                             XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    xcb_map_window(conn, window);
    xcb_flush(conn);
    while ((ev = xcb_wait_for_event(conn))) {
        int expose = (ev->response_type & 0x7f) == XCB_EXPOSE;

        free(ev);
        if (expose)
            break;
    }

    caps = xcb_present_query_capabilities_reply(conn,
               xcb_present_query_capabilities(conn, window), NULL);
    printf("Present capabilities: 0x%x\n", caps ? caps->capabilities : 0);
    if (plain) {
        free(caps);
        run_plain(screen, window, w, h, fullscreen);
        xcb_disconnect(conn);
        printf("%s: %d check(s) failed\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    check(caps && (caps->capabilities & XCB_PRESENT_CAPABILITY_SYNCOBJ),
          "PresentCapabilitySyncobj advertised");
    free(caps);

    if (!import_syncobj(fd, window, &acquire_handle, &acquire_id) ||
        !import_syncobj(fd, window, &release_handle, &release_id)) {
        check(0, "DRI3ImportSyncobj");
        return 1;
    }
    check(1, "DRI3ImportSyncobj");

    pix1 = filled_pixmap(screen, window, w, h, 0x00c04040);

    /* Present 1 with its acquire point still unsignalled */
    check(present_synced(window, pix1, 1, acquire_id, release_id, 1),
          "PresentPixmapSynced accepted");
    usleep(200 * 1000);
    drain_events();
    check(complete_mode[1] < 0, "present waits for the acquire point");
    check(!point_signalled(fd, release_handle, 1, 0),
          "release point unsignalled before the acquire point");

    signal_point(fd, acquire_handle, 1);
    check(wait_complete(1, 2000), "CompleteNotify after the acquire point");
    printf("present 1 mode: %s\n", mode_name(complete_mode[1]));

    if (complete_mode[1] == XCB_PRESENT_COMPLETE_MODE_FLIP) {
        check(!point_signalled(fd, release_handle, 1, 0),
              "flipped buffer's release point unsignalled while on screen");

        /* Present 2 replaces present 1 on the screen */
        pix2 = filled_pixmap(screen, window, w, h, 0x004040c0);
        signal_point(fd, acquire_handle, 2);
        check(present_synced(window, pix2, 2, acquire_id, release_id, 2),
              "second PresentPixmapSynced accepted");
        check(wait_complete(2, 2000), "second CompleteNotify");
        printf("present 2 mode: %s\n", mode_name(complete_mode[2]));
        check(point_signalled(fd, release_handle, 1, 1000),
              "release point 1 signalled after the next present");
    } else {
        if (fullscreen)
            printf("note: present 1 was not flipped (a compositor or another "
                   "window may prevent flipping)\n");
        check(point_signalled(fd, release_handle, 1, 1000),
              "release point signalled after the copy");
    }

    drain_events();
    check(idle_notifies == 0, "no IdleNotify for synced presents");

    xcb_destroy_window(conn, window);
    xcb_free_pixmap(conn, pix1);
    if (pix2 != XCB_NONE) {
        xcb_free_pixmap(conn, pix2);
        xcb_flush(conn);
        /* Information only: the server may keep the last flipped buffer
         * on the screen after its window is gone, until the next flip or
         * unflip.
         */
        printf("info: release point 2 %s within 1 s of destroying the window\n",
               point_signalled(fd, release_handle, 2, 1000) ?
               "signalled" : "not signalled");
    }
    xcb_dri3_free_syncobj(conn, acquire_id);
    xcb_dri3_free_syncobj(conn, release_id);
    xcb_flush(conn);

    drmSyncobjDestroy(fd, acquire_handle);
    drmSyncobjDestroy(fd, release_handle);
    close(fd);
    xcb_disconnect(conn);

    printf("%s: %d check(s) failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
