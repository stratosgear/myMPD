/*
 SPDX-License-Identifier: GPL-2.0-or-later
 myMPD (c) 2018-2021 Juergen Mang <mail@jcgames.de>
 https://github.com/jcorporation/mympd
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#include <mpd/client.h>

#include "../../dist/src/sds/sds.h"
#include "../sds_extras.h"
#include "../api.h"
#include "../log.h"
#include "../list.h"
#include "config_defs.h"
#include "../utility.h"
#include "../tiny_queue.h"
#include "../global.h"
#include "../mpd_shared/mpd_shared_typedefs.h"
#include "../mpd_shared/mpd_shared_tags.h"
#include "../mpd_shared.h"
#include "../mpd_shared/mpd_shared_sticker.h"
#include "mpd_worker_utility.h"
#include "mpd_worker_albumcache.h"

//privat definitions
static bool _album_cache_init(t_mpd_worker_state *mpd_worker_state, rax *album_cache);

//public functions
bool mpd_worker_album_cache_init(t_mpd_worker_state *mpd_worker_state) {
    rax *album_cache = raxNew();
    bool rc = _album_cache_init(mpd_worker_state, album_cache);
    //push album cache building response to mpd_client thread
    t_work_request *request = create_request(-1, 0, MPD_API_ALBUMCACHE_CREATED, "MPD_API_ALBUMCACHE_CREATED", "");
    request->data = sdscat(request->data, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"MPD_API_ALBUMCACHE_CREATED\",\"params\":{}}");
    if (rc == true) {
        request->extra = (void *) album_cache;
    }
    else {
        album_cache_free(&album_cache);
    }
    tiny_queue_push(mpd_client_queue, request, 0);
    return rc;
}

//private functions
static bool _album_cache_init(t_mpd_worker_state *mpd_worker_state, rax *album_cache) {
    LOG_VERBOSE("Creating album cache");
    unsigned start = 0;
    unsigned end = start + 1000;
    unsigned i = 0;
    //create search expression
    /*
    TODO: evaluate why this expression returns empty albums and artists
    sds expression = sdsnew("((Album != '') AND ");
    if (mpd_shared_tag_exists(mpd_worker_state->mpd_state->mympd_tag_types.tags, mpd_worker_state->mpd_state->mympd_tag_types.len, MPD_TAG_ALBUM_ARTIST) == true) {
        expression = sdscat(expression, " (AlbumArtist != ''))");
    }
    else if (mpd_shared_tag_exists(mpd_worker_state->mpd_state->mympd_tag_types.tags, mpd_worker_state->mpd_state->mympd_tag_types.len, MPD_TAG_ARTIST) == true) {
        expression = sdscat(expression, " (Artist != ''))");
    }
    else {
        sdsfree(expression);
        return false;
    }
    */
    
    //get first song from each album
    do {
        bool rc = mpd_search_db_songs(mpd_worker_state->mpd_state->conn, false);
        if (check_rc_error_and_recover(mpd_worker_state->mpd_state, NULL, NULL, 0, false, rc, "mpd_search_db_songs") == false) {
            LOG_ERROR("Album cache update failed");
            mpd_search_cancel(mpd_worker_state->mpd_state->conn);
            return false;
        }
        /*
        rc = mpd_search_add_expression(mpd_worker_state->mpd_state->conn, expression);
        if (check_rc_error_and_recover(mpd_worker_state->mpd_state, NULL, NULL, 0, false, rc, "mpd_search_add_expression") == false) {
            LOG_ERROR("Album cache update failed");
            mpd_search_cancel(mpd_worker_state->mpd_state->conn);
            return false;
        }
        */
        //Add empty uri constraint to get all songs, replace it with search expression if it works
        rc = mpd_search_add_uri_constraint(mpd_worker_state->mpd_state->conn, MPD_OPERATOR_DEFAULT, "");
        if (check_rc_error_and_recover(mpd_worker_state->mpd_state, NULL, NULL, 0, false, rc, "mpd_search_add_uri_constraint") == false) {
            LOG_ERROR("Album cache update failed");
            mpd_search_cancel(mpd_worker_state->mpd_state->conn);
            return false;
        }
        rc = mpd_search_add_window(mpd_worker_state->mpd_state->conn, start, end);
        if (check_rc_error_and_recover(mpd_worker_state->mpd_state, NULL, NULL, 0, false, rc, "mpd_search_add_window") == false) {
            LOG_ERROR("Album cache update failed");
            mpd_search_cancel(mpd_worker_state->mpd_state->conn);
            return false;
        }
        rc = mpd_search_commit(mpd_worker_state->mpd_state->conn);
        if (check_rc_error_and_recover(mpd_worker_state->mpd_state, NULL, NULL, 0, false, rc, "mpd_search_commit") == false) {
            LOG_ERROR("Album cache update failed");
            return false;
        }
        struct mpd_song *song;
        sds album = sdsempty();
        sds artist = sdsempty();
        sds key = sdsempty();
        while ((song = mpd_recv_song(mpd_worker_state->mpd_state->conn)) != NULL) {
            album = mpd_shared_get_tags(song, MPD_TAG_ALBUM, album);
            artist = mpd_shared_get_tags(song, MPD_TAG_ALBUM_ARTIST, artist);
            if (strcmp(album, "-") > 0 && strcmp(artist, "-") > 0) { //workarround for search expression not working
                sdsclear(key);
                key = sdscatfmt(key, "%s::%s", album, artist);
                if (raxTryInsert(album_cache, (unsigned char*)key, sdslen(key), (void *)song, NULL) == 0) {
                     //discard song data if key exists
                     mpd_song_free(song);
                }
            }
            else {
                LOG_WARN("Albumcache, skipping \"%s\"", mpd_song_get_uri(song));
                mpd_song_free(song);
            }
            i++;
        }
        sdsfree(album);
        sdsfree(artist);
        sdsfree(key);
        mpd_response_finish(mpd_worker_state->mpd_state->conn);
        if (check_error_and_recover2(mpd_worker_state->mpd_state, NULL, NULL, 0, false) == false) {
            LOG_ERROR("Album cache update failed");
            return false;        
        }
        start = end;
        end = end + 1000;
    } while (i >= start);
    LOG_VERBOSE("Album cache updated successfully");
    //sdsfree(expression);
    return true;
}
