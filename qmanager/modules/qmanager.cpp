/*****************************************************************************\
 *  Copyright (c) 2019 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

extern "C" {
#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <flux/core.h>
#include <flux/schedutil.h>
#include <jansson.h>
}

#include "qmanager/policies/base/queue_policy_base.hpp"
#include "qmanager/policies/base/queue_policy_base_impl.hpp"
#include "qmanager/policies/queue_policy_factory_impl.hpp"
#include "qmanager/modules/qmanager_opts.hpp"
#include "src/common/c++wrappers/eh_wrapper.hpp"

using namespace Flux;
using namespace Flux::queue_manager;
using namespace Flux::queue_manager::detail;
using namespace Flux::opts_manager;
using namespace Flux::cplusplus_wrappers;


/******************************************************************************
 *                                                                            *
 *                 Queue Manager Service Module Context                       *
 *                                                                            *
 ******************************************************************************/

struct qmanager_ctx_t {
    flux_t *h;
    schedutil_t *schedutil;
    optmgr_composer_t<qmanager_opts_t> opts;
    std::shared_ptr<queue_policy_base_t> queue;
};


/******************************************************************************
 *                                                                            *
 *                     Internal Queue Manager APIs                            *
 *                                                                            *
 ******************************************************************************/

static int post_sched_loop (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    int rc = -1;
    std::shared_ptr<job_t> job = nullptr;

    while ((job = ctx->queue->alloced_pop ()) != nullptr) {
        if (schedutil_alloc_respond_R (ctx->schedutil, job->msg,
                                       job->schedule.R.c_str (), NULL) < 0) {
            flux_log_error (ctx->h, "%s: schedutil_alloc_respond_R",
                            __FUNCTION__);
            goto out;
        }
        flux_log (ctx->h, LOG_DEBUG,
                  "alloc success (id=%jd)", (intmax_t)job->id);
    }
    while ((job = ctx->queue->rejected_pop ()) != nullptr) {
        std::string note = "alloc denied due to type=\"" + job->note + "\"";
        if (schedutil_alloc_respond_denied (ctx->schedutil,
                                            job->msg,
                                            note.c_str ()) < 0) {
            flux_log_error (ctx->h, "%s: schedutil_alloc_respond_denied",
                            __FUNCTION__);
            goto out;
        }
        flux_log (ctx->h, LOG_DEBUG,
                  "%s (id=%jd)", note.c_str (), (intmax_t)job->id);
    }
    rc = 0;

out:
    return rc;
}

// FIXME: This will be expanded when we implement full scheduler
// resilency schemes: Issue #470.
extern "C" int jobmanager_hello_cb (flux_t *h,
                                    flux_jobid_t id, int prio, uint32_t uid,
                                    double ts, const char *R, void *arg)

{
    int rc = -1;
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    ctx = *(static_cast<std::shared_ptr<qmanager_ctx_t> *>(arg));
    std::shared_ptr<job_t> running_job
        = std::make_shared<job_t> (job_state_kind_t::
                                   RUNNING, id, uid, prio, ts, R);

    if (ctx->queue->reconstruct (running_job) < 0) {
        flux_log_error (h, "%s: reconstruct (id=%jd)", __FUNCTION__, (intmax_t)id);
        goto out;
    }
    rc = 0;

out:
    return rc;
}

extern "C" void jobmanager_alloc_cb (flux_t *h, const flux_msg_t *msg,
                                     const char *jobspec, void *arg)
{
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    ctx = *(static_cast<std::shared_ptr<qmanager_ctx_t> *>(arg));
    std::shared_ptr<job_t> job = std::make_shared<job_t> ();

    if (schedutil_alloc_request_decode (msg, &job->id, &job->priority,
                                        &job->userid, &job->t_submit) < 0) {
        flux_log_error (h, "%s: schedutil_alloc_request_decode", __FUNCTION__);
        return;
    }
    job->jobspec = jobspec;
    job->msg = flux_msg_copy (msg, true);
    if (ctx->queue->insert (job) < 0) {
        flux_log_error (h, "%s: queue insert (id=%jd)",
                        __FUNCTION__, (intmax_t)job->id);
        return;
    }
    if (ctx->queue->run_sched_loop ((void *)ctx->h, true) < 0
        || post_sched_loop (ctx) < 0) {
        flux_log_error (ctx->h, "%s: schedule loop", __FUNCTION__);
        return;
    }
}

extern "C" void jobmanager_free_cb (flux_t *h, const flux_msg_t *msg,
                                    const char *R, void *arg)
{
    flux_jobid_t id;
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    ctx = *(static_cast<std::shared_ptr<qmanager_ctx_t> *>(arg));

    if (schedutil_free_request_decode (msg, &id) < 0) {
        flux_log_error (h, "%s: schedutil_free_request_decode", __FUNCTION__);
        return;
    }
    if ((ctx->queue->remove (id)) < 0) {
        flux_log_error (h, "%s: remove (id=%jd)", __FUNCTION__, (intmax_t)id);
        return;
    }
    if (ctx->queue->run_sched_loop ((void *)ctx->h, true) < 0) {
        flux_log_error (ctx->h, "%s: run_sched_loop", __FUNCTION__);
        return;
    }
    if (schedutil_free_respond (ctx->schedutil, msg) < 0) {
        flux_log_error (h, "%s: schedutil_free_respond", __FUNCTION__);
        return;
    }
    flux_log (ctx->h, LOG_DEBUG, "free succeeded (id=%jd)", (intmax_t)id);
    if (post_sched_loop (ctx) < 0) {
        flux_log_error (ctx->h, "%s: post_sched_loop", __FUNCTION__);
        return;
    }
}

static void jobmanager_exception_cb (flux_t *h, flux_jobid_t id,
                                     const char *type, int severity, void *arg)
{
    std::shared_ptr<job_t> job;
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    ctx = *(static_cast<std::shared_ptr<qmanager_ctx_t> *>(arg));

    if (severity > 0 || (job = ctx->queue->lookup (id)) == nullptr
        || !job->is_pending ())
        return;
    if (ctx->queue->remove (id) < 0) {
        flux_log_error (h, "%s: remove job (%jd)", __FUNCTION__, (intmax_t)id);
        return;
    }
    std::string note = std::string ("alloc aborted due to exception type=")
                       + type;
    if (schedutil_alloc_respond_denied (ctx->schedutil,
                                        job->msg,
                                        note.c_str ()) < 0) {
        flux_log_error (h, "%s: schedutil_alloc_respond_denied", __FUNCTION__);
        return;
    }
    flux_log (h, LOG_DEBUG, "%s (id=%jd)", note.c_str (), (intmax_t)id);
}

static int process_args (std::shared_ptr<qmanager_ctx_t> &ctx,
                         int argc, char **argv)
{
    int rc = 0;
    optmgr_kv_t<qmanager_opts_t> opts_store;
    std::string info_str = "";

    for (int i = 0; i < argc; i++) {
        const std::string kv (argv[i]);
        if ( (rc = opts_store.put (kv)) < 0) {
            flux_log_error (ctx->h, "%s: optmgr_kv_t::put (%s)",
                             __FUNCTION__, argv[i]);
            return rc;
        }
    }
    if ( (rc = opts_store.parse (info_str)) < 0) {
        flux_log_error (ctx->h, "%s: optmgr_kv_t::parse: %s",
                        __FUNCTION__, info_str.c_str ());
        return rc;
    }
    if (info_str != "") {
        flux_log (ctx->h, LOG_DEBUG, "%s: %s", __FUNCTION__, info_str.c_str ());
    }
    ctx->opts += opts_store.get_opt ();
    return rc;
}

static int process_config_file (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    int rc = 0;
    json_t *conf = NULL;

    if ( (rc = flux_conf_unpack (flux_get_conf (ctx->h), NULL,
                                     "{ s?:o }",
                                         "qmanager", &conf)) < 0) {
        flux_log_error (ctx->h, "%s: flux_conf_unpack", __FUNCTION__);
        return rc;
    }

    const char *k = NULL;
    json_t *v = NULL;
    optmgr_kv_t<qmanager_opts_t> opts_store;
    std::string info_str = "";
    json_object_foreach (conf, k, v) {
        std::string value = json_string_value (v);
        if ( (rc = opts_store.put (k, value)) < 0) {
            flux_log_error (ctx->h, "%s: optmgr_kv_t::put (%s, %s)",
                             __FUNCTION__, k, value.c_str ());
            return rc;
        }
    }
    if ( (rc = opts_store.parse (info_str)) < 0) {
        flux_log_error (ctx->h, "%s: optmgr_kv_t::parse: %s",
                        __FUNCTION__, info_str.c_str ());
        return rc;
    }
    if (info_str != "") {
        flux_log (ctx->h, LOG_DEBUG, "%s: %s", __FUNCTION__, info_str.c_str ());
    }
    ctx->opts += opts_store.get_opt ();
    return rc;
}

static void set_default (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    qmanager_opts_t ct_opts;
    ct_opts.set_queue_policy ("fcfs");
    ct_opts.set_queue_params ("");
    ct_opts.set_policy_params ("");
    ctx->opts += ct_opts;
}

static int handshake_jobmanager (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    int rc = -1;
    int queue_depth = 0;  /* Not implemented in job-manager */
    const qmanager_opts_t &opt = ctx->opts.get_opt ();
    const char *mode = (opt.get_queue_policy () == "fcfs")? "single"
                                                          : "unlimited";
    if (!(ctx->schedutil = schedutil_create (ctx->h,
                                             jobmanager_alloc_cb,
                                             jobmanager_free_cb,
                                             jobmanager_exception_cb,
                                             &ctx))) {
        flux_log_error (ctx->h, "%s: schedutil_create", __FUNCTION__);
        goto out;
    }
    if (schedutil_hello (ctx->schedutil, jobmanager_hello_cb, &ctx) < 0) {
        flux_log_error (ctx->h, "%s: schedutil_hello", __FUNCTION__);
        goto out;
    }
    if (schedutil_ready (ctx->schedutil, mode, &queue_depth)) {
        flux_log_error (ctx->h, "%s: schedutil_ready", __FUNCTION__);
        goto out;
    }
    rc = 0;
out:
    return rc;
}

static int enforce_queue_policy (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    std::string res_qp;
    std::string res_pp;
    const qmanager_opts_t &effect = ctx->opts.get_opt ();

    ctx->queue = create_queue_policy (effect.get_queue_policy (), "module");
    flux_log (ctx->h, LOG_DEBUG,
              "enforced policy: %s", effect.get_queue_policy ().c_str ());
    if (!ctx->queue) {
        errno = EINVAL;
        flux_log_error (ctx->h, "%s: create_queue_policy (%s)",
                        __FUNCTION__, effect.get_queue_policy ().c_str ());
        return -1;
    }
    if (effect.is_queue_params_set () && effect.get_queue_params () != ""
        && ctx->queue->set_queue_params (effect.get_queue_params ()) < 0) {
        flux_log_error (ctx->h, "%s: queue->set_params", __FUNCTION__);
        return -1;
    }
    if (effect.is_policy_params_set () && effect.get_policy_params () != ""
        && ctx->queue->set_policy_params (effect.get_policy_params ()) < 0) {
        flux_log_error (ctx->h, "%s: queue->set_params", __FUNCTION__);
        return -1;
    }
    if (ctx->queue->apply_params () < 0) {
        flux_log_error (ctx->h, "%s: queue->apply_params", __FUNCTION__);
        return -1;
    }
    ctx->queue->get_params (res_qp, res_pp);

    if (res_qp.empty ())
        res_qp = std::string ("default");
    if (res_pp.empty ())
        res_pp = std::string ("default");
    flux_log (ctx->h, LOG_DEBUG,
              "effective queue params: %s", res_qp.c_str ());
    flux_log (ctx->h, LOG_DEBUG,
              "effective policy params: %s", res_pp.c_str ());

    if (handshake_jobmanager (ctx) < 0) {
        flux_log_error (ctx->h, "%s: handshake_jobmanager", __FUNCTION__);
        return -1;
    }
    return 0;
}

static std::shared_ptr<qmanager_ctx_t> qmanager_new (flux_t *h)
{
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    try {
        ctx = std::make_shared<qmanager_ctx_t> ();
        ctx->h = h;
        ctx->schedutil = NULL;
        set_default (ctx);
    } catch (std::bad_alloc &e) {
        errno = ENOMEM;
    }

    return ctx;
}

static void qmanager_destroy (std::shared_ptr<qmanager_ctx_t> &ctx)
{
    if (ctx) {
        int saved_errno = errno;
        std::shared_ptr<job_t> job;
        while (ctx->queue && (job = ctx->queue->pending_pop ()) != nullptr)
            flux_respond_error (ctx->h, job->msg, ENOSYS, "unloading");
        while (ctx->queue && (job = ctx->queue->complete_pop ()) != nullptr)
            flux_respond_error (ctx->h, job->msg, ENOSYS, "unloading");
        schedutil_destroy (ctx->schedutil);
        errno = saved_errno;
    }
}


/******************************************************************************
 *                                                                            *
 *                               Module Main                                  *
 *                                                                            *
 ******************************************************************************/

int mod_start (flux_t *h, int argc, char **argv)
{
    int rc = -1;
    std::shared_ptr<qmanager_ctx_t> ctx = nullptr;
    if ( !(ctx = qmanager_new (h))) {
        flux_log_error (h, "%s: qmanager_new", __FUNCTION__);
        return rc;
    }
    if ( (rc = process_config_file (ctx)) < 0) {
        flux_log_error (h, "%s: config file parsing", __FUNCTION__);
        qmanager_destroy (ctx);
        return rc;
    }
    if ( (rc = process_args (ctx, argc, argv)) < 0) {
        flux_log_error (h, "%s: load line argument parsing", __FUNCTION__);
        qmanager_destroy (ctx);
        return rc;
    }
    if ( (rc = enforce_queue_policy (ctx)) < 0) {
        flux_log_error (h, "%s: enforce_queue_policy", __FUNCTION__);
        qmanager_destroy (ctx);
        return rc;
    }
    if ( (rc = flux_reactor_run (flux_get_reactor (h), 0)) < 0)
        flux_log_error (h, "%s: flux_reactor_run", __FUNCTION__);
    qmanager_destroy (ctx);
    return rc;
}

extern "C" int mod_main (flux_t *h, int argc, char **argv)
{
    eh_wrapper_t exception_safe_main;
    int rc = exception_safe_main (mod_start, h, argc, argv);
    if (exception_safe_main.bad ())
        flux_log_error (h, "%s: %s", __FUNCTION__,
                            exception_safe_main.get_err_message ());
    return rc;
}

MOD_NAME ("qmanager");

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
