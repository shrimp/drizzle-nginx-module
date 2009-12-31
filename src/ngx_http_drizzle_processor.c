/* Copyright (C) chaoslawful */
/* Copyright (C) agentzh */

#define DDEBUG 1
#include "ddebug.h"

#include "ngx_http_drizzle_processor.h"
#include "ngx_http_drizzle_module.h"
#include "ngx_http_drizzle_util.h"
#include "ngx_http_drizzle_output.h"
#include "ngx_http_upstream_drizzle.h"

#include <libdrizzle/drizzle_client.h>

static ngx_int_t ngx_http_upstream_drizzle_connect(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc);

static ngx_int_t ngx_http_upstream_drizzle_send_query(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc);

static ngx_int_t ngx_http_upstream_drizzle_recv_cols(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc);

static ngx_int_t ngx_http_upstream_drizzle_recv_rows(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc);


ngx_int_t
ngx_http_drizzle_process_events(ngx_http_request_t *r)
{
    ngx_http_upstream_t                         *u;
    ngx_connection_t                            *c;
    ngx_http_upstream_drizzle_peer_data_t       *dp;
    drizzle_con_st                              *dc;
    ngx_int_t                                    rc;

    dd("drizzle process events");

    u = r->upstream;
    c = u->peer.connection;

    dp = u->peer.data;

    if ( ! ngx_http_upstream_drizzle_is_my_peer(&u->peer)) {
        dd("it is not my peer");

        if (dp == NULL) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                           "drizzle: process events: peer data empty");

            return NGX_ERROR;
        }

        /* XXX this is a hack just for working together with
         * Maxim Dounin's ngx_http_upstream_keepalive module */
        dp = ((ngx_http_upstream_faked_keepalive_peer_data_t *) dp)->data;
    }

    dc = dp->drizzle_con;

    /* libdrizzle uses standard poll() event constants
     * and depends on drizzle_con_wait() to set them.
     * we can directly call drizzle_con_wait() here to
     * set those drizzle internal event states, because
     * epoll() and other underlying event mechamism used
     * by the nginx core can play well enough with poll().
     * */

    drizzle_con_wait(dc->drizzle);

    switch (dp->state) {
    case state_db_connect:
        rc = ngx_http_upstream_drizzle_connect(r, c, dp, dc);
        break;

    case state_db_idle: /* from connection pool */
    case state_db_send_query:
        rc = ngx_http_upstream_drizzle_send_query(r, c, dp, dc);
        break;

    case state_db_recv_cols:
        rc = ngx_http_upstream_drizzle_recv_cols(r, c, dp, dc);
        break;

    case state_db_recv_rows:
        rc = ngx_http_upstream_drizzle_recv_rows(r, c, dp, dc);
        break;

    default:
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                       "drizzle: unknown state: %d", (int) dp->state);
        return NGX_ERROR;
    }

    if (rc == NGX_ERROR) {
        ngx_http_upstream_drizzle_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR);

        return NGX_ERROR;
    }

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_http_upstream_drizzle_finalize_request(r, u, rc);

        return NGX_ERROR;
    }

    return rc;
}


static ngx_int_t
ngx_http_upstream_drizzle_connect(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc)
{
    ngx_http_upstream_t         *u;
    drizzle_return_t             ret;

    dd("drizzle connect");

    u = r->upstream;

    ret = drizzle_con_connect(dc);

    if (ret == DRIZZLE_RETURN_IO_WAIT) {
        return NGX_AGAIN;
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    if (ret != DRIZZLE_RETURN_OK) {
       ngx_log_error(NGX_LOG_ERR, c->log, 0,
                       "drizzle: failed to connect: %d: %s in upstream \"%V\"",
                       (int) ret,
                       drizzle_error(dc->drizzle),
                       &u->peer.name);

       return NGX_ERROR;
    }

    /* ret == DRIZZLE_RETURN_OK */

    return ngx_http_upstream_drizzle_send_query(r, c, dp, dc);
}


static ngx_int_t
ngx_http_upstream_drizzle_send_query(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc)
{
    ngx_http_upstream_t         *u = r->upstream;
    drizzle_return_t             ret;
    ngx_int_t                    rc;

    dd("drizzle send query");

    (void) drizzle_query(dc, &dp->drizzle_res, (const char *) dp->query.data,
            dp->query.len, &ret);

    if (ret == DRIZZLE_RETURN_IO_WAIT) {

        if (dp->state != state_db_send_query) {
            dp->state = state_db_send_query;

            if (c->write->timer_set) {
                ngx_del_timer(c->write);
            }

            ngx_add_timer(c->write, u->conf->send_timeout);

        }

        return NGX_AGAIN;
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    if (ret != DRIZZLE_RETURN_OK) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                       "drizzle: failed to send query: %d: %s"
                       " in upstream \"%V\"",
                       (int) ret,
                       drizzle_error(dc->drizzle),
                       &u->peer.name);

        return NGX_ERROR;
    }

    /* ret == DRIZZLE_RETURN_OK */

    dd_drizzle_result(&dp->drizzle_res);

    rc = ngx_http_drizzle_output_result_header(r, &dp->drizzle_res);

    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    if (rc == NGX_DONE) {
        /* no data set following the header */
        return rc;
    }

    return ngx_http_upstream_drizzle_recv_cols(r, c, dp, dc);
}


static ngx_int_t
ngx_http_upstream_drizzle_recv_cols(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc)
{
    ngx_http_upstream_t             *u = r->upstream;
    drizzle_column_st               *col;
    ngx_int_t                        rc;
    drizzle_return_t                 ret;

    dd("drizzle recv cols");

    for (;;) {
        col = drizzle_column_read(&dp->drizzle_res, &dp->drizzle_col, &ret);

        if (ret == DRIZZLE_RETURN_IO_WAIT) {

            if (dp->state != state_db_recv_cols) {
                dp->state = state_db_recv_cols;

                if (c->read->timer_set) {
                    ngx_del_timer(c->read);
                }

                ngx_add_timer(c->read, dp->loc_conf->recv_cols_timeout);
            }

            return NGX_AGAIN;
        }

        if (ret != DRIZZLE_RETURN_OK) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                           "drizzle: failed to recv cols: %d: %s"
                           " in upstream \"%V\"",
                           (int) ret,
                           drizzle_error(dc->drizzle),
                           &u->peer.name);

            return NGX_ERROR;
        }

        /* ret == DRIZZLE_RETURN_OK */

        rc = ngx_http_drizzle_output_col(r, col);

        if (col) {
            drizzle_column_free(col);
        }

        if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }

        if (col == NULL) { /* after the last column */
            if (c->read->timer_set) {
                ngx_del_timer(c->read);
            }

            return ngx_http_upstream_drizzle_recv_rows(r, c, dp, dc);
        }

        dd_drizzle_column(col);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_drizzle_recv_rows(ngx_http_request_t *r,
        ngx_connection_t *c, ngx_http_upstream_drizzle_peer_data_t *dp,
        drizzle_con_st *dc)
{
    ngx_http_upstream_t             *u = r->upstream;
    ngx_int_t                        rc;
    drizzle_return_t                 ret;
    size_t                           offset;
    size_t                           len;
    size_t                           total;
    drizzle_field_t                  field;

    dd("drizzle recv rows");

    for (;;) {
        if (dp->drizzle_row == 0) {
            dp->drizzle_row = drizzle_row_read(&dp->drizzle_res, &ret);

            if (ret == DRIZZLE_RETURN_IO_WAIT) {
                dp->drizzle_row = 0;

                goto io_wait;
            }

            if (ret != DRIZZLE_RETURN_OK) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                               "drizzle: failed to read row: %d: %s"
                               " in upstream \"%V\"",
                               (int) ret,
                               drizzle_error(dc->drizzle),
                               &u->peer.name);

                return NGX_ERROR;
            }

            /* ret == DRIZZLE_RETURN_OK */

            rc = ngx_http_drizzle_output_row(r, dp->drizzle_row);

            if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
                drizzle_result_free(&dp->drizzle_res);

                return rc;
            }

            if (dp->drizzle_row == 0) {
                /* after last row */

                drizzle_result_free(&dp->drizzle_res);

                if (c->read->timer_set) {
                    ngx_del_timer(c->read);
                }

                return ngx_http_upstream_drizzle_done(r, u, dp);
            }

            dd("drizzle row: %" PRId64 "\n", dp->drizzle_row);
        }

        /* dp->drizzle_row != 0 */

        for (;;) {
            /* here we rely on libdrizzle to buffer each field
             * for us */
            field = drizzle_field_read(&dp->drizzle_res, &offset, &len,
                                      &total, &ret);

            if (ret == DRIZZLE_RETURN_IO_WAIT) {
                goto io_wait;
            }

            if (ret == DRIZZLE_RETURN_ROW_END) {
                /* reached the end of the current row */
                break;
            }

            if (ret != DRIZZLE_RETURN_OK) {
                drizzle_result_free(&dp->drizzle_res);

                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                               "drizzle: failed to read row field: %d: %s"
                               " in upstream \"%V\"",
                               (int) ret,
                               drizzle_error(dc->drizzle),
                               &u->peer.name);

                return NGX_ERROR;
            }

            /* ret == DRIZZLE_RETURN_OK */

            rc = ngx_http_drizzle_output_field(r, offset, len, total, field);

            if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
                drizzle_result_free(&dp->drizzle_res);

                return rc;
            }

            if (field) {
                dd("drizzle field: %.*s", len, field);
            }
        }

        dp->drizzle_row = 0;
    }

    return NGX_OK;

io_wait:

    if (dp->state != state_db_recv_rows) {
        dp->state = state_db_recv_rows;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }

        ngx_add_timer(c->read, dp->loc_conf->recv_rows_timeout);
    }

    return NGX_AGAIN;
}


ngx_int_t
ngx_http_upstream_drizzle_done(ngx_http_request_t *r,
        ngx_http_upstream_t *u, ngx_http_upstream_drizzle_peer_data_t *dp)
{
    /* to persuade Maxim Dounin's ngx_http_upstream_keepalive
     * module to cache the current connection */

    u->header_sent = 1;
    u->length = 0;
    r->headers_out.status = NGX_HTTP_OK;
    u->headers_in.status_n = NGX_HTTP_OK;

    /* reset the state machine */
    dp->state = state_db_idle;

    ngx_http_upstream_drizzle_finalize_request(r, u, NGX_OK);

    return NGX_DONE;
}

