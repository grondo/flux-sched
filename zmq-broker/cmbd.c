/* cmbd.c - simple zmq message broker, to run on each node of a job */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <json/json.h>
#include <czmq.h>

#include "log.h"
#include "zmq.h"
#include "cmbd.h"
#include "cmb.h"
#include "util.h"
#include "plugin.h"

#define OPTIONS "t:T:e:vs:r:R:S:"
static const struct option longopts[] = {
    {"event-uri",   required_argument,  0, 'e'},
    {"tree-in-uri", required_argument,  0, 't'},
    {"tree-out-uri",required_argument,  0, 'T'},
    {"redis-server",required_argument,  0, 'r'},
    {"verbose",           no_argument,  0, 'v'},
    {"syncperiod",  required_argument,  0, 's'},
    {"rank",        required_argument,  0, 'R'},
    {"size",        required_argument,  0, 'S'},
    {0, 0, 0, 0},
};

static void _cmb_init (conf_t *conf, server_t **srvp);
static void _cmb_fini (conf_t *conf, server_t *srv);
static void _cmb_poll (conf_t *conf, server_t *srv);

static void usage (void)
{
    fprintf (stderr, 
"Usage: cmbd OPTIONS\n"
" -e,--event-uri URI     Set event URI e.g. epgm://eth0;239.192.1.1:5555\n"
" -t,--tree-in-uri URI   Set tree-in URI, e.g. tcp://*:5556\n"
" -T,--tree-out-uri URI  Set tree-out URI, e.g. tcp://hostname:5556\n"
" -r,--redis-server HOST Set redis server hostname\n"
" -v,--verbose           Show bus traffic\n"
" -s,--syncperiod N      Set sync period in seconds\n"
" -R,--rank N            Set cmbd address\n"
" -S,--size N            Set number of ranks in session\n"
            );
    exit (1);
}

int main (int argc, char *argv[])
{
    int c;
    conf_t *conf;
    server_t *srv;

    log_init (basename (argv[0]));

    conf = xzmalloc (sizeof (conf_t));
    conf->syncperiod_msec = 10*1000;
    conf->size = 1;
    conf->apisockpath = CMB_API_PATH;
    while ((c = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (c) {
            case 'e':   /* --event-uri URI */
                conf->event_uri = optarg;
                break;
            case 't':   /* --tree-in-uri URI */
                conf->treein_uri = optarg;
                break;
            case 'T':   /* --tree-out-uri URI */
                conf->treeout_uri = optarg;
                break;
            case 'v':   /* --verbose */
                conf->verbose = true;
                break;
            case 's':   /* --syncperiod sec */
                conf->syncperiod_msec = strtoul (optarg, NULL, 10) * 1000;
                break;
            case 'r':   /* --redis-server hostname */
                conf->redis_server = optarg;
                break;
            case 'R':   /* --rank N */
                conf->rank = strtoul (optarg, NULL, 10);
                break;
            case 'S':   /* --size N */
                conf->size = strtoul (optarg, NULL, 10);
                break;
            default:
                usage ();
        }
    }
    if (optind < argc)
        usage ();

    _cmb_init (conf, &srv);
    for (;;)
        _cmb_poll (conf, srv);
    _cmb_fini (conf, srv);

    free (conf);

    return 0;
}

static void _cmb_init (conf_t *conf, server_t **srvp)
{
    server_t *srv;
    zctx_t *zctx;

    srv = xzmalloc (sizeof (server_t));
    srv->zctx = zctx = zctx_new ();
    if (!srv->zctx)
        err_exit ("zctx_new");
    zctx_set_linger (srv->zctx, 5);

    /* broker side of plugin sockets */
    /* N.B. each plugin has a private zs_plout */
    zbind (zctx, &srv->zs_router,      ZMQ_ROUTER, ROUTER_URI, -1);
    zbind (zctx, &srv->zs_plin,        ZMQ_PULL, PLIN_URI,        -1);
    zbind (zctx, &srv->zs_plout_event, ZMQ_PUB,  PLOUT_EVENT_URI, -1);
    zbind (zctx, &srv->zs_plin_event,  ZMQ_PULL, PLIN_EVENT_URI,  -1);

    /* external sockets */
    if (conf->event_uri) {
        zbind (zctx, &srv->zs_eventout, ZMQ_PUB, conf->event_uri, -1);
        zbind (zctx, &srv->zs_eventin,  ZMQ_SUB, conf->event_uri, -1);
        zsocket_set_subscribe (srv->zs_eventin, "");
    }
    if (conf->treeout_uri)
        zconnect (zctx, &srv->zs_upreq, ZMQ_DEALER, conf->treeout_uri, -1,NULL);
    if (conf->treein_uri) {
        if (zsocket_bind (srv->zs_router, "%s", conf->treein_uri) < 0)
            err_exit ("zsocket_bind: %s", conf->treein_uri);
    }

    plugin_init (conf, srv);

    *srvp = srv;
}

static void _cmb_fini (conf_t *conf, server_t *srv)
{
    plugin_fini (conf, srv);

    zsocket_destroy (srv->zctx, srv->zs_router);
    zsocket_destroy (srv->zctx, srv->zs_plin);
    zsocket_destroy (srv->zctx, srv->zs_plout_event);
    zsocket_destroy (srv->zctx, srv->zs_plin_event);

    if (srv->zs_upreq)
        zsocket_destroy (srv->zctx, srv->zs_upreq);
    if (srv->zs_eventin)
        zsocket_destroy (srv->zctx, srv->zs_eventin);
    if (srv->zs_eventout)
        zsocket_destroy (srv->zctx, srv->zs_eventout);

    zctx_destroy (&srv->zctx);

    free (srv);
}

static void _route (conf_t *conf, void *src, void *d1, void *d2, char *s)
{
    zmsg_t *m, *cpy;

    m = zmsg_recv (src);
    if (!m)
        return;

    if (conf->verbose) {
        zmsg_dump (m);
        msg ("%s", s);
    }
    if (d2) {
        cpy = zmsg_dup (m);
        if (!cpy)
            err_exit ("zmsg_dup");
        if (zmsg_send (&cpy, d2) < 0)
            err_exit ("zmsg_send");
    }
    if (d1) {
        if (zmsg_send (&m, d1) < 0)
            err_exit ("zmsg_send");
    } else {
        zmsg_destroy (&m);
    }
}

static void _route_request (conf_t *conf, server_t *srv)
{
    zmsg_t *zmsg = zmsg_recv (srv->zs_router);

    if (zmsg)
        plugin_send (srv, conf, &zmsg); /* prints debug output */
    if (zmsg && srv->zs_upreq) {
        if (conf->verbose) {
            zmsg_dump (zmsg);
            msg ("router->upreq");
        }
        zmsg_send (&zmsg, srv->zs_upreq); 
    }
    if (zmsg) {
        if (conf->verbose) {
            zmsg_dump (zmsg);
            msg ("router->router (NAK)");
        }
        cmb_msg_sendnak (&zmsg, srv->zs_router);
    }

    assert (zmsg == NULL);
}

static void _cmb_poll (conf_t *conf, server_t *srv)
{
    zmq_pollitem_t zpa[] = {
{ .socket = srv->zs_router,     .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = srv->zs_upreq,      .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = srv->zs_plin,       .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = srv->zs_plin_event, .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
{ .socket = srv->zs_eventin,    .events = ZMQ_POLLIN, .revents = 0, .fd = -1 },
    };

    zpoll (zpa, sizeof (zpa) / sizeof (zpa[0]), -1);

    if (zpa[0].revents & ZMQ_POLLIN) /* router */
        _route_request (conf, srv);
    if (zpa[1].revents & ZMQ_POLLIN) /* upreq (upstream responding to req) */
        _route (conf, srv->zs_upreq, srv->zs_router, NULL, "upreq->router");
    if (zpa[2].revents & ZMQ_POLLIN) /* plin (plugin responding to req) */
        _route (conf, srv->zs_plin, srv->zs_router, NULL, "plin->router");
    if (zpa[3].revents & ZMQ_POLLIN) /* plin_event (plugin sending event) */
        _route (conf, srv->zs_plin_event, srv->zs_plout_event, srv->zs_eventout,
                "plin_event->plout_event,eventout");
    if (zpa[4].revents & ZMQ_POLLIN) /* eventin (external event input) */
        _route (conf, srv->zs_eventin, srv->zs_plout_event, NULL,
                "eventin->plout_event");
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */