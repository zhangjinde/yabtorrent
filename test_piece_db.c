/*
 * =====================================================================================
 *
 *       Filename:  test_piece_db.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  05/15/11 17:40:21
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *        Company:  
 *
 * =====================================================================================
 */


#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "CuTest.h"

#include <stdint.h>

#include "bt.h"
#include "bt_local.h"

void TestBTpiecedb_new_is_empty(
    CuTest * tc
)
{
    void *db;

    db = bt_piecedb_new();

    CuAssertTrue(tc, NULL == bt_piecedb_get(db, 0));
}

void TestBTPieceDB_add_piece(
    CuTest * tc
)
{
    void *db;

    db = bt_piecedb_new();
    bt_piecedb_set_piece_length(db, 40);
    bt_piecedb_add(db, "00000000000000000000");
    CuAssertTrue(tc, NULL != bt_piecedb_get(db, 0));
}

#if 0
void TxestBTPieceDB_BuildRequestInvolvesPieceIDX(
    CuTest * tc
)
{
    void *db;

    bt_piece_t *pce;

    bt_block_t blk;

    int idx = 0;

    db = bt_piecedb_new();
    bt_piecedb_add(db, "00000000000000000000");
//    bt_piecedb_get(db, idx);
//    bt_piece_init(pce, "00000000000000000000", 40);
//    bt_piece_build_request(pce, &blk);

    CuAssertTrue(tc, idx == bt_piece_get_idx(pce));
}
#endif

void TestBTPieceDB_GetLength_returns_correct_length_of_db(
    CuTest * tc
)
{
    void *db;

    db = bt_piecedb_new();
    bt_piecedb_set_piece_length(db, 40);
    CuAssertTrue(tc, 0 == bt_piecedb_get_length(db));
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    CuAssertTrue(tc, 4 == bt_piecedb_get_length(db));
}

void TestBTPieceDB_dont_poll_piece_that_peer_doesnt_have(
    CuTest * tc
)
{
    void *db;

    bt_piece_t *pce;

    bt_block_t blk;

    bt_bitfield_t bf;

    bt_bitfield_init(&bf, 4);
    db = bt_piecedb_new();
    bt_piecedb_set_piece_length(db, 40);
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
//    bt_piecedb_get(db, idx);
    pce = bt_piecedb_poll_best_from_bitfield(db, &bf);
    CuAssertTrue(tc, NULL == pce);
//    CuAssertTrue(tc, idx == bt_piece_get_idx(pce));
}

void TestBTPieceDB_poll_best_from_bitfield(
    CuTest * tc
)
{
    void *db;

    bt_piece_t *pce;

    bt_block_t blk;

    bt_bitfield_t bf;

    bt_bitfield_init(&bf, 4);
    bt_bitfield_mark(&bf, 3);
    db = bt_piecedb_new();
    bt_piecedb_set_piece_length(db, 40);
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
//    bt_piecedb_get(db, idx);
    pce = bt_piecedb_poll_best_from_bitfield(db, &bf);
    CuAssertTrue(tc, NULL != pce);
    CuAssertTrue(tc, bt_piece_get_idx(pce) == 3);
}

/*----------------------------------------------------------------------------*/

void TestBTPieceDB_AddingPiece_LastPieceFitsTotalSize(
    CuTest * tc
)
{
    void *db;

    bt_piece_t *pce;

    db = bt_piecedb_new();
    bt_piecedb_set_piece_length(db, 50);
    bt_piecedb_set_tot_file_size(db, 180);
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    bt_piecedb_add(db, "00000000000000000000");
    CuAssertTrue(tc, bt_piece_get_size(bt_piecedb_get(db, 2)) == 50);
    CuAssertTrue(tc, bt_piece_get_size(bt_piecedb_get(db, 3)) == 30);
}