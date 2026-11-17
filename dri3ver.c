#include <stdio.h>
#include <xcb/dri3.h>
int main(void)
{
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    xcb_dri3_query_version_reply_t *r =
        xcb_dri3_query_version_reply(c, xcb_dri3_query_version(c, 1, 4), NULL);
    if (!r) { puts("DRI3 not available"); return 1; }
    printf("DRI3 %u.%u\n", r->major_version, r->minor_version);
    return 0;
}
