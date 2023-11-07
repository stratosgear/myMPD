/*
 SPDX-License-Identifier: GPL-3.0-or-later
 myMPD (c) 2018-2023 Juergen Mang <mail@jcgames.de>
 https://github.com/jcorporation/mympd
*/

#include "compile_time.h"
#include "src/mpd_worker/mpd_worker.h"

#include "dist/sds/sds.h"
#include "src/lib/log.h"
#include "src/lib/mem.h"
#include "src/lib/mympd_state.h"
#include "src/lib/sds_extras.h"
#include "src/lib/thread.h"
#include "src/mpd_client/connection.h"
#include "src/mpd_worker/api.h"

#include <pthread.h>

/**
 * Private definitions
 */

static void *mpd_worker_run(void *arg);

/**
 * Public functions
 */

/**
 * Starts the worker thread in detached state.
 * @param mympd_state pointer to mympd_state struct
 * @param request the work request
 * @return true on success, else false
 */
bool mpd_worker_start(struct t_mympd_state *mympd_state, struct t_work_request *request) {
    MYMPD_LOG_NOTICE(NULL, "Starting mpd_worker thread for %s", get_cmd_id_method_name(request->cmd_id));
    pthread_t mpd_worker_thread;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        MYMPD_LOG_ERROR(NULL, "Can not init mpd_worker thread attribute");
        return false;
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        MYMPD_LOG_ERROR(NULL, "Can not set mpd_worker thread to detached");
        return false;
    }
    //create mpd worker state from mympd_state
    struct t_mpd_worker_state *mpd_worker_state = malloc_assert(sizeof(struct t_mpd_worker_state));
    mpd_worker_state->request = request;
    mpd_worker_state->smartpls = mympd_state->smartpls == true ?
        mympd_state->mpd_state->feat.playlists
        : false;
    mpd_worker_state->smartpls_sort = sdsdup(mympd_state->smartpls_sort);
    mpd_worker_state->smartpls_prefix = sdsdup(mympd_state->smartpls_prefix);
    mpd_worker_state->tag_disc_empty_is_first = mympd_state->tag_disc_empty_is_first;
    copy_tag_types(&mympd_state->smartpls_generate_tag_types, &mpd_worker_state->smartpls_generate_tag_types);
    mpd_worker_state->config = mympd_state->config;

    //mpd state
    mpd_worker_state->mpd_state = malloc_assert(sizeof(struct t_mpd_state));
    mpd_state_copy(mympd_state->mpd_state, mpd_worker_state->mpd_state);

    //partition state
    mpd_worker_state->partition_state = malloc_assert(sizeof(struct t_partition_state));
    //worker runs always in default partition
    partition_state_default(mpd_worker_state->partition_state, mympd_state->partition_state->name, mympd_state);
    //use mpd state from worker
    mpd_worker_state->partition_state->mpd_state = mpd_worker_state->mpd_state;

    //stickerdb
    mpd_worker_state->stickerdb = malloc_assert(sizeof(struct t_partition_state));
    //worker runs always in default partition
    partition_state_default(mpd_worker_state->stickerdb, mympd_state->partition_state->name, mympd_state);
    // do not use the shared mpd_state - we can connect to another mpd server for stickers
    mpd_worker_state->stickerdb->mpd_state = malloc_assert(sizeof(struct t_mpd_state));
    mpd_state_copy(mympd_state->stickerdb->mpd_state, mpd_worker_state->stickerdb->mpd_state);

    //create the worker thread
    if (pthread_create(&mpd_worker_thread, &attr, mpd_worker_run, mpd_worker_state) != 0) {
        MYMPD_LOG_ERROR(NULL, "Can not create mpd_worker thread");
        mpd_worker_state_free(mpd_worker_state);
        return false;
    }
    worker_threads++;
    return true;
}

/**
 * Private functions
 */

/**
 * This is the main function of the worker thread.
 * @param arg void pointer to the mpd_worker_state
 */
static void *mpd_worker_run(void *arg) {
    thread_logname = sds_replace(thread_logname, "mpdworker");
    set_threadname(thread_logname);
    struct t_mpd_worker_state *mpd_worker_state = (struct t_mpd_worker_state *) arg;

    if (mpd_client_connect(mpd_worker_state->partition_state, false) == true) {
        //call api handler
        mpd_worker_api(mpd_worker_state);
        //disconnect
        mpd_client_disconnect_silent(mpd_worker_state->partition_state, MPD_REMOVED);
    }
    if (mpd_worker_state->stickerdb->conn != NULL) {
        mpd_client_disconnect_silent(mpd_worker_state->stickerdb, MPD_DISCONNECT_INSTANT);
    }
    MYMPD_LOG_NOTICE(NULL, "Stopping mpd_worker thread");
    mpd_worker_state_free(mpd_worker_state);
    worker_threads--;
    FREE_SDS(thread_logname);
    return NULL;
}
