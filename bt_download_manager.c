/**
 * Copyright (c) 2011, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. 
 *
 * @file
 * @brief Major class tasked with managing downloads
 *        bt_dm works similar to the mediator pattern
 * @author  Willem Thiart himself@willemthiart.com
 * @version 0.1
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* for uint32_t */
#include <stdint.h>

/* for varags */
#include <stdarg.h>

#include "bitfield.h"
#include "event_timer.h"
#include "config.h"
#include "linked_list_queue.h"
#include "sparse_counter.h"

#include "pwp_connection.h"
#include "pwp_handshaker.h"
#include "pwp_msghandler.h"

#include "bt.h"
#include "bt_local.h"
#include "bt_peermanager.h"
#include "bt_block_readwriter_i.h"
#include "bt_string.h"
#include "bt_piece_db.h"
#include "bt_piece.h"
#include "bt_blacklist.h"
#include "bt_download_manager_private.h"
#include "bt_choker_peer.h"
#include "bt_choker.h"
#include "bt_choker_leecher.h"
#include "bt_choker_seeder.h"
#include "bt_selector_random.h"
#include "bt_selector_rarestfirst.h"
#include "bt_selector_sequential.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <time.h>

typedef struct
{
    /* database for writing pieces */
    bt_piecedb_i ipdb;
    bt_piecedb_t* pdb;

    /* callbacks */
    bt_dm_cbs_t cb;

    /* callback context */
    void *cb_ctx;

    /* job management */
    void *job_lock;
    linked_list_queue_t *jobs;

    /* configuration */
    void* cfg;

    /* peer manager */
    void* pm;

    /* peer and piece blacklisting */
    void* blacklist;

    /*  leeching choker */
    void *lchoke;

    /* timer */
    void *ticker;

    /* for selecting pieces */
    bt_pieceselector_i ips;
    void* pselector;

    /* are we seeding? */
    int am_seeding;

    sparsecounter_t* pieces_completed;

} bt_dm_private_t;

void *bt_dm_get_piecedb(bt_dm_t* me_);

static void __FUNC_log(void *bto, void *src, const char *fmt, ...)
{
    bt_dm_private_t *me = bto;
    char buf[1024], *p;
    va_list args;

    if (!me->cb.log)
        return;

    p = buf;
    sprintf(p, "%s,", config_get(me->cfg,"my_peerid"));
    p += strlen(buf);

    va_start(args, fmt);
    vsprintf(p, fmt, args);
    me->cb.log(me->cb_ctx, NULL, buf);
}

static void __log(void *bto, void *src, const char *fmt, ...)
{
    bt_dm_private_t *me = bto;
    char buf[1024];
    va_list args;

    if (!me->cb.log)
        return;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    __FUNC_log(bto,src,buf);
}

static int __FUNC_peerconn_send_to_peer(void *bto,
                                        const void* pc_peer,
                                        const void *data,
                                        const int len);


void __FUNC_peer_periodic(void* cb_ctx, void* peer, void* udata)
{
    bt_peer_t* p = peer;

    if (pwp_conn_flag_is_set(p->pc, PC_FAILED_CONNECTION)) return;
    if (!pwp_conn_flag_is_set(p->pc, PC_HANDSHAKE_RECEIVED)) return;
    pwp_conn_periodic(p->pc);
}

/**
 * Peer stats visitor
 * Processes each peer connection retrieving stats */
void __FUNC_peer_stats_visitor(void* cb_ctx, void* peer, void* udata)
{
    bt_dm_stats_t *s = udata;
    bt_dm_peer_stats_t *ps; 
    bt_peer_t* p = peer;

    ps = &s->peers[s->npeers++];
    ps->choked = pwp_conn_im_choked(p->pc);
    ps->choking = pwp_conn_im_choking(p->pc);
    ps->connected = pwp_conn_flag_is_set(p->pc, PC_HANDSHAKE_RECEIVED);
    ps->failed_connection = pwp_conn_flag_is_set(p->pc, PC_FAILED_CONNECTION);
    ps->drate = pwp_conn_get_download_rate(p->pc);
    ps->urate = pwp_conn_get_upload_rate(p->pc);
}

/**
 * Take this PWP message and process it on the Peer Connection side
 * @return 1 on sucess; 0 otherwise
 **/
int bt_dm_dispatch_from_buffer(
        void *bto,
        void *peer_nethandle,
        const unsigned char* buf,
        unsigned int len)
{
    bt_dm_private_t *me = bto;
    bt_peer_t* peer;

    /* get the peer that this message is for via nethandle */
    if (!(peer = bt_peermanager_nethandle_to_peer(me->pm, peer_nethandle)))
    {
        return 0;
    }

    /* handle handshake */
    if (!pwp_conn_flag_is_set(peer->pc, PC_HANDSHAKE_RECEIVED))
    {
        switch (pwp_handshaker_dispatch_from_buffer(peer->mh, &buf, &len))
        {
        case 1:
            /* we're done with handshaking */
            pwp_handshaker_release(peer->mh);
            peer->mh = pwp_msghandler_new(peer->pc);
            pwp_conn_set_state(peer->pc, PC_HANDSHAKE_RECEIVED);
            __log(me, NULL, "send,bitfield");
            if (0 == pwp_send_bitfield(config_get_int(me->cfg,"npieces"),
                    me->pieces_completed, __FUNC_peerconn_send_to_peer,
                    me, peer))
            {
                bt_dm_remove_peer(me,peer);
            }
            break;
        default:
            return 0;
        }
    }

    /* handle regular PWP traffic */
    switch (pwp_msghandler_dispatch_from_buffer(peer->mh, buf, len))
    {
        case 1:
            /* successful */
            break;
        case 0:
            /* error, we need to disconnect */
            __log(bto,NULL,"disconnecting,%s", "bad msg detected by PWP handler");
            bt_dm_remove_peer(me,peer);
            break;
    }

    return 1;
}

void bt_dm_peer_connect_fail(void *bto, void* nethandle)
{
    bt_dm_private_t *me = bto;
    bt_peer_t *peer;

    if (!(peer = bt_peermanager_nethandle_to_peer(me->pm, nethandle)))
    {
        return;
    }

    pwp_conn_set_state(peer->pc, PC_FAILED_CONNECTION);
}

/**
 * @return 0 on error */
int bt_dm_peer_connect(void *bto, void* nethandle, char *ip, const int port)
{
    bt_dm_private_t *me = bto;
    bt_peer_t *peer;

    /* this is the first time we have come across this peer */
    if (!(peer = bt_peermanager_nethandle_to_peer(me->pm, nethandle)))
    {
        if (!(peer = bt_dm_add_peer((bt_dm_t*)me, "", 0,
                        ip, strlen(ip), port, nethandle)))
        {
            __log(bto,NULL,"cant add peer %s:%d %lx\n",
                ip, port, (unsigned long int)nethandle);
            return 0;
        }
    }

    pwp_handshaker_send_handshake(bto, peer,
        __FUNC_peerconn_send_to_peer,
        config_get(me->cfg,"infohash"),
        config_get(me->cfg,"my_peerid"));

//    pwp_conn_send_handshake(peer->pc);
//    __log(bto,NULL,"CONNECTED: peerid:%d ip:%s", netpeerid, ip);
    return 1;
}

static int __get_drate(const void *bto, const void *pc)
{
//    return pwp_conn_get_download_rate(pc);
    return 0;
}

static int __get_urate(const void *bto, const void *pc)
{
//    return pwp_conn_get_upload_rate(pc);
    return 0;
}

static int __get_is_interested(void *bto, void *pc)
{
    return pwp_conn_peer_is_interested(pc);
}

static void __choke_peer(void *bto, void *pc)
{
    pwp_conn_choke_peer(pc);
}

static void __unchoke_peer(void *bto, void *pc)
{
    pwp_conn_unchoke_peer(pc);
}

static bt_choker_peer_i iface_choker_peer = {
    .get_drate = __get_drate,
    .get_urate = __get_urate,
    .get_is_interested = __get_is_interested,
    .choke_peer = __choke_peer,
    .unchoke_peer = __unchoke_peer
};

static void __leecher_peer_reciprocation(void *bto)
{
    bt_dm_private_t *me = bto;

    bt_leeching_choker_decide_best_npeers(me->lchoke);
    eventtimer_push_event(me->ticker, 10, me, __leecher_peer_reciprocation);
}

static void __leecher_peer_optimistic_unchoke(void *bto)
{
    bt_dm_private_t *me = bto;

    bt_leeching_choker_optimistically_unchoke(me->lchoke);
    eventtimer_push_event(me->ticker, 30, me, __leecher_peer_optimistic_unchoke);
}

/**
 * Peer connections are given this as a callback whenever they want to send
 * information */
static int __FUNC_peerconn_send_to_peer(void *bto,
                                        const void* pc_peer,
                                        const void *data,
                                        const int len)
{
    const bt_peer_t * peer = pc_peer;
    bt_dm_private_t *me = bto;

    assert(peer);
    assert(me->cb.peer_send);
    return me->cb.peer_send(me, &me->cb_ctx, peer->nethandle, data, len);
}

typedef struct {
    bt_peer_t* peer;
    bt_block_t blk;
} bt_job_pollblock_t;

enum {
    BT_JOB_NONE,
    BT_JOB_POLLBLOCK
};

typedef struct {
    int type;
    union {
        bt_job_pollblock_t pollblock;
    };
} bt_job_t;

static void* __offer_job(void *me_, void* j_)
{
    bt_dm_private_t* me = me_;

    llqueue_offer(me->jobs, j_);
    return NULL;
}

static void* __poll_job(void *me_, void* __unused)
{
    bt_dm_private_t* me = me_;

    return llqueue_poll(me->jobs);
}

static void __dispatch_job(bt_dm_private_t* me, bt_job_t* j)
{
    assert(j);

    switch (j->type)
    {
    case BT_JOB_POLLBLOCK:
    {
        assert(me->ips.poll_piece);

        while (1)
        {
            int p_idx;
            bt_piece_t* pce;

            p_idx = me->ips.poll_piece(me->pselector, j->pollblock.peer);

            if (-1 == p_idx)
                break;

            pce = me->ipdb.get_piece(me->pdb, p_idx);

            if (pce && bt_piece_is_complete(pce))
            {
                me->ips.have_piece(me->pselector, p_idx);
                continue;
            }

            while (!bt_piece_is_fully_requested(pce))
            {
                bt_block_t blk;
                bt_piece_poll_block_request(pce, &blk);
                pwp_conn_offer_block(j->pollblock.peer->pc, &blk);
            }

            break;
        }
    }
        break;
    default:
        assert(0);
        break;
    }

    // TODO: replace with mempool
    free(j);
}

static int __FUNC_peerconn_pollblock(void *bto, void* peer)
{
    bt_dm_private_t *me = bto;
    bt_job_t* j;

    /* TODO: replace malloc() with memory pool/arena */
    j = malloc(sizeof(bt_job_t));
    j->type = BT_JOB_POLLBLOCK;
    j->pollblock.peer = peer;
    me->cb.call_exclusively(me, me->cb_ctx, &me->job_lock, j, __offer_job);
    return 0;
}

static void __FUNC_peerconn_send_have(void* cb_ctx, void* peer, void* udata)
{
    bt_peer_t* p = peer;

    if (!pwp_conn_flag_is_set(p->pc, PC_HANDSHAKE_RECEIVED)) return;
    pwp_conn_send_have(p->pc, bt_piece_get_idx(udata));
}

/**
 * Received a block from a peer
 * @param peer Peer received from
 * @param data Data to be pushed */
int __FUNC_peerconn_pushblock(void *bto, void* pr, bt_block_t *b, const void *data)
{
    bt_peer_t * peer = pr;
    bt_dm_private_t *me = bto;
    bt_piece_t *p;

    assert(me->ipdb.get_piece);

    p = me->ipdb.get_piece(me->pdb, b->piece_idx);

    assert(p);

    switch (bt_piece_write_block(p, NULL, b, data, peer))
    {
    case 2: /* complete piece */
    {
        __log(me, NULL, "client,piece downloaded,pieceidx=%d",
              bt_piece_get_idx(p));

        assert(me->ips.have_piece);
        me->ips.have_piece(me->pselector, b->piece_idx);
        sc_mark_complete(me->pieces_completed, b->piece_idx, 1);
        bt_peermanager_forall(me->pm,me,p,__FUNC_peerconn_send_have);
    }
        break;

    case 0: /* write error */
        printf("error writing block\n");
        break;

    case -1: /* invalid piece created */
    {
        /* only peer involved in piece download, therefore treat as
         * untrusted and blacklist */
        if (1 == bt_piece_num_peers(p))
        {
            bt_blacklist_add_peer(me->blacklist,p,peer);
        }
        else 
        {
            int i = 0;
            void* peer2;

            for (peer2 = bt_piece_get_peers(p,&i);
                 peer2;
                 peer2 = bt_piece_get_peers(p,&i))
            {
                bt_blacklist_add_peer_as_potentially_blacklisted(
                        me->blacklist,p,peer2);
            }

            bt_piece_drop_download_progress(p);
            me->ips.peer_giveback_piece(me->pselector, NULL, p->idx);
        }
    }
        break;
    default:
        break;
    }

#if 0
        /* dump everything to disk if the whole download is complete */
        if (bt_piecedb_all_pieces_are_complete(me))
        {
            me->am_seeding = 1;
//            bt_diskcache_disk_dump(me->dc);
        }
#endif

    return 1;
}

void __FUNC_peerconn_log(void *bto, void *src_peer, const char *buf, ...)
{
    bt_dm_private_t *me = bto;
    bt_peer_t *peer = src_peer;
    char buffer[1000];

    sprintf(buffer, "pwp,%s,%s", peer->peer_id, buf);
    __FUNC_log(bto,NULL,buffer);
    //me->cb.log(me->cb_ctx, NULL, buffer);
}

int __FUNC_peerconn_disconnect(void *bto,
        void* pr, char *reason)
{
    bt_dm_private_t *me = bto;
    bt_peer_t * peer = pr;

    __log(bto,NULL,"disconnecting,%s", reason);
    bt_dm_remove_peer(me,peer);
    return 1;
}

static void __FUNC_peerconn_peer_have_piece(
        void* bt,
        void* peer,
        int idx
        )
{
    bt_dm_private_t *me = bt;

    me->ips.peer_have_piece(me->pselector, peer, idx);
}

static void __FUNC_peerconn_giveback_block(
        void* bt,
        void* peer,
        bt_block_t* b
        )
{
    bt_dm_private_t *me = bt;
    void* p;

    if (b->len < 0)
        return;

    p = me->ipdb.get_piece(me->pdb, b->piece_idx);
    assert(p);

    bt_piece_giveback_block(p, b);
    me->ips.peer_giveback_piece(me->pselector, peer, b->piece_idx);
}

static void __FUNC_peerconn_write_block_to_stream(
        void* cb_ctx,
        bt_block_t * blk,
        unsigned char ** msg)
{
    bt_dm_private_t *me = cb_ctx;
    void* p;

    if (!(p = me->ipdb.get_piece(me->pdb, blk->piece_idx)))
    {
        __log(me,NULL,"ERROR,unable to obtain piece");
        return;
    }

    if (0 == bt_piece_write_block_to_stream(p, blk, msg))
    {
        __log(me,NULL,"ERROR,unable to write block to stream");
    }
}

/**
 * Add the peer.
 * Initiate connection with 
 * @return freshly created bt_peer
 */
void *bt_dm_add_peer(bt_dm_t* me_,
                              const char *peer_id,
                              const int peer_id_len,
                              const char *ip, const int ip_len, const int port,
                              void* nethandle)
{
    bt_dm_private_t *me = (void*)me_;
    bt_peer_t* p;

    /*  ensure we aren't adding ourselves as a peer */
    if (!strncmp(ip, config_get(me->cfg,"my_ip"), ip_len) &&
            port == atoi(config_get(me->cfg,"pwp_listen_port")))
    {
        return NULL;
    }

    /* remember the peer */
    if (!(p = bt_peermanager_add_peer(me->pm, peer_id, peer_id_len,
                    ip, ip_len, port)))
    {
#if 0 /* debug */
        fprintf(stderr, "cant add %s:%d, it's been added already\n", ip, port);
#endif
        return NULL;
    }
    else
    {
        if (me->pselector)
            me->ips.add_peer(me->pselector, p);
    }

    if (nethandle)
        p->nethandle = nethandle;

    /* create a peer connection for this peer */
    void* pc = p->pc = pwp_conn_new();
    pwp_conn_set_cbs(pc, &((pwp_conn_cbs_t) {
        .log = __FUNC_peerconn_log,
        .send = __FUNC_peerconn_send_to_peer,
        .pushblock = __FUNC_peerconn_pushblock,
        .pollblock = __FUNC_peerconn_pollblock,
        .disconnect = __FUNC_peerconn_disconnect,
        .peer_have_piece = __FUNC_peerconn_peer_have_piece,
        .peer_giveback_block = __FUNC_peerconn_giveback_block,
        .write_block_to_stream = __FUNC_peerconn_write_block_to_stream,
        .call_exclusively = me->cb.call_exclusively
        }), me);
    pwp_conn_set_progress(pc, me->pieces_completed);
    pwp_conn_set_piece_info(pc,
            config_get_int(me->cfg,"npieces"),
            config_get_int(me->cfg,"piece_length"));
    pwp_conn_set_peer(pc, p);

    /* the remote peer will have always send a handshake */
    if (NULL == me->cb.peer_connect)
    {
        //fprintf(stderr, "cant add, peer_connect function not available\n");
        return NULL;
    }

#if 1
    if (!nethandle)
    {
        if (0 == me->cb.peer_connect(me,
                    &me->cb_ctx,
                    &p->nethandle,
                    p->ip,
                    p->port,
                    bt_dm_dispatch_from_buffer,
                    bt_dm_peer_connect,
                    bt_dm_peer_connect_fail))
        {
            __log(me,NULL,"failed connection to peer");
            return 0;
        }
    }
#endif

    p->mh = pwp_handshaker_new(
            config_get(me->cfg,"infohash"),
            config_get(me->cfg,"my_peerid"));

    bt_leeching_choker_add_peer(me->lchoke, p->pc);

    return p;
}

/**
 * Remove the peer.
 * Disconnect the peer
 * @todo add disconnection functionality
 * @return 1 on sucess; otherwise 0
 */
int bt_dm_remove_peer(bt_dm_t* me_, void* pr)
{
    bt_dm_private_t* me = (void*)me_;
    bt_peer_t* peer = pr;

    if (0 == bt_peermanager_remove_peer(me->pm,peer))
    {
        __log(me_,NULL,"ERROR,couldn't remove peer");
        return 0;
    }

    me->ips.remove_peer(me->pselector, peer);

    return 1;
}

void bt_dm_periodic(bt_dm_t* me_, bt_dm_stats_t *stats)
{
    bt_dm_private_t *me = (void*)me_;
    int ii;

    /* TODO: pump out keep alive message */

    /*  shutdown if we are setup to not seed */
    if (1 == me->am_seeding && 1 == config_get_int(me->cfg,"shutdown_when_complete"))
    {
        goto cleanup;
    }

    /* process jobs */
    while (0 < llqueue_count(me->jobs))
    {
        void * j;

        j = me->cb.call_exclusively(me, me->cb_ctx, &me->job_lock, NULL, __poll_job);
        assert(j);
        __dispatch_job(me,j);
    }

    /* run each peer connection step */
    bt_peermanager_forall(me->pm,me,NULL,__FUNC_peer_periodic);

    /* TODO: dispatch eventtimer events */

cleanup:

    if (stats)
    {
        if (stats->npeers_size < bt_peermanager_count(me->pm))
        {
            stats->npeers_size = bt_peermanager_count(me->pm);
            stats->peers = realloc(stats->peers,
                    stats->npeers_size * sizeof(bt_dm_peer_stats_t));
        }
        stats->npeers = 0;
        bt_peermanager_forall(me->pm,me,stats,__FUNC_peer_stats_visitor);
    }

    return;
}

void* bt_dm_get_config(bt_dm_t* me_)
{
    bt_dm_private_t *me = (void*)me_;

    return me->cfg;
}

/**
 * Set callback functions
 */
void bt_dm_set_cbs(bt_dm_t* me_, bt_dm_cbs_t * func, void* cb_ctx)
{
    bt_dm_private_t *me = (void*)me_;

    memcpy(&me->cb, func, sizeof(bt_dm_cbs_t));
    me->cb_ctx = cb_ctx;
}

/**
 * @return number of peers this client is involved with
 */
int bt_dm_get_num_peers(bt_dm_t* me_)
{
    bt_dm_private_t *me = (void*)me_;

    return bt_peermanager_count(me->pm);
}

void *bt_dm_get_piecedb(bt_dm_t* me_)
{
    bt_dm_private_t *me = (void*)me_;

    return me->pdb;
}

/**
 * Set the current piece selector
 * This allows us to use dependency injection to de-couple the
 * implementation of the piece selector from bt_dm
 * @param ips Struct of function pointers for piece selector operation
 * @param piece_selector Selector instance. If NULL we call the constructor. */
void bt_dm_set_piece_selector(bt_dm_t* me_, bt_pieceselector_i* ips, void* piece_selector)
{
    bt_dm_private_t* me = (void*)me_;

    memcpy(&me->ips, ips, sizeof(bt_pieceselector_i));

    if (!piece_selector)
        me->pselector = me->ips.new(0);
    else
        me->pselector = piece_selector;

    bt_dm_check_pieces(me_);
}

/**
 * Initiliase the bittorrent client
 * bt_dm uses the mediator pattern to manage the bittorrent download
 * @return 1 on sucess; otherwise 0
 * \nosubgrouping
 */
void *bt_dm_new()
{
    bt_dm_private_t *me;

    me = calloc(1, sizeof(bt_dm_private_t));

    /* default configuration */
    me->cfg = config_new();
    config_set(me->cfg,"default", "0");
    config_set_if_not_set(me->cfg,"infohash", "00000000000000000000");
    config_set_if_not_set(me->cfg,"my_ip", "127.0.0.1");
    config_set_if_not_set(me->cfg,"pwp_listen_port", "6881");
    config_set_if_not_set(me->cfg,"max_peer_connections", "32");
    config_set_if_not_set(me->cfg,"max_active_peers", "32");
    config_set_if_not_set(me->cfg,"max_pending_requests", "10");
    /* How many pieces are there of this file
     * The size of a piece is determined by the publisher of the torrent.
     * A good recommendation is to use a piece size so that the metainfo file does
     * not exceed 70 kilobytes.  */
    config_set_if_not_set(me->cfg,"npieces", "0");
    config_set_if_not_set(me->cfg,"piece_length", "0");
    config_set_if_not_set(me->cfg,"download_path", ".");
    /* Set maximum amount of megabytes used by piece cache */
    config_set_if_not_set(me->cfg,"max_cache_mem_bytes", "1000000");
    /* If this is set, the client will shutdown when the download is completed. */
    config_set_if_not_set(me->cfg,"shutdown_when_complete", "0");

    /* need to be able to tell the time */
    me->ticker = eventtimer_new();

    /* peer manager */
    me->pm = bt_peermanager_new(me);
    bt_peermanager_set_config(me->pm, me->cfg);

    me->blacklist = bt_blacklist_new();

    /*  set leeching choker */
    me->lchoke = bt_leeching_choker_new(
            atoi(config_get(me->cfg,"max_active_peers")));
    bt_leeching_choker_set_choker_peer_iface(me->lchoke, me,
                                             &iface_choker_peer);

    /* start reciprocation timer */
    eventtimer_push_event(me->ticker, 10, me, __leecher_peer_reciprocation);

    /* start optimistic unchoker timer */
    eventtimer_push_event(me->ticker, 30, me, __leecher_peer_optimistic_unchoke);

    /* job management */
    me->jobs = llqueue_new();
    me->job_lock = NULL;

    /* we don't need to specify the amount of pieces we need */
    me->pieces_completed = sc_init(0);

    return me;
}

void bt_dm_set_piece_db(bt_dm_t* me_, bt_piecedb_i* ipdb, void* piece_db)
{
    bt_dm_private_t* me = (void*)me_;

    memcpy(&me->ipdb,ipdb,sizeof(bt_piecedb_i));
    me->pdb = piece_db;
}

void *bt_peer_get_nethandle(void* pr)
{
    bt_peer_t* peer = pr;

    return peer->nethandle;
}

/**
 * Scan over currently downloaded pieces */
void bt_dm_check_pieces(bt_dm_t* me_)
{
    bt_dm_private_t* me = (void*)me_;
    int i, end;

    for (i=0, end = config_get_int(me->cfg,"npieces"); i<end; i++)
    {
        bt_piece_t* p = me->ipdb.get_piece(me->pdb, i);

        if (p && bt_piece_is_complete(p))
        {
            me->ips.have_piece(me->pselector, i);
            sc_mark_complete(me->pieces_completed, i, 1);
        }
    }
}

/**
 * Release all memory used by the client
 * Close all peer connections
 */
int bt_dm_release(bt_dm_t* me_)
{
    //TODO add destructors
    return 1;
}
