/*
** 2004 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** $Id: btree.c,v 1.1 2009/11/30 11:50:58 erik Exp $
**
** This file implements a external (disk-based) database using BTrees.
** See the header comment on "btreeInt.h" for additional information.
** Including a description of file format and an overview of operation.
*/
#include "btreeInt.h"

/*
** The header string that appears at the beginning of every
** SQLite database.
*/
static const char zMagicHeader[] = SQLITE_FILE_HEADER;

/*
** Set this global variable to 1 to enable tracing using the TRACE
** macro.
*/
#if 0
int sqlite3BtreeTrace=0;  /* True to enable tracing */
# define TRACE(X)  if(sqlite3BtreeTrace){printf X;fflush(stdout);}
#else
# define TRACE(X)
#endif



#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** A list of BtShared objects that are eligible for participation
** in shared cache.  This variable has file scope during normal builds,
** but the test harness needs to access it so we make it global for 
** test builds.
*/
#ifdef SQLITE_TEST
BtShared *SQLITE_WSD sqlite3SharedCacheList = 0;
#else
static BtShared *SQLITE_WSD sqlite3SharedCacheList = 0;
#endif
#endif /* SQLITE_OMIT_SHARED_CACHE */

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Enable or disable the shared pager and schema features.
**
** This routine has no effect on existing database connections.
** The shared cache setting effects only future calls to
** sqlite3_open(), sqlite3_open16(), or sqlite3_open_v2().
*/
int sqlite3_enable_shared_cache(int enable){
  sqlite3GlobalConfig.sharedCacheEnabled = enable;
  return SQLITE_OK;
}
#endif


/*
** Forward declaration
*/
static int checkReadLocks(Btree*, Pgno, BtCursor*, i64);


#ifdef SQLITE_OMIT_SHARED_CACHE
  /*
  ** The functions queryTableLock(), lockTable() and unlockAllTables()
  ** manipulate entries in the BtShared.pLock linked list used to store
  ** shared-cache table level locks. If the library is compiled with the
  ** shared-cache feature disabled, then there is only ever one user
  ** of each BtShared structure and so this locking is not necessary. 
  ** So define the lock related functions as no-ops.
  */
  #define queryTableLock(a,b,c) SQLITE_OK
  #define lockTable(a,b,c) SQLITE_OK
  #define unlockAllTables(a)
#endif

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Query to see if btree handle p may obtain a lock of type eLock 
** (READ_LOCK or WRITE_LOCK) on the table with root-page iTab. Return
** SQLITE_OK if the lock may be obtained (by calling lockTable()), or
** SQLITE_LOCKED if not.
*/
static int queryTableLock(Btree *p, Pgno iTab, u8 eLock){
  BtShared *pBt = p->pBt;
  BtLock *pIter;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( eLock==READ_LOCK || eLock==WRITE_LOCK );
  assert( p->db!=0 );
  
  /* This is a no-op if the shared-cache is not enabled */
  if( !p->sharable ){
    return SQLITE_OK;
  }

  /* If some other connection is holding an exclusive lock, the
  ** requested lock may not be obtained.
  */
  if( pBt->pExclusive && pBt->pExclusive!=p ){
    return SQLITE_LOCKED;
  }

  /* This (along with lockTable()) is where the ReadUncommitted flag is
  ** dealt with. If the caller is querying for a read-lock and the flag is
  ** set, it is unconditionally granted - even if there are write-locks
  ** on the table. If a write-lock is requested, the ReadUncommitted flag
  ** is not considered.
  **
  ** In function lockTable(), if a read-lock is demanded and the 
  ** ReadUncommitted flag is set, no entry is added to the locks list 
  ** (BtShared.pLock).
  **
  ** To summarize: If the ReadUncommitted flag is set, then read cursors do
  ** not create or respect table locks. The locking procedure for a 
  ** write-cursor does not change.
  */
  if( 
    0==(p->db->flags&SQLITE_ReadUncommitted) || 
    eLock==WRITE_LOCK ||
    iTab==MASTER_ROOT
  ){
    for(pIter=pBt->pLock; pIter; pIter=pIter->pNext){
      if( pIter->pBtree!=p && pIter->iTable==iTab && 
          (pIter->eLock!=eLock || eLock!=READ_LOCK) ){
        return SQLITE_LOCKED;
      }
    }
  }
  return SQLITE_OK;
}
#endif /* !SQLITE_OMIT_SHARED_CACHE */

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Add a lock on the table with root-page iTable to the shared-btree used
** by Btree handle p. Parameter eLock must be either READ_LOCK or 
** WRITE_LOCK.
**
** SQLITE_OK is returned if the lock is added successfully. SQLITE_BUSY and
** SQLITE_NOMEM may also be returned.
*/
static int lockTable(Btree *p, Pgno iTable, u8 eLock){
  BtShared *pBt = p->pBt;
  BtLock *pLock = 0;
  BtLock *pIter;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( eLock==READ_LOCK || eLock==WRITE_LOCK );
  assert( p->db!=0 );

  /* This is a no-op if the shared-cache is not enabled */
  if( !p->sharable ){
    return SQLITE_OK;
  }

  assert( SQLITE_OK==queryTableLock(p, iTable, eLock) );

  /* If the read-uncommitted flag is set and a read-lock is requested,
  ** return early without adding an entry to the BtShared.pLock list. See
  ** comment in function queryTableLock() for more info on handling 
  ** the ReadUncommitted flag.
  */
  if( 
    (p->db->flags&SQLITE_ReadUncommitted) && 
    (eLock==READ_LOCK) &&
    iTable!=MASTER_ROOT
  ){
    return SQLITE_OK;
  }

  /* First search the list for an existing lock on this table. */
  for(pIter=pBt->pLock; pIter; pIter=pIter->pNext){
    if( pIter->iTable==iTable && pIter->pBtree==p ){
      pLock = pIter;
      break;
    }
  }

  /* If the above search did not find a BtLock struct associating Btree p
  ** with table iTable, allocate one and link it into the list.
  */
  if( !pLock ){
    pLock = (BtLock *)sqlite3MallocZero(sizeof(BtLock));
    if( !pLock ){
      return SQLITE_NOMEM;
    }
    pLock->iTable = iTable;
    pLock->pBtree = p;
    pLock->pNext = pBt->pLock;
    pBt->pLock = pLock;
  }

  /* Set the BtLock.eLock variable to the maximum of the current lock
  ** and the requested lock. This means if a write-lock was already held
  ** and a read-lock requested, we don't incorrectly downgrade the lock.
  */
  assert( WRITE_LOCK>READ_LOCK );
  if( eLock>pLock->eLock ){
    pLock->eLock = eLock;
  }

  return SQLITE_OK;
}
#endif /* !SQLITE_OMIT_SHARED_CACHE */

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Release all the table locks (locks obtained via calls to the lockTable()
** procedure) held by Btree handle p.
*/
static void unlockAllTables(Btree *p){
  BtShared *pBt = p->pBt;
  BtLock **ppIter = &pBt->pLock;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( p->sharable || 0==*ppIter );

  while( *ppIter ){
    BtLock *pLock = *ppIter;
    assert( pBt->pExclusive==0 || pBt->pExclusive==pLock->pBtree );
    if( pLock->pBtree==p ){
      *ppIter = pLock->pNext;
      sqlite3_free(pLock);
    }else{
      ppIter = &pLock->pNext;
    }
  }

  if( pBt->pExclusive==p ){
    pBt->pExclusive = 0;
  }
}
#endif /* SQLITE_OMIT_SHARED_CACHE */

static void releasePage(MemPage *pPage);  /* Forward reference */

/*
** Verify that the cursor holds a mutex on the BtShared
*/
#ifndef NDEBUG
static int cursorHoldsMutex(BtCursor *p){
  return sqlite3_mutex_held(p->pBt->mutex);
}
#endif


#ifndef SQLITE_OMIT_INCRBLOB
/*
** Invalidate the overflow page-list cache for cursor pCur, if any.
*/
static void invalidateOverflowCache(BtCursor *pCur){
  assert( cursorHoldsMutex(pCur) );
  sqlite3_free(pCur->aOverflow);
  pCur->aOverflow = 0;
}

/*
** Invalidate the overflow page-list cache for all cursors opened
** on the shared btree structure pBt.
*/
static void invalidateAllOverflowCache(BtShared *pBt){
  BtCursor *p;
  assert( sqlite3_mutex_held(pBt->mutex) );
  for(p=pBt->pCursor; p; p=p->pNext){
    invalidateOverflowCache(p);
  }
}
#else
  #define invalidateOverflowCache(x)
  #define invalidateAllOverflowCache(x)
#endif

/*
** Set bit pgno of the BtShared.pHasContent bitvec. This is called 
** when a page that previously contained data becomes a free-list leaf 
** page.
**
** The BtShared.pHasContent bitvec exists to work around an obscure
** bug caused by the interaction of two useful IO optimizations surrounding
** free-list leaf pages:
**
**   1) When all data is deleted from a page and the page becomes
**      a free-list leaf page, the page is not written to the database
**      (as free-list leaf pages contain no meaningful data). Sometimes
**      such a page is not even journalled (as it will not be modified,
**      why bother journalling it?).
**
**   2) When a free-list leaf page is reused, its content is not read
**      from the database or written to the journal file (why should it
**      be, if it is not at all meaningful?).
**
** By themselves, these optimizations work fine and provide a handy
** performance boost to bulk delete or insert operations. However, if
** a page is moved to the free-list and then reused within the same
** transaction, a problem comes up. If the page is not journalled when
** it is moved to the free-list and it is also not journalled when it
** is extracted from the free-list and reused, then the original data
** may be lost. In the event of a rollback, it may not be possible
** to restore the database to its original configuration.
**
** The solution is the BtShared.pHasContent bitvec. Whenever a page is 
** moved to become a free-list leaf page, the corresponding bit is
** set in the bitvec. Whenever a leaf page is extracted from the free-list,
** optimization 2 above is ommitted if the corresponding bit is already
** set in BtShared.pHasContent. The contents of the bitvec are cleared
** at the end of every transaction.
*/
static int btreeSetHasContent(BtShared *pBt, Pgno pgno){
  int rc = SQLITE_OK;
  if( !pBt->pHasContent ){
    int nPage;
    rc = sqlite3PagerPagecount(pBt->pPager, &nPage);
    if( rc==SQLITE_OK ){
      pBt->pHasContent = sqlite3BitvecCreate((u32)nPage);
      if( !pBt->pHasContent ){
        rc = SQLITE_NOMEM;
      }
    }
  }
  if( rc==SQLITE_OK && pgno<=sqlite3BitvecSize(pBt->pHasContent) ){
    rc = sqlite3BitvecSet(pBt->pHasContent, pgno);
  }
  return rc;
}

/*
** Query the BtShared.pHasContent vector.
**
** This function is called when a free-list leaf page is removed from the
** free-list for reuse. It returns false if it is safe to retrieve the
** page from the pager layer with the 'no-content' flag set. True otherwise.
*/
static int btreeGetHasContent(BtShared *pBt, Pgno pgno){
  Bitvec *p = pBt->pHasContent;
  return (p && (pgno>sqlite3BitvecSize(p) || sqlite3BitvecTest(p, pgno)));
}

/*
** Clear (destroy) the BtShared.pHasContent bitvec. This should be
** invoked at the conclusion of each write-transaction.
*/
static void btreeClearHasContent(BtShared *pBt){
  sqlite3BitvecDestroy(pBt->pHasContent);
  pBt->pHasContent = 0;
}

/*
** Save the current cursor position in the variables BtCursor.nKey 
** and BtCursor.pKey. The cursor's state is set to CURSOR_REQUIRESEEK.
*/
static int saveCursorPosition(BtCursor *pCur){
  int rc;

  assert( CURSOR_VALID==pCur->eState );
  assert( 0==pCur->pKey );
  assert( cursorHoldsMutex(pCur) );

  rc = sqlite3BtreeKeySize(pCur, &pCur->nKey);

  /* If this is an intKey table, then the above call to BtreeKeySize()
  ** stores the integer key in pCur->nKey. In this case this value is
  ** all that is required. Otherwise, if pCur is not open on an intKey
  ** table, then malloc space for and store the pCur->nKey bytes of key 
  ** data.
  */
  if( rc==SQLITE_OK && 0==pCur->apPage[0]->intKey){
    void *pKey = sqlite3Malloc( (int)pCur->nKey );
    if( pKey ){
      rc = sqlite3BtreeKey(pCur, 0, (int)pCur->nKey, pKey);
      if( rc==SQLITE_OK ){
        pCur->pKey = pKey;
      }else{
        sqlite3_free(pKey);
      }
    }else{
      rc = SQLITE_NOMEM;
    }
  }
  assert( !pCur->apPage[0]->intKey || !pCur->pKey );

  if( rc==SQLITE_OK ){
    int i;
    for(i=0; i<=pCur->iPage; i++){
      releasePage(pCur->apPage[i]);
      pCur->apPage[i] = 0;
    }
    pCur->iPage = -1;
    pCur->eState = CURSOR_REQUIRESEEK;
  }

  invalidateOverflowCache(pCur);
  return rc;
}

/*
** Save the positions of all cursors except pExcept open on the table 
** with root-page iRoot. Usually, this is called just before cursor
** pExcept is used to modify the table (BtreeDelete() or BtreeInsert()).
*/
static int saveAllCursors(BtShared *pBt, Pgno iRoot, BtCursor *pExcept){
  BtCursor *p;
  assert( sqlite3_mutex_held(pBt->mutex) );
  assert( pExcept==0 || pExcept->pBt==pBt );
  for(p=pBt->pCursor; p; p=p->pNext){
    if( p!=pExcept && (0==iRoot || p->pgnoRoot==iRoot) && 
        p->eState==CURSOR_VALID ){
      int rc = saveCursorPosition(p);
      if( SQLITE_OK!=rc ){
        return rc;
      }
    }
  }
  return SQLITE_OK;
}

/*
** Clear the current cursor position.
*/
void sqlite3BtreeClearCursor(BtCursor *pCur){
  assert( cursorHoldsMutex(pCur) );
  sqlite3_free(pCur->pKey);
  pCur->pKey = 0;
  pCur->eState = CURSOR_INVALID;
}

/*
** Restore the cursor to the position it was in (or as close to as possible)
** when saveCursorPosition() was called. Note that this call deletes the 
** saved position info stored by saveCursorPosition(), so there can be
** at most one effective restoreCursorPosition() call after each 
** saveCursorPosition().
*/
int sqlite3BtreeRestoreCursorPosition(BtCursor *pCur){
  int rc;
  assert( cursorHoldsMutex(pCur) );
  assert( pCur->eState>=CURSOR_REQUIRESEEK );
  if( pCur->eState==CURSOR_FAULT ){
    return pCur->skip;
  }
  pCur->eState = CURSOR_INVALID;
  rc = sqlite3BtreeMoveto(pCur, pCur->pKey, pCur->nKey, 0, &pCur->skip);
  if( rc==SQLITE_OK ){
    sqlite3_free(pCur->pKey);
    pCur->pKey = 0;
    assert( pCur->eState==CURSOR_VALID || pCur->eState==CURSOR_INVALID );
  }
  return rc;
}

#define restoreCursorPosition(p) \
  (p->eState>=CURSOR_REQUIRESEEK ? \
         sqlite3BtreeRestoreCursorPosition(p) : \
         SQLITE_OK)

/*
** Determine whether or not a cursor has moved from the position it
** was last placed at.  Cursors can move when the row they are pointing
** at is deleted out from under them.
**
** This routine returns an error code if something goes wrong.  The
** integer *pHasMoved is set to one if the cursor has moved and 0 if not.
*/
int sqlite3BtreeCursorHasMoved(BtCursor *pCur, int *pHasMoved){
  int rc;

  rc = restoreCursorPosition(pCur);
  if( rc ){
    *pHasMoved = 1;
    return rc;
  }
  if( pCur->eState!=CURSOR_VALID || pCur->skip!=0 ){
    *pHasMoved = 1;
  }else{
    *pHasMoved = 0;
  }
  return SQLITE_OK;
}

#ifndef SQLITE_OMIT_AUTOVACUUM
/*
** Given a page number of a regular database page, return the page
** number for the pointer-map page that contains the entry for the
** input page number.
*/
static Pgno ptrmapPageno(BtShared *pBt, Pgno pgno){
  int nPagesPerMapPage;
  Pgno iPtrMap, ret;
  assert( sqlite3_mutex_held(pBt->mutex) );
  nPagesPerMapPage = (pBt->usableSize/5)+1;
  iPtrMap = (pgno-2)/nPagesPerMapPage;
  ret = (iPtrMap*nPagesPerMapPage) + 2; 
  if( ret==PENDING_BYTE_PAGE(pBt) ){
    ret++;
  }
  return ret;
}

/*
** Write an entry into the pointer map.
**
** This routine updates the pointer map entry for page number 'key'
** so that it maps to type 'eType' and parent page number 'pgno'.
** An error code is returned if something goes wrong, otherwise SQLITE_OK.
*/
static int ptrmapPut(BtShared *pBt, Pgno key, u8 eType, Pgno parent){
  DbPage *pDbPage;  /* The pointer map page */
  u8 *pPtrmap;      /* The pointer map data */
  Pgno iPtrmap;     /* The pointer map page number */
  int offset;       /* Offset in pointer map page */
  int rc;

  assert( sqlite3_mutex_held(pBt->mutex) );
  /* The master-journal page number must never be used as a pointer map page */
  assert( 0==PTRMAP_ISPAGE(pBt, PENDING_BYTE_PAGE(pBt)) );

  assert( pBt->autoVacuum );
  if( key==0 ){
    return SQLITE_CORRUPT_BKPT;
  }
  iPtrmap = PTRMAP_PAGENO(pBt, key);
  rc = sqlite3PagerGet(pBt->pPager, iPtrmap, &pDbPage);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  offset = PTRMAP_PTROFFSET(iPtrmap, key);
  pPtrmap = (u8 *)sqlite3PagerGetData(pDbPage);

  if( eType!=pPtrmap[offset] || get4byte(&pPtrmap[offset+1])!=parent ){
    TRACE(("PTRMAP_UPDATE: %d->(%d,%d)\n", key, eType, parent));
    rc = sqlite3PagerWrite(pDbPage);
    if( rc==SQLITE_OK ){
      pPtrmap[offset] = eType;
      put4byte(&pPtrmap[offset+1], parent);
    }
  }

  sqlite3PagerUnref(pDbPage);
  return rc;
}

/*
** Read an entry from the pointer map.
**
** This routine retrieves the pointer map entry for page 'key', writing
** the type and parent page number to *pEType and *pPgno respectively.
** An error code is returned if something goes wrong, otherwise SQLITE_OK.
*/
static int ptrmapGet(BtShared *pBt, Pgno key, u8 *pEType, Pgno *pPgno){
  DbPage *pDbPage;   /* The pointer map page */
  int iPtrmap;       /* Pointer map page index */
  u8 *pPtrmap;       /* Pointer map page data */
  int offset;        /* Offset of entry in pointer map */
  int rc;

  assert( sqlite3_mutex_held(pBt->mutex) );

  iPtrmap = PTRMAP_PAGENO(pBt, key);
  rc = sqlite3PagerGet(pBt->pPager, iPtrmap, &pDbPage);
  if( rc!=0 ){
    return rc;
  }
  pPtrmap = (u8 *)sqlite3PagerGetData(pDbPage);

  offset = PTRMAP_PTROFFSET(iPtrmap, key);
  assert( pEType!=0 );
  *pEType = pPtrmap[offset];
  if( pPgno ) *pPgno = get4byte(&pPtrmap[offset+1]);

  sqlite3PagerUnref(pDbPage);
  if( *pEType<1 || *pEType>5 ) return SQLITE_CORRUPT_BKPT;
  return SQLITE_OK;
}

#else /* if defined SQLITE_OMIT_AUTOVACUUM */
  #define ptrmapPut(w,x,y,z) SQLITE_OK
  #define ptrmapGet(w,x,y,z) SQLITE_OK
  #define ptrmapPutOvfl(y,z) SQLITE_OK
#endif

/*
** Given a btree page and a cell index (0 means the first cell on
** the page, 1 means the second cell, and so forth) return a pointer
** to the cell content.
**
** This routine works only for pages that do not contain overflow cells.
*/
#define findCell(P,I) \
  ((P)->aData + ((P)->maskPage & get2byte(&(P)->aData[(P)->cellOffset+2*(I)])))

/*
** This a more complex version of findCell() that works for
** pages that do contain overflow cells.  See insert
*/
static u8 *findOverflowCell(MemPage *pPage, int iCell){
  int i;
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  for(i=pPage->nOverflow-1; i>=0; i--){
    int k;
    struct _OvflCell *pOvfl;
    pOvfl = &pPage->aOvfl[i];
    k = pOvfl->idx;
    if( k<=iCell ){
      if( k==iCell ){
        return pOvfl->pCell;
      }
      iCell--;
    }
  }
  return findCell(pPage, iCell);
}

/*
** Parse a cell content block and fill in the CellInfo structure.  There
** are two versions of this function.  sqlite3BtreeParseCell() takes a 
** cell index as the second argument and sqlite3BtreeParseCellPtr() 
** takes a pointer to the body of the cell as its second argument.
**
** Within this file, the parseCell() macro can be called instead of
** sqlite3BtreeParseCellPtr(). Using some compilers, this will be faster.
*/
void sqlite3BtreeParseCellPtr(
  MemPage *pPage,         /* Page containing the cell */
  u8 *pCell,              /* Pointer to the cell text. */
  CellInfo *pInfo         /* Fill in this structure */
){
  u16 n;                  /* Number bytes in cell content header */
  u32 nPayload;           /* Number of bytes of cell payload */

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );

  pInfo->pCell = pCell;
  assert( pPage->leaf==0 || pPage->leaf==1 );
  n = pPage->childPtrSize;
  assert( n==4-4*pPage->leaf );
  if( pPage->intKey ){
    if( pPage->hasData ){
      n += getVarint32(&pCell[n], nPayload);
    }else{
      nPayload = 0;
    }
    n += getVarint(&pCell[n], (u64*)&pInfo->nKey);
    pInfo->nData = nPayload;
  }else{
    pInfo->nData = 0;
    n += getVarint32(&pCell[n], nPayload);
    pInfo->nKey = nPayload;
  }
  pInfo->nPayload = nPayload;
  pInfo->nHeader = n;
  if( likely(nPayload<=pPage->maxLocal) ){
    /* This is the (easy) common case where the entire payload fits
    ** on the local page.  No overflow is required.
    */
    int nSize;          /* Total size of cell content in bytes */
    nSize = nPayload + n;
    pInfo->nLocal = (u16)nPayload;
    pInfo->iOverflow = 0;
    if( (nSize & ~3)==0 ){
      nSize = 4;        /* Minimum cell size is 4 */
    }
    pInfo->nSize = (u16)nSize;
  }else{
    /* If the payload will not fit completely on the local page, we have
    ** to decide how much to store locally and how much to spill onto
    ** overflow pages.  The strategy is to minimize the amount of unused
    ** space on overflow pages while keeping the amount of local storage
    ** in between minLocal and maxLocal.
    **
    ** Warning:  changing the way overflow payload is distributed in any
    ** way will result in an incompatible file format.
    */
    int minLocal;  /* Minimum amount of payload held locally */
    int maxLocal;  /* Maximum amount of payload held locally */
    int surplus;   /* Overflow payload available for local storage */

    minLocal = pPage->minLocal;
    maxLocal = pPage->maxLocal;
    surplus = minLocal + (nPayload - minLocal)%(pPage->pBt->usableSize - 4);
    if( surplus <= maxLocal ){
      pInfo->nLocal = (u16)surplus;
    }else{
      pInfo->nLocal = (u16)minLocal;
    }
    pInfo->iOverflow = (u16)(pInfo->nLocal + n);
    pInfo->nSize = pInfo->iOverflow + 4;
  }
}
#define parseCell(pPage, iCell, pInfo) \
  sqlite3BtreeParseCellPtr((pPage), findCell((pPage), (iCell)), (pInfo))
void sqlite3BtreeParseCell(
  MemPage *pPage,         /* Page containing the cell */
  int iCell,              /* The cell index.  First cell is 0 */
  CellInfo *pInfo         /* Fill in this structure */
){
  parseCell(pPage, iCell, pInfo);
}

/*
** Compute the total number of bytes that a Cell needs in the cell
** data area of the btree-page.  The return number includes the cell
** data header and the local payload, but not any overflow page or
** the space used by the cell pointer.
*/
#ifndef NDEBUG
static u16 cellSize(MemPage *pPage, int iCell){
  CellInfo info;
  sqlite3BtreeParseCell(pPage, iCell, &info);
  return info.nSize;
}
#endif
static u16 cellSizePtr(MemPage *pPage, u8 *pCell){
  CellInfo info;
  sqlite3BtreeParseCellPtr(pPage, pCell, &info);
  return info.nSize;
}

#ifndef SQLITE_OMIT_AUTOVACUUM
/*
** If the cell pCell, part of page pPage contains a pointer
** to an overflow page, insert an entry into the pointer-map
** for the overflow page.
*/
static int ptrmapPutOvflPtr(MemPage *pPage, u8 *pCell){
  CellInfo info;
  assert( pCell!=0 );
  sqlite3BtreeParseCellPtr(pPage, pCell, &info);
  assert( (info.nData+(pPage->intKey?0:info.nKey))==info.nPayload );
  if( (info.nData+(pPage->intKey?0:info.nKey))>info.nLocal ){
    Pgno ovfl = get4byte(&pCell[info.iOverflow]);
    return ptrmapPut(pPage->pBt, ovfl, PTRMAP_OVERFLOW1, pPage->pgno);
  }
  return SQLITE_OK;
}
/*
** If the cell with index iCell on page pPage contains a pointer
** to an overflow page, insert an entry into the pointer-map
** for the overflow page.
*/
static int ptrmapPutOvfl(MemPage *pPage, int iCell){
  u8 *pCell;
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  pCell = findOverflowCell(pPage, iCell);
  return ptrmapPutOvflPtr(pPage, pCell);
}
#endif


/*
** Defragment the page given.  All Cells are moved to the
** end of the page and all free space is collected into one
** big FreeBlk that occurs in between the header and cell
** pointer array and the cell content area.
*/
static int defragmentPage(MemPage *pPage){
  int i;                     /* Loop counter */
  int pc;                    /* Address of a i-th cell */
  int addr;                  /* Offset of first byte after cell pointer array */
  int hdr;                   /* Offset to the page header */
  int size;                  /* Size of a cell */
  int usableSize;            /* Number of usable bytes on a page */
  int cellOffset;            /* Offset to the cell pointer array */
  int cbrk;                  /* Offset to the cell content area */
  int nCell;                 /* Number of cells on the page */
  unsigned char *data;       /* The page data */
  unsigned char *temp;       /* Temp area for cell content */

  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  assert( pPage->pBt!=0 );
  assert( pPage->pBt->usableSize <= SQLITE_MAX_PAGE_SIZE );
  assert( pPage->nOverflow==0 );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  temp = sqlite3PagerTempSpace(pPage->pBt->pPager);
  data = pPage->aData;
  hdr = pPage->hdrOffset;
  cellOffset = pPage->cellOffset;
  nCell = pPage->nCell;
  assert( nCell==get2byte(&data[hdr+3]) );
  usableSize = pPage->pBt->usableSize;
  cbrk = get2byte(&data[hdr+5]);
  memcpy(&temp[cbrk], &data[cbrk], usableSize - cbrk);
  cbrk = usableSize;
  for(i=0; i<nCell; i++){
    u8 *pAddr;     /* The i-th cell pointer */
    pAddr = &data[cellOffset + i*2];
    pc = get2byte(pAddr);
    if( pc>=usableSize ){
      return SQLITE_CORRUPT_BKPT;
    }
    size = cellSizePtr(pPage, &temp[pc]);
    cbrk -= size;
    if( cbrk<cellOffset+2*nCell || pc+size>usableSize ){
      return SQLITE_CORRUPT_BKPT;
    }
    assert( cbrk+size<=usableSize && cbrk>=0 );
    memcpy(&data[cbrk], &temp[pc], size);
    put2byte(pAddr, cbrk);
  }
  assert( cbrk>=cellOffset+2*nCell );
  put2byte(&data[hdr+5], cbrk);
  data[hdr+1] = 0;
  data[hdr+2] = 0;
  data[hdr+7] = 0;
  addr = cellOffset+2*nCell;
  memset(&data[addr], 0, cbrk-addr);
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  if( cbrk-addr!=pPage->nFree ){
    return SQLITE_CORRUPT_BKPT;
  }
  return SQLITE_OK;
}

/*
** Allocate nByte bytes of space on a page.
**
** Return the index into pPage->aData[] of the first byte of
** the new allocation.  The caller guarantees that there is enough
** space.  This routine will never fail.
**
** If the page contains nBytes of free space but does not contain
** nBytes of contiguous free space, then this routine automatically
** calls defragementPage() to consolidate all free space before 
** allocating the new chunk.
*/
static int allocateSpace(MemPage *pPage, int nByte){
  int addr, pc, hdr;
  int size;
  int nFrag;
  int top;
  int nCell;
  int cellOffset;
  unsigned char *data;
  
  data = pPage->aData;
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  assert( pPage->pBt );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  assert( nByte>=0 );  /* Minimum cell size is 4 */
  assert( pPage->nFree>=nByte );
  assert( pPage->nOverflow==0 );
  pPage->nFree -= (u16)nByte;
  hdr = pPage->hdrOffset;

  nFrag = data[hdr+7];
  if( nFrag<60 ){
    /* Search the freelist looking for a slot big enough to satisfy the
    ** space request. */
    addr = hdr+1;
    while( (pc = get2byte(&data[addr]))>0 ){
      size = get2byte(&data[pc+2]);
      if( size>=nByte ){
        int x = size - nByte;
        if( size<nByte+4 ){
          memcpy(&data[addr], &data[pc], 2);
          data[hdr+7] = (u8)(nFrag + x);
          return pc;
        }else{
          put2byte(&data[pc+2], x);
          return pc + x;
        }
      }
      addr = pc;
    }
  }

  /* Allocate memory from the gap in between the cell pointer array
  ** and the cell content area.
  */
  top = get2byte(&data[hdr+5]);
  nCell = get2byte(&data[hdr+3]);
  cellOffset = pPage->cellOffset;
  if( nFrag>=60 || cellOffset + 2*nCell > top - nByte ){
    defragmentPage(pPage);
    top = get2byte(&data[hdr+5]);
  }
  top -= nByte;
  assert( cellOffset + 2*nCell <= top );
  put2byte(&data[hdr+5], top);
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  return top;
}

/*
** Return a section of the pPage->aData to the freelist.
** The first byte of the new free block is pPage->aDisk[start]
** and the size of the block is "size" bytes.
**
** Most of the effort here is involved in coalesing adjacent
** free blocks into a single big free block.
*/
static int freeSpace(MemPage *pPage, int start, int size){
  int addr, pbegin, hdr;
  unsigned char *data = pPage->aData;

  assert( pPage->pBt!=0 );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  assert( start>=pPage->hdrOffset+6+(pPage->leaf?0:4) );
  assert( (start + size)<=pPage->pBt->usableSize );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  assert( size>=0 );   /* Minimum cell size is 4 */

#ifdef SQLITE_SECURE_DELETE
  /* Overwrite deleted information with zeros when the SECURE_DELETE 
  ** option is enabled at compile-time */
  memset(&data[start], 0, size);
#endif

  /* Add the space back into the linked list of freeblocks */
  hdr = pPage->hdrOffset;
  addr = hdr + 1;
  while( (pbegin = get2byte(&data[addr]))<start && pbegin>0 ){
    assert( pbegin<=pPage->pBt->usableSize-4 );
    if( pbegin<=addr ) {
      return SQLITE_CORRUPT_BKPT;
    }
    addr = pbegin;
  }
  if ( pbegin>pPage->pBt->usableSize-4 ) {
    return SQLITE_CORRUPT_BKPT;
  }
  assert( pbegin>addr || pbegin==0 );
  put2byte(&data[addr], start);
  put2byte(&data[start], pbegin);
  put2byte(&data[start+2], size);
  pPage->nFree += (u16)size;

  /* Coalesce adjacent free blocks */
  addr = pPage->hdrOffset + 1;
  while( (pbegin = get2byte(&data[addr]))>0 ){
    int pnext, psize, x;
    assert( pbegin>addr );
    assert( pbegin<=pPage->pBt->usableSize-4 );
    pnext = get2byte(&data[pbegin]);
    psize = get2byte(&data[pbegin+2]);
    if( pbegin + psize + 3 >= pnext && pnext>0 ){
      int frag = pnext - (pbegin+psize);
      if( (frag<0) || (frag>(int)data[pPage->hdrOffset+7]) ){
        return SQLITE_CORRUPT_BKPT;
      }
      data[pPage->hdrOffset+7] -= (u8)frag;
      x = get2byte(&data[pnext]);
      put2byte(&data[pbegin], x);
      x = pnext + get2byte(&data[pnext+2]) - pbegin;
      put2byte(&data[pbegin+2], x);
    }else{
      addr = pbegin;
    }
  }

  /* If the cell content area begins with a freeblock, remove it. */
  if( data[hdr+1]==data[hdr+5] && data[hdr+2]==data[hdr+6] ){
    int top;
    pbegin = get2byte(&data[hdr+1]);
    memcpy(&data[hdr+1], &data[pbegin], 2);
    top = get2byte(&data[hdr+5]) + get2byte(&data[pbegin+2]);
    put2byte(&data[hdr+5], top);
  }
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  return SQLITE_OK;
}

/*
** Decode the flags byte (the first byte of the header) for a page
** and initialize fields of the MemPage structure accordingly.
**
** Only the following combinations are supported.  Anything different
** indicates a corrupt database files:
**
**         PTF_ZERODATA
**         PTF_ZERODATA | PTF_LEAF
**         PTF_LEAFDATA | PTF_INTKEY
**         PTF_LEAFDATA | PTF_INTKEY | PTF_LEAF
*/
static int decodeFlags(MemPage *pPage, int flagByte){
  BtShared *pBt;     /* A copy of pPage->pBt */

  assert( pPage->hdrOffset==(pPage->pgno==1 ? 100 : 0) );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  pPage->leaf = (u8)(flagByte>>3);  assert( PTF_LEAF == 1<<3 );
  flagByte &= ~PTF_LEAF;
  pPage->childPtrSize = 4-4*pPage->leaf;
  pBt = pPage->pBt;
  if( flagByte==(PTF_LEAFDATA | PTF_INTKEY) ){
    pPage->intKey = 1;
    pPage->hasData = pPage->leaf;
    pPage->maxLocal = pBt->maxLeaf;
    pPage->minLocal = pBt->minLeaf;
  }else if( flagByte==PTF_ZERODATA ){
    pPage->intKey = 0;
    pPage->hasData = 0;
    pPage->maxLocal = pBt->maxLocal;
    pPage->minLocal = pBt->minLocal;
  }else{
    return SQLITE_CORRUPT_BKPT;
  }
  return SQLITE_OK;
}

/*
** Initialize the auxiliary information for a disk block.
**
** Return SQLITE_OK on success.  If we see that the page does
** not contain a well-formed database page, then return 
** SQLITE_CORRUPT.  Note that a return of SQLITE_OK does not
** guarantee that the page is well-formed.  It only shows that
** we failed to detect any corruption.
*/
int sqlite3BtreeInitPage(MemPage *pPage){

  assert( pPage->pBt!=0 );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  assert( pPage->pgno==sqlite3PagerPagenumber(pPage->pDbPage) );
  assert( pPage == sqlite3PagerGetExtra(pPage->pDbPage) );
  assert( pPage->aData == sqlite3PagerGetData(pPage->pDbPage) );

  if( !pPage->isInit ){
    u16 pc;            /* Address of a freeblock within pPage->aData[] */
    u8 hdr;            /* Offset to beginning of page header */
    u8 *data;          /* Equal to pPage->aData */
    BtShared *pBt;        /* The main btree structure */
    u16 usableSize;    /* Amount of usable space on each page */
    u16 cellOffset;    /* Offset from start of page to first cell pointer */
    u16 nFree;         /* Number of unused bytes on the page */
    u16 top;           /* First byte of the cell content area */

    pBt = pPage->pBt;

    hdr = pPage->hdrOffset;
    data = pPage->aData;
    if( decodeFlags(pPage, data[hdr]) ) return SQLITE_CORRUPT_BKPT;
    assert( pBt->pageSize>=512 && pBt->pageSize<=32768 );
    pPage->maskPage = pBt->pageSize - 1;
    pPage->nOverflow = 0;
    usableSize = pBt->usableSize;
    pPage->cellOffset = cellOffset = hdr + 12 - 4*pPage->leaf;
    top = get2byte(&data[hdr+5]);
    pPage->nCell = get2byte(&data[hdr+3]);
    if( pPage->nCell>MX_CELL(pBt) ){
      /* To many cells for a single page.  The page must be corrupt */
      return SQLITE_CORRUPT_BKPT;
    }
  
    /* Compute the total free space on the page */
    pc = get2byte(&data[hdr+1]);
    nFree = data[hdr+7] + top - (cellOffset + 2*pPage->nCell);
    while( pc>0 ){
      u16 next, size;
      if( pc>usableSize-4 ){
        /* Free block is off the page */
        return SQLITE_CORRUPT_BKPT; 
      }
      next = get2byte(&data[pc]);
      size = get2byte(&data[pc+2]);
      if( next>0 && next<=pc+size+3 ){
        /* Free blocks must be in accending order */
        return SQLITE_CORRUPT_BKPT; 
      }
      nFree += size;
      pc = next;
    }
    pPage->nFree = (u16)nFree;
    if( nFree>=usableSize ){
      /* Free space cannot exceed total page size */
      return SQLITE_CORRUPT_BKPT; 
    }

#if 0
  /* Check that all the offsets in the cell offset array are within range. 
  ** 
  ** Omitting this consistency check and using the pPage->maskPage mask
  ** to prevent overrunning the page buffer in findCell() results in a
  ** 2.5% performance gain.
  */
  {
    u8 *pOff;        /* Iterator used to check all cell offsets are in range */
    u8 *pEnd;        /* Pointer to end of cell offset array */
    u8 mask;         /* Mask of bits that must be zero in MSB of cell offsets */
    mask = ~(((u8)(pBt->pageSize>>8))-1);
    pEnd = &data[cellOffset + pPage->nCell*2];
    for(pOff=&data[cellOffset]; pOff!=pEnd && !((*pOff)&mask); pOff+=2);
    if( pOff!=pEnd ){
      return SQLITE_CORRUPT_BKPT;
    }
  }
#endif

    pPage->isInit = 1;
  }
  return SQLITE_OK;
}

/*
** Set up a raw page so that it looks like a database page holding
** no entries.
*/
static void zeroPage(MemPage *pPage, int flags){
  unsigned char *data = pPage->aData;
  BtShared *pBt = pPage->pBt;
  u8 hdr = pPage->hdrOffset;
  u16 first;

  assert( sqlite3PagerPagenumber(pPage->pDbPage)==pPage->pgno );
  assert( sqlite3PagerGetExtra(pPage->pDbPage) == (void*)pPage );
  assert( sqlite3PagerGetData(pPage->pDbPage) == data );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  assert( sqlite3_mutex_held(pBt->mutex) );
  /*memset(&data[hdr], 0, pBt->usableSize - hdr);*/
  data[hdr] = (char)flags;
  first = hdr + 8 + 4*((flags&PTF_LEAF)==0 ?1:0);
  memset(&data[hdr+1], 0, 4);
  data[hdr+7] = 0;
  put2byte(&data[hdr+5], pBt->usableSize);
  pPage->nFree = pBt->usableSize - first;
  decodeFlags(pPage, flags);
  pPage->hdrOffset = hdr;
  pPage->cellOffset = first;
  pPage->nOverflow = 0;
  assert( pBt->pageSize>=512 && pBt->pageSize<=32768 );
  pPage->maskPage = pBt->pageSize - 1;
  pPage->nCell = 0;
  pPage->isInit = 1;
}


/*
** Convert a DbPage obtained from the pager into a MemPage used by
** the btree layer.
*/
static MemPage *btreePageFromDbPage(DbPage *pDbPage, Pgno pgno, BtShared *pBt){
  MemPage *pPage = (MemPage*)sqlite3PagerGetExtra(pDbPage);
  pPage->aData = sqlite3PagerGetData(pDbPage);
  pPage->pDbPage = pDbPage;
  pPage->pBt = pBt;
  pPage->pgno = pgno;
  pPage->hdrOffset = pPage->pgno==1 ? 100 : 0;
  return pPage; 
}

/*
** Get a page from the pager.  Initialize the MemPage.pBt and
** MemPage.aData elements if needed.
**
** If the noContent flag is set, it means that we do not care about
** the content of the page at this time.  So do not go to the disk
** to fetch the content.  Just fill in the content with zeros for now.
** If in the future we call sqlite3PagerWrite() on this page, that
** means we have started to be concerned about content and the disk
** read should occur at that point.
*/
int sqlite3BtreeGetPage(
  BtShared *pBt,       /* The btree */
  Pgno pgno,           /* Number of the page to fetch */
  MemPage **ppPage,    /* Return the page in this parameter */
  int noContent        /* Do not load page content if true */
){
  int rc;
  DbPage *pDbPage;

  assert( sqlite3_mutex_held(pBt->mutex) );
  rc = sqlite3PagerAcquire(pBt->pPager, pgno, (DbPage**)&pDbPage, noContent);
  if( rc ) return rc;
  *ppPage = btreePageFromDbPage(pDbPage, pgno, pBt);
  return SQLITE_OK;
}

/*
** Retrieve a page from the pager cache. If the requested page is not
** already in the pager cache return NULL. Initialize the MemPage.pBt and
** MemPage.aData elements if needed.
*/
static MemPage *btreePageLookup(BtShared *pBt, Pgno pgno){
  DbPage *pDbPage;
  assert( sqlite3_mutex_held(pBt->mutex) );
  pDbPage = sqlite3PagerLookup(pBt->pPager, pgno);
  if( pDbPage ){
    return btreePageFromDbPage(pDbPage, pgno, pBt);
  }
  return 0;
}

/*
** Return the size of the database file in pages. If there is any kind of
** error, return ((unsigned int)-1).
*/
static Pgno pagerPagecount(BtShared *pBt){
  int nPage = -1;
  int rc;
  assert( pBt->pPage1 );
  rc = sqlite3PagerPagecount(pBt->pPager, &nPage);
  assert( rc==SQLITE_OK || nPage==-1 );
  return (Pgno)nPage;
}

/*
** Get a page from the pager and initialize it.  This routine
** is just a convenience wrapper around separate calls to
** sqlite3BtreeGetPage() and sqlite3BtreeInitPage().
*/
static int getAndInitPage(
  BtShared *pBt,          /* The database file */
  Pgno pgno,           /* Number of the page to get */
  MemPage **ppPage     /* Write the page pointer here */
){
  int rc;
  MemPage *pPage;

  assert( sqlite3_mutex_held(pBt->mutex) );
  if( pgno==0 ){
    return SQLITE_CORRUPT_BKPT; 
  }

  /* It is often the case that the page we want is already in cache.
  ** If so, get it directly.  This saves us from having to call
  ** pagerPagecount() to make sure pgno is within limits, which results
  ** in a measureable performance improvements.
  */
  *ppPage = pPage = btreePageLookup(pBt, pgno);
  if( pPage ){
    /* Page is already in cache */
    rc = SQLITE_OK;
  }else{
    /* Page not in cache.  Acquire it. */
    if( pgno>pagerPagecount(pBt) ){
      return SQLITE_CORRUPT_BKPT; 
    }
    rc = sqlite3BtreeGetPage(pBt, pgno, ppPage, 0);
    if( rc ) return rc;
    pPage = *ppPage;
  }
  if( !pPage->isInit ){
    rc = sqlite3BtreeInitPage(pPage);
  }
  if( rc!=SQLITE_OK ){
    releasePage(pPage);
    *ppPage = 0;
  }
  return rc;
}

/*
** Release a MemPage.  This should be called once for each prior
** call to sqlite3BtreeGetPage.
*/
static void releasePage(MemPage *pPage){
  if( pPage ){
    assert( pPage->nOverflow==0 || sqlite3PagerPageRefcount(pPage->pDbPage)>1 );
    assert( pPage->aData );
    assert( pPage->pBt );
    assert( sqlite3PagerGetExtra(pPage->pDbPage) == (void*)pPage );
    assert( sqlite3PagerGetData(pPage->pDbPage)==pPage->aData );
    assert( sqlite3_mutex_held(pPage->pBt->mutex) );
    sqlite3PagerUnref(pPage->pDbPage);
  }
}

/*
** During a rollback, when the pager reloads information into the cache
** so that the cache is restored to its original state at the start of
** the transaction, for each page restored this routine is called.
**
** This routine needs to reset the extra data section at the end of the
** page to agree with the restored data.
*/
static void pageReinit(DbPage *pData){
  MemPage *pPage;
  pPage = (MemPage *)sqlite3PagerGetExtra(pData);
  if( pPage->isInit ){
    assert( sqlite3_mutex_held(pPage->pBt->mutex) );
    pPage->isInit = 0;
    if( sqlite3PagerPageRefcount(pData)>0 ){
      sqlite3BtreeInitPage(pPage);
    }
  }
}

/*
** Invoke the busy handler for a btree.
*/
static int btreeInvokeBusyHandler(void *pArg){
  BtShared *pBt = (BtShared*)pArg;
  assert( pBt->db );
  assert( sqlite3_mutex_held(pBt->db->mutex) );
  return sqlite3InvokeBusyHandler(&pBt->db->busyHandler);
}

/*
** Open a database file.
** 
** zFilename is the name of the database file.  If zFilename is NULL
** a new database with a random name is created.  This randomly named
** database file will be deleted when sqlite3BtreeClose() is called.
** If zFilename is ":memory:" then an in-memory database is created
** that is automatically destroyed when it is closed.
*/
int sqlite3BtreeOpen(
  const char *zFilename,  /* Name of the file containing the BTree database */
  sqlite3 *db,            /* Associated database handle */
  Btree **ppBtree,        /* Pointer to new Btree object written here */
  int flags,              /* Options */
  int vfsFlags            /* Flags passed through to sqlite3_vfs.xOpen() */
){
  sqlite3_vfs *pVfs;      /* The VFS to use for this btree */
  BtShared *pBt = 0;      /* Shared part of btree structure */
  Btree *p;               /* Handle to return */
  int rc = SQLITE_OK;
  u8 nReserve;
  unsigned char zDbHeader[100];

  /* Set the variable isMemdb to true for an in-memory database, or 
  ** false for a file-based database. This symbol is only required if
  ** either of the shared-data or autovacuum features are compiled 
  ** into the library.
  */
#if !defined(SQLITE_OMIT_SHARED_CACHE) || !defined(SQLITE_OMIT_AUTOVACUUM)
  #ifdef SQLITE_OMIT_MEMORYDB
    const int isMemdb = 0;
  #else
    const int isMemdb = zFilename && !strcmp(zFilename, ":memory:");
  #endif
#endif

  assert( db!=0 );
  assert( sqlite3_mutex_held(db->mutex) );

  pVfs = db->pVfs;
  p = sqlite3MallocZero(sizeof(Btree));
  if( !p ){
    return SQLITE_NOMEM;
  }
  p->inTrans = TRANS_NONE;
  p->db = db;

#if !defined(SQLITE_OMIT_SHARED_CACHE) && !defined(SQLITE_OMIT_DISKIO)
  /*
  ** If this Btree is a candidate for shared cache, try to find an
  ** existing BtShared object that we can share with
  */
  if( isMemdb==0
   && (db->flags & SQLITE_Vtab)==0
   && zFilename && zFilename[0]
  ){
    if( sqlite3GlobalConfig.sharedCacheEnabled ){
      int nFullPathname = pVfs->mxPathname+1;
      char *zFullPathname = sqlite3Malloc(nFullPathname);
      sqlite3_mutex *mutexShared;
      p->sharable = 1;
      db->flags |= SQLITE_SharedCache;
      if( !zFullPathname ){
        sqlite3_free(p);
        return SQLITE_NOMEM;
      }
      sqlite3OsFullPathname(pVfs, zFilename, nFullPathname, zFullPathname);
      mutexShared = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER);
      sqlite3_mutex_enter(mutexShared);
      for(pBt=GLOBAL(BtShared*,sqlite3SharedCacheList); pBt; pBt=pBt->pNext){
        assert( pBt->nRef>0 );
        if( 0==strcmp(zFullPathname, sqlite3PagerFilename(pBt->pPager))
                 && sqlite3PagerVfs(pBt->pPager)==pVfs ){
          p->pBt = pBt;
          pBt->nRef++;
          break;
        }
      }
      sqlite3_mutex_leave(mutexShared);
      sqlite3_free(zFullPathname);
    }
#ifdef SQLITE_DEBUG
    else{
      /* In debug mode, we mark all persistent databases as sharable
      ** even when they are not.  This exercises the locking code and
      ** gives more opportunity for asserts(sqlite3_mutex_held())
      ** statements to find locking problems.
      */
      p->sharable = 1;
    }
#endif
  }
#endif
  if( pBt==0 ){
    /*
    ** The following asserts make sure that structures used by the btree are
    ** the right size.  This is to guard against size changes that result
    ** when compiling on a different architecture.
    */
    assert( sizeof(i64)==8 || sizeof(i64)==4 );
    assert( sizeof(u64)==8 || sizeof(u64)==4 );
    assert( sizeof(u32)==4 );
    assert( sizeof(u16)==2 );
    assert( sizeof(Pgno)==4 );
  
    pBt = sqlite3MallocZero( sizeof(*pBt) );
    if( pBt==0 ){
      rc = SQLITE_NOMEM;
      goto btree_open_out;
    }
    rc = sqlite3PagerOpen(pVfs, &pBt->pPager, zFilename,
                          EXTRA_SIZE, flags, vfsFlags);
    if( rc==SQLITE_OK ){
      rc = sqlite3PagerReadFileheader(pBt->pPager,sizeof(zDbHeader),zDbHeader);
    }
    if( rc!=SQLITE_OK ){
      goto btree_open_out;
    }
    sqlite3PagerSetBusyhandler(pBt->pPager, btreeInvokeBusyHandler, pBt);
    p->pBt = pBt;
  
    sqlite3PagerSetReiniter(pBt->pPager, pageReinit);
    pBt->pCursor = 0;
    pBt->pPage1 = 0;
    pBt->readOnly = sqlite3PagerIsreadonly(pBt->pPager);
    pBt->pageSize = get2byte(&zDbHeader[16]);
    if( pBt->pageSize<512 || pBt->pageSize>SQLITE_MAX_PAGE_SIZE
         || ((pBt->pageSize-1)&pBt->pageSize)!=0 ){
      pBt->pageSize = 0;
      sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize);
#ifndef SQLITE_OMIT_AUTOVACUUM
      /* If the magic name ":memory:" will create an in-memory database, then
      ** leave the autoVacuum mode at 0 (do not auto-vacuum), even if
      ** SQLITE_DEFAULT_AUTOVACUUM is true. On the other hand, if
      ** SQLITE_OMIT_MEMORYDB has been defined, then ":memory:" is just a
      ** regular file-name. In this case the auto-vacuum applies as per normal.
      */
      if( zFilename && !isMemdb ){
        pBt->autoVacuum = (SQLITE_DEFAULT_AUTOVACUUM ? 1 : 0);
        pBt->incrVacuum = (SQLITE_DEFAULT_AUTOVACUUM==2 ? 1 : 0);
      }
#endif
      nReserve = 0;
    }else{
      nReserve = zDbHeader[20];
      pBt->pageSizeFixed = 1;
#ifndef SQLITE_OMIT_AUTOVACUUM
      pBt->autoVacuum = (get4byte(&zDbHeader[36 + 4*4])?1:0);
      pBt->incrVacuum = (get4byte(&zDbHeader[36 + 7*4])?1:0);
#endif
    }
    pBt->usableSize = pBt->pageSize - nReserve;
    assert( (pBt->pageSize & 7)==0 );  /* 8-byte alignment of pageSize */
    sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize);
   
#if !defined(SQLITE_OMIT_SHARED_CACHE) && !defined(SQLITE_OMIT_DISKIO)
    /* Add the new BtShared object to the linked list sharable BtShareds.
    */
    if( p->sharable ){
      sqlite3_mutex *mutexShared;
      pBt->nRef = 1;
      mutexShared = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER);
      if( SQLITE_THREADSAFE && sqlite3GlobalConfig.bCoreMutex ){
        pBt->mutex = sqlite3MutexAlloc(SQLITE_MUTEX_FAST);
        if( pBt->mutex==0 ){
          rc = SQLITE_NOMEM;
          db->mallocFailed = 0;
          goto btree_open_out;
        }
      }
      sqlite3_mutex_enter(mutexShared);
      pBt->pNext = GLOBAL(BtShared*,sqlite3SharedCacheList);
      GLOBAL(BtShared*,sqlite3SharedCacheList) = pBt;
      sqlite3_mutex_leave(mutexShared);
    }
#endif
  }

#if !defined(SQLITE_OMIT_SHARED_CACHE) && !defined(SQLITE_OMIT_DISKIO)
  /* If the new Btree uses a sharable pBtShared, then link the new
  ** Btree into the list of all sharable Btrees for the same connection.
  ** The list is kept in ascending order by pBt address.
  */
  if( p->sharable ){
    int i;
    Btree *pSib;
    for(i=0; i<db->nDb; i++){
      if( (pSib = db->aDb[i].pBt)!=0 && pSib->sharable ){
        while( pSib->pPrev ){ pSib = pSib->pPrev; }
        if( p->pBt<pSib->pBt ){
          p->pNext = pSib;
          p->pPrev = 0;
          pSib->pPrev = p;
        }else{
          while( pSib->pNext && pSib->pNext->pBt<p->pBt ){
            pSib = pSib->pNext;
          }
          p->pNext = pSib->pNext;
          p->pPrev = pSib;
          if( p->pNext ){
            p->pNext->pPrev = p;
          }
          pSib->pNext = p;
        }
        break;
      }
    }
  }
#endif
  *ppBtree = p;

btree_open_out:
  if( rc!=SQLITE_OK ){
    if( pBt && pBt->pPager ){
      sqlite3PagerClose(pBt->pPager);
    }
    sqlite3_free(pBt);
    sqlite3_free(p);
    *ppBtree = 0;
  }
  return rc;
}

/*
** Decrement the BtShared.nRef counter.  When it reaches zero,
** remove the BtShared structure from the sharing list.  Return
** true if the BtShared.nRef counter reaches zero and return
** false if it is still positive.
*/
static int removeFromSharingList(BtShared *pBt){
#ifndef SQLITE_OMIT_SHARED_CACHE
  sqlite3_mutex *pMaster;
  BtShared *pList;
  int removed = 0;

  assert( sqlite3_mutex_notheld(pBt->mutex) );
  pMaster = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER);
  sqlite3_mutex_enter(pMaster);
  pBt->nRef--;
  if( pBt->nRef<=0 ){
    if( GLOBAL(BtShared*,sqlite3SharedCacheList)==pBt ){
      GLOBAL(BtShared*,sqlite3SharedCacheList) = pBt->pNext;
    }else{
      pList = GLOBAL(BtShared*,sqlite3SharedCacheList);
      while( ALWAYS(pList) && pList->pNext!=pBt ){
        pList=pList->pNext;
      }
      if( ALWAYS(pList) ){
        pList->pNext = pBt->pNext;
      }
    }
    if( SQLITE_THREADSAFE ){
      sqlite3_mutex_free(pBt->mutex);
    }
    removed = 1;
  }
  sqlite3_mutex_leave(pMaster);
  return removed;
#else
  return 1;
#endif
}

/*
** Make sure pBt->pTmpSpace points to an allocation of 
** MX_CELL_SIZE(pBt) bytes.
*/
static void allocateTempSpace(BtShared *pBt){
  if( !pBt->pTmpSpace ){
    pBt->pTmpSpace = sqlite3PageMalloc( pBt->pageSize );
  }
}

/*
** Free the pBt->pTmpSpace allocation
*/
static void freeTempSpace(BtShared *pBt){
  sqlite3PageFree( pBt->pTmpSpace);
  pBt->pTmpSpace = 0;
}

/*
** Close an open database and invalidate all cursors.
*/
int sqlite3BtreeClose(Btree *p){
  BtShared *pBt = p->pBt;
  BtCursor *pCur;

  /* Close all cursors opened via this handle.  */
  assert( sqlite3_mutex_held(p->db->mutex) );
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  pCur = pBt->pCursor;
  while( pCur ){
    BtCursor *pTmp = pCur;
    pCur = pCur->pNext;
    if( pTmp->pBtree==p ){
      sqlite3BtreeCloseCursor(pTmp);
    }
  }

  /* Rollback any active transaction and free the handle structure.
  ** The call to sqlite3BtreeRollback() drops any table-locks held by
  ** this handle.
  */
  sqlite3BtreeRollback(p);
  sqlite3BtreeLeave(p);

  /* If there are still other outstanding references to the shared-btree
  ** structure, return now. The remainder of this procedure cleans 
  ** up the shared-btree.
  */
  assert( p->wantToLock==0 && p->locked==0 );
  if( !p->sharable || removeFromSharingList(pBt) ){
    /* The pBt is no longer on the sharing list, so we can access
    ** it without having to hold the mutex.
    **
    ** Clean out and delete the BtShared object.
    */
    assert( !pBt->pCursor );
    sqlite3PagerClose(pBt->pPager);
    if( pBt->xFreeSchema && pBt->pSchema ){
      pBt->xFreeSchema(pBt->pSchema);
    }
    sqlite3_free(pBt->pSchema);
    freeTempSpace(pBt);
    sqlite3_free(pBt);
  }

#ifndef SQLITE_OMIT_SHARED_CACHE
  assert( p->wantToLock==0 );
  assert( p->locked==0 );
  if( p->pPrev ) p->pPrev->pNext = p->pNext;
  if( p->pNext ) p->pNext->pPrev = p->pPrev;
#endif

  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** Change the limit on the number of pages allowed in the cache.
**
** The maximum number of cache pages is set to the absolute
** value of mxPage.  If mxPage is negative, the pager will
** operate asynchronously - it will not stop to do fsync()s
** to insure data is written to the disk surface before
** continuing.  Transactions still work if synchronous is off,
** and the database cannot be corrupted if this program
** crashes.  But if the operating system crashes or there is
** an abrupt power failure when synchronous is off, the database
** could be left in an inconsistent and unrecoverable state.
** Synchronous is on by default so database corruption is not
** normally a worry.
*/
int sqlite3BtreeSetCacheSize(Btree *p, int mxPage){
  BtShared *pBt = p->pBt;
  assert( sqlite3_mutex_held(p->db->mutex) );
  sqlite3BtreeEnter(p);
  sqlite3PagerSetCachesize(pBt->pPager, mxPage);
  sqlite3BtreeLeave(p);
  return SQLITE_OK;
}

/*
** Change the way data is synced to disk in order to increase or decrease
** how well the database resists damage due to OS crashes and power
** failures.  Level 1 is the same as asynchronous (no syncs() occur and
** there is a high probability of damage)  Level 2 is the default.  There
** is a very low but non-zero probability of damage.  Level 3 reduces the
** probability of damage to near zero but with a write performance reduction.
*/
#ifndef SQLITE_OMIT_PAGER_PRAGMAS
int sqlite3BtreeSetSafetyLevel(Btree *p, int level, int fullSync){
  BtShared *pBt = p->pBt;
  assert( sqlite3_mutex_held(p->db->mutex) );
  sqlite3BtreeEnter(p);
  sqlite3PagerSetSafetyLevel(pBt->pPager, level, fullSync);
  sqlite3BtreeLeave(p);
  return SQLITE_OK;
}
#endif

/*
** Return TRUE if the given btree is set to safety level 1.  In other
** words, return TRUE if no sync() occurs on the disk files.
*/
int sqlite3BtreeSyncDisabled(Btree *p){
  BtShared *pBt = p->pBt;
  int rc;
  assert( sqlite3_mutex_held(p->db->mutex) );  
  sqlite3BtreeEnter(p);
  assert( pBt && pBt->pPager );
  rc = sqlite3PagerNosync(pBt->pPager);
  sqlite3BtreeLeave(p);
  return rc;
}

#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) || !defined(SQLITE_OMIT_VACUUM)
/*
** Change the default pages size and the number of reserved bytes per page.
**
** The page size must be a power of 2 between 512 and 65536.  If the page
** size supplied does not meet this constraint then the page size is not
** changed.
**
** Page sizes are constrained to be a power of two so that the region
** of the database file used for locking (beginning at PENDING_BYTE,
** the first byte past the 1GB boundary, 0x40000000) needs to occur
** at the beginning of a page.
**
** If parameter nReserve is less than zero, then the number of reserved
** bytes per page is left unchanged.
*/
int sqlite3BtreeSetPageSize(Btree *p, int pageSize, int nReserve){
  int rc = SQLITE_OK;
  BtShared *pBt = p->pBt;
  assert( nReserve>=-1 && nReserve<=255 );
  sqlite3BtreeEnter(p);
  if( pBt->pageSizeFixed ){
    sqlite3BtreeLeave(p);
    return SQLITE_READONLY;
  }
  if( nReserve<0 ){
    nReserve = pBt->pageSize - pBt->usableSize;
  }
  assert( nReserve>=0 && nReserve<=255 );
  if( pageSize>=512 && pageSize<=SQLITE_MAX_PAGE_SIZE &&
        ((pageSize-1)&pageSize)==0 ){
    assert( (pageSize & 7)==0 );
    assert( !pBt->pPage1 && !pBt->pCursor );
    pBt->pageSize = (u16)pageSize;
    freeTempSpace(pBt);
    rc = sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize);
  }
  pBt->usableSize = pBt->pageSize - (u16)nReserve;
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Return the currently defined page size
*/
int sqlite3BtreeGetPageSize(Btree *p){
  return p->pBt->pageSize;
}
int sqlite3BtreeGetReserve(Btree *p){
  int n;
  sqlite3BtreeEnter(p);
  n = p->pBt->pageSize - p->pBt->usableSize;
  sqlite3BtreeLeave(p);
  return n;
}

/*
** Set the maximum page count for a database if mxPage is positive.
** No changes are made if mxPage is 0 or negative.
** Regardless of the value of mxPage, return the maximum page count.
*/
int sqlite3BtreeMaxPageCount(Btree *p, int mxPage){
  int n;
  sqlite3BtreeEnter(p);
  n = sqlite3PagerMaxPageCount(p->pBt->pPager, mxPage);
  sqlite3BtreeLeave(p);
  return n;
}
#endif /* !defined(SQLITE_OMIT_PAGER_PRAGMAS) || !defined(SQLITE_OMIT_VACUUM) */

/*
** Change the 'auto-vacuum' property of the database. If the 'autoVacuum'
** parameter is non-zero, then auto-vacuum mode is enabled. If zero, it
** is disabled. The default value for the auto-vacuum property is 
** determined by the SQLITE_DEFAULT_AUTOVACUUM macro.
*/
int sqlite3BtreeSetAutoVacuum(Btree *p, int autoVacuum){
#ifdef SQLITE_OMIT_AUTOVACUUM
  return SQLITE_READONLY;
#else
  BtShared *pBt = p->pBt;
  int rc = SQLITE_OK;
  u8 av = autoVacuum ?1:0;

  sqlite3BtreeEnter(p);
  if( pBt->pageSizeFixed && av!=pBt->autoVacuum ){
    rc = SQLITE_READONLY;
  }else{
    pBt->autoVacuum = av;
  }
  sqlite3BtreeLeave(p);
  return rc;
#endif
}

/*
** Return the value of the 'auto-vacuum' property. If auto-vacuum is 
** enabled 1 is returned. Otherwise 0.
*/
int sqlite3BtreeGetAutoVacuum(Btree *p){
#ifdef SQLITE_OMIT_AUTOVACUUM
  return BTREE_AUTOVACUUM_NONE;
#else
  int rc;
  sqlite3BtreeEnter(p);
  rc = (
    (!p->pBt->autoVacuum)?BTREE_AUTOVACUUM_NONE:
    (!p->pBt->incrVacuum)?BTREE_AUTOVACUUM_FULL:
    BTREE_AUTOVACUUM_INCR
  );
  sqlite3BtreeLeave(p);
  return rc;
#endif
}


/*
** Get a reference to pPage1 of the database file.  This will
** also acquire a readlock on that file.
**
** SQLITE_OK is returned on success.  If the file is not a
** well-formed database file, then SQLITE_CORRUPT is returned.
** SQLITE_BUSY is returned if the database is locked.  SQLITE_NOMEM
** is returned if we run out of memory. 
*/
static int lockBtree(BtShared *pBt){
  int rc;
  MemPage *pPage1;
  int nPage;

  assert( sqlite3_mutex_held(pBt->mutex) );
  if( pBt->pPage1 ) return SQLITE_OK;
  rc = sqlite3BtreeGetPage(pBt, 1, &pPage1, 0);
  if( rc!=SQLITE_OK ) return rc;

  /* Do some checking to help insure the file we opened really is
  ** a valid database file. 
  */
  rc = sqlite3PagerPagecount(pBt->pPager, &nPage);
  if( rc!=SQLITE_OK ){
    goto page1_init_failed;
  }else if( nPage>0 ){
    int pageSize;
    int usableSize;
    u8 *page1 = pPage1->aData;
    rc = SQLITE_NOTADB;
    if( memcmp(page1, zMagicHeader, 16)!=0 ){
      goto page1_init_failed;
    }
    if( page1[18]>1 ){
      pBt->readOnly = 1;
    }
    if( page1[19]>1 ){
      goto page1_init_failed;
    }

    /* The maximum embedded fraction must be exactly 25%.  And the minimum
    ** embedded fraction must be 12.5% for both leaf-data and non-leaf-data.
    ** The original design allowed these amounts to vary, but as of
    ** version 3.6.0, we require them to be fixed.
    */
    if( memcmp(&page1[21], "\100\040\040",3)!=0 ){
      goto page1_init_failed;
    }
    pageSize = get2byte(&page1[16]);
    if( ((pageSize-1)&pageSize)!=0 || pageSize<512 ||
        (SQLITE_MAX_PAGE_SIZE<32768 && pageSize>SQLITE_MAX_PAGE_SIZE)
    ){
      goto page1_init_failed;
    }
    assert( (pageSize & 7)==0 );
    usableSize = pageSize - page1[20];
    if( pageSize!=pBt->pageSize ){
      /* After reading the first page of the database assuming a page size
      ** of BtShared.pageSize, we have discovered that the page-size is
      ** actually pageSize. Unlock the database, leave pBt->pPage1 at
      ** zero and return SQLITE_OK. The caller will call this function
      ** again with the correct page-size.
      */
      releasePage(pPage1);
      pBt->usableSize = (u16)usableSize;
      pBt->pageSize = (u16)pageSize;
      freeTempSpace(pBt);
      sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize);
      return SQLITE_OK;
    }
    if( usableSize<500 ){
      goto page1_init_failed;
    }
    pBt->pageSize = (u16)pageSize;
    pBt->usableSize = (u16)usableSize;
#ifndef SQLITE_OMIT_AUTOVACUUM
    pBt->autoVacuum = (get4byte(&page1[36 + 4*4])?1:0);
    pBt->incrVacuum = (get4byte(&page1[36 + 7*4])?1:0);
#endif
  }

  /* maxLocal is the maximum amount of payload to store locally for
  ** a cell.  Make sure it is small enough so that at least minFanout
  ** cells can will fit on one page.  We assume a 10-byte page header.
  ** Besides the payload, the cell must store:
  **     2-byte pointer to the cell
  **     4-byte child pointer
  **     9-byte nKey value
  **     4-byte nData value
  **     4-byte overflow page pointer
  ** So a cell consists of a 2-byte poiner, a header which is as much as
  ** 17 bytes long, 0 to N bytes of payload, and an optional 4 byte overflow
  ** page pointer.
  */
  pBt->maxLocal = (pBt->usableSize-12)*64/255 - 23;
  pBt->minLocal = (pBt->usableSize-12)*32/255 - 23;
  pBt->maxLeaf = pBt->usableSize - 35;
  pBt->minLeaf = (pBt->usableSize-12)*32/255 - 23;
  assert( pBt->maxLeaf + 23 <= MX_CELL_SIZE(pBt) );
  pBt->pPage1 = pPage1;
  return SQLITE_OK;

page1_init_failed:
  releasePage(pPage1);
  pBt->pPage1 = 0;
  return rc;
}

/*
** This routine works like lockBtree() except that it also invokes the
** busy callback if there is lock contention.
*/
static int lockBtreeWithRetry(Btree *pRef){
  int rc = SQLITE_OK;

  assert( sqlite3BtreeHoldsMutex(pRef) );
  if( pRef->inTrans==TRANS_NONE ){
    u8 inTransaction = pRef->pBt->inTransaction;
    btreeIntegrity(pRef);
    rc = sqlite3BtreeBeginTrans(pRef, 0);
    pRef->pBt->inTransaction = inTransaction;
    pRef->inTrans = TRANS_NONE;
    if( rc==SQLITE_OK ){
      pRef->pBt->nTransaction--;
    }
    btreeIntegrity(pRef);
  }
  return rc;
}
       

/*
** If there are no outstanding cursors and we are not in the middle
** of a transaction but there is a read lock on the database, then
** this routine unrefs the first page of the database file which 
** has the effect of releasing the read lock.
**
** If there are any outstanding cursors, this routine is a no-op.
**
** If there is a transaction in progress, this routine is a no-op.
*/
static void unlockBtreeIfUnused(BtShared *pBt){
  assert( sqlite3_mutex_held(pBt->mutex) );
  if( pBt->inTransaction==TRANS_NONE && pBt->pCursor==0 && pBt->pPage1!=0 ){
    if( sqlite3PagerRefcount(pBt->pPager)>=1 ){
      assert( pBt->pPage1->aData );
#if 0
      if( pBt->pPage1->aData==0 ){
        MemPage *pPage = pBt->pPage1;
        pPage->aData = sqlite3PagerGetData(pPage->pDbPage);
        pPage->pBt = pBt;
        pPage->pgno = 1;
      }
#endif
      releasePage(pBt->pPage1);
    }
    pBt->pPage1 = 0;
    pBt->inStmt = 0;
  }
}

/*
** Create a new database by initializing the first page of the
** file.
*/
static int newDatabase(BtShared *pBt){
  MemPage *pP1;
  unsigned char *data;
  int rc;
  int nPage;

  assert( sqlite3_mutex_held(pBt->mutex) );
  rc = sqlite3PagerPagecount(pBt->pPager, &nPage);
  if( rc!=SQLITE_OK || nPage>0 ){
    return rc;
  }
  pP1 = pBt->pPage1;
  assert( pP1!=0 );
  data = pP1->aData;
  rc = sqlite3PagerWrite(pP1->pDbPage);
  if( rc ) return rc;
  memcpy(data, zMagicHeader, sizeof(zMagicHeader));
  assert( sizeof(zMagicHeader)==16 );
  put2byte(&data[16], pBt->pageSize);
  data[18] = 1;
  data[19] = 1;
  assert( pBt->usableSize<=pBt->pageSize && pBt->usableSize+255>=pBt->pageSize);
  data[20] = (u8)(pBt->pageSize - pBt->usableSize);
  data[21] = 64;
  data[22] = 32;
  data[23] = 32;
  memset(&data[24], 0, 100-24);
  zeroPage(pP1, PTF_INTKEY|PTF_LEAF|PTF_LEAFDATA );
  pBt->pageSizeFixed = 1;
#ifndef SQLITE_OMIT_AUTOVACUUM
  assert( pBt->autoVacuum==1 || pBt->autoVacuum==0 );
  assert( pBt->incrVacuum==1 || pBt->incrVacuum==0 );
  put4byte(&data[36 + 4*4], pBt->autoVacuum);
  put4byte(&data[36 + 7*4], pBt->incrVacuum);
#endif
  return SQLITE_OK;
}

/*
** Attempt to start a new transaction. A write-transaction
** is started if the second argument is nonzero, otherwise a read-
** transaction.  If the second argument is 2 or more and exclusive
** transaction is started, meaning that no other process is allowed
** to access the database.  A preexisting transaction may not be
** upgraded to exclusive by calling this routine a second time - the
** exclusivity flag only works for a new transaction.
**
** A write-transaction must be started before attempting any 
** changes to the database.  None of the following routines 
** will work unless a transaction is started first:
**
**      sqlite3BtreeCreateTable()
**      sqlite3BtreeCreateIndex()
**      sqlite3BtreeClearTable()
**      sqlite3BtreeDropTable()
**      sqlite3BtreeInsert()
**      sqlite3BtreeDelete()
**      sqlite3BtreeUpdateMeta()
**
** If an initial attempt to acquire the lock fails because of lock contention
** and the database was previously unlocked, then invoke the busy handler
** if there is one.  But if there was previously a read-lock, do not
** invoke the busy handler - just return SQLITE_BUSY.  SQLITE_BUSY is 
** returned when there is already a read-lock in order to avoid a deadlock.
**
** Suppose there are two processes A and B.  A has a read lock and B has
** a reserved lock.  B tries to promote to exclusive but is blocked because
** of A's read lock.  A tries to promote to reserved but is blocked by B.
** One or the other of the two processes must give way or there can be
** no progress.  By returning SQLITE_BUSY and not invoking the busy callback
** when A already has a read lock, we encourage A to give up and let B
** proceed.
*/
int sqlite3BtreeBeginTrans(Btree *p, int wrflag){
  BtShared *pBt = p->pBt;
  int rc = SQLITE_OK;

  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  btreeIntegrity(p);

  /* If the btree is already in a write-transaction, or it
  ** is already in a read-transaction and a read-transaction
  ** is requested, this is a no-op.
  */
  if( p->inTrans==TRANS_WRITE || (p->inTrans==TRANS_READ && !wrflag) ){
    goto trans_begun;
  }

  /* Write transactions are not possible on a read-only database */
  if( pBt->readOnly && wrflag ){
    rc = SQLITE_READONLY;
    goto trans_begun;
  }

  /* If another database handle has already opened a write transaction 
  ** on this shared-btree structure and a second write transaction is
  ** requested, return SQLITE_BUSY.
  */
  if( pBt->inTransaction==TRANS_WRITE && wrflag ){
    rc = SQLITE_BUSY;
    goto trans_begun;
  }

#ifndef SQLITE_OMIT_SHARED_CACHE
  if( wrflag>1 ){
    BtLock *pIter;
    for(pIter=pBt->pLock; pIter; pIter=pIter->pNext){
      if( pIter->pBtree!=p ){
        rc = SQLITE_BUSY;
        goto trans_begun;
      }
    }
  }
#endif

  do {
    if( pBt->pPage1==0 ){
      do{
        rc = lockBtree(pBt);
      }while( pBt->pPage1==0 && rc==SQLITE_OK );
    }

    if( rc==SQLITE_OK && wrflag ){
      if( pBt->readOnly ){
        rc = SQLITE_READONLY;
      }else{
        rc = sqlite3PagerBegin(pBt->pPager, wrflag>1);
        if( rc==SQLITE_OK ){
          rc = newDatabase(pBt);
        }
      }
    }
  
    if( rc==SQLITE_OK ){
      if( wrflag ) pBt->inStmt = 0;
    }else{
      unlockBtreeIfUnused(pBt);
    }
  }while( rc==SQLITE_BUSY && pBt->inTransaction==TRANS_NONE &&
          btreeInvokeBusyHandler(pBt) );

  if( rc==SQLITE_OK ){
    if( p->inTrans==TRANS_NONE ){
      pBt->nTransaction++;
    }
    p->inTrans = (wrflag?TRANS_WRITE:TRANS_READ);
    if( p->inTrans>pBt->inTransaction ){
      pBt->inTransaction = p->inTrans;
    }
#ifndef SQLITE_OMIT_SHARED_CACHE
    if( wrflag>1 ){
      assert( !pBt->pExclusive );
      pBt->pExclusive = p;
    }
#endif
  }


trans_begun:
  if( rc==SQLITE_OK && wrflag ){
    /* This call makes sure that the pager has the correct number of
    ** open savepoints. If the second parameter is greater than 0 and
    ** the sub-journal is not already open, then it will be opened here.
    */
    rc = sqlite3PagerOpenSavepoint(pBt->pPager, p->db->nSavepoint);
  }

  btreeIntegrity(p);
  sqlite3BtreeLeave(p);
  return rc;
}

#ifndef SQLITE_OMIT_AUTOVACUUM

/*
** Set the pointer-map entries for all children of page pPage. Also, if
** pPage contains cells that point to overflow pages, set the pointer
** map entries for the overflow pages as well.
*/
static int setChildPtrmaps(MemPage *pPage){
  int i;                             /* Counter variable */
  int nCell;                         /* Number of cells in page pPage */
  int rc;                            /* Return code */
  BtShared *pBt = pPage->pBt;
  u8 isInitOrig = pPage->isInit;
  Pgno pgno = pPage->pgno;

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  rc = sqlite3BtreeInitPage(pPage);
  if( rc!=SQLITE_OK ){
    goto set_child_ptrmaps_out;
  }
  nCell = pPage->nCell;

  for(i=0; i<nCell; i++){
    u8 *pCell = findCell(pPage, i);

    rc = ptrmapPutOvflPtr(pPage, pCell);
    if( rc!=SQLITE_OK ){
      goto set_child_ptrmaps_out;
    }

    if( !pPage->leaf ){
      Pgno childPgno = get4byte(pCell);
      rc = ptrmapPut(pBt, childPgno, PTRMAP_BTREE, pgno);
      if( rc!=SQLITE_OK ) goto set_child_ptrmaps_out;
    }
  }

  if( !pPage->leaf ){
    Pgno childPgno = get4byte(&pPage->aData[pPage->hdrOffset+8]);
    rc = ptrmapPut(pBt, childPgno, PTRMAP_BTREE, pgno);
  }

set_child_ptrmaps_out:
  pPage->isInit = isInitOrig;
  return rc;
}

/*
** Somewhere on pPage, which is guarenteed to be a btree page, not an overflow
** page, is a pointer to page iFrom. Modify this pointer so that it points to
** iTo. Parameter eType describes the type of pointer to be modified, as 
** follows:
**
** PTRMAP_BTREE:     pPage is a btree-page. The pointer points at a child 
**                   page of pPage.
**
** PTRMAP_OVERFLOW1: pPage is a btree-page. The pointer points at an overflow
**                   page pointed to by one of the cells on pPage.
**
** PTRMAP_OVERFLOW2: pPage is an overflow-page. The pointer points at the next
**                   overflow page in the list.
*/
static int modifyPagePointer(MemPage *pPage, Pgno iFrom, Pgno iTo, u8 eType){
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  if( eType==PTRMAP_OVERFLOW2 ){
    /* The pointer is always the first 4 bytes of the page in this case.  */
    if( get4byte(pPage->aData)!=iFrom ){
      return SQLITE_CORRUPT_BKPT;
    }
    put4byte(pPage->aData, iTo);
  }else{
    u8 isInitOrig = pPage->isInit;
    int i;
    int nCell;

    sqlite3BtreeInitPage(pPage);
    nCell = pPage->nCell;

    for(i=0; i<nCell; i++){
      u8 *pCell = findCell(pPage, i);
      if( eType==PTRMAP_OVERFLOW1 ){
        CellInfo info;
        sqlite3BtreeParseCellPtr(pPage, pCell, &info);
        if( info.iOverflow ){
          if( iFrom==get4byte(&pCell[info.iOverflow]) ){
            put4byte(&pCell[info.iOverflow], iTo);
            break;
          }
        }
      }else{
        if( get4byte(pCell)==iFrom ){
          put4byte(pCell, iTo);
          break;
        }
      }
    }
  
    if( i==nCell ){
      if( eType!=PTRMAP_BTREE || 
          get4byte(&pPage->aData[pPage->hdrOffset+8])!=iFrom ){
        return SQLITE_CORRUPT_BKPT;
      }
      put4byte(&pPage->aData[pPage->hdrOffset+8], iTo);
    }

    pPage->isInit = isInitOrig;
  }
  return SQLITE_OK;
}


/*
** Move the open database page pDbPage to location iFreePage in the 
** database. The pDbPage reference remains valid.
*/
static int relocatePage(
  BtShared *pBt,           /* Btree */
  MemPage *pDbPage,        /* Open page to move */
  u8 eType,                /* Pointer map 'type' entry for pDbPage */
  Pgno iPtrPage,           /* Pointer map 'page-no' entry for pDbPage */
  Pgno iFreePage,          /* The location to move pDbPage to */
  int isCommit
){
  MemPage *pPtrPage;   /* The page that contains a pointer to pDbPage */
  Pgno iDbPage = pDbPage->pgno;
  Pager *pPager = pBt->pPager;
  int rc;

  assert( eType==PTRMAP_OVERFLOW2 || eType==PTRMAP_OVERFLOW1 || 
      eType==PTRMAP_BTREE || eType==PTRMAP_ROOTPAGE );
  assert( sqlite3_mutex_held(pBt->mutex) );
  assert( pDbPage->pBt==pBt );

  /* Move page iDbPage from its current location to page number iFreePage */
  TRACE(("AUTOVACUUM: Moving %d to free page %d (ptr page %d type %d)\n", 
      iDbPage, iFreePage, iPtrPage, eType));
  rc = sqlite3PagerMovepage(pPager, pDbPage->pDbPage, iFreePage, isCommit);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  pDbPage->pgno = iFreePage;

  /* If pDbPage was a btree-page, then it may have child pages and/or cells
  ** that point to overflow pages. The pointer map entries for all these
  ** pages need to be changed.
  **
  ** If pDbPage is an overflow page, then the first 4 bytes may store a
  ** pointer to a subsequent overflow page. If this is the case, then
  ** the pointer map needs to be updated for the subsequent overflow page.
  */
  if( eType==PTRMAP_BTREE || eType==PTRMAP_ROOTPAGE ){
    rc = setChildPtrmaps(pDbPage);
    if( rc!=SQLITE_OK ){
      return rc;
    }
  }else{
    Pgno nextOvfl = get4byte(pDbPage->aData);
    if( nextOvfl!=0 ){
      rc = ptrmapPut(pBt, nextOvfl, PTRMAP_OVERFLOW2, iFreePage);
      if( rc!=SQLITE_OK ){
        return rc;
      }
    }
  }

  /* Fix the database pointer on page iPtrPage that pointed at iDbPage so
  ** that it points at iFreePage. Also fix the pointer map entry for
  ** iPtrPage.
  */
  if( eType!=PTRMAP_ROOTPAGE ){
    rc = sqlite3BtreeGetPage(pBt, iPtrPage, &pPtrPage, 0);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    rc = sqlite3PagerWrite(pPtrPage->pDbPage);
    if( rc!=SQLITE_OK ){
      releasePage(pPtrPage);
      return rc;
    }
    rc = modifyPagePointer(pPtrPage, iDbPage, iFreePage, eType);
    releasePage(pPtrPage);
    if( rc==SQLITE_OK ){
      rc = ptrmapPut(pBt, iFreePage, eType, iPtrPage);
    }
  }
  return rc;
}

/* Forward declaration required by incrVacuumStep(). */
static int allocateBtreePage(BtShared *, MemPage **, Pgno *, Pgno, u8);

/*
** Perform a single step of an incremental-vacuum. If successful,
** return SQLITE_OK. If there is no work to do (and therefore no
** point in calling this function again), return SQLITE_DONE.
**
** More specificly, this function attempts to re-organize the 
** database so that the last page of the file currently in use
** is no longer in use.
**
** If the nFin parameter is non-zero, the implementation assumes
** that the caller will keep calling incrVacuumStep() until
** it returns SQLITE_DONE or an error, and that nFin is the
** number of pages the database file will contain after this 
** process is complete.
*/
static int incrVacuumStep(BtShared *pBt, Pgno nFin, Pgno iLastPg){
  Pgno nFreeList;           /* Number of pages still on the free-list */

  assert( sqlite3_mutex_held(pBt->mutex) );

  if( !PTRMAP_ISPAGE(pBt, iLastPg) && iLastPg!=PENDING_BYTE_PAGE(pBt) ){
    int rc;
    u8 eType;
    Pgno iPtrPage;

    nFreeList = get4byte(&pBt->pPage1->aData[36]);
    if( nFreeList==0 || nFin==iLastPg ){
      return SQLITE_DONE;
    }

    rc = ptrmapGet(pBt, iLastPg, &eType, &iPtrPage);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    if( eType==PTRMAP_ROOTPAGE ){
      return SQLITE_CORRUPT_BKPT;
    }

    if( eType==PTRMAP_FREEPAGE ){
      if( nFin==0 ){
        /* Remove the page from the files free-list. This is not required
        ** if nFin is non-zero. In that case, the free-list will be
        ** truncated to zero after this function returns, so it doesn't 
        ** matter if it still contains some garbage entries.
        */
        Pgno iFreePg;
        MemPage *pFreePg;
        rc = allocateBtreePage(pBt, &pFreePg, &iFreePg, iLastPg, 1);
        if( rc!=SQLITE_OK ){
          return rc;
        }
        assert( iFreePg==iLastPg );
        releasePage(pFreePg);
      }
    } else {
      Pgno iFreePg;             /* Index of free page to move pLastPg to */
      MemPage *pLastPg;

      rc = sqlite3BtreeGetPage(pBt, iLastPg, &pLastPg, 0);
      if( rc!=SQLITE_OK ){
        return rc;
      }

      /* If nFin is zero, this loop runs exactly once and page pLastPg
      ** is swapped with the first free page pulled off the free list.
      **
      ** On the other hand, if nFin is greater than zero, then keep
      ** looping until a free-page located within the first nFin pages
      ** of the file is found.
      */
      do {
        MemPage *pFreePg;
        rc = allocateBtreePage(pBt, &pFreePg, &iFreePg, 0, 0);
        if( rc!=SQLITE_OK ){
          releasePage(pLastPg);
          return rc;
        }
        releasePage(pFreePg);
      }while( nFin!=0 && iFreePg>nFin );
      assert( iFreePg<iLastPg );
      
      rc = sqlite3PagerWrite(pLastPg->pDbPage);
      if( rc==SQLITE_OK ){
        rc = relocatePage(pBt, pLastPg, eType, iPtrPage, iFreePg, nFin!=0);
      }
      releasePage(pLastPg);
      if( rc!=SQLITE_OK ){
        return rc;
      }
    }
  }

  if( nFin==0 ){
    iLastPg--;
    while( iLastPg==PENDING_BYTE_PAGE(pBt)||PTRMAP_ISPAGE(pBt, iLastPg) ){
      iLastPg--;
    }
    sqlite3PagerTruncateImage(pBt->pPager, iLastPg);
  }
  return SQLITE_OK;
}

/*
** A write-transaction must be opened before calling this function.
** It performs a single unit of work towards an incremental vacuum.
**
** If the incremental vacuum is finished after this function has run,
** SQLITE_DONE is returned. If it is not finished, but no error occured,
** SQLITE_OK is returned. Otherwise an SQLite error code. 
*/
int sqlite3BtreeIncrVacuum(Btree *p){
  int rc;
  BtShared *pBt = p->pBt;

  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( pBt->inTransaction==TRANS_WRITE && p->inTrans==TRANS_WRITE );
  if( !pBt->autoVacuum ){
    rc = SQLITE_DONE;
  }else{
    invalidateAllOverflowCache(pBt);
    rc = incrVacuumStep(pBt, 0, pagerPagecount(pBt));
  }
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** This routine is called prior to sqlite3PagerCommit when a transaction
** is commited for an auto-vacuum database.
**
** If SQLITE_OK is returned, then *pnTrunc is set to the number of pages
** the database file should be truncated to during the commit process. 
** i.e. the database has been reorganized so that only the first *pnTrunc
** pages are in use.
*/
static int autoVacuumCommit(BtShared *pBt){
  int rc = SQLITE_OK;
  Pager *pPager = pBt->pPager;
  VVA_ONLY( int nRef = sqlite3PagerRefcount(pPager) );

  assert( sqlite3_mutex_held(pBt->mutex) );
  invalidateAllOverflowCache(pBt);
  assert(pBt->autoVacuum);
  if( !pBt->incrVacuum ){
    Pgno nFin;
    Pgno nFree;
    Pgno nPtrmap;
    Pgno iFree;
    const int pgsz = pBt->pageSize;
    Pgno nOrig = pagerPagecount(pBt);

    if( PTRMAP_ISPAGE(pBt, nOrig) ){
      return SQLITE_CORRUPT_BKPT;
    }
    if( nOrig==PENDING_BYTE_PAGE(pBt) ){
      nOrig--;
    }
    nFree = get4byte(&pBt->pPage1->aData[36]);
    nPtrmap = (nFree-nOrig+PTRMAP_PAGENO(pBt, nOrig)+pgsz/5)/(pgsz/5);
    nFin = nOrig - nFree - nPtrmap;
    if( nOrig>PENDING_BYTE_PAGE(pBt) && nFin<=PENDING_BYTE_PAGE(pBt) ){
      nFin--;
    }
    while( PTRMAP_ISPAGE(pBt, nFin) || nFin==PENDING_BYTE_PAGE(pBt) ){
      nFin--;
    }

    for(iFree=nOrig; iFree>nFin && rc==SQLITE_OK; iFree--){
      rc = incrVacuumStep(pBt, nFin, iFree);
    }
    if( (rc==SQLITE_DONE || rc==SQLITE_OK) && nFree>0 ){
      rc = SQLITE_OK;
      rc = sqlite3PagerWrite(pBt->pPage1->pDbPage);
      put4byte(&pBt->pPage1->aData[32], 0);
      put4byte(&pBt->pPage1->aData[36], 0);
      sqlite3PagerTruncateImage(pBt->pPager, nFin);
    }
    if( rc!=SQLITE_OK ){
      sqlite3PagerRollback(pPager);
    }
  }

  assert( nRef==sqlite3PagerRefcount(pPager) );
  return rc;
}

#endif /* ifndef SQLITE_OMIT_AUTOVACUUM */

/*
** This routine does the first phase of a two-phase commit.  This routine
** causes a rollback journal to be created (if it does not already exist)
** and populated with enough information so that if a power loss occurs
** the database can be restored to its original state by playing back
** the journal.  Then the contents of the journal are flushed out to
** the disk.  After the journal is safely on oxide, the changes to the
** database are written into the database file and flushed to oxide.
** At the end of this call, the rollback journal still exists on the
** disk and we are still holding all locks, so the transaction has not
** committed.  See sqlite3BtreeCommit() for the second phase of the
** commit process.
**
** This call is a no-op if no write-transaction is currently active on pBt.
**
** Otherwise, sync the database file for the btree pBt. zMaster points to
** the name of a master journal file that should be written into the
** individual journal file, or is NULL, indicating no master journal file 
** (single database transaction).
**
** When this is called, the master journal should already have been
** created, populated with this journal pointer and synced to disk.
**
** Once this is routine has returned, the only thing required to commit
** the write-transaction for this database file is to delete the journal.
*/
int sqlite3BtreeCommitPhaseOne(Btree *p, const char *zMaster){
  int rc = SQLITE_OK;
  if( p->inTrans==TRANS_WRITE ){
    BtShared *pBt = p->pBt;
    sqlite3BtreeEnter(p);
    pBt->db = p->db;
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pBt->autoVacuum ){
      rc = autoVacuumCommit(pBt);
      if( rc!=SQLITE_OK ){
        sqlite3BtreeLeave(p);
        return rc;
      }
    }
#endif
    rc = sqlite3PagerCommitPhaseOne(pBt->pPager, zMaster, 0);
    sqlite3BtreeLeave(p);
  }
  return rc;
}

/*
** Commit the transaction currently in progress.
**
** This routine implements the second phase of a 2-phase commit.  The
** sqlite3BtreeSync() routine does the first phase and should be invoked
** prior to calling this routine.  The sqlite3BtreeSync() routine did
** all the work of writing information out to disk and flushing the
** contents so that they are written onto the disk platter.  All this
** routine has to do is delete or truncate the rollback journal
** (which causes the transaction to commit) and drop locks.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int sqlite3BtreeCommitPhaseTwo(Btree *p){
  BtShared *pBt = p->pBt;

  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  btreeIntegrity(p);

  /* If the handle has a write-transaction open, commit the shared-btrees 
  ** transaction and set the shared state to TRANS_READ.
  */
  if( p->inTrans==TRANS_WRITE ){
    int rc;
    assert( pBt->inTransaction==TRANS_WRITE );
    assert( pBt->nTransaction>0 );
    rc = sqlite3PagerCommitPhaseTwo(pBt->pPager);
    if( rc!=SQLITE_OK ){
      sqlite3BtreeLeave(p);
      return rc;
    }
    pBt->inTransaction = TRANS_READ;
    pBt->inStmt = 0;
  }
  unlockAllTables(p);

  /* If the handle has any kind of transaction open, decrement the transaction
  ** count of the shared btree. If the transaction count reaches 0, set
  ** the shared state to TRANS_NONE. The unlockBtreeIfUnused() call below
  ** will unlock the pager.
  */
  if( p->inTrans!=TRANS_NONE ){
    pBt->nTransaction--;
    if( 0==pBt->nTransaction ){
      pBt->inTransaction = TRANS_NONE;
    }
  }

  /* Set the handles current transaction state to TRANS_NONE and unlock
  ** the pager if this call closed the only read or write transaction.
  */
  btreeClearHasContent(pBt);
  p->inTrans = TRANS_NONE;
  unlockBtreeIfUnused(pBt);

  btreeIntegrity(p);
  sqlite3BtreeLeave(p);
  return SQLITE_OK;
}

/*
** Do both phases of a commit.
*/
int sqlite3BtreeCommit(Btree *p){
  int rc;
  sqlite3BtreeEnter(p);
  rc = sqlite3BtreeCommitPhaseOne(p, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3BtreeCommitPhaseTwo(p);
  }
  sqlite3BtreeLeave(p);
  return rc;
}

#ifndef NDEBUG
/*
** Return the number of write-cursors open on this handle. This is for use
** in assert() expressions, so it is only compiled if NDEBUG is not
** defined.
**
** For the purposes of this routine, a write-cursor is any cursor that
** is capable of writing to the databse.  That means the cursor was
** originally opened for writing and the cursor has not be disabled
** by having its state changed to CURSOR_FAULT.
*/
static int countWriteCursors(BtShared *pBt){
  BtCursor *pCur;
  int r = 0;
  for(pCur=pBt->pCursor; pCur; pCur=pCur->pNext){
    if( pCur->wrFlag && pCur->eState!=CURSOR_FAULT ) r++; 
  }
  return r;
}
#endif

/*
** This routine sets the state to CURSOR_FAULT and the error
** code to errCode for every cursor on BtShared that pBtree
** references.
**
** Every cursor is tripped, including cursors that belong
** to other database connections that happen to be sharing
** the cache with pBtree.
**
** This routine gets called when a rollback occurs.
** All cursors using the same cache must be tripped
** to prevent them from trying to use the btree after
** the rollback.  The rollback may have deleted tables
** or moved root pages, so it is not sufficient to
** save the state of the cursor.  The cursor must be
** invalidated.
*/
void sqlite3BtreeTripAllCursors(Btree *pBtree, int errCode){
  BtCursor *p;
  sqlite3BtreeEnter(pBtree);
  for(p=pBtree->pBt->pCursor; p; p=p->pNext){
    int i;
    sqlite3BtreeClearCursor(p);
    p->eState = CURSOR_FAULT;
    p->skip = errCode;
    for(i=0; i<=p->iPage; i++){
      releasePage(p->apPage[i]);
      p->apPage[i] = 0;
    }
  }
  sqlite3BtreeLeave(pBtree);
}

/*
** Rollback the transaction in progress.  All cursors will be
** invalided by this operation.  Any attempt to use a cursor
** that was open at the beginning of this operation will result
** in an error.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int sqlite3BtreeRollback(Btree *p){
  int rc;
  BtShared *pBt = p->pBt;
  MemPage *pPage1;

  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  rc = saveAllCursors(pBt, 0, 0);
#ifndef SQLITE_OMIT_SHARED_CACHE
  if( rc!=SQLITE_OK ){
    /* This is a horrible situation. An IO or malloc() error occured whilst
    ** trying to save cursor positions. If this is an automatic rollback (as
    ** the result of a constraint, malloc() failure or IO error) then 
    ** the cache may be internally inconsistent (not contain valid trees) so
    ** we cannot simply return the error to the caller. Instead, abort 
    ** all queries that may be using any of the cursors that failed to save.
    */
    sqlite3BtreeTripAllCursors(p, rc);
  }
#endif
  btreeIntegrity(p);
  unlockAllTables(p);

  if( p->inTrans==TRANS_WRITE ){
    int rc2;

    assert( TRANS_WRITE==pBt->inTransaction );
    rc2 = sqlite3PagerRollback(pBt->pPager);
    if( rc2!=SQLITE_OK ){
      rc = rc2;
    }

    /* The rollback may have destroyed the pPage1->aData value.  So
    ** call sqlite3BtreeGetPage() on page 1 again to make
    ** sure pPage1->aData is set correctly. */
    if( sqlite3BtreeGetPage(pBt, 1, &pPage1, 0)==SQLITE_OK ){
      releasePage(pPage1);
    }
    assert( countWriteCursors(pBt)==0 );
    pBt->inTransaction = TRANS_READ;
  }

  if( p->inTrans!=TRANS_NONE ){
    assert( pBt->nTransaction>0 );
    pBt->nTransaction--;
    if( 0==pBt->nTransaction ){
      pBt->inTransaction = TRANS_NONE;
    }
  }

  btreeClearHasContent(pBt);
  p->inTrans = TRANS_NONE;
  pBt->inStmt = 0;
  unlockBtreeIfUnused(pBt);

  btreeIntegrity(p);
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Start a statement subtransaction.  The subtransaction can
** can be rolled back independently of the main transaction.
** You must start a transaction before starting a subtransaction.
** The subtransaction is ended automatically if the main transaction
** commits or rolls back.
**
** Only one subtransaction may be active at a time.  It is an error to try
** to start a new subtransaction if another subtransaction is already active.
**
** Statement subtransactions are used around individual SQL statements
** that are contained within a BEGIN...COMMIT block.  If a constraint
** error occurs within the statement, the effect of that one statement
** can be rolled back without having to rollback the entire transaction.
*/
int sqlite3BtreeBeginStmt(Btree *p){
  int rc;
  BtShared *pBt = p->pBt;
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( p->inTrans==TRANS_WRITE );
  assert( !pBt->inStmt );
  assert( pBt->readOnly==0 );
  if( NEVER(p->inTrans!=TRANS_WRITE || pBt->inStmt || pBt->readOnly) ){
    rc = SQLITE_INTERNAL;
  }else{
    assert( pBt->inTransaction==TRANS_WRITE );
    /* At the pager level, a statement transaction is a savepoint with
    ** an index greater than all savepoints created explicitly using
    ** SQL statements. It is illegal to open, release or rollback any
    ** such savepoints while the statement transaction savepoint is active.
    */
    rc = sqlite3PagerOpenSavepoint(pBt->pPager, p->db->nSavepoint+1);
    pBt->inStmt = 1;
  }
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Commit the statment subtransaction currently in progress.  If no
** subtransaction is active, this is a no-op.
*/
int sqlite3BtreeCommitStmt(Btree *p){
  int rc;
  BtShared *pBt = p->pBt;
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( pBt->readOnly==0 );
  if( pBt->inStmt ){
    int iStmtpoint = p->db->nSavepoint;
    rc = sqlite3PagerSavepoint(pBt->pPager, SAVEPOINT_RELEASE, iStmtpoint);
  }else{
    rc = SQLITE_OK;
  }
  pBt->inStmt = 0;
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Rollback the active statement subtransaction.  If no subtransaction
** is active this routine is a no-op.
**
** All cursors will be invalidated by this operation.  Any attempt
** to use a cursor that was open at the beginning of this operation
** will result in an error.
*/
int sqlite3BtreeRollbackStmt(Btree *p){
  int rc = SQLITE_OK;
  BtShared *pBt = p->pBt;
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( pBt->readOnly==0 );
  if( pBt->inStmt ){
    int iStmtpoint = p->db->nSavepoint;
    rc = sqlite3PagerSavepoint(pBt->pPager, SAVEPOINT_ROLLBACK, iStmtpoint);
    if( rc==SQLITE_OK ){
      rc = sqlite3PagerSavepoint(pBt->pPager, SAVEPOINT_RELEASE, iStmtpoint);
    }
    pBt->inStmt = 0;
  }
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** The second argument to this function, op, is always SAVEPOINT_ROLLBACK
** or SAVEPOINT_RELEASE. This function either releases or rolls back the
** savepoint identified by parameter iSavepoint, depending on the value 
** of op.
**
** Normally, iSavepoint is greater than or equal to zero. However, if op is
** SAVEPOINT_ROLLBACK, then iSavepoint may also be -1. In this case the 
** contents of the entire transaction are rolled back. This is different
** from a normal transaction rollback, as no locks are released and the
** transaction remains open.
*/
int sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint){
  int rc = SQLITE_OK;
  if( p && p->inTrans==TRANS_WRITE ){
    BtShared *pBt = p->pBt;
    assert( pBt->inStmt==0 );
    assert( op==SAVEPOINT_RELEASE || op==SAVEPOINT_ROLLBACK );
    assert( iSavepoint>=0 || (iSavepoint==-1 && op==SAVEPOINT_ROLLBACK) );
    sqlite3BtreeEnter(p);
    pBt->db = p->db;
    rc = sqlite3PagerSavepoint(pBt->pPager, op, iSavepoint);
    if( rc==SQLITE_OK ){
      rc = newDatabase(pBt);
    }
    sqlite3BtreeLeave(p);
  }
  return rc;
}

/*
** Create a new cursor for the BTree whose root is on the page
** iTable.  The act of acquiring a cursor gets a read lock on 
** the database file.
**
** If wrFlag==0, then the cursor can only be used for reading.
** If wrFlag==1, then the cursor can be used for reading or for
** writing if other conditions for writing are also met.  These
** are the conditions that must be met in order for writing to
** be allowed:
**
** 1:  The cursor must have been opened with wrFlag==1
**
** 2:  Other database connections that share the same pager cache
**     but which are not in the READ_UNCOMMITTED state may not have
**     cursors open with wrFlag==0 on the same table.  Otherwise
**     the changes made by this write cursor would be visible to
**     the read cursors in the other database connection.
**
** 3:  The database must be writable (not on read-only media)
**
** 4:  There must be an active transaction.
**
** No checking is done to make sure that page iTable really is the
** root page of a b-tree.  If it is not, then the cursor acquired
** will not work correctly.
**
** It is assumed that the sqlite3BtreeCursorSize() bytes of memory 
** pointed to by pCur have been zeroed by the caller.
*/
static int btreeCursor(
  Btree *p,                              /* The btree */
  int iTable,                            /* Root page of table to open */
  int wrFlag,                            /* 1 to write. 0 read-only */
  struct KeyInfo *pKeyInfo,              /* First arg to comparison function */
  BtCursor *pCur                         /* Space for new cursor */
){
  int rc;
  Pgno nPage;
  BtShared *pBt = p->pBt;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( wrFlag==0 || wrFlag==1 );
  if( wrFlag ){
    assert( !pBt->readOnly );
    if( NEVER(pBt->readOnly) ){
      return SQLITE_READONLY;
    }
    if( checkReadLocks(p, iTable, 0, 0) ){
      return SQLITE_LOCKED;
    }
  }

  if( pBt->pPage1==0 ){
    rc = lockBtreeWithRetry(p);
    if( rc!=SQLITE_OK ){
      return rc;
    }
  }
  pCur->pgnoRoot = (Pgno)iTable;
  rc = sqlite3PagerPagecount(pBt->pPager, (int *)&nPage); 
  if( rc!=SQLITE_OK ){
    return rc;
  }
  if( iTable==1 && nPage==0 ){
    rc = SQLITE_EMPTY;
    goto create_cursor_exception;
  }
  rc = getAndInitPage(pBt, pCur->pgnoRoot, &pCur->apPage[0]);
  if( rc!=SQLITE_OK ){
    goto create_cursor_exception;
  }

  /* Now that no other errors can occur, finish filling in the BtCursor
  ** variables, link the cursor into the BtShared list and set *ppCur (the
  ** output argument to this function).
  */
  pCur->pKeyInfo = pKeyInfo;
  pCur->pBtree = p;
  pCur->pBt = pBt;
  pCur->wrFlag = (u8)wrFlag;
  pCur->pNext = pBt->pCursor;
  if( pCur->pNext ){
    pCur->pNext->pPrev = pCur;
  }
  pBt->pCursor = pCur;
  pCur->eState = CURSOR_INVALID;

  return SQLITE_OK;

create_cursor_exception:
  releasePage(pCur->apPage[0]);
  unlockBtreeIfUnused(pBt);
  return rc;
}
int sqlite3BtreeCursor(
  Btree *p,                                   /* The btree */
  int iTable,                                 /* Root page of table to open */
  int wrFlag,                                 /* 1 to write. 0 read-only */
  struct KeyInfo *pKeyInfo,                   /* First arg to xCompare() */
  BtCursor *pCur                              /* Write new cursor here */
){
  int rc;
  sqlite3BtreeEnter(p);
  p->pBt->db = p->db;
  rc = btreeCursor(p, iTable, wrFlag, pKeyInfo, pCur);
  sqlite3BtreeLeave(p);
  return rc;
}
int sqlite3BtreeCursorSize(){
  return sizeof(BtCursor);
}



/*
** Close a cursor.  The read lock on the database file is released
** when the last cursor is closed.
*/
int sqlite3BtreeCloseCursor(BtCursor *pCur){
  Btree *pBtree = pCur->pBtree;
  if( pBtree ){
    int i;
    BtShared *pBt = pCur->pBt;
    sqlite3BtreeEnter(pBtree);
    pBt->db = pBtree->db;
    sqlite3BtreeClearCursor(pCur);
    if( pCur->pPrev ){
      pCur->pPrev->pNext = pCur->pNext;
    }else{
      pBt->pCursor = pCur->pNext;
    }
    if( pCur->pNext ){
      pCur->pNext->pPrev = pCur->pPrev;
    }
    for(i=0; i<=pCur->iPage; i++){
      releasePage(pCur->apPage[i]);
    }
    unlockBtreeIfUnused(pBt);
    invalidateOverflowCache(pCur);
    /* sqlite3_free(pCur); */
    sqlite3BtreeLeave(pBtree);
  }
  return SQLITE_OK;
}

/*
** Make a temporary cursor by filling in the fields of pTempCur.
** The temporary cursor is not on the cursor list for the Btree.
*/
void sqlite3BtreeGetTempCursor(BtCursor *pCur, BtCursor *pTempCur){
  int i;
  assert( cursorHoldsMutex(pCur) );
  memcpy(pTempCur, pCur, sizeof(BtCursor));
  pTempCur->pNext = 0;
  pTempCur->pPrev = 0;
  for(i=0; i<=pTempCur->iPage; i++){
    sqlite3PagerRef(pTempCur->apPage[i]->pDbPage);
  }
  assert( pTempCur->pKey==0 );
}

/*
** Delete a temporary cursor such as was made by the CreateTemporaryCursor()
** function above.
*/
void sqlite3BtreeReleaseTempCursor(BtCursor *pCur){
  int i;
  assert( cursorHoldsMutex(pCur) );
  for(i=0; i<=pCur->iPage; i++){
    sqlite3PagerUnref(pCur->apPage[i]->pDbPage);
  }
  sqlite3_free(pCur->pKey);
}

/*
** Make sure the BtCursor* given in the argument has a valid
** BtCursor.info structure.  If it is not already valid, call
** sqlite3BtreeParseCell() to fill it in.
**
** BtCursor.info is a cache of the information in the current cell.
** Using this cache reduces the number of calls to sqlite3BtreeParseCell().
**
** 2007-06-25:  There is a bug in some versions of MSVC that cause the
** compiler to crash when getCellInfo() is implemented as a macro.
** But there is a measureable speed advantage to using the macro on gcc
** (when less compiler optimizations like -Os or -O0 are used and the
** compiler is not doing agressive inlining.)  So we use a real function
** for MSVC and a macro for everything else.  Ticket #2457.
*/
#ifndef NDEBUG
  static void assertCellInfo(BtCursor *pCur){
    CellInfo info;
    int iPage = pCur->iPage;
    memset(&info, 0, sizeof(info));
    sqlite3BtreeParseCell(pCur->apPage[iPage], pCur->aiIdx[iPage], &info);
    assert( memcmp(&info, &pCur->info, sizeof(info))==0 );
  }
#else
  #define assertCellInfo(x)
#endif
#ifdef _MSC_VER
  /* Use a real function in MSVC to work around bugs in that compiler. */
  static void getCellInfo(BtCursor *pCur){
    if( pCur->info.nSize==0 ){
      int iPage = pCur->iPage;
      sqlite3BtreeParseCell(pCur->apPage[iPage],pCur->aiIdx[iPage],&pCur->info);
      pCur->validNKey = 1;
    }else{
      assertCellInfo(pCur);
    }
  }
#else /* if not _MSC_VER */
  /* Use a macro in all other compilers so that the function is inlined */
#define getCellInfo(pCur)                                                      \
  if( pCur->info.nSize==0 ){                                                   \
    int iPage = pCur->iPage;                                                   \
    sqlite3BtreeParseCell(pCur->apPage[iPage],pCur->aiIdx[iPage],&pCur->info); \
    pCur->validNKey = 1;                                                       \
  }else{                                                                       \
    assertCellInfo(pCur);                                                      \
  }
#endif /* _MSC_VER */

/*
** Set *pSize to the size of the buffer needed to hold the value of
** the key for the current entry.  If the cursor is not pointing
** to a valid entry, *pSize is set to 0. 
**
** For a table with the INTKEY flag set, this routine returns the key
** itself, not the number of bytes in the key.
*/
int sqlite3BtreeKeySize(BtCursor *pCur, i64 *pSize){
  int rc;

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc==SQLITE_OK ){
    assert( pCur->eState==CURSOR_INVALID || pCur->eState==CURSOR_VALID );
    if( pCur->eState==CURSOR_INVALID ){
      *pSize = 0;
    }else{
      getCellInfo(pCur);
      *pSize = pCur->info.nKey;
    }
  }
  return rc;
}

/*
** Set *pSize to the number of bytes of data in the entry the
** cursor currently points to.  Always return SQLITE_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int sqlite3BtreeDataSize(BtCursor *pCur, u32 *pSize){
  int rc;

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc==SQLITE_OK ){
    assert( pCur->eState==CURSOR_INVALID || pCur->eState==CURSOR_VALID );
    if( pCur->eState==CURSOR_INVALID ){
      /* Not pointing at a valid entry - set *pSize to 0. */
      *pSize = 0;
    }else{
      getCellInfo(pCur);
      *pSize = pCur->info.nData;
    }
  }
  return rc;
}

/*
** Given the page number of an overflow page in the database (parameter
** ovfl), this function finds the page number of the next page in the 
** linked list of overflow pages. If possible, it uses the auto-vacuum
** pointer-map data instead of reading the content of page ovfl to do so. 
**
** If an error occurs an SQLite error code is returned. Otherwise:
**
** The page number of the next overflow page in the linked list is 
** written to *pPgnoNext. If page ovfl is the last page in its linked 
** list, *pPgnoNext is set to zero. 
**
** If ppPage is not NULL, and a reference to the MemPage object corresponding
** to page number pOvfl was obtained, then *ppPage is set to point to that
** reference. It is the responsibility of the caller to call releasePage()
** on *ppPage to free the reference. In no reference was obtained (because
** the pointer-map was used to obtain the value for *pPgnoNext), then
** *ppPage is set to zero.
*/
static int getOverflowPage(
  BtShared *pBt, 
  Pgno ovfl,                   /* Overflow page */
  MemPage **ppPage,            /* OUT: MemPage handle (may be NULL) */
  Pgno *pPgnoNext              /* OUT: Next overflow page number */
){
  Pgno next = 0;
  MemPage *pPage = 0;
  int rc = SQLITE_OK;

  assert( sqlite3_mutex_held(pBt->mutex) );
  assert(pPgnoNext);

#ifndef SQLITE_OMIT_AUTOVACUUM
  /* Try to find the next page in the overflow list using the
  ** autovacuum pointer-map pages. Guess that the next page in 
  ** the overflow list is page number (ovfl+1). If that guess turns 
  ** out to be wrong, fall back to loading the data of page 
  ** number ovfl to determine the next page number.
  */
  if( pBt->autoVacuum ){
    Pgno pgno;
    Pgno iGuess = ovfl+1;
    u8 eType;

    while( PTRMAP_ISPAGE(pBt, iGuess) || iGuess==PENDING_BYTE_PAGE(pBt) ){
      iGuess++;
    }

    if( iGuess<=pagerPagecount(pBt) ){
      rc = ptrmapGet(pBt, iGuess, &eType, &pgno);
      if( rc==SQLITE_OK && eType==PTRMAP_OVERFLOW2 && pgno==ovfl ){
        next = iGuess;
        rc = SQLITE_DONE;
      }
    }
  }
#endif

  if( rc==SQLITE_OK ){
    rc = sqlite3BtreeGetPage(pBt, ovfl, &pPage, 0);
    assert(rc==SQLITE_OK || pPage==0);
    if( next==0 && rc==SQLITE_OK ){
      next = get4byte(pPage->aData);
    }
  }

  *pPgnoNext = next;
  if( ppPage ){
    *ppPage = pPage;
  }else{
    releasePage(pPage);
  }
  return (rc==SQLITE_DONE ? SQLITE_OK : rc);
}

/*
** Copy data from a buffer to a page, or from a page to a buffer.
**
** pPayload is a pointer to data stored on database page pDbPage.
** If argument eOp is false, then nByte bytes of data are copied
** from pPayload to the buffer pointed at by pBuf. If eOp is true,
** then sqlite3PagerWrite() is called on pDbPage and nByte bytes
** of data are copied from the buffer pBuf to pPayload.
**
** SQLITE_OK is returned on success, otherwise an error code.
*/
static int copyPayload(
  void *pPayload,           /* Pointer to page data */
  void *pBuf,               /* Pointer to buffer */
  int nByte,                /* Number of bytes to copy */
  int eOp,                  /* 0 -> copy from page, 1 -> copy to page */
  DbPage *pDbPage           /* Page containing pPayload */
){
  if( eOp ){
    /* Copy data from buffer to page (a write operation) */
    int rc = sqlite3PagerWrite(pDbPage);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    memcpy(pPayload, pBuf, nByte);
  }else{
    /* Copy data from page to buffer (a read operation) */
    memcpy(pBuf, pPayload, nByte);
  }
  return SQLITE_OK;
}

/*
** This function is used to read or overwrite payload information
** for the entry that the pCur cursor is pointing to. If the eOp
** parameter is 0, this is a read operation (data copied into
** buffer pBuf). If it is non-zero, a write (data copied from
** buffer pBuf).
**
** A total of "amt" bytes are read or written beginning at "offset".
** Data is read to or from the buffer pBuf.
**
** This routine does not make a distinction between key and data.
** It just reads or writes bytes from the payload area.  Data might 
** appear on the main page or be scattered out on multiple overflow 
** pages.
**
** If the BtCursor.isIncrblobHandle flag is set, and the current
** cursor entry uses one or more overflow pages, this function
** allocates space for and lazily popluates the overflow page-list 
** cache array (BtCursor.aOverflow). Subsequent calls use this
** cache to make seeking to the supplied offset more efficient.
**
** Once an overflow page-list cache has been allocated, it may be
** invalidated if some other cursor writes to the same table, or if
** the cursor is moved to a different row. Additionally, in auto-vacuum
** mode, the following events may invalidate an overflow page-list cache.
**
**   * An incremental vacuum,
**   * A commit in auto_vacuum="full" mode,
**   * Creating a table (may require moving an overflow page).
*/
static int accessPayload(
  BtCursor *pCur,      /* Cursor pointing to entry to read from */
  u32 offset,          /* Begin reading this far into payload */
  u32 amt,             /* Read this many bytes */
  unsigned char *pBuf, /* Write the bytes into this buffer */ 
  int skipKey,         /* offset begins at data if this is true */
  int eOp              /* zero to read. non-zero to write. */
){
  unsigned char *aPayload;
  int rc = SQLITE_OK;
  u32 nKey;
  int iIdx = 0;
  MemPage *pPage = pCur->apPage[pCur->iPage]; /* Btree page of current entry */
  BtShared *pBt = pCur->pBt;                  /* Btree this cursor belongs to */

  assert( pPage );
  assert( pCur->eState==CURSOR_VALID );
  assert( pCur->aiIdx[pCur->iPage]<pPage->nCell );
  assert( cursorHoldsMutex(pCur) );

  getCellInfo(pCur);
  aPayload = pCur->info.pCell + pCur->info.nHeader;
  nKey = (pPage->intKey ? 0 : (int)pCur->info.nKey);

  if( skipKey ){
    offset += nKey;
  }
  if( offset+amt > nKey+pCur->info.nData 
   || &aPayload[pCur->info.nLocal] > &pPage->aData[pBt->usableSize]
  ){
    /* Trying to read or write past the end of the data is an error */
    return SQLITE_CORRUPT_BKPT;
  }

  /* Check if data must be read/written to/from the btree page itself. */
  if( offset<pCur->info.nLocal ){
    int a = amt;
    if( a+offset>pCur->info.nLocal ){
      a = pCur->info.nLocal - offset;
    }
    rc = copyPayload(&aPayload[offset], pBuf, a, eOp, pPage->pDbPage);
    offset = 0;
    pBuf += a;
    amt -= a;
  }else{
    offset -= pCur->info.nLocal;
  }

  if( rc==SQLITE_OK && amt>0 ){
    const u32 ovflSize = pBt->usableSize - 4;  /* Bytes content per ovfl page */
    Pgno nextPage;

    nextPage = get4byte(&aPayload[pCur->info.nLocal]);

#ifndef SQLITE_OMIT_INCRBLOB
    /* If the isIncrblobHandle flag is set and the BtCursor.aOverflow[]
    ** has not been allocated, allocate it now. The array is sized at
    ** one entry for each overflow page in the overflow chain. The
    ** page number of the first overflow page is stored in aOverflow[0],
    ** etc. A value of 0 in the aOverflow[] array means "not yet known"
    ** (the cache is lazily populated).
    */
    if( pCur->isIncrblobHandle && !pCur->aOverflow ){
      int nOvfl = (pCur->info.nPayload-pCur->info.nLocal+ovflSize-1)/ovflSize;
      pCur->aOverflow = (Pgno *)sqlite3MallocZero(sizeof(Pgno)*nOvfl);
      if( nOvfl && !pCur->aOverflow ){
        rc = SQLITE_NOMEM;
      }
    }

    /* If the overflow page-list cache has been allocated and the
    ** entry for the first required overflow page is valid, skip
    ** directly to it.
    */
    if( pCur->aOverflow && pCur->aOverflow[offset/ovflSize] ){
      iIdx = (offset/ovflSize);
      nextPage = pCur->aOverflow[iIdx];
      offset = (offset%ovflSize);
    }
#endif

    for( ; rc==SQLITE_OK && amt>0 && nextPage; iIdx++){

#ifndef SQLITE_OMIT_INCRBLOB
      /* If required, populate the overflow page-list cache. */
      if( pCur->aOverflow ){
        assert(!pCur->aOverflow[iIdx] || pCur->aOverflow[iIdx]==nextPage);
        pCur->aOverflow[iIdx] = nextPage;
      }
#endif

      if( offset>=ovflSize ){
        /* The only reason to read this page is to obtain the page
        ** number for the next page in the overflow chain. The page
        ** data is not required. So first try to lookup the overflow
        ** page-list cache, if any, then fall back to the getOverflowPage()
        ** function.
        */
#ifndef SQLITE_OMIT_INCRBLOB
        if( pCur->aOverflow && pCur->aOverflow[iIdx+1] ){
          nextPage = pCur->aOverflow[iIdx+1];
        } else 
#endif
          rc = getOverflowPage(pBt, nextPage, 0, &nextPage);
        offset -= ovflSize;
      }else{
        /* Need to read this page properly. It contains some of the
        ** range of data that is being read (eOp==0) or written (eOp!=0).
        */
        DbPage *pDbPage;
        int a = amt;
        rc = sqlite3PagerGet(pBt->pPager, nextPage, &pDbPage);
        if( rc==SQLITE_OK ){
          aPayload = sqlite3PagerGetData(pDbPage);
          nextPage = get4byte(aPayload);
          if( a + offset > ovflSize ){
            a = ovflSize - offset;
          }
          rc = copyPayload(&aPayload[offset+4], pBuf, a, eOp, pDbPage);
          sqlite3PagerUnref(pDbPage);
          offset = 0;
          amt -= a;
          pBuf += a;
        }
      }
    }
  }

  if( rc==SQLITE_OK && amt>0 ){
    return SQLITE_CORRUPT_BKPT;
  }
  return rc;
}

/*
** Read part of the key associated with cursor pCur.  Exactly
** "amt" bytes will be transfered into pBuf[].  The transfer
** begins at "offset".
**
** Return SQLITE_OK on success or an error code if anything goes
** wrong.  An error is returned if "offset+amt" is larger than
** the available payload.
*/
int sqlite3BtreeKey(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  int rc;

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc==SQLITE_OK ){
    assert( pCur->eState==CURSOR_VALID );
    assert( pCur->iPage>=0 && pCur->apPage[pCur->iPage] );
    if( pCur->apPage[0]->intKey ){
      return SQLITE_CORRUPT_BKPT;
    }
    assert( pCur->aiIdx[pCur->iPage]<pCur->apPage[pCur->iPage]->nCell );
    rc = accessPayload(pCur, offset, amt, (unsigned char*)pBuf, 0, 0);
  }
  return rc;
}

/*
** Read part of the data associated with cursor pCur.  Exactly
** "amt" bytes will be transfered into pBuf[].  The transfer
** begins at "offset".
**
** Return SQLITE_OK on success or an error code if anything goes
** wrong.  An error is returned if "offset+amt" is larger than
** the available payload.
*/
int sqlite3BtreeData(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  int rc;

#ifndef SQLITE_OMIT_INCRBLOB
  if ( pCur->eState==CURSOR_INVALID ){
    return SQLITE_ABORT;
  }
#endif

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc==SQLITE_OK ){
    assert( pCur->eState==CURSOR_VALID );
    assert( pCur->iPage>=0 && pCur->apPage[pCur->iPage] );
    assert( pCur->aiIdx[pCur->iPage]<pCur->apPage[pCur->iPage]->nCell );
    rc = accessPayload(pCur, offset, amt, pBuf, 1, 0);
  }
  return rc;
}

/*
** Return a pointer to payload information from the entry that the 
** pCur cursor is pointing to.  The pointer is to the beginning of
** the key if skipKey==0 and it points to the beginning of data if
** skipKey==1.  The number of bytes of available key/data is written
** into *pAmt.  If *pAmt==0, then the value returned will not be
** a valid pointer.
**
** This routine is an optimization.  It is common for the entire key
** and data to fit on the local page and for there to be no overflow
** pages.  When that is so, this routine can be used to access the
** key and data without making a copy.  If the key and/or data spills
** onto overflow pages, then accessPayload() must be used to reassembly
** the key/data and copy it into a preallocated buffer.
**
** The pointer returned by this routine looks directly into the cached
** page of the database.  The data might change or move the next time
** any btree routine is called.
*/
static const unsigned char *fetchPayload(
  BtCursor *pCur,      /* Cursor pointing to entry to read from */
  int *pAmt,           /* Write the number of available bytes here */
  int skipKey          /* read beginning at data if this is true */
){
  unsigned char *aPayload;
  MemPage *pPage;
  u32 nKey;
  u32 nLocal;

  assert( pCur!=0 && pCur->iPage>=0 && pCur->apPage[pCur->iPage]);
  assert( pCur->eState==CURSOR_VALID );
  assert( cursorHoldsMutex(pCur) );
  pPage = pCur->apPage[pCur->iPage];
  assert( pCur->aiIdx[pCur->iPage]<pPage->nCell );
  getCellInfo(pCur);
  aPayload = pCur->info.pCell;
  aPayload += pCur->info.nHeader;
  if( pPage->intKey ){
    nKey = 0;
  }else{
    nKey = (int)pCur->info.nKey;
  }
  if( skipKey ){
    aPayload += nKey;
    nLocal = pCur->info.nLocal - nKey;
  }else{
    nLocal = pCur->info.nLocal;
    if( nLocal>nKey ){
      nLocal = nKey;
    }
  }
  *pAmt = nLocal;
  return aPayload;
}


/*
** For the entry that cursor pCur is point to, return as
** many bytes of the key or data as are available on the local
** b-tree page.  Write the number of available bytes into *pAmt.
**
** The pointer returned is ephemeral.  The key/data may move
** or be destroyed on the next call to any Btree routine,
** including calls from other threads against the same cache.
** Hence, a mutex on the BtShared should be held prior to calling
** this routine.
**
** These routines is used to get quick access to key and data
** in the common case where no overflow pages are used.
*/
const void *sqlite3BtreeKeyFetch(BtCursor *pCur, int *pAmt){
  assert( cursorHoldsMutex(pCur) );
  if( pCur->eState==CURSOR_VALID ){
    return (const void*)fetchPayload(pCur, pAmt, 0);
  }
  return 0;
}
const void *sqlite3BtreeDataFetch(BtCursor *pCur, int *pAmt){
  assert( cursorHoldsMutex(pCur) );
  if( pCur->eState==CURSOR_VALID ){
    return (const void*)fetchPayload(pCur, pAmt, 1);
  }
  return 0;
}


/*
** Move the cursor down to a new child page.  The newPgno argument is the
** page number of the child page to move to.
*/
static int moveToChild(BtCursor *pCur, u32 newPgno){
  int rc;
  int i = pCur->iPage;
  MemPage *pNewPage;
  BtShared *pBt = pCur->pBt;

  assert( cursorHoldsMutex(pCur) );
  assert( pCur->eState==CURSOR_VALID );
  assert( pCur->iPage<BTCURSOR_MAX_DEPTH );
  if( pCur->iPage>=(BTCURSOR_MAX_DEPTH-1) ){
    return SQLITE_CORRUPT_BKPT;
  }
  rc = getAndInitPage(pBt, newPgno, &pNewPage);
  if( rc ) return rc;
  pCur->apPage[i+1] = pNewPage;
  pCur->aiIdx[i+1] = 0;
  pCur->iPage++;

  pCur->info.nSize = 0;
  pCur->validNKey = 0;
  if( pNewPage->nCell<1 ){
    return SQLITE_CORRUPT_BKPT;
  }
  return SQLITE_OK;
}

#ifndef NDEBUG
/*
** Page pParent is an internal (non-leaf) tree page. This function 
** asserts that page number iChild is the left-child if the iIdx'th
** cell in page pParent. Or, if iIdx is equal to the total number of
** cells in pParent, that page number iChild is the right-child of
** the page.
*/
static void assertParentIndex(MemPage *pParent, int iIdx, Pgno iChild){
  assert( iIdx<=pParent->nCell );
  if( iIdx==pParent->nCell ){
    assert( get4byte(&pParent->aData[pParent->hdrOffset+8])==iChild );
  }else{
    assert( get4byte(findCell(pParent, iIdx))==iChild );
  }
}
#else
#  define assertParentIndex(x,y,z) 
#endif

/*
** Move the cursor up to the parent page.
**
** pCur->idx is set to the cell index that contains the pointer
** to the page we are coming from.  If we are coming from the
** right-most child page then pCur->idx is set to one more than
** the largest cell index.
*/
void sqlite3BtreeMoveToParent(BtCursor *pCur){
  assert( cursorHoldsMutex(pCur) );
  assert( pCur->eState==CURSOR_VALID );
  assert( pCur->iPage>0 );
  assert( pCur->apPage[pCur->iPage] );
  assertParentIndex(
    pCur->apPage[pCur->iPage-1], 
    pCur->aiIdx[pCur->iPage-1], 
    pCur->apPage[pCur->iPage]->pgno
  );
  releasePage(pCur->apPage[pCur->iPage]);
  pCur->iPage--;
  pCur->info.nSize = 0;
  pCur->validNKey = 0;
}

/*
** Move the cursor to the root page
*/
static int moveToRoot(BtCursor *pCur){
  MemPage *pRoot;
  int rc = SQLITE_OK;
  Btree *p = pCur->pBtree;
  BtShared *pBt = p->pBt;

  assert( cursorHoldsMutex(pCur) );
  assert( CURSOR_INVALID < CURSOR_REQUIRESEEK );
  assert( CURSOR_VALID   < CURSOR_REQUIRESEEK );
  assert( CURSOR_FAULT   > CURSOR_REQUIRESEEK );
  if( pCur->eState>=CURSOR_REQUIRESEEK ){
    if( pCur->eState==CURSOR_FAULT ){
      return pCur->skip;
    }
    sqlite3BtreeClearCursor(pCur);
  }

  if( pCur->iPage>=0 ){
    int i;
    for(i=1; i<=pCur->iPage; i++){
      releasePage(pCur->apPage[i]);
    }
  }else{
    if( 
      SQLITE_OK!=(rc = getAndInitPage(pBt, pCur->pgnoRoot, &pCur->apPage[0]))
    ){
      pCur->eState = CURSOR_INVALID;
      return rc;
    }
  }

  pRoot = pCur->apPage[0];
  assert( pRoot->pgno==pCur->pgnoRoot );
  pCur->iPage = 0;
  pCur->aiIdx[0] = 0;
  pCur->info.nSize = 0;
  pCur->atLast = 0;
  pCur->validNKey = 0;

  if( pRoot->nCell==0 && !pRoot->leaf ){
    Pgno subpage;
    assert( pRoot->pgno==1 );
    subpage = get4byte(&pRoot->aData[pRoot->hdrOffset+8]);
    assert( subpage>0 );
    pCur->eState = CURSOR_VALID;
    rc = moveToChild(pCur, subpage);
  }else{
    pCur->eState = ((pRoot->nCell>0)?CURSOR_VALID:CURSOR_INVALID);
  }
  return rc;
}

/*
** Move the cursor down to the left-most leaf entry beneath the
** entry to which it is currently pointing.
**
** The left-most leaf is the one with the smallest key - the first
** in ascending order.
*/
static int moveToLeftmost(BtCursor *pCur){
  Pgno pgno;
  int rc = SQLITE_OK;
  MemPage *pPage;

  assert( cursorHoldsMutex(pCur) );
  assert( pCur->eState==CURSOR_VALID );
  while( rc==SQLITE_OK && !(pPage = pCur->apPage[pCur->iPage])->leaf ){
    assert( pCur->aiIdx[pCur->iPage]<pPage->nCell );
    pgno = get4byte(findCell(pPage, pCur->aiIdx[pCur->iPage]));
    rc = moveToChild(pCur, pgno);
  }
  return rc;
}

/*
** Move the cursor down to the right-most leaf entry beneath the
** page to which it is currently pointing.  Notice the difference
** between moveToLeftmost() and moveToRightmost().  moveToLeftmost()
** finds the left-most entry beneath the *entry* whereas moveToRightmost()
** finds the right-most entry beneath the *page*.
**
** The right-most entry is the one with the largest key - the last
** key in ascending order.
*/
static int moveToRightmost(BtCursor *pCur){
  Pgno pgno;
  int rc = SQLITE_OK;
  MemPage *pPage = 0;

  assert( cursorHoldsMutex(pCur) );
  assert( pCur->eState==CURSOR_VALID );
  while( rc==SQLITE_OK && !(pPage = pCur->apPage[pCur->iPage])->leaf ){
    pgno = get4byte(&pPage->aData[pPage->hdrOffset+8]);
    pCur->aiIdx[pCur->iPage] = pPage->nCell;
    rc = moveToChild(pCur, pgno);
  }
  if( rc==SQLITE_OK ){
    pCur->aiIdx[pCur->iPage] = pPage->nCell-1;
    pCur->info.nSize = 0;
    pCur->validNKey = 0;
  }
  return rc;
}

/* Move the cursor to the first entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqlite3BtreeFirst(BtCursor *pCur, int *pRes){
  int rc;

  assert( cursorHoldsMutex(pCur) );
  assert( sqlite3_mutex_held(pCur->pBtree->db->mutex) );
  rc = moveToRoot(pCur);
  if( rc==SQLITE_OK ){
    if( pCur->eState==CURSOR_INVALID ){
      assert( pCur->apPage[pCur->iPage]->nCell==0 );
      *pRes = 1;
      rc = SQLITE_OK;
    }else{
      assert( pCur->apPage[pCur->iPage]->nCell>0 );
      *pRes = 0;
      rc = moveToLeftmost(pCur);
    }
  }
  return rc;
}

/* Move the cursor to the last entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
*/
int sqlite3BtreeLast(BtCursor *pCur, int *pRes){
  int rc;
 
  assert( cursorHoldsMutex(pCur) );
  assert( sqlite3_mutex_held(pCur->pBtree->db->mutex) );
  rc = moveToRoot(pCur);
  if( rc==SQLITE_OK ){
    if( CURSOR_INVALID==pCur->eState ){
      assert( pCur->apPage[pCur->iPage]->nCell==0 );
      *pRes = 1;
    }else{
      assert( pCur->eState==CURSOR_VALID );
      *pRes = 0;
      rc = moveToRightmost(pCur);
      getCellInfo(pCur);
      pCur->atLast = rc==SQLITE_OK ?1:0;
    }
  }
  return rc;
}

/* Move the cursor so that it points to an entry near the key 
** specified by pIdxKey or intKey.   Return a success code.
**
** For INTKEY tables, the intKey parameter is used.  pIdxKey 
** must be NULL.  For index tables, pIdxKey is used and intKey
** is ignored.
**
** If an exact match is not found, then the cursor is always
** left pointing at a leaf page which would hold the entry if it
** were present.  The cursor might point to an entry that comes
** before or after the key.
**
** An integer is written into *pRes which is the result of
** comparing the key with the entry to which the cursor is 
** pointing.  The meaning of the integer written into
** *pRes is as follows:
**
**     *pRes<0      The cursor is left pointing at an entry that
**                  is smaller than intKey/pIdxKey or if the table is empty
**                  and the cursor is therefore left point to nothing.
**
**     *pRes==0     The cursor is left pointing at an entry that
**                  exactly matches intKey/pIdxKey.
**
**     *pRes>0      The cursor is left pointing at an entry that
**                  is larger than intKey/pIdxKey.
**
*/
int sqlite3BtreeMovetoUnpacked(
  BtCursor *pCur,          /* The cursor to be moved */
  UnpackedRecord *pIdxKey, /* Unpacked index key */
  i64 intKey,              /* The table key */
  int biasRight,           /* If true, bias the search to the high end */
  int *pRes                /* Write search results here */
){
  int rc;

  assert( cursorHoldsMutex(pCur) );
  assert( sqlite3_mutex_held(pCur->pBtree->db->mutex) );

  /* If the cursor is already positioned at the point we are trying
  ** to move to, then just return without doing any work */
  if( pCur->eState==CURSOR_VALID && pCur->validNKey 
   && pCur->apPage[0]->intKey 
  ){
    if( pCur->info.nKey==intKey ){
      *pRes = 0;
      return SQLITE_OK;
    }
    if( pCur->atLast && pCur->info.nKey<intKey ){
      *pRes = -1;
      return SQLITE_OK;
    }
  }

  rc = moveToRoot(pCur);
  if( rc ){
    return rc;
  }
  assert( pCur->apPage[pCur->iPage] );
  assert( pCur->apPage[pCur->iPage]->isInit );
  if( pCur->eState==CURSOR_INVALID ){
    *pRes = -1;
    assert( pCur->apPage[pCur->iPage]->nCell==0 );
    return SQLITE_OK;
  }
  assert( pCur->apPage[0]->intKey || pIdxKey );
  for(;;){
    int lwr, upr;
    Pgno chldPg;
    MemPage *pPage = pCur->apPage[pCur->iPage];
    int c = -1;  /* pRes return if table is empty must be -1 */
    lwr = 0;
    upr = pPage->nCell-1;
    if( (!pPage->intKey && pIdxKey==0) || upr<0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto moveto_finish;
    }
    if( biasRight ){
      pCur->aiIdx[pCur->iPage] = (u16)upr;
    }else{
      pCur->aiIdx[pCur->iPage] = (u16)((upr+lwr)/2);
    }
    for(;;){
      void *pCellKey;
      i64 nCellKey;
      int idx = pCur->aiIdx[pCur->iPage];
      pCur->info.nSize = 0;
      pCur->validNKey = 1;
      if( pPage->intKey ){
        u8 *pCell;
        pCell = findCell(pPage, idx) + pPage->childPtrSize;
        if( pPage->hasData ){
          u32 dummy;
          pCell += getVarint32(pCell, dummy);
        }
        getVarint(pCell, (u64*)&nCellKey);
        if( nCellKey==intKey ){
          c = 0;
        }else if( nCellKey<intKey ){
          c = -1;
        }else{
          assert( nCellKey>intKey );
          c = +1;
        }
      }else{
        int available;
        pCellKey = (void *)fetchPayload(pCur, &available, 0);
        nCellKey = pCur->info.nKey;
        if( available>=nCellKey ){
          c = sqlite3VdbeRecordCompare((int)nCellKey, pCellKey, pIdxKey);
        }else{
          pCellKey = sqlite3Malloc( (int)nCellKey );
          if( pCellKey==0 ){
            rc = SQLITE_NOMEM;
            goto moveto_finish;
          }
          rc = sqlite3BtreeKey(pCur, 0, (int)nCellKey, (void*)pCellKey);
          c = sqlite3VdbeRecordCompare((int)nCellKey, pCellKey, pIdxKey);
          sqlite3_free(pCellKey);
          if( rc ) goto moveto_finish;
        }
      }
      if( c==0 ){
        pCur->info.nKey = nCellKey;
        if( pPage->intKey && !pPage->leaf ){
          lwr = idx;
          upr = lwr - 1;
          break;
        }else{
          *pRes = 0;
          rc = SQLITE_OK;
          goto moveto_finish;
        }
      }
      if( c<0 ){
        lwr = idx+1;
      }else{
        upr = idx-1;
      }
      if( lwr>upr ){
        pCur->info.nKey = nCellKey;
        break;
      }
      pCur->aiIdx[pCur->iPage] = (u16)((lwr+upr)/2);
    }
    assert( lwr==upr+1 );
    assert( pPage->isInit );
    if( pPage->leaf ){
      chldPg = 0;
    }else if( lwr>=pPage->nCell ){
      chldPg = get4byte(&pPage->aData[pPage->hdrOffset+8]);
    }else{
      chldPg = get4byte(findCell(pPage, lwr));
    }
    if( chldPg==0 ){
      assert( pCur->aiIdx[pCur->iPage]<pCur->apPage[pCur->iPage]->nCell );
      if( pRes ) *pRes = c;
      rc = SQLITE_OK;
      goto moveto_finish;
    }
    pCur->aiIdx[pCur->iPage] = (u16)lwr;
    pCur->info.nSize = 0;
    pCur->validNKey = 0;
    rc = moveToChild(pCur, chldPg);
    if( rc ) goto moveto_finish;
  }
moveto_finish:
  return rc;
}

/*
** In this version of BtreeMoveto, pKey is a packed index record
** such as is generated by the OP_MakeRecord opcode.  Unpack the
** record and then call BtreeMovetoUnpacked() to do the work.
*/
int sqlite3BtreeMoveto(
  BtCursor *pCur,     /* Cursor open on the btree to be searched */
  const void *pKey,   /* Packed key if the btree is an index */
  i64 nKey,           /* Integer key for tables.  Size of pKey for indices */
  int bias,           /* Bias search to the high end */
  int *pRes           /* Write search results here */
){
  int rc;                    /* Status code */
  UnpackedRecord *pIdxKey;   /* Unpacked index key */
  UnpackedRecord aSpace[16]; /* Temp space for pIdxKey - to avoid a malloc */

  if( pKey ){
    assert( nKey==(i64)(int)nKey );
    pIdxKey = sqlite3VdbeRecordUnpack(pCur->pKeyInfo, (int)nKey, pKey,
                                      aSpace, sizeof(aSpace));
    if( pIdxKey==0 ) return SQLITE_NOMEM;
  }else{
    pIdxKey = 0;
  }
  rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, nKey, bias, pRes);
  if( pKey ){
    sqlite3VdbeDeleteUnpackedRecord(pIdxKey);
  }
  return rc;
}


/*
** Return TRUE if the cursor is not pointing at an entry of the table.
**
** TRUE will be returned after a call to sqlite3BtreeNext() moves
** past the last entry in the table or sqlite3BtreePrev() moves past
** the first entry.  TRUE is also returned if the table is empty.
*/
int sqlite3BtreeEof(BtCursor *pCur){
  /* TODO: What if the cursor is in CURSOR_REQUIRESEEK but all table entries
  ** have been deleted? This API will need to change to return an error code
  ** as well as the boolean result value.
  */
  return (CURSOR_VALID!=pCur->eState);
}

/*
** Return the database connection handle for a cursor.
*/
sqlite3 *sqlite3BtreeCursorDb(const BtCursor *pCur){
  assert( sqlite3_mutex_held(pCur->pBtree->db->mutex) );
  return pCur->pBtree->db;
}

/*
** Advance the cursor to the next entry in the database.  If
** successful then set *pRes=0.  If the cursor
** was already pointing to the last entry in the database before
** this routine was called, then set *pRes=1.
*/
int sqlite3BtreeNext(BtCursor *pCur, int *pRes){
  int rc;
  int idx;
  MemPage *pPage;

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  assert( pRes!=0 );
  if( CURSOR_INVALID==pCur->eState ){
    *pRes = 1;
    return SQLITE_OK;
  }
  if( pCur->skip>0 ){
    pCur->skip = 0;
    *pRes = 0;
    return SQLITE_OK;
  }
  pCur->skip = 0;

  pPage = pCur->apPage[pCur->iPage];
  idx = ++pCur->aiIdx[pCur->iPage];
  assert( pPage->isInit );
  assert( idx<=pPage->nCell );

  pCur->info.nSize = 0;
  pCur->validNKey = 0;
  if( idx>=pPage->nCell ){
    if( !pPage->leaf ){
      rc = moveToChild(pCur, get4byte(&pPage->aData[pPage->hdrOffset+8]));
      if( rc ) return rc;
      rc = moveToLeftmost(pCur);
      *pRes = 0;
      return rc;
    }
    do{
      if( pCur->iPage==0 ){
        *pRes = 1;
        pCur->eState = CURSOR_INVALID;
        return SQLITE_OK;
      }
      sqlite3BtreeMoveToParent(pCur);
      pPage = pCur->apPage[pCur->iPage];
    }while( pCur->aiIdx[pCur->iPage]>=pPage->nCell );
    *pRes = 0;
    if( pPage->intKey ){
      rc = sqlite3BtreeNext(pCur, pRes);
    }else{
      rc = SQLITE_OK;
    }
    return rc;
  }
  *pRes = 0;
  if( pPage->leaf ){
    return SQLITE_OK;
  }
  rc = moveToLeftmost(pCur);
  return rc;
}


/*
** Step the cursor to the back to the previous entry in the database.  If
** successful then set *pRes=0.  If the cursor
** was already pointing to the first entry in the database before
** this routine was called, then set *pRes=1.
*/
int sqlite3BtreePrevious(BtCursor *pCur, int *pRes){
  int rc;
  MemPage *pPage;

  assert( cursorHoldsMutex(pCur) );
  rc = restoreCursorPosition(pCur);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  pCur->atLast = 0;
  if( CURSOR_INVALID==pCur->eState ){
    *pRes = 1;
    return SQLITE_OK;
  }
  if( pCur->skip<0 ){
    pCur->skip = 0;
    *pRes = 0;
    return SQLITE_OK;
  }
  pCur->skip = 0;

  pPage = pCur->apPage[pCur->iPage];
  assert( pPage->isInit );
  if( !pPage->leaf ){
    int idx = pCur->aiIdx[pCur->iPage];
    rc = moveToChild(pCur, get4byte(findCell(pPage, idx)));
    if( rc ){
      return rc;
    }
    rc = moveToRightmost(pCur);
  }else{
    while( pCur->aiIdx[pCur->iPage]==0 ){
      if( pCur->iPage==0 ){
        pCur->eState = CURSOR_INVALID;
        *pRes = 1;
        return SQLITE_OK;
      }
      sqlite3BtreeMoveToParent(pCur);
    }
    pCur->info.nSize = 0;
    pCur->validNKey = 0;

    pCur->aiIdx[pCur->iPage]--;
    pPage = pCur->apPage[pCur->iPage];
    if( pPage->intKey && !pPage->leaf ){
      rc = sqlite3BtreePrevious(pCur, pRes);
    }else{
      rc = SQLITE_OK;
    }
  }
  *pRes = 0;
  return rc;
}

/*
** Allocate a new page from the database file.
**
** The new page is marked as dirty.  (In other words, sqlite3PagerWrite()
** has already been called on the new page.)  The new page has also
** been referenced and the calling routine is responsible for calling
** sqlite3PagerUnref() on the new page when it is done.
**
** SQLITE_OK is returned on success.  Any other return value indicates
** an error.  *ppPage and *pPgno are undefined in the event of an error.
** Do not invoke sqlite3PagerUnref() on *ppPage if an error is returned.
**
** If the "nearby" parameter is not 0, then a (feeble) effort is made to 
** locate a page close to the page number "nearby".  This can be used in an
** attempt to keep related pages close to each other in the database file,
** which in turn can make database access faster.
**
** If the "exact" parameter is not 0, and the page-number nearby exists 
** anywhere on the free-list, then it is guarenteed to be returned. This
** is only used by auto-vacuum databases when allocating a new table.
*/
static int allocateBtreePage(
  BtShared *pBt, 
  MemPage **ppPage, 
  Pgno *pPgno, 
  Pgno nearby,
  u8 exact
){
  MemPage *pPage1;
  int rc;
  int n;     /* Number of pages on the freelist */
  int k;     /* Number of leaves on the trunk of the freelist */
  MemPage *pTrunk = 0;
  MemPage *pPrevTrunk = 0;

  assert( sqlite3_mutex_held(pBt->mutex) );
  pPage1 = pBt->pPage1;
  n = get4byte(&pPage1->aData[36]);
  if( n>0 ){
    /* There are pages on the freelist.  Reuse one of those pages. */
    Pgno iTrunk;
    u8 searchList = 0; /* If the free-list must be searched for 'nearby' */
    
    /* If the 'exact' parameter was true and a query of the pointer-map
    ** shows that the page 'nearby' is somewhere on the free-list, then
    ** the entire-list will be searched for that page.
    */
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( exact && nearby<=pagerPagecount(pBt) ){
      u8 eType;
      assert( nearby>0 );
      assert( pBt->autoVacuum );
      rc = ptrmapGet(pBt, nearby, &eType, 0);
      if( rc ) return rc;
      if( eType==PTRMAP_FREEPAGE ){
        searchList = 1;
      }
      *pPgno = nearby;
    }
#endif

    /* Decrement the free-list count by 1. Set iTrunk to the index of the
    ** first free-list trunk page. iPrevTrunk is initially 1.
    */
    rc = sqlite3PagerWrite(pPage1->pDbPage);
    if( rc ) return rc;
    put4byte(&pPage1->aData[36], n-1);

    /* The code within this loop is run only once if the 'searchList' variable
    ** is not true. Otherwise, it runs once for each trunk-page on the
    ** free-list until the page 'nearby' is located.
    */
    do {
      pPrevTrunk = pTrunk;
      if( pPrevTrunk ){
        iTrunk = get4byte(&pPrevTrunk->aData[0]);
      }else{
        iTrunk = get4byte(&pPage1->aData[32]);
      }
      rc = sqlite3BtreeGetPage(pBt, iTrunk, &pTrunk, 0);
      if( rc ){
        pTrunk = 0;
        goto end_allocate_page;
      }

      k = get4byte(&pTrunk->aData[4]);
      if( k==0 && !searchList ){
        /* The trunk has no leaves and the list is not being searched. 
        ** So extract the trunk page itself and use it as the newly 
        ** allocated page */
        assert( pPrevTrunk==0 );
        rc = sqlite3PagerWrite(pTrunk->pDbPage);
        if( rc ){
          goto end_allocate_page;
        }
        *pPgno = iTrunk;
        memcpy(&pPage1->aData[32], &pTrunk->aData[0], 4);
        *ppPage = pTrunk;
        pTrunk = 0;
        TRACE(("ALLOCATE: %d trunk - %d free pages left\n", *pPgno, n-1));
      }else if( k>pBt->usableSize/4 - 2 ){
        /* Value of k is out of range.  Database corruption */
        rc = SQLITE_CORRUPT_BKPT;
        goto end_allocate_page;
#ifndef SQLITE_OMIT_AUTOVACUUM
      }else if( searchList && nearby==iTrunk ){
        /* The list is being searched and this trunk page is the page
        ** to allocate, regardless of whether it has leaves.
        */
        assert( *pPgno==iTrunk );
        *ppPage = pTrunk;
        searchList = 0;
        rc = sqlite3PagerWrite(pTrunk->pDbPage);
        if( rc ){
          goto end_allocate_page;
        }
        if( k==0 ){
          if( !pPrevTrunk ){
            memcpy(&pPage1->aData[32], &pTrunk->aData[0], 4);
          }else{
            memcpy(&pPrevTrunk->aData[0], &pTrunk->aData[0], 4);
          }
        }else{
          /* The trunk page is required by the caller but it contains 
          ** pointers to free-list leaves. The first leaf becomes a trunk
          ** page in this case.
          */
          MemPage *pNewTrunk;
          Pgno iNewTrunk = get4byte(&pTrunk->aData[8]);
          rc = sqlite3BtreeGetPage(pBt, iNewTrunk, &pNewTrunk, 0);
          if( rc!=SQLITE_OK ){
            goto end_allocate_page;
          }
          rc = sqlite3PagerWrite(pNewTrunk->pDbPage);
          if( rc!=SQLITE_OK ){
            releasePage(pNewTrunk);
            goto end_allocate_page;
          }
          memcpy(&pNewTrunk->aData[0], &pTrunk->aData[0], 4);
          put4byte(&pNewTrunk->aData[4], k-1);
          memcpy(&pNewTrunk->aData[8], &pTrunk->aData[12], (k-1)*4);
          releasePage(pNewTrunk);
          if( !pPrevTrunk ){
            assert( sqlite3PagerIswriteable(pPage1->pDbPage) );
            put4byte(&pPage1->aData[32], iNewTrunk);
          }else{
            rc = sqlite3PagerWrite(pPrevTrunk->pDbPage);
            if( rc ){
              goto end_allocate_page;
            }
            put4byte(&pPrevTrunk->aData[0], iNewTrunk);
          }
        }
        pTrunk = 0;
        TRACE(("ALLOCATE: %d trunk - %d free pages left\n", *pPgno, n-1));
#endif
      }else{
        /* Extract a leaf from the trunk */
        int closest;
        Pgno iPage;
        unsigned char *aData = pTrunk->aData;
        rc = sqlite3PagerWrite(pTrunk->pDbPage);
        if( rc ){
          goto end_allocate_page;
        }
        if( nearby>0 ){
          int i, dist;
          closest = 0;
          dist = get4byte(&aData[8]) - nearby;
          if( dist<0 ) dist = -dist;
          for(i=1; i<k; i++){
            int d2 = get4byte(&aData[8+i*4]) - nearby;
            if( d2<0 ) d2 = -d2;
            if( d2<dist ){
              closest = i;
              dist = d2;
            }
          }
        }else{
          closest = 0;
        }

        iPage = get4byte(&aData[8+closest*4]);
        if( !searchList || iPage==nearby ){
          int noContent;
          Pgno nPage;
          *pPgno = iPage;
          nPage = pagerPagecount(pBt);
          if( *pPgno>nPage ){
            /* Free page off the end of the file */
            rc = SQLITE_CORRUPT_BKPT;
            goto end_allocate_page;
          }
          TRACE(("ALLOCATE: %d was leaf %d of %d on trunk %d"
                 ": %d more free pages\n",
                 *pPgno, closest+1, k, pTrunk->pgno, n-1));
          if( closest<k-1 ){
            memcpy(&aData[8+closest*4], &aData[4+k*4], 4);
          }
          put4byte(&aData[4], k-1);
          assert( sqlite3PagerIswriteable(pTrunk->pDbPage) );
          noContent = !btreeGetHasContent(pBt, *pPgno);
          rc = sqlite3BtreeGetPage(pBt, *pPgno, ppPage, noContent);
          if( rc==SQLITE_OK ){
            rc = sqlite3PagerWrite((*ppPage)->pDbPage);
            if( rc!=SQLITE_OK ){
              releasePage(*ppPage);
            }
          }
          searchList = 0;
        }
      }
      releasePage(pPrevTrunk);
      pPrevTrunk = 0;
    }while( searchList );
  }else{
    /* There are no pages on the freelist, so create a new page at the
    ** end of the file */
    int nPage = pagerPagecount(pBt);
    *pPgno = nPage + 1;

    if( *pPgno==PENDING_BYTE_PAGE(pBt) ){
      (*pPgno)++;
    }

#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pBt->autoVacuum && PTRMAP_ISPAGE(pBt, *pPgno) ){
      /* If *pPgno refers to a pointer-map page, allocate two new pages
      ** at the end of the file instead of one. The first allocated page
      ** becomes a new pointer-map page, the second is used by the caller.
      */
      TRACE(("ALLOCATE: %d from end of file (pointer-map page)\n", *pPgno));
      assert( *pPgno!=PENDING_BYTE_PAGE(pBt) );
      (*pPgno)++;
      if( *pPgno==PENDING_BYTE_PAGE(pBt) ){ (*pPgno)++; }
    }
#endif

    assert( *pPgno!=PENDING_BYTE_PAGE(pBt) );
    rc = sqlite3BtreeGetPage(pBt, *pPgno, ppPage, 0);
    if( rc ) return rc;
    rc = sqlite3PagerWrite((*ppPage)->pDbPage);
    if( rc!=SQLITE_OK ){
      releasePage(*ppPage);
    }
    TRACE(("ALLOCATE: %d from end of file\n", *pPgno));
  }

  assert( *pPgno!=PENDING_BYTE_PAGE(pBt) );

end_allocate_page:
  releasePage(pTrunk);
  releasePage(pPrevTrunk);
  if( rc==SQLITE_OK ){
    if( sqlite3PagerPageRefcount((*ppPage)->pDbPage)>1 ){
      releasePage(*ppPage);
      return SQLITE_CORRUPT_BKPT;
    }
    (*ppPage)->isInit = 0;
  }
  return rc;
}

/*
** This function is used to add page iPage to the database file free-list. 
** It is assumed that the page is not already a part of the free-list.
**
** The value passed as the second argument to this function is optional.
** If the caller happens to have a pointer to the MemPage object 
** corresponding to page iPage handy, it may pass it as the second value. 
** Otherwise, it may pass NULL.
**
** If a pointer to a MemPage object is passed as the second argument,
** its reference count is not altered by this function.
*/
static int freePage2(BtShared *pBt, MemPage *pMemPage, Pgno iPage){
  MemPage *pTrunk = 0;                /* Free-list trunk page */
  Pgno iTrunk = 0;                    /* Page number of free-list trunk page */ 
  MemPage *pPage1 = pBt->pPage1;      /* Local reference to page 1 */
  MemPage *pPage;                     /* Page being freed. May be NULL. */
  int rc;                             /* Return Code */
  int nFree;                          /* Initial number of pages on free-list */

  assert( sqlite3_mutex_held(pBt->mutex) );
  assert( iPage>1 );
  assert( !pMemPage || pMemPage->pgno==iPage );

  if( pMemPage ){
    pPage = pMemPage;
    sqlite3PagerRef(pPage->pDbPage);
  }else{
    pPage = btreePageLookup(pBt, iPage);
  }

  /* Increment the free page count on pPage1 */
  rc = sqlite3PagerWrite(pPage1->pDbPage);
  if( rc ) goto freepage_out;
  nFree = get4byte(&pPage1->aData[36]);
  put4byte(&pPage1->aData[36], nFree+1);

#ifdef SQLITE_SECURE_DELETE
  /* If the SQLITE_SECURE_DELETE compile-time option is enabled, then
  ** always fully overwrite deleted information with zeros.
  */
  if( (!pPage && (rc = sqlite3BtreeGetPage(pBt, iPage, &pPage, 0)))
   ||            (rc = sqlite3PagerWrite(pPage->pDbPage))
  ){
    goto freepage_out;
  }
  memset(pPage->aData, 0, pPage->pBt->pageSize);
#endif

  /* If the database supports auto-vacuum, write an entry in the pointer-map
  ** to indicate that the page is free.
  */
  if( ISAUTOVACUUM ){
    rc = ptrmapPut(pBt, iPage, PTRMAP_FREEPAGE, 0);
    if( rc ) goto freepage_out;
  }

  /* Now manipulate the actual database free-list structure. There are two
  ** possibilities. If the free-list is currently empty, or if the first
  ** trunk page in the free-list is full, then this page will become a
  ** new free-list trunk page. Otherwise, it will become a leaf of the
  ** first trunk page in the current free-list. This block tests if it
  ** is possible to add the page as a new free-list leaf.
  */
  if( nFree!=0 ){
    int nLeaf;                /* Initial number of leaf cells on trunk page */

    iTrunk = get4byte(&pPage1->aData[32]);
    rc = sqlite3BtreeGetPage(pBt, iTrunk, &pTrunk, 0);
    if( rc!=SQLITE_OK ){
      goto freepage_out;
    }

    nLeaf = get4byte(&pTrunk->aData[4]);
    if( nLeaf<0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto freepage_out;
    }
    if( nLeaf<pBt->usableSize/4 - 8 ){
      /* In this case there is room on the trunk page to insert the page
      ** being freed as a new leaf.
      **
      ** Note that the trunk page is not really full until it contains
      ** usableSize/4 - 2 entries, not usableSize/4 - 8 entries as we have
      ** coded.  But due to a coding error in versions of SQLite prior to
      ** 3.6.0, databases with freelist trunk pages holding more than
      ** usableSize/4 - 8 entries will be reported as corrupt.  In order
      ** to maintain backwards compatibility with older versions of SQLite,
      ** we will contain to restrict the number of entries to usableSize/4 - 8
      ** for now.  At some point in the future (once everyone has upgraded
      ** to 3.6.0 or later) we should consider fixing the conditional above
      ** to read "usableSize/4-2" instead of "usableSize/4-8".
      */
      rc = sqlite3PagerWrite(pTrunk->pDbPage);
      if( rc==SQLITE_OK ){
        put4byte(&pTrunk->aData[4], nLeaf+1);
        put4byte(&pTrunk->aData[8+nLeaf*4], iPage);
#ifndef SQLITE_SECURE_DELETE
        if( pPage ){
          sqlite3PagerDontWrite(pPage->pDbPage);
        }
#endif
        rc = btreeSetHasContent(pBt, iPage);
      }
      TRACE(("FREE-PAGE: %d leaf on trunk page %d\n",pPage->pgno,pTrunk->pgno));
      goto freepage_out;
    }
  }

  /* If control flows to this point, then it was not possible to add the
  ** the page being freed as a leaf page of the first trunk in the free-list.
  ** Possibly because the free-list is empty, or possibly because the 
  ** first trunk in the free-list is full. Either way, the page being freed
  ** will become the new first trunk page in the free-list.
  */
  if(   ((!pPage) && (0 != (rc = sqlite3BtreeGetPage(pBt, iPage, &pPage, 0))))
     || (0 != (rc = sqlite3PagerWrite(pPage->pDbPage)))
  ){
    goto freepage_out;
  }
  put4byte(pPage->aData, iTrunk);
  put4byte(&pPage->aData[4], 0);
  put4byte(&pPage1->aData[32], iPage);
  TRACE(("FREE-PAGE: %d new trunk page replacing %d\n", pPage->pgno, iTrunk));

freepage_out:
  if( pPage ){
    pPage->isInit = 0;
  }
  releasePage(pPage);
  releasePage(pTrunk);
  return rc;
}
static int freePage(MemPage *pPage){
  return freePage2(pPage->pBt, pPage, pPage->pgno);
}

/*
** Free any overflow pages associated with the given Cell.
*/
static int clearCell(MemPage *pPage, unsigned char *pCell){
  BtShared *pBt = pPage->pBt;
  CellInfo info;
  Pgno ovflPgno;
  int rc;
  int nOvfl;
  u16 ovflPageSize;

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  sqlite3BtreeParseCellPtr(pPage, pCell, &info);
  if( info.iOverflow==0 ){
    return SQLITE_OK;  /* No overflow pages. Return without doing anything */
  }
  ovflPgno = get4byte(&pCell[info.iOverflow]);
  assert( pBt->usableSize > 4 );
  ovflPageSize = pBt->usableSize - 4;
  nOvfl = (info.nPayload - info.nLocal + ovflPageSize - 1)/ovflPageSize;
  assert( ovflPgno==0 || nOvfl>0 );
  while( nOvfl-- ){
    Pgno iNext = 0;
    MemPage *pOvfl = 0;
    if( ovflPgno==0 || ovflPgno>pagerPagecount(pBt) ){
      return SQLITE_CORRUPT_BKPT;
    }
    if( nOvfl ){
      rc = getOverflowPage(pBt, ovflPgno, &pOvfl, &iNext);
      if( rc ) return rc;
    }
    rc = freePage2(pBt, pOvfl, ovflPgno);
    if( pOvfl ){
      sqlite3PagerUnref(pOvfl->pDbPage);
    }
    if( rc ) return rc;
    ovflPgno = iNext;
  }
  return SQLITE_OK;
}

/*
** Create the byte sequence used to represent a cell on page pPage
** and write that byte sequence into pCell[].  Overflow pages are
** allocated and filled in as necessary.  The calling procedure
** is responsible for making sure sufficient space has been allocated
** for pCell[].
**
** Note that pCell does not necessary need to point to the pPage->aData
** area.  pCell might point to some temporary storage.  The cell will
** be constructed in this temporary area then copied into pPage->aData
** later.
*/
static int fillInCell(
  MemPage *pPage,                /* The page that contains the cell */
  unsigned char *pCell,          /* Complete text of the cell */
  const void *pKey, i64 nKey,    /* The key */
  const void *pData,int nData,   /* The data */
  int nZero,                     /* Extra zero bytes to append to pData */
  int *pnSize                    /* Write cell size here */
){
  int nPayload;
  const u8 *pSrc;
  int nSrc, n, rc;
  int spaceLeft;
  MemPage *pOvfl = 0;
  MemPage *pToRelease = 0;
  unsigned char *pPrior;
  unsigned char *pPayload;
  BtShared *pBt = pPage->pBt;
  Pgno pgnoOvfl = 0;
  int nHeader;
  CellInfo info;

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );

  /* pPage is not necessarily writeable since pCell might be auxiliary
  ** buffer space that is separate from the pPage buffer area */
  assert( pCell<pPage->aData || pCell>=&pPage->aData[pBt->pageSize]
            || sqlite3PagerIswriteable(pPage->pDbPage) );

  /* Fill in the header. */
  nHeader = 0;
  if( !pPage->leaf ){
    nHeader += 4;
  }
  if( pPage->hasData ){
    nHeader += putVarint(&pCell[nHeader], nData+nZero);
  }else{
    nData = nZero = 0;
  }
  nHeader += putVarint(&pCell[nHeader], *(u64*)&nKey);
  sqlite3BtreeParseCellPtr(pPage, pCell, &info);
  assert( info.nHeader==nHeader );
  assert( info.nKey==nKey );
  assert( info.nData==(u32)(nData+nZero) );
  
  /* Fill in the payload */
  nPayload = nData + nZero;
  if( pPage->intKey ){
    pSrc = pData;
    nSrc = nData;
    nData = 0;
  }else{ 
    if( nKey>0x7fffffff || pKey==0 ){
      return SQLITE_CORRUPT;
    }
    nPayload += (int)nKey;
    pSrc = pKey;
    nSrc = (int)nKey;
  }
  *pnSize = info.nSize;
  spaceLeft = info.nLocal;
  pPayload = &pCell[nHeader];
  pPrior = &pCell[info.iOverflow];

  while( nPayload>0 ){
    if( spaceLeft==0 ){
#ifndef SQLITE_OMIT_AUTOVACUUM
      Pgno pgnoPtrmap = pgnoOvfl; /* Overflow page pointer-map entry page */
      if( pBt->autoVacuum ){
        do{
          pgnoOvfl++;
        } while( 
          PTRMAP_ISPAGE(pBt, pgnoOvfl) || pgnoOvfl==PENDING_BYTE_PAGE(pBt) 
        );
      }
#endif
      rc = allocateBtreePage(pBt, &pOvfl, &pgnoOvfl, pgnoOvfl, 0);
#ifndef SQLITE_OMIT_AUTOVACUUM
      /* If the database supports auto-vacuum, and the second or subsequent
      ** overflow page is being allocated, add an entry to the pointer-map
      ** for that page now. 
      **
      ** If this is the first overflow page, then write a partial entry 
      ** to the pointer-map. If we write nothing to this pointer-map slot,
      ** then the optimistic overflow chain processing in clearCell()
      ** may misinterpret the uninitialised values and delete the
      ** wrong pages from the database.
      */
      if( pBt->autoVacuum && rc==SQLITE_OK ){
        u8 eType = (pgnoPtrmap?PTRMAP_OVERFLOW2:PTRMAP_OVERFLOW1);
        rc = ptrmapPut(pBt, pgnoOvfl, eType, pgnoPtrmap);
        if( rc ){
          releasePage(pOvfl);
        }
      }
#endif
      if( rc ){
        releasePage(pToRelease);
        return rc;
      }

      /* If pToRelease is not zero than pPrior points into the data area
      ** of pToRelease.  Make sure pToRelease is still writeable. */
      assert( pToRelease==0 || sqlite3PagerIswriteable(pToRelease->pDbPage) );

      /* If pPrior is part of the data area of pPage, then make sure pPage
      ** is still writeable */
      assert( pPrior<pPage->aData || pPrior>=&pPage->aData[pBt->pageSize]
            || sqlite3PagerIswriteable(pPage->pDbPage) );

      put4byte(pPrior, pgnoOvfl);
      releasePage(pToRelease);
      pToRelease = pOvfl;
      pPrior = pOvfl->aData;
      put4byte(pPrior, 0);
      pPayload = &pOvfl->aData[4];
      spaceLeft = pBt->usableSize - 4;
    }
    n = nPayload;
    if( n>spaceLeft ) n = spaceLeft;

    /* If pToRelease is not zero than pPayload points into the data area
    ** of pToRelease.  Make sure pToRelease is still writeable. */
    assert( pToRelease==0 || sqlite3PagerIswriteable(pToRelease->pDbPage) );

    /* If pPayload is part of the data area of pPage, then make sure pPage
    ** is still writeable */
    assert( pPayload<pPage->aData || pPayload>=&pPage->aData[pBt->pageSize]
            || sqlite3PagerIswriteable(pPage->pDbPage) );

    if( nSrc>0 ){
      if( n>nSrc ) n = nSrc;
      assert( pSrc );
      memcpy(pPayload, pSrc, n);
    }else{
      memset(pPayload, 0, n);
    }
    nPayload -= n;
    pPayload += n;
    pSrc += n;
    nSrc -= n;
    spaceLeft -= n;
    if( nSrc==0 ){
      nSrc = nData;
      pSrc = pData;
    }
  }
  releasePage(pToRelease);
  return SQLITE_OK;
}

/*
** Remove the i-th cell from pPage.  This routine effects pPage only.
** The cell content is not freed or deallocated.  It is assumed that
** the cell content has been copied someplace else.  This routine just
** removes the reference to the cell from pPage.
**
** "sz" must be the number of bytes in the cell.
*/
static int dropCell(MemPage *pPage, int idx, int sz){
  int i;          /* Loop counter */
  int pc;         /* Offset to cell content of cell being deleted */
  u8 *data;       /* pPage->aData */
  u8 *ptr;        /* Used to move bytes around within data[] */
  int rc;         /* The return code */

  assert( idx>=0 && idx<pPage->nCell );
  assert( sz==cellSize(pPage, idx) );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  data = pPage->aData;
  ptr = &data[pPage->cellOffset + 2*idx];
  pc = get2byte(ptr);
  if( (pc<pPage->hdrOffset+6+(pPage->leaf?0:4))
     || (pc+sz>pPage->pBt->usableSize) ){
    return SQLITE_CORRUPT_BKPT;
  }
  rc = freeSpace(pPage, pc, sz);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  for(i=idx+1; i<pPage->nCell; i++, ptr+=2){
    ptr[0] = ptr[2];
    ptr[1] = ptr[3];
  }
  pPage->nCell--;
  put2byte(&data[pPage->hdrOffset+3], pPage->nCell);
  pPage->nFree += 2;
  return SQLITE_OK;
}

/*
** Insert a new cell on pPage at cell index "i".  pCell points to the
** content of the cell.
**
** If the cell content will fit on the page, then put it there.  If it
** will not fit, then make a copy of the cell content into pTemp if
** pTemp is not null.  Regardless of pTemp, allocate a new entry
** in pPage->aOvfl[] and make it point to the cell content (either
** in pTemp or the original pCell) and also record its index. 
** Allocating a new entry in pPage->aCell[] implies that 
** pPage->nOverflow is incremented.
**
** If nSkip is non-zero, then do not copy the first nSkip bytes of the
** cell. The caller will overwrite them after this function returns. If
** nSkip is non-zero, then pCell may not point to an invalid memory location 
** (but pCell+nSkip is always valid).
*/
static int insertCell(
  MemPage *pPage,   /* Page into which we are copying */
  int i,            /* New cell becomes the i-th cell of the page */
  u8 *pCell,        /* Content of the new cell */
  int sz,           /* Bytes of content in pCell */
  u8 *pTemp,        /* Temp storage space for pCell, if needed */
  u8 nSkip          /* Do not write the first nSkip bytes of the cell */
){
  int idx;          /* Where to write new cell content in data[] */
  int j;            /* Loop counter */
  int top;          /* First byte of content for any cell in data[] */
  int end;          /* First byte past the last cell pointer in data[] */
  int ins;          /* Index in data[] where new cell pointer is inserted */
  int hdr;          /* Offset into data[] of the page header */
  int cellOffset;   /* Address of first cell pointer in data[] */
  u8 *data;         /* The content of the whole page */
  u8 *ptr;          /* Used for moving information around in data[] */

  assert( i>=0 && i<=pPage->nCell+pPage->nOverflow );
  assert( pPage->nCell<=MX_CELL(pPage->pBt) && MX_CELL(pPage->pBt)<=5460 );
  assert( pPage->nOverflow<=ArraySize(pPage->aOvfl) );
  assert( sz==cellSizePtr(pPage, pCell) );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  if( pPage->nOverflow || sz+2>pPage->nFree ){
    if( pTemp ){
      memcpy(pTemp+nSkip, pCell+nSkip, sz-nSkip);
      pCell = pTemp;
    }
    j = pPage->nOverflow++;
    assert( j<(int)(sizeof(pPage->aOvfl)/sizeof(pPage->aOvfl[0])) );
    pPage->aOvfl[j].pCell = pCell;
    pPage->aOvfl[j].idx = (u16)i;
    pPage->nFree = 0;
  }else{
    int rc = sqlite3PagerWrite(pPage->pDbPage);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    assert( sqlite3PagerIswriteable(pPage->pDbPage) );
    data = pPage->aData;
    hdr = pPage->hdrOffset;
    top = get2byte(&data[hdr+5]);
    cellOffset = pPage->cellOffset;
    end = cellOffset + 2*pPage->nCell + 2;
    ins = cellOffset + 2*i;
    if( end > top - sz ){
      rc = defragmentPage(pPage);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      top = get2byte(&data[hdr+5]);
      assert( end + sz <= top );
    }
    idx = allocateSpace(pPage, sz);
    assert( idx>0 );
    assert( end <= get2byte(&data[hdr+5]) );
    if (idx+sz > pPage->pBt->usableSize) {
      return SQLITE_CORRUPT_BKPT;
    }
    pPage->nCell++;
    pPage->nFree -= 2;
    memcpy(&data[idx+nSkip], pCell+nSkip, sz-nSkip);
    for(j=end-2, ptr=&data[j]; j>ins; j-=2, ptr-=2){
      ptr[0] = ptr[-2];
      ptr[1] = ptr[-1];
    }
    put2byte(&data[ins], idx);
    put2byte(&data[hdr+3], pPage->nCell);
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pPage->pBt->autoVacuum ){
      /* The cell may contain a pointer to an overflow page. If so, write
      ** the entry for the overflow page into the pointer map.
      */
      CellInfo info;
      sqlite3BtreeParseCellPtr(pPage, pCell, &info);
      assert( (info.nData+(pPage->intKey?0:info.nKey))==info.nPayload );
      if( (info.nData+(pPage->intKey?0:info.nKey))>info.nLocal ){
        Pgno pgnoOvfl = get4byte(&pCell[info.iOverflow]);
        rc = ptrmapPut(pPage->pBt, pgnoOvfl, PTRMAP_OVERFLOW1, pPage->pgno);
        if( rc!=SQLITE_OK ) return rc;
      }
    }
#endif
  }

  return SQLITE_OK;
}

/*
** Add a list of cells to a page.  The page should be initially empty.
** The cells are guaranteed to fit on the page.
*/
static void assemblePage(
  MemPage *pPage,   /* The page to be assemblied */
  int nCell,        /* The number of cells to add to this page */
  u8 **apCell,      /* Pointers to cell bodies */
  u16 *aSize        /* Sizes of the cells */
){
  int i;            /* Loop counter */
  int totalSize;    /* Total size of all cells */
  int hdr;          /* Index of page header */
  int cellptr;      /* Address of next cell pointer */
  int cellbody;     /* Address of next cell body */
  u8 *data;         /* Data for the page */

  assert( pPage->nOverflow==0 );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  assert( nCell>=0 && nCell<=MX_CELL(pPage->pBt) && MX_CELL(pPage->pBt)<=5460 );
  totalSize = 0;
  for(i=0; i<nCell; i++){
    totalSize += aSize[i];
  }
  assert( totalSize+2*nCell<=pPage->nFree );
  assert( pPage->nCell==0 );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  cellptr = pPage->cellOffset;
  data = pPage->aData;
  hdr = pPage->hdrOffset;
  put2byte(&data[hdr+3], nCell);
  if( nCell ){
    cellbody = allocateSpace(pPage, totalSize);
    assert( cellbody>0 );
    assert( pPage->nFree >= 2*nCell );
    pPage->nFree -= 2*nCell;
    for(i=0; i<nCell; i++){
      put2byte(&data[cellptr], cellbody);
      memcpy(&data[cellbody], apCell[i], aSize[i]);
      cellptr += 2;
      cellbody += aSize[i];
    }
    assert( cellbody==pPage->pBt->usableSize );
  }
  pPage->nCell = (u16)nCell;
}

/*
** The following parameters determine how many adjacent pages get involved
** in a balancing operation.  NN is the number of neighbors on either side
** of the page that participate in the balancing operation.  NB is the
** total number of pages that participate, including the target page and
** NN neighbors on either side.
**
** The minimum value of NN is 1 (of course).  Increasing NN above 1
** (to 2 or 3) gives a modest improvement in SELECT and DELETE performance
** in exchange for a larger degradation in INSERT and UPDATE performance.
** The value of NN appears to give the best results overall.
*/
#define NN 1             /* Number of neighbors on either side of pPage */
#define NB (NN*2+1)      /* Total pages involved in the balance */

/* Forward reference */
static int balance(BtCursor*, int);

#ifndef SQLITE_OMIT_QUICKBALANCE
/*
** This version of balance() handles the common special case where
** a new entry is being inserted on the extreme right-end of the
** tree, in other words, when the new entry will become the largest
** entry in the tree.
**
** Instead of trying balance the 3 right-most leaf pages, just add
** a new page to the right-hand side and put the one new entry in
** that page.  This leaves the right side of the tree somewhat
** unbalanced.  But odds are that we will be inserting new entries
** at the end soon afterwards so the nearly empty page will quickly
** fill up.  On average.
**
** pPage is the leaf page which is the right-most page in the tree.
** pParent is its parent.  pPage must have a single overflow entry
** which is also the right-most entry on the page.
*/
static int balance_quick(BtCursor *pCur){
  int rc;
  MemPage *pNew = 0;
  Pgno pgnoNew;
  u8 *pCell;
  u16 szCell;
  CellInfo info;
  MemPage *pPage = pCur->apPage[pCur->iPage];
  MemPage *pParent = pCur->apPage[pCur->iPage-1];
  BtShared *pBt = pPage->pBt;
  int parentIdx = pParent->nCell;   /* pParent new divider cell index */
  int parentSize;                   /* Size of new divider cell */
  u8 parentCell[64];                /* Space for the new divider cell */

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );

  /* Allocate a new page. Insert the overflow cell from pPage
  ** into it. Then remove the overflow cell from pPage.
  */
  rc = allocateBtreePage(pBt, &pNew, &pgnoNew, 0, 0);
  if( rc==SQLITE_OK ){
    pCell = pPage->aOvfl[0].pCell;
    szCell = cellSizePtr(pPage, pCell);
    assert( sqlite3PagerIswriteable(pNew->pDbPage) );
    zeroPage(pNew, pPage->aData[0]);
    assemblePage(pNew, 1, &pCell, &szCell);
    pPage->nOverflow = 0;
  
    /* pPage is currently the right-child of pParent. Change this
    ** so that the right-child is the new page allocated above and
    ** pPage is the next-to-right child. 
    **
    ** Ignore the return value of the call to fillInCell(). fillInCell()
    ** may only return other than SQLITE_OK if it is required to allocate
    ** one or more overflow pages. Since an internal table B-Tree cell 
    ** may never spill over onto an overflow page (it is a maximum of 
    ** 13 bytes in size), it is not neccessary to check the return code.
    **
    ** Similarly, the insertCell() function cannot fail if the page
    ** being inserted into is already writable and the cell does not 
    ** contain an overflow pointer. So ignore this return code too.
    */
    assert( pPage->nCell>0 );
    pCell = findCell(pPage, pPage->nCell-1);
    sqlite3BtreeParseCellPtr(pPage, pCell, &info);
    fillInCell(pParent, parentCell, 0, info.nKey, 0, 0, 0, &parentSize);
    assert( parentSize<64 );
    assert( sqlite3PagerIswriteable(pParent->pDbPage) );
    insertCell(pParent, parentIdx, parentCell, parentSize, 0, 4);
    put4byte(findOverflowCell(pParent,parentIdx), pPage->pgno);
    put4byte(&pParent->aData[pParent->hdrOffset+8], pgnoNew);
  
    /* If this is an auto-vacuum database, update the pointer map
    ** with entries for the new page, and any pointer from the 
    ** cell on the page to an overflow page.
    */
    if( ISAUTOVACUUM ){
      rc = ptrmapPut(pBt, pgnoNew, PTRMAP_BTREE, pParent->pgno);
      if( rc==SQLITE_OK ){
        rc = ptrmapPutOvfl(pNew, 0);
      }
    }

    /* Release the reference to the new page. */
    releasePage(pNew);
  }

  /* At this point the pPage->nFree variable is not set correctly with
  ** respect to the content of the page (because it was set to 0 by 
  ** insertCell). So call sqlite3BtreeInitPage() to make sure it is
  ** correct.
  **
  ** This has to be done even if an error will be returned. Normally, if
  ** an error occurs during tree balancing, the contents of MemPage are
  ** not important, as they will be recalculated when the page is rolled
  ** back. But here, in balance_quick(), it is possible that pPage has 
  ** not yet been marked dirty or written into the journal file. Therefore
  ** it will not be rolled back and so it is important to make sure that
  ** the page data and contents of MemPage are consistent.
  */
  pPage->isInit = 0;
  sqlite3BtreeInitPage(pPage);
  assert( pPage->nOverflow==0 );

  /* If everything else succeeded, balance the parent page, in 
  ** case the divider cell inserted caused it to become overfull.
  */
  if( rc==SQLITE_OK ){
    releasePage(pPage);
    pCur->iPage--;
    rc = balance(pCur, 0);
  }
  return rc;
}
#endif /* SQLITE_OMIT_QUICKBALANCE */

/*
** This routine redistributes Cells on pPage and up to NN*2 siblings
** of pPage so that all pages have about the same amount of free space.
** Usually NN siblings on either side of pPage is used in the balancing,
** though more siblings might come from one side if pPage is the first
** or last child of its parent.  If pPage has fewer than 2*NN siblings
** (something which can only happen if pPage is the root page or a 
** child of root) then all available siblings participate in the balancing.
**
** The number of siblings of pPage might be increased or decreased by one or
** two in an effort to keep pages nearly full but not over full. The root page
** is special and is allowed to be nearly empty. If pPage is 
** the root page, then the depth of the tree might be increased
** or decreased by one, as necessary, to keep the root page from being
** overfull or completely empty.
**
** Note that when this routine is called, some of the Cells on pPage
** might not actually be stored in pPage->aData[].  This can happen
** if the page is overfull.  Part of the job of this routine is to
** make sure all Cells for pPage once again fit in pPage->aData[].
**
** In the course of balancing the siblings of pPage, the parent of pPage
** might become overfull or underfull.  If that happens, then this routine
** is called recursively on the parent.
**
** If this routine fails for any reason, it might leave the database
** in a corrupted state.  So if this routine fails, the database should
** be rolled back.
*/
static int balance_nonroot(BtCursor *pCur){
  MemPage *pPage;              /* The over or underfull page to balance */
  MemPage *pParent;            /* The parent of pPage */
  BtShared *pBt;               /* The whole database */
  int nCell = 0;               /* Number of cells in apCell[] */
  int nMaxCells = 0;           /* Allocated size of apCell, szCell, aFrom. */
  int nOld = 0;                /* Number of pages in apOld[] */
  int nNew = 0;                /* Number of pages in apNew[] */
  int nDiv;                    /* Number of cells in apDiv[] */
  int i, j, k;                 /* Loop counters */
  int idx;                     /* Index of pPage in pParent->aCell[] */
  int nxDiv;                   /* Next divider slot in pParent->aCell[] */
  int rc;                      /* The return code */
  int leafCorrection;          /* 4 if pPage is a leaf.  0 if not */
  int leafData;                /* True if pPage is a leaf of a LEAFDATA tree */
  int usableSpace;             /* Bytes in pPage beyond the header */
  int pageFlags;               /* Value of pPage->aData[0] */
  int subtotal;                /* Subtotal of bytes in cells on one page */
  int iSpace1 = 0;             /* First unused byte of aSpace1[] */
  int iSpace2 = 0;             /* First unused byte of aSpace2[] */
  int szScratch;               /* Size of scratch memory requested */
  MemPage *apOld[NB];          /* pPage and up to two siblings */
  Pgno pgnoOld[NB];            /* Page numbers for each page in apOld[] */
  MemPage *apCopy[NB];         /* Private copies of apOld[] pages */
  MemPage *apNew[NB+2];        /* pPage and up to NB siblings after balancing */
  Pgno pgnoNew[NB+2];          /* Page numbers for each page in apNew[] */
  u8 *apDiv[NB];               /* Divider cells in pParent */
  int cntNew[NB+2];            /* Index in aCell[] of cell after i-th page */
  int szNew[NB+2];             /* Combined size of cells place on i-th page */
  u8 **apCell = 0;             /* All cells begin balanced */
  u16 *szCell;                 /* Local size of all cells in apCell[] */
  u8 *aCopy[NB];         /* Space for holding data of apCopy[] */
  u8 *aSpace1;           /* Space for copies of dividers cells before balance */
  u8 *aSpace2 = 0;       /* Space for overflow dividers cells after balance */
  u8 *aFrom = 0;

  pPage = pCur->apPage[pCur->iPage];
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  VVA_ONLY( pCur->pagesShuffled = 1 );

  /* 
  ** Find the parent page.
  */
  assert( pCur->iPage>0 );
  assert( pPage->isInit );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) || pPage->nOverflow==1 );
  pBt = pPage->pBt;
  pParent = pCur->apPage[pCur->iPage-1];
  assert( pParent );
  if( SQLITE_OK!=(rc = sqlite3PagerWrite(pParent->pDbPage)) ){
    goto balance_cleanup;
  }

  TRACE(("BALANCE: begin page %d child of %d\n", pPage->pgno, pParent->pgno));

#ifndef SQLITE_OMIT_QUICKBALANCE
  /*
  ** A special case:  If a new entry has just been inserted into a
  ** table (that is, a btree with integer keys and all data at the leaves)
  ** and the new entry is the right-most entry in the tree (it has the
  ** largest key) then use the special balance_quick() routine for
  ** balancing.  balance_quick() is much faster and results in a tighter
  ** packing of data in the common case.
  */
  if( pPage->leaf &&
      pPage->intKey &&
      pPage->nOverflow==1 &&
      pPage->aOvfl[0].idx==pPage->nCell &&
      pParent->pgno!=1 &&
      get4byte(&pParent->aData[pParent->hdrOffset+8])==pPage->pgno
  ){
    assert( pPage->intKey );
    /*
    ** TODO: Check the siblings to the left of pPage. It may be that
    ** they are not full and no new page is required.
    */
    return balance_quick(pCur);
  }
#endif

  if( SQLITE_OK!=(rc = sqlite3PagerWrite(pPage->pDbPage)) ){
    goto balance_cleanup;
  }

  /*
  ** Find the cell in the parent page whose left child points back
  ** to pPage.  The "idx" variable is the index of that cell.  If pPage
  ** is the rightmost child of pParent then set idx to pParent->nCell 
  */
  idx = pCur->aiIdx[pCur->iPage-1];
  assertParentIndex(pParent, idx, pPage->pgno);

  /*
  ** Find sibling pages to pPage and the cells in pParent that divide
  ** the siblings.  An attempt is made to find NN siblings on either
  ** side of pPage.  More siblings are taken from one side, however, if
  ** pPage there are fewer than NN siblings on the other side.  If pParent
  ** has NB or fewer children then all children of pParent are taken.
  */
  nxDiv = idx - NN;
  if( nxDiv + NB > pParent->nCell ){
    nxDiv = pParent->nCell - NB + 1;
  }
  if( nxDiv<0 ){
    nxDiv = 0;
  }
  nDiv = 0;
  for(i=0, k=nxDiv; i<NB; i++, k++){
    if( k<pParent->nCell ){
      apDiv[i] = findCell(pParent, k);
      nDiv++;
      assert( !pParent->leaf );
      pgnoOld[i] = get4byte(apDiv[i]);
    }else if( k==pParent->nCell ){
      pgnoOld[i] = get4byte(&pParent->aData[pParent->hdrOffset+8]);
    }else{
      break;
    }
    rc = getAndInitPage(pBt, pgnoOld[i], &apOld[i]);
    if( rc ) goto balance_cleanup;
    /* apOld[i]->idxParent = k; */
    apCopy[i] = 0;
    assert( i==nOld );
    nOld++;
    nMaxCells += 1+apOld[i]->nCell+apOld[i]->nOverflow;
  }

  /* Make nMaxCells a multiple of 4 in order to preserve 8-byte
  ** alignment */
  nMaxCells = (nMaxCells + 3)&~3;

  /*
  ** Allocate space for memory structures
  */
  szScratch =
       nMaxCells*sizeof(u8*)                       /* apCell */
     + nMaxCells*sizeof(u16)                       /* szCell */
     + (ROUND8(sizeof(MemPage))+pBt->pageSize)*NB  /* aCopy */
     + pBt->pageSize                               /* aSpace1 */
     + (ISAUTOVACUUM ? nMaxCells : 0);             /* aFrom */
  apCell = sqlite3ScratchMalloc( szScratch ); 
  if( apCell==0 ){
    rc = SQLITE_NOMEM;
    goto balance_cleanup;
  }
  szCell = (u16*)&apCell[nMaxCells];
  aCopy[0] = (u8*)&szCell[nMaxCells];
  assert( ((aCopy[0] - (u8*)0) & 7)==0 ); /* 8-byte alignment required */
  for(i=1; i<NB; i++){
    aCopy[i] = &aCopy[i-1][pBt->pageSize+ROUND8(sizeof(MemPage))];
    assert( ((aCopy[i] - (u8*)0) & 7)==0 ); /* 8-byte alignment required */
  }
  aSpace1 = &aCopy[NB-1][pBt->pageSize+ROUND8(sizeof(MemPage))];
  assert( ((aSpace1 - (u8*)0) & 7)==0 ); /* 8-byte alignment required */
  if( ISAUTOVACUUM ){
    aFrom = &aSpace1[pBt->pageSize];
  }
  aSpace2 = sqlite3PageMalloc(pBt->pageSize);
  if( aSpace2==0 ){
    rc = SQLITE_NOMEM;
    goto balance_cleanup;
  }
  
  /*
  ** Make copies of the content of pPage and its siblings into aOld[].
  ** The rest of this function will use data from the copies rather
  ** that the original pages since the original pages will be in the
  ** process of being overwritten.
  */
  for(i=0; i<nOld; i++){
    MemPage *p = apCopy[i] = (MemPage*)aCopy[i];
    memcpy(p, apOld[i], sizeof(MemPage));
    p->aData = (void*)&p[1];
    memcpy(p->aData, apOld[i]->aData, pBt->pageSize);
  }

  /*
  ** Load pointers to all cells on sibling pages and the divider cells
  ** into the local apCell[] array.  Make copies of the divider cells
  ** into space obtained form aSpace1[] and remove the the divider Cells
  ** from pParent.
  **
  ** If the siblings are on leaf pages, then the child pointers of the
  ** divider cells are stripped from the cells before they are copied
  ** into aSpace1[].  In this way, all cells in apCell[] are without
  ** child pointers.  If siblings are not leaves, then all cell in
  ** apCell[] include child pointers.  Either way, all cells in apCell[]
  ** are alike.
  **
  ** leafCorrection:  4 if pPage is a leaf.  0 if pPage is not a leaf.
  **       leafData:  1 if pPage holds key+data and pParent holds only keys.
  */
  nCell = 0;
  leafCorrection = pPage->leaf*4;
  leafData = pPage->hasData;
  for(i=0; i<nOld; i++){
    MemPage *pOld = apCopy[i];
    int limit = pOld->nCell+pOld->nOverflow;
    for(j=0; j<limit; j++){
      assert( nCell<nMaxCells );
      apCell[nCell] = findOverflowCell(pOld, j);
      szCell[nCell] = cellSizePtr(pOld, apCell[nCell]);
      if( ISAUTOVACUUM ){
        int a;
        aFrom[nCell] = (u8)i;   assert( i>=0 && i<6 );
        for(a=0; a<pOld->nOverflow; a++){
          if( pOld->aOvfl[a].pCell==apCell[nCell] ){
            aFrom[nCell] = 0xFF;
            break;
          }
        }
      }
      nCell++;
    }
    if( i<nOld-1 ){
      u16 sz = cellSizePtr(pParent, apDiv[i]);
      if( leafData ){
        /* With the LEAFDATA flag, pParent cells hold only INTKEYs that
        ** are duplicates of keys on the child pages.  We need to remove
        ** the divider cells from pParent, but the dividers cells are not
        ** added to apCell[] because they are duplicates of child cells.
        */
        dropCell(pParent, nxDiv, sz);
      }else{
        u8 *pTemp;
        assert( nCell<nMaxCells );
        szCell[nCell] = sz;
        pTemp = &aSpace1[iSpace1];
        iSpace1 += sz;
        assert( sz<=pBt->pageSize/4 );
        assert( iSpace1<=pBt->pageSize );
        memcpy(pTemp, apDiv[i], sz);
        apCell[nCell] = pTemp+leafCorrection;
        if( ISAUTOVACUUM ){
          aFrom[nCell] = 0xFF;
        }
        dropCell(pParent, nxDiv, sz);
        assert( leafCorrection==0 || leafCorrection==4 );
        szCell[nCell] -= (u16)leafCorrection;
        assert( get4byte(pTemp)==pgnoOld[i] );
        if( !pOld->leaf ){
          assert( leafCorrection==0 );
          /* The right pointer of the child page pOld becomes the left
          ** pointer of the divider cell */
          memcpy(apCell[nCell], &pOld->aData[pOld->hdrOffset+8], 4);
        }else{
          assert( leafCorrection==4 );
          if( szCell[nCell]<4 ){
            /* Do not allow any cells smaller than 4 bytes. */
            szCell[nCell] = 4;
          }
        }
        nCell++;
      }
    }
  }

  /*
  ** Figure out the number of pages needed to hold all nCell cells.
  ** Store this number in "k".  Also compute szNew[] which is the total
  ** size of all cells on the i-th page and cntNew[] which is the index
  ** in apCell[] of the cell that divides page i from page i+1.  
  ** cntNew[k] should equal nCell.
  **
  ** Values computed by this block:
  **
  **           k: The total number of sibling pages
  **    szNew[i]: Spaced used on the i-th sibling page.
  **   cntNew[i]: Index in apCell[] and szCell[] for the first cell to
  **              the right of the i-th sibling page.
  ** usableSpace: Number of bytes of space available on each sibling.
  ** 
  */
  usableSpace = pBt->usableSize - 12 + leafCorrection;
  for(subtotal=k=i=0; i<nCell; i++){
    assert( i<nMaxCells );
    subtotal += szCell[i] + 2;
    if( subtotal > usableSpace ){
      szNew[k] = subtotal - szCell[i];
      cntNew[k] = i;
      if( leafData ){ i--; }
      subtotal = 0;
      k++;
    }
  }
  szNew[k] = subtotal;
  cntNew[k] = nCell;
  k++;

  /*
  ** The packing computed by the previous block is biased toward the siblings
  ** on the left side.  The left siblings are always nearly full, while the
  ** right-most sibling might be nearly empty.  This block of code attempts
  ** to adjust the packing of siblings to get a better balance.
  **
  ** This adjustment is more than an optimization.  The packing above might
  ** be so out of balance as to be illegal.  For example, the right-most
  ** sibling might be completely empty.  This adjustment is not optional.
  */
  for(i=k-1; i>0; i--){
    int szRight = szNew[i];  /* Size of sibling on the right */
    int szLeft = szNew[i-1]; /* Size of sibling on the left */
    int r;              /* Index of right-most cell in left sibling */
    int d;              /* Index of first cell to the left of right sibling */

    r = cntNew[i-1] - 1;
    d = r + 1 - leafData;
    assert( d<nMaxCells );
    assert( r<nMaxCells );
    while( szRight==0 || szRight+szCell[d]+2<=szLeft-(szCell[r]+2) ){
      szRight += szCell[d] + 2;
      szLeft -= szCell[r] + 2;
      cntNew[i-1]--;
      r = cntNew[i-1] - 1;
      d = r + 1 - leafData;
    }
    szNew[i] = szRight;
    szNew[i-1] = szLeft;
  }

  /* Either we found one or more cells (cntnew[0])>0) or we are the
  ** a virtual root page.  A virtual root page is when the real root
  ** page is page 1 and we are the only child of that page.
  */
  assert( cntNew[0]>0 || (pParent->pgno==1 && pParent->nCell==0) );

  /*
  ** Allocate k new pages.  Reuse old pages where possible.
  */
  assert( pPage->pgno>1 );
  pageFlags = pPage->aData[0];
  for(i=0; i<k; i++){
    MemPage *pNew;
    if( i<nOld ){
      pNew = apNew[i] = apOld[i];
      pgnoNew[i] = pgnoOld[i];
      apOld[i] = 0;
      rc = sqlite3PagerWrite(pNew->pDbPage);
      nNew++;
      if( rc ) goto balance_cleanup;
    }else{
      assert( i>0 );
      rc = allocateBtreePage(pBt, &pNew, &pgnoNew[i], pgnoNew[i-1], 0);
      if( rc ) goto balance_cleanup;
      apNew[i] = pNew;
      nNew++;
    }
  }

  /* Free any old pages that were not reused as new pages.
  */
  while( i<nOld ){
    rc = freePage(apOld[i]);
    if( rc ) goto balance_cleanup;
    releasePage(apOld[i]);
    apOld[i] = 0;
    i++;
  }

  /*
  ** Put the new pages in accending order.  This helps to
  ** keep entries in the disk file in order so that a scan
  ** of the table is a linear scan through the file.  That
  ** in turn helps the operating system to deliver pages
  ** from the disk more rapidly.
  **
  ** An O(n^2) insertion sort algorithm is used, but since
  ** n is never more than NB (a small constant), that should
  ** not be a problem.
  **
  ** When NB==3, this one optimization makes the database
  ** about 25% faster for large insertions and deletions.
  */
  for(i=0; i<k-1; i++){
    int minV = pgnoNew[i];
    int minI = i;
    for(j=i+1; j<k; j++){
      if( pgnoNew[j]<(unsigned)minV ){
        minI = j;
        minV = pgnoNew[j];
      }
    }
    if( minI>i ){
      int t;
      MemPage *pT;
      t = pgnoNew[i];
      pT = apNew[i];
      pgnoNew[i] = pgnoNew[minI];
      apNew[i] = apNew[minI];
      pgnoNew[minI] = t;
      apNew[minI] = pT;
    }
  }
  TRACE(("BALANCE: old: %d %d %d  new: %d(%d) %d(%d) %d(%d) %d(%d) %d(%d)\n",
    pgnoOld[0], 
    nOld>=2 ? pgnoOld[1] : 0,
    nOld>=3 ? pgnoOld[2] : 0,
    pgnoNew[0], szNew[0],
    nNew>=2 ? pgnoNew[1] : 0, nNew>=2 ? szNew[1] : 0,
    nNew>=3 ? pgnoNew[2] : 0, nNew>=3 ? szNew[2] : 0,
    nNew>=4 ? pgnoNew[3] : 0, nNew>=4 ? szNew[3] : 0,
    nNew>=5 ? pgnoNew[4] : 0, nNew>=5 ? szNew[4] : 0));

  /*
  ** Evenly distribute the data in apCell[] across the new pages.
  ** Insert divider cells into pParent as necessary.
  */
  j = 0;
  for(i=0; i<nNew; i++){
    /* Assemble the new sibling page. */
    MemPage *pNew = apNew[i];
    assert( j<nMaxCells );
    assert( pNew->pgno==pgnoNew[i] );
    zeroPage(pNew, pageFlags);
    assemblePage(pNew, cntNew[i]-j, &apCell[j], &szCell[j]);
    assert( pNew->nCell>0 || (nNew==1 && cntNew[0]==0) );
    assert( pNew->nOverflow==0 );

    /* If this is an auto-vacuum database, update the pointer map entries
    ** that point to the siblings that were rearranged. These can be: left
    ** children of cells, the right-child of the page, or overflow pages
    ** pointed to by cells.
    */
    if( ISAUTOVACUUM ){
      for(k=j; k<cntNew[i]; k++){
        assert( k<nMaxCells );
        if( aFrom[k]==0xFF || apCopy[aFrom[k]]->pgno!=pNew->pgno ){
          rc = ptrmapPutOvfl(pNew, k-j);
          if( rc==SQLITE_OK && leafCorrection==0 ){
            rc = ptrmapPut(pBt, get4byte(apCell[k]), PTRMAP_BTREE, pNew->pgno);
          }
          if( rc!=SQLITE_OK ){
            goto balance_cleanup;
          }
        }
      }
    }

    j = cntNew[i];

    /* If the sibling page assembled above was not the right-most sibling,
    ** insert a divider cell into the parent page.
    */
    if( i<nNew-1 && j<nCell ){
      u8 *pCell;
      u8 *pTemp;
      int sz;

      assert( j<nMaxCells );
      pCell = apCell[j];
      sz = szCell[j] + leafCorrection;
      pTemp = &aSpace2[iSpace2];
      if( !pNew->leaf ){
        memcpy(&pNew->aData[8], pCell, 4);
        if( ISAUTOVACUUM 
         && (aFrom[j]==0xFF || apCopy[aFrom[j]]->pgno!=pNew->pgno)
        ){
          rc = ptrmapPut(pBt, get4byte(pCell), PTRMAP_BTREE, pNew->pgno);
          if( rc!=SQLITE_OK ){
            goto balance_cleanup;
          }
        }
      }else if( leafData ){
        /* If the tree is a leaf-data tree, and the siblings are leaves, 
        ** then there is no divider cell in apCell[]. Instead, the divider 
        ** cell consists of the integer key for the right-most cell of 
        ** the sibling-page assembled above only.
        */
        CellInfo info;
        j--;
        sqlite3BtreeParseCellPtr(pNew, apCell[j], &info);
        pCell = pTemp;
        rc = fillInCell(pParent, pCell, 0, info.nKey, 0, 0, 0, &sz);
        if( rc!=SQLITE_OK ){
          goto balance_cleanup;
        }
        pTemp = 0;
      }else{
        pCell -= 4;
        /* Obscure case for non-leaf-data trees: If the cell at pCell was
        ** previously stored on a leaf node, and its reported size was 4
        ** bytes, then it may actually be smaller than this 
        ** (see sqlite3BtreeParseCellPtr(), 4 bytes is the minimum size of
        ** any cell). But it is important to pass the correct size to 
        ** insertCell(), so reparse the cell now.
        **
        ** Note that this can never happen in an SQLite data file, as all
        ** cells are at least 4 bytes. It only happens in b-trees used
        ** to evaluate "IN (SELECT ...)" and similar clauses.
        */
        if( szCell[j]==4 ){
          assert(leafCorrection==4);
          sz = cellSizePtr(pParent, pCell);
        }
      }
      iSpace2 += sz;
      assert( sz<=pBt->pageSize/4 );
      assert( iSpace2<=pBt->pageSize );
      rc = insertCell(pParent, nxDiv, pCell, sz, pTemp, 4);
      if( rc!=SQLITE_OK ) goto balance_cleanup;
      assert( sqlite3PagerIswriteable(pParent->pDbPage) );
      put4byte(findOverflowCell(pParent,nxDiv), pNew->pgno);

      /* If this is an auto-vacuum database, and not a leaf-data tree,
      ** then update the pointer map with an entry for the overflow page
      ** that the cell just inserted points to (if any).
      */
      if( ISAUTOVACUUM && !leafData ){
        rc = ptrmapPutOvfl(pParent, nxDiv);
        if( rc!=SQLITE_OK ){
          goto balance_cleanup;
        }
      }
      j++;
      nxDiv++;
    }

    /* Set the pointer-map entry for the new sibling page. */
    if( ISAUTOVACUUM ){
      rc = ptrmapPut(pBt, pNew->pgno, PTRMAP_BTREE, pParent->pgno);
      if( rc!=SQLITE_OK ){
        goto balance_cleanup;
      }
    }
  }
  assert( j==nCell );
  assert( nOld>0 );
  assert( nNew>0 );
  if( (pageFlags & PTF_LEAF)==0 ){
    u8 *zChild = &apCopy[nOld-1]->aData[8];
    memcpy(&apNew[nNew-1]->aData[8], zChild, 4);
    if( ISAUTOVACUUM ){
      rc = ptrmapPut(pBt, get4byte(zChild), PTRMAP_BTREE, apNew[nNew-1]->pgno);
      if( rc!=SQLITE_OK ){
        goto balance_cleanup;
      }
    }
  }
  assert( sqlite3PagerIswriteable(pParent->pDbPage) );
  if( nxDiv==pParent->nCell+pParent->nOverflow ){
    /* Right-most sibling is the right-most child of pParent */
    put4byte(&pParent->aData[pParent->hdrOffset+8], pgnoNew[nNew-1]);
  }else{
    /* Right-most sibling is the left child of the first entry in pParent
    ** past the right-most divider entry */
    put4byte(findOverflowCell(pParent, nxDiv), pgnoNew[nNew-1]);
  }

  /*
  ** Balance the parent page.  Note that the current page (pPage) might
  ** have been added to the freelist so it might no longer be initialized.
  ** But the parent page will always be initialized.
  */
  assert( pParent->isInit );
  sqlite3ScratchFree(apCell);
  apCell = 0;
  TRACE(("BALANCE: finished with %d: old=%d new=%d cells=%d\n",
          pPage->pgno, nOld, nNew, nCell));
  pPage->nOverflow = 0;
  releasePage(pPage);
  pCur->iPage--;
  rc = balance(pCur, 0);
  
  /*
  ** Cleanup before returning.
  */
balance_cleanup:
  sqlite3PageFree(aSpace2);
  sqlite3ScratchFree(apCell);
  for(i=0; i<nOld; i++){
    releasePage(apOld[i]);
  }
  for(i=0; i<nNew; i++){
    releasePage(apNew[i]);
  }
  pCur->apPage[pCur->iPage]->nOverflow = 0;

  return rc;
}

/*
** This routine is called for the root page of a btree when the root
** page contains no cells.  This is an opportunity to make the tree
** shallower by one level.
*/
static int balance_shallower(BtCursor *pCur){
  MemPage *pPage;              /* Root page of B-Tree */
  MemPage *pChild;             /* The only child page of pPage */
  Pgno pgnoChild;              /* Page number for pChild */
  int rc = SQLITE_OK;          /* Return code from subprocedures */
  BtShared *pBt;                  /* The main BTree structure */
  int mxCellPerPage;           /* Maximum number of cells per page */
  u8 **apCell;                 /* All cells from pages being balanced */
  u16 *szCell;                 /* Local size of all cells */

  assert( pCur->iPage==0 );
  pPage = pCur->apPage[0];

  assert( pPage->nCell==0 );
  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  pBt = pPage->pBt;
  mxCellPerPage = MX_CELL(pBt);
  apCell = sqlite3Malloc( mxCellPerPage*(sizeof(u8*)+sizeof(u16)) );
  if( apCell==0 ) return SQLITE_NOMEM;
  szCell = (u16*)&apCell[mxCellPerPage];
  if( pPage->leaf ){
    /* The table is completely empty */
    TRACE(("BALANCE: empty table %d\n", pPage->pgno));
  }else{
    /* The root page is empty but has one child.  Transfer the
    ** information from that one child into the root page if it 
    ** will fit.  This reduces the depth of the tree by one.
    **
    ** If the root page is page 1, it has less space available than
    ** its child (due to the 100 byte header that occurs at the beginning
    ** of the database fle), so it might not be able to hold all of the 
    ** information currently contained in the child.  If this is the 
    ** case, then do not do the transfer.  Leave page 1 empty except
    ** for the right-pointer to the child page.  The child page becomes
    ** the virtual root of the tree.
    */
    VVA_ONLY( pCur->pagesShuffled = 1 );
    pgnoChild = get4byte(&pPage->aData[pPage->hdrOffset+8]);
    assert( pgnoChild>0 );
    assert( pgnoChild<=pagerPagecount(pPage->pBt) );
    rc = sqlite3BtreeGetPage(pPage->pBt, pgnoChild, &pChild, 0);
    if( rc ) goto end_shallow_balance;
    if( pPage->pgno==1 ){
      rc = sqlite3BtreeInitPage(pChild);
      if( rc ) goto end_shallow_balance;
      assert( pChild->nOverflow==0 );
      if( pChild->nFree>=100 ){
        /* The child information will fit on the root page, so do the
        ** copy */
        int i;
        zeroPage(pPage, pChild->aData[0]);
        for(i=0; i<pChild->nCell; i++){
          apCell[i] = findCell(pChild,i);
          szCell[i] = cellSizePtr(pChild, apCell[i]);
        }
        assemblePage(pPage, pChild->nCell, apCell, szCell);
        /* Copy the right-pointer of the child to the parent. */
        assert( sqlite3PagerIswriteable(pPage->pDbPage) );
        put4byte(&pPage->aData[pPage->hdrOffset+8], 
            get4byte(&pChild->aData[pChild->hdrOffset+8]));
        rc = freePage(pChild);
        TRACE(("BALANCE: child %d transfer to page 1\n", pChild->pgno));
      }else{
        /* The child has more information that will fit on the root.
        ** The tree is already balanced.  Do nothing. */
        TRACE(("BALANCE: child %d will not fit on page 1\n", pChild->pgno));
      }
    }else{
      memcpy(pPage->aData, pChild->aData, pPage->pBt->usableSize);
      pPage->isInit = 0;
      rc = sqlite3BtreeInitPage(pPage);
      assert( rc==SQLITE_OK );
      freePage(pChild);
      TRACE(("BALANCE: transfer child %d into root %d\n",
              pChild->pgno, pPage->pgno));
    }
    assert( pPage->nOverflow==0 );
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( ISAUTOVACUUM && rc==SQLITE_OK ){
      rc = setChildPtrmaps(pPage);
    }
#endif
    releasePage(pChild);
  }
end_shallow_balance:
  sqlite3_free(apCell);
  return rc;
}


/*
** The root page is overfull
**
** When this happens, Create a new child page and copy the
** contents of the root into the child.  Then make the root
** page an empty page with rightChild pointing to the new
** child.   Finally, call balance_internal() on the new child
** to cause it to split.
*/
static int balance_deeper(BtCursor *pCur){
  int rc;             /* Return value from subprocedures */
  MemPage *pPage;     /* Pointer to the root page */
  MemPage *pChild;    /* Pointer to a new child page */
  Pgno pgnoChild;     /* Page number of the new child page */
  BtShared *pBt;         /* The BTree */
  int usableSize;     /* Total usable size of a page */
  u8 *data;           /* Content of the parent page */
  u8 *cdata;          /* Content of the child page */
  int hdr;            /* Offset to page header in parent */
  int cbrk;           /* Offset to content of first cell in parent */

  assert( pCur->iPage==0 );
  assert( pCur->apPage[0]->nOverflow>0 );

  VVA_ONLY( pCur->pagesShuffled = 1 );
  pPage = pCur->apPage[0];
  pBt = pPage->pBt;
  assert( sqlite3_mutex_held(pBt->mutex) );
  assert( sqlite3PagerIswriteable(pPage->pDbPage) );
  rc = allocateBtreePage(pBt, &pChild, &pgnoChild, pPage->pgno, 0);
  if( rc ) return rc;
  assert( sqlite3PagerIswriteable(pChild->pDbPage) );
  usableSize = pBt->usableSize;
  data = pPage->aData;
  hdr = pPage->hdrOffset;
  cbrk = get2byte(&data[hdr+5]);
  cdata = pChild->aData;
  memcpy(cdata, &data[hdr], pPage->cellOffset+2*pPage->nCell-hdr);
  memcpy(&cdata[cbrk], &data[cbrk], usableSize-cbrk);

  assert( pChild->isInit==0 );
  rc = sqlite3BtreeInitPage(pChild);
  if( rc==SQLITE_OK ){
    int nCopy = pPage->nOverflow*sizeof(pPage->aOvfl[0]);
    memcpy(pChild->aOvfl, pPage->aOvfl, nCopy);
    pChild->nOverflow = pPage->nOverflow;
    if( pChild->nOverflow ){
      pChild->nFree = 0;
    }
    assert( pChild->nCell==pPage->nCell );
    assert( sqlite3PagerIswriteable(pPage->pDbPage) );
    zeroPage(pPage, pChild->aData[0] & ~PTF_LEAF);
    put4byte(&pPage->aData[pPage->hdrOffset+8], pgnoChild);
    TRACE(("BALANCE: copy root %d into %d\n", pPage->pgno, pChild->pgno));
    if( ISAUTOVACUUM ){
      rc = ptrmapPut(pBt, pChild->pgno, PTRMAP_BTREE, pPage->pgno);
#ifndef SQLITE_OMIT_AUTOVACUUM
      if( rc==SQLITE_OK ){
        rc = setChildPtrmaps(pChild);
      }
      if( rc ){
        pChild->nOverflow = 0;
      }
#endif
    }
  }

  if( rc==SQLITE_OK ){
    pCur->iPage++;
    pCur->apPage[1] = pChild;
    pCur->aiIdx[0] = 0;
    rc = balance_nonroot(pCur);
  }else{
    releasePage(pChild);
  }

  return rc;
}

/*
** The page that pCur currently points to has just been modified in
** some way. This function figures out if this modification means the
** tree needs to be balanced, and if so calls the appropriate balancing 
** routine.
** 
** Parameter isInsert is true if a new cell was just inserted into the
** page, or false otherwise.
*/
static int balance(BtCursor *pCur, int isInsert){
  int rc = SQLITE_OK;
  MemPage *pPage = pCur->apPage[pCur->iPage];

  assert( sqlite3_mutex_held(pPage->pBt->mutex) );
  if( pCur->iPage==0 ){
    rc = sqlite3PagerWrite(pPage->pDbPage);
    if( rc==SQLITE_OK && pPage->nOverflow>0 ){
      rc = balance_deeper(pCur);
      assert( pCur->apPage[0]==pPage );
      assert( pPage->nOverflow==0 || rc!=SQLITE_OK );
    }
    if( rc==SQLITE_OK && pPage->nCell==0 ){
      rc = balance_shallower(pCur);
      assert( pCur->apPage[0]==pPage );
      assert( pPage->nOverflow==0 || rc!=SQLITE_OK );
    }
  }else{
    if( pPage->nOverflow>0 || 
        (!isInsert && pPage->nFree>pPage->pBt->usableSize*2/3) ){
      rc = balance_nonroot(pCur);
    }
  }
  return rc;
}

/*
** This routine checks all cursors that point to table pgnoRoot.
** If any of those cursors were opened with wrFlag==0 in a different
** database connection (a database connection that shares the pager
** cache with the current connection) and that other connection 
** is not in the ReadUncommmitted state, then this routine returns 
** SQLITE_LOCKED.
**
** As well as cursors with wrFlag==0, cursors with wrFlag==1 and 
** isIncrblobHandle==1 are also considered 'read' cursors. Incremental 
** blob cursors are used for both reading and writing.
**
** When pgnoRoot is the root page of an intkey table, this function is also
** responsible for invalidating incremental blob cursors when the table row
** on which they are opened is deleted or modified. Cursors are invalidated
** according to the following rules:
**
**   1) When BtreeClearTable() is called to completely delete the contents
**      of a B-Tree table, pExclude is set to zero and parameter iRow is 
**      set to non-zero. In this case all incremental blob cursors open
**      on the table rooted at pgnoRoot are invalidated.
**
**   2) When BtreeInsert(), BtreeDelete() or BtreePutData() is called to 
**      modify a table row via an SQL statement, pExclude is set to the 
**      write cursor used to do the modification and parameter iRow is set
**      to the integer row id of the B-Tree entry being modified. Unless
**      pExclude is itself an incremental blob cursor, then all incremental
**      blob cursors open on row iRow of the B-Tree are invalidated.
**
**   3) If both pExclude and iRow are set to zero, no incremental blob 
**      cursors are invalidated.
*/
static int checkReadLocks(
  Btree *pBtree, 
  Pgno pgnoRoot, 
  BtCursor *pExclude,
  i64 iRow
){
  BtCursor *p;
  BtShared *pBt = pBtree->pBt;
  sqlite3 *db = pBtree->db;
  assert( sqlite3BtreeHoldsMutex(pBtree) );
  for(p=pBt->pCursor; p; p=p->pNext){
    if( p==pExclude ) continue;
    if( p->pgnoRoot!=pgnoRoot ) continue;
#ifndef SQLITE_OMIT_INCRBLOB
    if( p->isIncrblobHandle && ( 
         (!pExclude && iRow)
      || (pExclude && !pExclude->isIncrblobHandle && p->info.nKey==iRow)
    )){
      p->eState = CURSOR_INVALID;
    }
#endif
    if( p->eState!=CURSOR_VALID ) continue;
    if( p->wrFlag==0 
#ifndef SQLITE_OMIT_INCRBLOB
     || p->isIncrblobHandle
#endif
    ){
      sqlite3 *dbOther = p->pBtree->db;
      if( dbOther==0 ||
         (dbOther!=db && (dbOther->flags & SQLITE_ReadUncommitted)==0) ){
        return SQLITE_LOCKED;
      }
    }
  }
  return SQLITE_OK;
}

/*
** Insert a new record into the BTree.  The key is given by (pKey,nKey)
** and the data is given by (pData,nData).  The cursor is used only to
** define what table the record should be inserted into.  The cursor
** is left pointing at a random location.
**
** For an INTKEY table, only the nKey value of the key is used.  pKey is
** ignored.  For a ZERODATA table, the pData and nData are both ignored.
*/
int sqlite3BtreeInsert(
  BtCursor *pCur,                /* Insert data into the table of this cursor */
  const void *pKey, i64 nKey,    /* The key of the new record */
  const void *pData, int nData,  /* The data of the new record */
  int nZero,                     /* Number of extra 0 bytes to append to data */
  int appendBias                 /* True if this is likely an append */
){
  int rc;
  int loc;
  int szNew;
  int idx;
  MemPage *pPage;
  Btree *p = pCur->pBtree;
  BtShared *pBt = p->pBt;
  unsigned char *oldCell;
  unsigned char *newCell = 0;

  assert( cursorHoldsMutex(pCur) );
  assert( pBt->inTransaction==TRANS_WRITE );
  assert( !pBt->readOnly );
  assert( pCur->wrFlag );
  if( checkReadLocks(pCur->pBtree, pCur->pgnoRoot, pCur, nKey) ){
    return SQLITE_LOCKED; /* The table pCur points to has a read lock */
  }
  if( pCur->eState==CURSOR_FAULT ){
    return pCur->skip;
  }

  /* Save the positions of any other cursors open on this table */
  sqlite3BtreeClearCursor(pCur);
  if( 
    SQLITE_OK!=(rc = saveAllCursors(pBt, pCur->pgnoRoot, pCur)) ||
    SQLITE_OK!=(rc = sqlite3BtreeMoveto(pCur, pKey, nKey, appendBias, &loc))
  ){
    return rc;
  }

  pPage = pCur->apPage[pCur->iPage];
  assert( pPage->intKey || nKey>=0 );
  assert( pPage->leaf || !pPage->intKey );
  TRACE(("INSERT: table=%d nkey=%lld ndata=%d page=%d %s\n",
          pCur->pgnoRoot, nKey, nData, pPage->pgno,
          loc==0 ? "overwrite" : "new entry"));
  assert( pPage->isInit );
  allocateTempSpace(pBt);
  newCell = pBt->pTmpSpace;
  if( newCell==0 ) return SQLITE_NOMEM;
  rc = fillInCell(pPage, newCell, pKey, nKey, pData, nData, nZero, &szNew);
  if( rc ) goto end_insert;
  assert( szNew==cellSizePtr(pPage, newCell) );
  assert( szNew<=MX_CELL_SIZE(pBt) );
  idx = pCur->aiIdx[pCur->iPage];
  if( loc==0 && CURSOR_VALID==pCur->eState ){
    u16 szOld;
    assert( idx<pPage->nCell );
    rc = sqlite3PagerWrite(pPage->pDbPage);
    if( rc ){
      goto end_insert;
    }
    oldCell = findCell(pPage, idx);
    if( !pPage->leaf ){
      memcpy(newCell, oldCell, 4);
    }
    szOld = cellSizePtr(pPage, oldCell);
    rc = clearCell(pPage, oldCell);
    if( rc ) goto end_insert;
    rc = dropCell(pPage, idx, szOld);
    if( rc!=SQLITE_OK ) {
      goto end_insert;
    }
  }else if( loc<0 && pPage->nCell>0 ){
    assert( pPage->leaf );
    idx = ++pCur->aiIdx[pCur->iPage];
    pCur->info.nSize = 0;
    pCur->validNKey = 0;
  }else{
    assert( pPage->leaf );
  }
  rc = insertCell(pPage, idx, newCell, szNew, 0, 0);
  if( rc==SQLITE_OK ){
    rc = balance(pCur, 1);
  }

  /* Must make sure nOverflow is reset to zero even if the balance()
  ** fails.  Internal data structure corruption will result otherwise. */
  pCur->apPage[pCur->iPage]->nOverflow = 0;

  if( rc==SQLITE_OK ){
    moveToRoot(pCur);
  }
end_insert:
  return rc;
}

/*
** Delete the entry that the cursor is pointing to.  The cursor
** is left pointing at a arbitrary location.
*/
int sqlite3BtreeDelete(BtCursor *pCur){
  MemPage *pPage = pCur->apPage[pCur->iPage];
  int idx;
  unsigned char *pCell;
  int rc;
  Pgno pgnoChild = 0;
  Btree *p = pCur->pBtree;
  BtShared *pBt = p->pBt;

  assert( cursorHoldsMutex(pCur) );
  assert( pPage->isInit );
  assert( pBt->inTransaction==TRANS_WRITE );
  assert( !pBt->readOnly );
  if( pCur->eState==CURSOR_FAULT ){
    return pCur->skip;
  }
  if( NEVER(pCur->aiIdx[pCur->iPage]>=pPage->nCell) ){
    return SQLITE_ERROR;  /* The cursor is not pointing to anything */
  }
  assert( pCur->wrFlag );
  if( checkReadLocks(pCur->pBtree, pCur->pgnoRoot, pCur, pCur->info.nKey) ){
    return SQLITE_LOCKED; /* The table pCur points to has a read lock */
  }

  /* Restore the current cursor position (a no-op if the cursor is not in 
  ** CURSOR_REQUIRESEEK state) and save the positions of any other cursors 
  ** open on the same table. Then call sqlite3PagerWrite() on the page
  ** that the entry will be deleted from.
  */
  if( 
    (rc = restoreCursorPosition(pCur))!=0 ||
    (rc = saveAllCursors(pBt, pCur->pgnoRoot, pCur))!=0 ||
    (rc = sqlite3PagerWrite(pPage->pDbPage))!=0
  ){
    return rc;
  }

  /* Locate the cell within its page and leave pCell pointing to the
  ** data. The clearCell() call frees any overflow pages associated with the
  ** cell. The cell itself is still intact.
  */
  idx = pCur->aiIdx[pCur->iPage];
  pCell = findCell(pPage, idx);
  if( !pPage->leaf ){
    pgnoChild = get4byte(pCell);
  }
  rc = clearCell(pPage, pCell);
  if( rc ){
    return rc;
  }

  if( !pPage->leaf ){
    /*
    ** The entry we are about to delete is not a leaf so if we do not
    ** do something we will leave a hole on an internal page.
    ** We have to fill the hole by moving in a cell from a leaf.  The
    ** next Cell after the one to be deleted is guaranteed to exist and
    ** to be a leaf so we can use it.
    */
    BtCursor leafCur;
    MemPage *pLeafPage = 0;

    unsigned char *pNext;
    int notUsed;
    unsigned char *tempCell = 0;
    assert( !pPage->intKey );
    sqlite3BtreeGetTempCursor(pCur, &leafCur);
    rc = sqlite3BtreeNext(&leafCur, &notUsed);
    if( rc==SQLITE_OK ){
      assert( leafCur.aiIdx[leafCur.iPage]==0 );
      pLeafPage = leafCur.apPage[leafCur.iPage];
      rc = sqlite3PagerWrite(pLeafPage->pDbPage);
    }
    if( rc==SQLITE_OK ){
      int leafCursorInvalid = 0;
      u16 szNext;
      TRACE(("DELETE: table=%d delete internal from %d replace from leaf %d\n",
         pCur->pgnoRoot, pPage->pgno, pLeafPage->pgno));
      dropCell(pPage, idx, cellSizePtr(pPage, pCell));
      pNext = findCell(pLeafPage, 0);
      szNext = cellSizePtr(pLeafPage, pNext);
      assert( MX_CELL_SIZE(pBt)>=szNext+4 );
      allocateTempSpace(pBt);
      tempCell = pBt->pTmpSpace;
      if( tempCell==0 ){
        rc = SQLITE_NOMEM;
      }
      if( rc==SQLITE_OK ){
        rc = insertCell(pPage, idx, pNext-4, szNext+4, tempCell, 0);
      }


      /* The "if" statement in the next code block is critical.  The
      ** slightest error in that statement would allow SQLite to operate
      ** correctly most of the time but produce very rare failures.  To
      ** guard against this, the following macros help to verify that
      ** the "if" statement is well tested.
      */
      testcase( pPage->nOverflow==0 && pPage->nFree<pBt->usableSize*2/3 
                 && pLeafPage->nFree+2+szNext > pBt->usableSize*2/3 );
      testcase( pPage->nOverflow==0 && pPage->nFree==pBt->usableSize*2/3 
                 && pLeafPage->nFree+2+szNext > pBt->usableSize*2/3 );
      testcase( pPage->nOverflow==0 && pPage->nFree==pBt->usableSize*2/3+1 
                 && pLeafPage->nFree+2+szNext > pBt->usableSize*2/3 );
      testcase( pPage->nOverflow>0 && pPage->nFree<=pBt->usableSize*2/3
                 && pLeafPage->nFree+2+szNext > pBt->usableSize*2/3 );
      testcase( (pPage->nOverflow>0 || (pPage->nFree > pBt->usableSize*2/3))
                 && pLeafPage->nFree+2+szNext == pBt->usableSize*2/3 );


      if( (pPage->nOverflow>0 || (pPage->nFree > pBt->usableSize*2/3)) &&
          (pLeafPage->nFree+2+szNext > pBt->usableSize*2/3)
      ){
        /* This branch is taken if the internal node is now either overflowing
        ** or underfull and the leaf node will be underfull after the just cell 
        ** copied to the internal node is deleted from it. This is a special
        ** case because the call to balance() to correct the internal node
        ** may change the tree structure and invalidate the contents of
        ** the leafCur.apPage[] and leafCur.aiIdx[] arrays, which will be
        ** used by the balance() required to correct the underfull leaf
        ** node.
        **
        ** The formula used in the expression above are based on facets of
        ** the SQLite file-format that do not change over time.
        */
        testcase( pPage->nFree==pBt->usableSize*2/3+1 );
        testcase( pLeafPage->nFree+2+szNext==pBt->usableSize*2/3+1 );
        leafCursorInvalid = 1;
      }        

      if( rc==SQLITE_OK ){
        assert( sqlite3PagerIswriteable(pPage->pDbPage) );
        put4byte(findOverflowCell(pPage, idx), pgnoChild);
        VVA_ONLY( pCur->pagesShuffled = 0 );
        rc = balance(pCur, 0);
      }

      if( rc==SQLITE_OK && leafCursorInvalid ){
        /* The leaf-node is now underfull and so the tree needs to be 
        ** rebalanced. However, the balance() operation on the internal
        ** node above may have modified the structure of the B-Tree and
        ** so the current contents of leafCur.apPage[] and leafCur.aiIdx[]
        ** may not be trusted.
        **
        ** It is not possible to copy the ancestry from pCur, as the same
        ** balance() call has invalidated the pCur->apPage[] and aiIdx[]
        ** arrays. 
        **
        ** The call to saveCursorPosition() below internally saves the 
        ** key that leafCur is currently pointing to. Currently, there
        ** are two copies of that key in the tree - one here on the leaf
        ** page and one on some internal node in the tree. The copy on
        ** the leaf node is always the next key in tree-order after the 
        ** copy on the internal node. So, the call to sqlite3BtreeNext()
        ** calls restoreCursorPosition() to point the cursor to the copy
        ** stored on the internal node, then advances to the next entry,
        ** which happens to be the copy of the key on the internal node.
        ** Net effect: leafCur is pointing back to the duplicate cell
        ** that needs to be removed, and the leafCur.apPage[] and
        ** leafCur.aiIdx[] arrays are correct.
        */
        VVA_ONLY( Pgno leafPgno = pLeafPage->pgno );
        rc = saveCursorPosition(&leafCur);
        if( rc==SQLITE_OK ){
          rc = sqlite3BtreeNext(&leafCur, &notUsed);
        }
        pLeafPage = leafCur.apPage[leafCur.iPage];
        assert( pLeafPage->pgno==leafPgno );
        assert( leafCur.aiIdx[leafCur.iPage]==0 );
      }

      if( SQLITE_OK==rc
       && SQLITE_OK==(rc = sqlite3PagerWrite(pLeafPage->pDbPage)) 
      ){
        dropCell(pLeafPage, 0, szNext);
        VVA_ONLY( leafCur.pagesShuffled = 0 );
        rc = balance(&leafCur, 0);
        assert( leafCursorInvalid || !leafCur.pagesShuffled
                                   || !pCur->pagesShuffled );
      }
    }
    sqlite3BtreeReleaseTempCursor(&leafCur);
  }else{
    TRACE(("DELETE: table=%d delete from leaf %d\n",
       pCur->pgnoRoot, pPage->pgno));
    rc = dropCell(pPage, idx, cellSizePtr(pPage, pCell));
    if( rc==SQLITE_OK ){
      rc = balance(pCur, 0);
    }
  }
  if( rc==SQLITE_OK ){
    moveToRoot(pCur);
  }
  return rc;
}

/*
** Create a new BTree table.  Write into *piTable the page
** number for the root page of the new table.
**
** The type of type is determined by the flags parameter.  Only the
** following values of flags are currently in use.  Other values for
** flags might not work:
**
**     BTREE_INTKEY|BTREE_LEAFDATA     Used for SQL tables with rowid keys
**     BTREE_ZERODATA                  Used for SQL indices
*/
static int btreeCreateTable(Btree *p, int *piTable, int flags){
  BtShared *pBt = p->pBt;
  MemPage *pRoot;
  Pgno pgnoRoot;
  int rc;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( pBt->inTransaction==TRANS_WRITE );
  assert( !pBt->readOnly );

#ifdef SQLITE_OMIT_AUTOVACUUM
  rc = allocateBtreePage(pBt, &pRoot, &pgnoRoot, 1, 0);
  if( rc ){
    return rc;
  }
#else
  if( pBt->autoVacuum ){
    Pgno pgnoMove;      /* Move a page here to make room for the root-page */
    MemPage *pPageMove; /* The page to move to. */

    /* Creating a new table may probably require moving an existing database
    ** to make room for the new tables root page. In case this page turns
    ** out to be an overflow page, delete all overflow page-map caches
    ** held by open cursors.
    */
    invalidateAllOverflowCache(pBt);

    /* Read the value of meta[3] from the database to determine where the
    ** root page of the new table should go. meta[3] is the largest root-page
    ** created so far, so the new root-page is (meta[3]+1).
    */
    rc = sqlite3BtreeGetMeta(p, 4, &pgnoRoot);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    pgnoRoot++;

    /* The new root-page may not be allocated on a pointer-map page, or the
    ** PENDING_BYTE page.
    */
    while( pgnoRoot==PTRMAP_PAGENO(pBt, pgnoRoot) ||
        pgnoRoot==PENDING_BYTE_PAGE(pBt) ){
      pgnoRoot++;
    }
    assert( pgnoRoot>=3 );

    /* Allocate a page. The page that currently resides at pgnoRoot will
    ** be moved to the allocated page (unless the allocated page happens
    ** to reside at pgnoRoot).
    */
    rc = allocateBtreePage(pBt, &pPageMove, &pgnoMove, pgnoRoot, 1);
    if( rc!=SQLITE_OK ){
      return rc;
    }

    if( pgnoMove!=pgnoRoot ){
      /* pgnoRoot is the page that will be used for the root-page of
      ** the new table (assuming an error did not occur). But we were
      ** allocated pgnoMove. If required (i.e. if it was not allocated
      ** by extending the file), the current page at position pgnoMove
      ** is already journaled.
      */
      u8 eType;
      Pgno iPtrPage;

      releasePage(pPageMove);

      /* Move the page currently at pgnoRoot to pgnoMove. */
      rc = sqlite3BtreeGetPage(pBt, pgnoRoot, &pRoot, 0);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      rc = ptrmapGet(pBt, pgnoRoot, &eType, &iPtrPage);
      if( rc!=SQLITE_OK || eType==PTRMAP_ROOTPAGE || eType==PTRMAP_FREEPAGE ){
        releasePage(pRoot);
        return rc;
      }
      assert( eType!=PTRMAP_ROOTPAGE );
      assert( eType!=PTRMAP_FREEPAGE );
      rc = relocatePage(pBt, pRoot, eType, iPtrPage, pgnoMove, 0);
      releasePage(pRoot);

      /* Obtain the page at pgnoRoot */
      if( rc!=SQLITE_OK ){
        return rc;
      }
      rc = sqlite3BtreeGetPage(pBt, pgnoRoot, &pRoot, 0);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      rc = sqlite3PagerWrite(pRoot->pDbPage);
      if( rc!=SQLITE_OK ){
        releasePage(pRoot);
        return rc;
      }
    }else{
      pRoot = pPageMove;
    } 

    /* Update the pointer-map and meta-data with the new root-page number. */
    rc = ptrmapPut(pBt, pgnoRoot, PTRMAP_ROOTPAGE, 0);
    if( rc ){
      releasePage(pRoot);
      return rc;
    }
    rc = sqlite3BtreeUpdateMeta(p, 4, pgnoRoot);
    if( rc ){
      releasePage(pRoot);
      return rc;
    }

  }else{
    rc = allocateBtreePage(pBt, &pRoot, &pgnoRoot, 1, 0);
    if( rc ) return rc;
  }
#endif
  assert( sqlite3PagerIswriteable(pRoot->pDbPage) );
  zeroPage(pRoot, flags | PTF_LEAF);
  sqlite3PagerUnref(pRoot->pDbPage);
  *piTable = (int)pgnoRoot;
  return SQLITE_OK;
}
int sqlite3BtreeCreateTable(Btree *p, int *piTable, int flags){
  int rc;
  sqlite3BtreeEnter(p);
  p->pBt->db = p->db;
  rc = btreeCreateTable(p, piTable, flags);
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Erase the given database page and all its children.  Return
** the page to the freelist.
*/
static int clearDatabasePage(
  BtShared *pBt,           /* The BTree that contains the table */
  Pgno pgno,            /* Page number to clear */
  int freePageFlag,     /* Deallocate page if true */
  int *pnChange
){
  MemPage *pPage = 0;
  int rc;
  unsigned char *pCell;
  int i;

  assert( sqlite3_mutex_held(pBt->mutex) );
  if( pgno>pagerPagecount(pBt) ){
    return SQLITE_CORRUPT_BKPT;
  }

  rc = getAndInitPage(pBt, pgno, &pPage);
  if( rc ) goto cleardatabasepage_out;
  for(i=0; i<pPage->nCell; i++){
    pCell = findCell(pPage, i);
    if( !pPage->leaf ){
      rc = clearDatabasePage(pBt, get4byte(pCell), 1, pnChange);
      if( rc ) goto cleardatabasepage_out;
    }
    rc = clearCell(pPage, pCell);
    if( rc ) goto cleardatabasepage_out;
  }
  if( !pPage->leaf ){
    rc = clearDatabasePage(pBt, get4byte(&pPage->aData[8]), 1, pnChange);
    if( rc ) goto cleardatabasepage_out;
  }else if( pnChange ){
    assert( pPage->intKey );
    *pnChange += pPage->nCell;
  }
  if( freePageFlag ){
    rc = freePage(pPage);
  }else if( (rc = sqlite3PagerWrite(pPage->pDbPage))==0 ){
    zeroPage(pPage, pPage->aData[0] | PTF_LEAF);
  }

cleardatabasepage_out:
  releasePage(pPage);
  return rc;
}

/*
** Delete all information from a single table in the database.  iTable is
** the page number of the root of the table.  After this routine returns,
** the root page is empty, but still exists.
**
** This routine will fail with SQLITE_LOCKED if there are any open
** read cursors on the table.  Open write cursors are moved to the
** root of the table.
**
** If pnChange is not NULL, then table iTable must be an intkey table. The
** integer value pointed to by pnChange is incremented by the number of
** entries in the table.
*/
int sqlite3BtreeClearTable(Btree *p, int iTable, int *pnChange){
  int rc;
  BtShared *pBt = p->pBt;
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( p->inTrans==TRANS_WRITE );
  if( (rc = checkReadLocks(p, iTable, 0, 1))!=SQLITE_OK ){
    /* nothing to do */
  }else if( SQLITE_OK!=(rc = saveAllCursors(pBt, iTable, 0)) ){
    /* nothing to do */
  }else{
    rc = clearDatabasePage(pBt, (Pgno)iTable, 0, pnChange);
  }
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Erase all information in a table and add the root of the table to
** the freelist.  Except, the root of the principle table (the one on
** page 1) is never added to the freelist.
**
** This routine will fail with SQLITE_LOCKED if there are any open
** cursors on the table.
**
** If AUTOVACUUM is enabled and the page at iTable is not the last
** root page in the database file, then the last root page 
** in the database file is moved into the slot formerly occupied by
** iTable and that last slot formerly occupied by the last root page
** is added to the freelist instead of iTable.  In this say, all
** root pages are kept at the beginning of the database file, which
** is necessary for AUTOVACUUM to work right.  *piMoved is set to the 
** page number that used to be the last root page in the file before
** the move.  If no page gets moved, *piMoved is set to 0.
** The last root page is recorded in meta[3] and the value of
** meta[3] is updated by this procedure.
*/
static int btreeDropTable(Btree *p, Pgno iTable, int *piMoved){
  int rc;
  MemPage *pPage = 0;
  BtShared *pBt = p->pBt;

  assert( sqlite3BtreeHoldsMutex(p) );
  assert( p->inTrans==TRANS_WRITE );

  /* It is illegal to drop a table if any cursors are open on the
  ** database. This is because in auto-vacuum mode the backend may
  ** need to move another root-page to fill a gap left by the deleted
  ** root page. If an open cursor was using this page a problem would 
  ** occur.
  */
  if( pBt->pCursor ){
    return SQLITE_LOCKED;
  }

  rc = sqlite3BtreeGetPage(pBt, (Pgno)iTable, &pPage, 0);
  if( rc ) return rc;
  rc = sqlite3BtreeClearTable(p, iTable, 0);
  if( rc ){
    releasePage(pPage);
    return rc;
  }

  *piMoved = 0;

  if( iTable>1 ){
#ifdef SQLITE_OMIT_AUTOVACUUM
    rc = freePage(pPage);
    releasePage(pPage);
#else
    if( pBt->autoVacuum ){
      Pgno maxRootPgno;
      rc = sqlite3BtreeGetMeta(p, 4, &maxRootPgno);
      if( rc!=SQLITE_OK ){
        releasePage(pPage);
        return rc;
      }

      if( iTable==maxRootPgno ){
        /* If the table being dropped is the table with the largest root-page
        ** number in the database, put the root page on the free list. 
        */
        rc = freePage(pPage);
        releasePage(pPage);
        if( rc!=SQLITE_OK ){
          return rc;
        }
      }else{
        /* The table being dropped does not have the largest root-page
        ** number in the database. So move the page that does into the 
        ** gap left by the deleted root-page.
        */
        MemPage *pMove;
        releasePage(pPage);
        rc = sqlite3BtreeGetPage(pBt, maxRootPgno, &pMove, 0);
        if( rc!=SQLITE_OK ){
          return rc;
        }
        rc = relocatePage(pBt, pMove, PTRMAP_ROOTPAGE, 0, iTable, 0);
        releasePage(pMove);
        if( rc!=SQLITE_OK ){
          return rc;
        }
        rc = sqlite3BtreeGetPage(pBt, maxRootPgno, &pMove, 0);
        if( rc!=SQLITE_OK ){
          return rc;
        }
        rc = freePage(pMove);
        releasePage(pMove);
        if( rc!=SQLITE_OK ){
          return rc;
        }
        *piMoved = maxRootPgno;
      }

      /* Set the new 'max-root-page' value in the database header. This
      ** is the old value less one, less one more if that happens to
      ** be a root-page number, less one again if that is the
      ** PENDING_BYTE_PAGE.
      */
      maxRootPgno--;
      if( maxRootPgno==PENDING_BYTE_PAGE(pBt) ){
        maxRootPgno--;
      }
      if( maxRootPgno==PTRMAP_PAGENO(pBt, maxRootPgno) ){
        maxRootPgno--;
      }
      assert( maxRootPgno!=PENDING_BYTE_PAGE(pBt) );

      rc = sqlite3BtreeUpdateMeta(p, 4, maxRootPgno);
    }else{
      rc = freePage(pPage);
      releasePage(pPage);
    }
#endif
  }else{
    /* If sqlite3BtreeDropTable was called on page 1. */
    zeroPage(pPage, PTF_INTKEY|PTF_LEAF );
    releasePage(pPage);
  }
  return rc;  
}
int sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved){
  int rc;
  sqlite3BtreeEnter(p);
  p->pBt->db = p->db;
  rc = btreeDropTable(p, iTable, piMoved);
  sqlite3BtreeLeave(p);
  return rc;
}


/*
** Read the meta-information out of a database file.  Meta[0]
** is the number of free pages currently in the database.  Meta[1]
** through meta[15] are available for use by higher layers.  Meta[0]
** is read-only, the others are read/write.
** 
** The schema layer numbers meta values differently.  At the schema
** layer (and the SetCookie and ReadCookie opcodes) the number of
** free pages is not visible.  So Cookie[0] is the same as Meta[1].
*/
int sqlite3BtreeGetMeta(Btree *p, int idx, u32 *pMeta){
  DbPage *pDbPage = 0;
  int rc;
  unsigned char *pP1;
  BtShared *pBt = p->pBt;

  sqlite3BtreeEnter(p);
  pBt->db = p->db;

  /* Reading a meta-data value requires a read-lock on page 1 (and hence
  ** the sqlite_master table. We grab this lock regardless of whether or
  ** not the SQLITE_ReadUncommitted flag is set (the table rooted at page
  ** 1 is treated as a special case by queryTableLock() and lockTable()).
  */
  rc = queryTableLock(p, 1, READ_LOCK);
  if( rc!=SQLITE_OK ){
    sqlite3BtreeLeave(p);
    return rc;
  }

  assert( idx>=0 && idx<=15 );
  if( pBt->pPage1 ){
    /* The b-tree is already holding a reference to page 1 of the database
    ** file. In this case the required meta-data value can be read directly
    ** from the page data of this reference. This is slightly faster than
    ** requesting a new reference from the pager layer.
    */
    pP1 = (unsigned char *)pBt->pPage1->aData;
  }else{
    /* The b-tree does not have a reference to page 1 of the database file.
    ** Obtain one from the pager layer.
    */
    rc = sqlite3PagerGet(pBt->pPager, 1, &pDbPage);
    if( rc ){
      sqlite3BtreeLeave(p);
      return rc;
    }
    pP1 = (unsigned char *)sqlite3PagerGetData(pDbPage);
  }
  *pMeta = get4byte(&pP1[36 + idx*4]);

  /* If the b-tree is not holding a reference to page 1, then one was 
  ** requested from the pager layer in the above block. Release it now.
  */
  if( !pBt->pPage1 ){
    sqlite3PagerUnref(pDbPage);
  }

  /* If autovacuumed is disabled in this build but we are trying to 
  ** access an autovacuumed database, then make the database readonly. 
  */
#ifdef SQLITE_OMIT_AUTOVACUUM
  if( idx==4 && *pMeta>0 ) pBt->readOnly = 1;
#endif

  /* Grab the read-lock on page 1. */
  rc = lockTable(p, 1, READ_LOCK);
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Write meta-information back into the database.  Meta[0] is
** read-only and may not be written.
*/
int sqlite3BtreeUpdateMeta(Btree *p, int idx, u32 iMeta){
  BtShared *pBt = p->pBt;
  unsigned char *pP1;
  int rc;
  assert( idx>=1 && idx<=15 );
  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  assert( p->inTrans==TRANS_WRITE );
  assert( pBt->pPage1!=0 );
  pP1 = pBt->pPage1->aData;
  rc = sqlite3PagerWrite(pBt->pPage1->pDbPage);
  if( rc==SQLITE_OK ){
    put4byte(&pP1[36 + idx*4], iMeta);
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( idx==7 ){
      assert( pBt->autoVacuum || iMeta==0 );
      assert( iMeta==0 || iMeta==1 );
      pBt->incrVacuum = (u8)iMeta;
    }
#endif
  }
  sqlite3BtreeLeave(p);
  return rc;
}

/*
** Return the flag byte at the beginning of the page that the cursor
** is currently pointing to.
*/
int sqlite3BtreeFlags(BtCursor *pCur){
  /* TODO: What about CURSOR_REQUIRESEEK state? Probably need to call
  ** restoreCursorPosition() here.
  */
  MemPage *pPage;
  restoreCursorPosition(pCur);
  pPage = pCur->apPage[pCur->iPage];
  assert( cursorHoldsMutex(pCur) );
  assert( pPage!=0 );
  assert( pPage->pBt==pCur->pBt );
  return pPage->aData[pPage->hdrOffset];
}


/*
** Return the pager associated with a BTree.  This routine is used for
** testing and debugging only.
*/
Pager *sqlite3BtreePager(Btree *p){
  return p->pBt->pPager;
}

#ifndef SQLITE_OMIT_INTEGRITY_CHECK
/*
** Append a message to the error message string.
*/
static void checkAppendMsg(
  IntegrityCk *pCheck,
  char *zMsg1,
  const char *zFormat,
  ...
){
  va_list ap;
  if( !pCheck->mxErr ) return;
  pCheck->mxErr--;
  pCheck->nErr++;
  va_start(ap, zFormat);
  if( pCheck->errMsg.nChar ){
    sqlite3StrAccumAppend(&pCheck->errMsg, "\n", 1);
  }
  if( zMsg1 ){
    sqlite3StrAccumAppend(&pCheck->errMsg, zMsg1, -1);
  }
  sqlite3VXPrintf(&pCheck->errMsg, 1, zFormat, ap);
  va_end(ap);
  if( pCheck->errMsg.mallocFailed ){
    pCheck->mallocFailed = 1;
  }
}
#endif /* SQLITE_OMIT_INTEGRITY_CHECK */

#ifndef SQLITE_OMIT_INTEGRITY_CHECK
/*
** Add 1 to the reference count for page iPage.  If this is the second
** reference to the page, add an error message to pCheck->zErrMsg.
** Return 1 if there are 2 ore more references to the page and 0 if
** if this is the first reference to the page.
**
** Also check that the page number is in bounds.
*/
static int checkRef(IntegrityCk *pCheck, Pgno iPage, char *zContext){
  if( iPage==0 ) return 1;
  if( iPage>pCheck->nPage ){
    checkAppendMsg(pCheck, zContext, "invalid page number %d", iPage);
    return 1;
  }
  if( pCheck->anRef[iPage]==1 ){
    checkAppendMsg(pCheck, zContext, "2nd reference to page %d", iPage);
    return 1;
  }
  return  (pCheck->anRef[iPage]++)>1;
}

#ifndef SQLITE_OMIT_AUTOVACUUM
/*
** Check that the entry in the pointer-map for page iChild maps to 
** page iParent, pointer type ptrType. If not, append an error message
** to pCheck.
*/
static void checkPtrmap(
  IntegrityCk *pCheck,   /* Integrity check context */
  Pgno iChild,           /* Child page number */
  u8 eType,              /* Expected pointer map type */
  Pgno iParent,          /* Expected pointer map parent page number */
  char *zContext         /* Context description (used for error msg) */
){
  int rc;
  u8 ePtrmapType;
  Pgno iPtrmapParent;

  rc = ptrmapGet(pCheck->pBt, iChild, &ePtrmapType, &iPtrmapParent);
  if( rc!=SQLITE_OK ){
    if( rc==SQLITE_NOMEM ) pCheck->mallocFailed = 1;
    checkAppendMsg(pCheck, zContext, "Failed to read ptrmap key=%d", iChild);
    return;
  }

  if( ePtrmapType!=eType || iPtrmapParent!=iParent ){
    checkAppendMsg(pCheck, zContext, 
      "Bad ptr map entry key=%d expected=(%d,%d) got=(%d,%d)", 
      iChild, eType, iParent, ePtrmapType, iPtrmapParent);
  }
}
#endif

/*
** Check the integrity of the freelist or of an overflow page list.
** Verify that the number of pages on the list is N.
*/
static void checkList(
  IntegrityCk *pCheck,  /* Integrity checking context */
  int isFreeList,       /* True for a freelist.  False for overflow page list */
  int iPage,            /* Page number for first page in the list */
  int N,                /* Expected number of pages in the list */
  char *zContext        /* Context for error messages */
){
  int i;
  int expected = N;
  int iFirst = iPage;
  while( N-- > 0 && pCheck->mxErr ){
    DbPage *pOvflPage;
    unsigned char *pOvflData;
    if( iPage<1 ){
      checkAppendMsg(pCheck, zContext,
         "%d of %d pages missing from overflow list starting at %d",
          N+1, expected, iFirst);
      break;
    }
    if( checkRef(pCheck, iPage, zContext) ) break;
    if( sqlite3PagerGet(pCheck->pPager, (Pgno)iPage, &pOvflPage) ){
      checkAppendMsg(pCheck, zContext, "failed to get page %d", iPage);
      break;
    }
    pOvflData = (unsigned char *)sqlite3PagerGetData(pOvflPage);
    if( isFreeList ){
      int n = get4byte(&pOvflData[4]);
#ifndef SQLITE_OMIT_AUTOVACUUM
      if( pCheck->pBt->autoVacuum ){
        checkPtrmap(pCheck, iPage, PTRMAP_FREEPAGE, 0, zContext);
      }
#endif
      if( n>pCheck->pBt->usableSize/4-2 ){
        checkAppendMsg(pCheck, zContext,
           "freelist leaf count too big on page %d", iPage);
        N--;
      }else{
        for(i=0; i<n; i++){
          Pgno iFreePage = get4byte(&pOvflData[8+i*4]);
#ifndef SQLITE_OMIT_AUTOVACUUM
          if( pCheck->pBt->autoVacuum ){
            checkPtrmap(pCheck, iFreePage, PTRMAP_FREEPAGE, 0, zContext);
          }
#endif
          checkRef(pCheck, iFreePage, zContext);
        }
        N -= n;
      }
    }
#ifndef SQLITE_OMIT_AUTOVACUUM
    else{
      /* If this database supports auto-vacuum and iPage is not the last
      ** page in this overflow list, check that the pointer-map entry for
      ** the following page matches iPage.
      */
      if( pCheck->pBt->autoVacuum && N>0 ){
        i = get4byte(pOvflData);
        checkPtrmap(pCheck, i, PTRMAP_OVERFLOW2, iPage, zContext);
      }
    }
#endif
    iPage = get4byte(pOvflData);
    sqlite3PagerUnref(pOvflPage);
  }
}
#endif /* SQLITE_OMIT_INTEGRITY_CHECK */

#ifndef SQLITE_OMIT_INTEGRITY_CHECK
/*
** Do various sanity checks on a single page of a tree.  Return
** the tree depth.  Root pages return 0.  Parents of root pages
** return 1, and so forth.
** 
** These checks are done:
**
**      1.  Make sure that cells and freeblocks do not overlap
**          but combine to completely cover the page.
**  NO  2.  Make sure cell keys are in order.
**  NO  3.  Make sure no key is less than or equal to zLowerBound.
**  NO  4.  Make sure no key is greater than or equal to zUpperBound.
**      5.  Check the integrity of overflow pages.
**      6.  Recursively call checkTreePage on all children.
**      7.  Verify that the depth of all children is the same.
**      8.  Make sure this page is at least 33% full or else it is
**          the root of the tree.
*/
static int checkTreePage(
  IntegrityCk *pCheck,  /* Context for the sanity check */
  int iPage,            /* Page number of the page to check */
  char *zParentContext  /* Parent context */
){
  MemPage *pPage;
  int i, rc, depth, d2, pgno, cnt;
  int hdr, cellStart;
  int nCell;
  u8 *data;
  BtShared *pBt;
  int usableSize;
  char zContext[100];
  char *hit = 0;

  sqlite3_snprintf(sizeof(zContext), zContext, "Page %d: ", iPage);

  /* Check that the page exists
  */
  pBt = pCheck->pBt;
  usableSize = pBt->usableSize;
  if( iPage==0 ) return 0;
  if( checkRef(pCheck, iPage, zParentContext) ) return 0;
  if( (rc = sqlite3BtreeGetPage(pBt, (Pgno)iPage, &pPage, 0))!=0 ){
    if( rc==SQLITE_NOMEM ) pCheck->mallocFailed = 1;
    checkAppendMsg(pCheck, zContext,
       "unable to get the page. error code=%d", rc);
    return 0;
  }
  if( (rc = sqlite3BtreeInitPage(pPage))!=0 ){
    assert( rc==SQLITE_CORRUPT );  /* The only possible error from InitPage */
    checkAppendMsg(pCheck, zContext, 
                   "sqlite3BtreeInitPage() returns error code %d", rc);
    releasePage(pPage);
    return 0;
  }

  /* Check out all the cells.
  */
  depth = 0;
  for(i=0; i<pPage->nCell && pCheck->mxErr; i++){
    u8 *pCell;
    u32 sz;
    CellInfo info;

    /* Check payload overflow pages
    */
    sqlite3_snprintf(sizeof(zContext), zContext,
             "On tree page %d cell %d: ", iPage, i);
    pCell = findCell(pPage,i);
    sqlite3BtreeParseCellPtr(pPage, pCell, &info);
    sz = info.nData;
    if( !pPage->intKey ) sz += (int)info.nKey;
    assert( sz==info.nPayload );
    if( sz>info.nLocal ){
      int nPage = (sz - info.nLocal + usableSize - 5)/(usableSize - 4);
      Pgno pgnoOvfl = get4byte(&pCell[info.iOverflow]);
#ifndef SQLITE_OMIT_AUTOVACUUM
      if( pBt->autoVacuum ){
        checkPtrmap(pCheck, pgnoOvfl, PTRMAP_OVERFLOW1, iPage, zContext);
      }
#endif
      checkList(pCheck, 0, pgnoOvfl, nPage, zContext);
    }

    /* Check sanity of left child page.
    */
    if( !pPage->leaf ){
      pgno = get4byte(pCell);
#ifndef SQLITE_OMIT_AUTOVACUUM
      if( pBt->autoVacuum ){
        checkPtrmap(pCheck, pgno, PTRMAP_BTREE, iPage, zContext);
      }
#endif
      d2 = checkTreePage(pCheck, pgno, zContext);
      if( i>0 && d2!=depth ){
        checkAppendMsg(pCheck, zContext, "Child page depth differs");
      }
      depth = d2;
    }
  }
  if( !pPage->leaf ){
    pgno = get4byte(&pPage->aData[pPage->hdrOffset+8]);
    sqlite3_snprintf(sizeof(zContext), zContext, 
                     "On page %d at right child: ", iPage);
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pBt->autoVacuum ){
      checkPtrmap(pCheck, pgno, PTRMAP_BTREE, iPage, 0);
    }
#endif
    checkTreePage(pCheck, pgno, zContext);
  }
 
  /* Check for complete coverage of the page
  */
  data = pPage->aData;
  hdr = pPage->hdrOffset;
  hit = sqlite3PageMalloc( pBt->pageSize );
  if( hit==0 ){
    pCheck->mallocFailed = 1;
  }else{
    u16 contentOffset = get2byte(&data[hdr+5]);
    if (contentOffset > usableSize) {
      checkAppendMsg(pCheck, 0, 
                     "Corruption detected in header on page %d",iPage,0);
      goto check_page_abort;
    }
    memset(hit+contentOffset, 0, usableSize-contentOffset);
    memset(hit, 1, contentOffset);
    nCell = get2byte(&data[hdr+3]);
    cellStart = hdr + 12 - 4*pPage->leaf;
    for(i=0; i<nCell; i++){
      int pc = get2byte(&data[cellStart+i*2]);
      u16 size = 1024;
      int j;
      if( pc<=usableSize ){
        size = cellSizePtr(pPage, &data[pc]);
      }
      if( (pc+size-1)>=usableSize || pc<0 ){
        checkAppendMsg(pCheck, 0, 
            "Corruption detected in cell %d on page %d",i,iPage,0);
      }else{
        for(j=pc+size-1; j>=pc; j--) hit[j]++;
      }
    }
    for(cnt=0, i=get2byte(&data[hdr+1]); i>0 && i<usableSize && cnt<10000; 
           cnt++){
      int size = get2byte(&data[i+2]);
      int j;
      if( (i+size-1)>=usableSize || i<0 ){
        checkAppendMsg(pCheck, 0,  
            "Corruption detected in cell %d on page %d",i,iPage,0);
      }else{
        for(j=i+size-1; j>=i; j--) hit[j]++;
      }
      i = get2byte(&data[i]);
    }
    for(i=cnt=0; i<usableSize; i++){
      if( hit[i]==0 ){
        cnt++;
      }else if( hit[i]>1 ){
        checkAppendMsg(pCheck, 0,
          "Multiple uses for byte %d of page %d", i, iPage);
        break;
      }
    }
    if( cnt!=data[hdr+7] ){
      checkAppendMsg(pCheck, 0, 
          "Fragmented space is %d byte reported as %d on page %d",
          cnt, data[hdr+7], iPage);
    }
  }
check_page_abort:
  if (hit) sqlite3PageFree(hit);

  releasePage(pPage);
  return depth+1;
}
#endif /* SQLITE_OMIT_INTEGRITY_CHECK */

#ifndef SQLITE_OMIT_INTEGRITY_CHECK
/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of
** a table.  nRoot is the number of entries in aRoot.
**
** Write the number of error seen in *pnErr.  Except for some memory
** allocation errors,  an error message held in memory obtained from
** malloc is returned if *pnErr is non-zero.  If *pnErr==0 then NULL is
** returned.  If a memory allocation error occurs, NULL is returned.
*/
char *sqlite3BtreeIntegrityCheck(
  Btree *p,     /* The btree to be checked */
  int *aRoot,   /* An array of root pages numbers for individual trees */
  int nRoot,    /* Number of entries in aRoot[] */
  int mxErr,    /* Stop reporting errors after this many */
  int *pnErr    /* Write number of errors seen to this variable */
){
  Pgno i;
  int nRef;
  IntegrityCk sCheck;
  BtShared *pBt = p->pBt;
  char zErr[100];

  sqlite3BtreeEnter(p);
  pBt->db = p->db;
  nRef = sqlite3PagerRefcount(pBt->pPager);
  if( lockBtreeWithRetry(p)!=SQLITE_OK ){
    *pnErr = 1;
    sqlite3BtreeLeave(p);
    return sqlite3DbStrDup(0, "cannot acquire a read lock on the database");
  }
  sCheck.pBt = pBt;
  sCheck.pPager = pBt->pPager;
  sCheck.nPage = pagerPagecount(sCheck.pBt);
  sCheck.mxErr = mxErr;
  sCheck.nErr = 0;
  sCheck.mallocFailed = 0;
  *pnErr = 0;
  if( sCheck.nPage==0 ){
    unlockBtreeIfUnused(pBt);
    sqlite3BtreeLeave(p);
    return 0;
  }
  sCheck.anRef = sqlite3Malloc( (sCheck.nPage+1)*sizeof(sCheck.anRef[0]) );
  if( !sCheck.anRef ){
    unlockBtreeIfUnused(pBt);
    *pnErr = 1;
    sqlite3BtreeLeave(p);
    return 0;
  }
  for(i=0; i<=sCheck.nPage; i++){ sCheck.anRef[i] = 0; }
  i = PENDING_BYTE_PAGE(pBt);
  if( i<=sCheck.nPage ){
    sCheck.anRef[i] = 1;
  }
  sqlite3StrAccumInit(&sCheck.errMsg, zErr, sizeof(zErr), 20000);

  /* Check the integrity of the freelist
  */
  checkList(&sCheck, 1, get4byte(&pBt->pPage1->aData[32]),
            get4byte(&pBt->pPage1->aData[36]), "Main freelist: ");

  /* Check all the tables.
  */
  for(i=0; (int)i<nRoot && sCheck.mxErr; i++){
    if( aRoot[i]==0 ) continue;
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pBt->autoVacuum && aRoot[i]>1 ){
      checkPtrmap(&sCheck, aRoot[i], PTRMAP_ROOTPAGE, 0, 0);
    }
#endif
    checkTreePage(&sCheck, aRoot[i], "List of tree roots: ");
  }

  /* Make sure every page in the file is referenced
  */
  for(i=1; i<=sCheck.nPage && sCheck.mxErr; i++){
#ifdef SQLITE_OMIT_AUTOVACUUM
    if( sCheck.anRef[i]==0 ){
      checkAppendMsg(&sCheck, 0, "Page %d is never used", i);
    }
#else
    /* If the database supports auto-vacuum, make sure no tables contain
    ** references to pointer-map pages.
    */
    if( sCheck.anRef[i]==0 && 
       (PTRMAP_PAGENO(pBt, i)!=i || !pBt->autoVacuum) ){
      checkAppendMsg(&sCheck, 0, "Page %d is never used", i);
    }
    if( sCheck.anRef[i]!=0 && 
       (PTRMAP_PAGENO(pBt, i)==i && pBt->autoVacuum) ){
      checkAppendMsg(&sCheck, 0, "Pointer map page %d is referenced", i);
    }
#endif
  }

  /* Make sure this analysis did not leave any unref() pages.
  ** This is an internal consistency check; an integrity check
  ** of the integrity check.
  */
  unlockBtreeIfUnused(pBt);
  if( NEVER(nRef != sqlite3PagerRefcount(pBt->pPager)) ){
    checkAppendMsg(&sCheck, 0, 
      "Outstanding page count goes from %d to %d during this analysis",
      nRef, sqlite3PagerRefcount(pBt->pPager)
    );
  }

  /* Clean  up and report errors.
  */
  sqlite3BtreeLeave(p);
  sqlite3_free(sCheck.anRef);
  if( sCheck.mallocFailed ){
    sqlite3StrAccumReset(&sCheck.errMsg);
    *pnErr = sCheck.nErr+1;
    return 0;
  }
  *pnErr = sCheck.nErr;
  if( sCheck.nErr==0 ) sqlite3StrAccumReset(&sCheck.errMsg);
  return sqlite3StrAccumFinish(&sCheck.errMsg);
}
#endif /* SQLITE_OMIT_INTEGRITY_CHECK */

/*
** Return the full pathname of the underlying database file.
**
** The pager filename is invariant as long as the pager is
** open so it is safe to access without the BtShared mutex.
*/
const char *sqlite3BtreeGetFilename(Btree *p){
  assert( p->pBt->pPager!=0 );
  return sqlite3PagerFilename(p->pBt->pPager);
}

/*
** Return the pathname of the journal file for this database. The return
** value of this routine is the same regardless of whether the journal file
** has been created or not.
**
** The pager journal filename is invariant as long as the pager is
** open so it is safe to access without the BtShared mutex.
*/
const char *sqlite3BtreeGetJournalname(Btree *p){
  assert( p->pBt->pPager!=0 );
  return sqlite3PagerJournalname(p->pBt->pPager);
}

/*
** Return non-zero if a transaction is active.
*/
int sqlite3BtreeIsInTrans(Btree *p){
  assert( p==0 || sqlite3_mutex_held(p->db->mutex) );
  return (p && (p->inTrans==TRANS_WRITE));
}

/*
** Return non-zero if a statement transaction is active.
*/
int sqlite3BtreeIsInStmt(Btree *p){
  assert( sqlite3BtreeHoldsMutex(p) );
  return ALWAYS(p->pBt) && p->pBt->inStmt;
}

/*
** Return non-zero if a read (or write) transaction is active.
*/
int sqlite3BtreeIsInReadTrans(Btree *p){
  assert( p );
  assert( sqlite3_mutex_held(p->db->mutex) );
  return p->inTrans!=TRANS_NONE;
}

int sqlite3BtreeIsInBackup(Btree *p){
  assert( p );
  assert( sqlite3_mutex_held(p->db->mutex) );
  return p->nBackup!=0;
}

/*
** This function returns a pointer to a blob of memory associated with
** a single shared-btree. The memory is used by client code for its own
** purposes (for example, to store a high-level schema associated with 
** the shared-btree). The btree layer manages reference counting issues.
**
** The first time this is called on a shared-btree, nBytes bytes of memory
** are allocated, zeroed, and returned to the caller. For each subsequent 
** call the nBytes parameter is ignored and a pointer to the same blob
** of memory returned. 
**
** If the nBytes parameter is 0 and the blob of memory has not yet been
** allocated, a null pointer is returned. If the blob has already been
** allocated, it is returned as normal.
**
** Just before the shared-btree is closed, the function passed as the 
** xFree argument when the memory allocation was made is invoked on the 
** blob of allocated memory. This function should not call sqlite3_free()
** on the memory, the btree layer does that.
*/
void *sqlite3BtreeSchema(Btree *p, int nBytes, void(*xFree)(void *)){
  BtShared *pBt = p->pBt;
  sqlite3BtreeEnter(p);
  if( !pBt->pSchema && nBytes ){
    pBt->pSchema = sqlite3MallocZero(nBytes);
    pBt->xFreeSchema = xFree;
  }
  sqlite3BtreeLeave(p);
  return pBt->pSchema;
}

/*
** Return true if another user of the same shared btree as the argument
** handle holds an exclusive lock on the sqlite_master table.
*/
int sqlite3BtreeSchemaLocked(Btree *p){
  int rc;
  assert( sqlite3_mutex_held(p->db->mutex) );
  sqlite3BtreeEnter(p);
  rc = (queryTableLock(p, MASTER_ROOT, READ_LOCK)!=SQLITE_OK);
  sqlite3BtreeLeave(p);
  return rc;
}


#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Obtain a lock on the table whose root page is iTab.  The
** lock is a write lock if isWritelock is true or a read lock
** if it is false.
*/
int sqlite3BtreeLockTable(Btree *p, int iTab, u8 isWriteLock){
  int rc = SQLITE_OK;
  if( p->sharable ){
    u8 lockType = READ_LOCK + isWriteLock;
    assert( READ_LOCK+1==WRITE_LOCK );
    assert( isWriteLock==0 || isWriteLock==1 );
    sqlite3BtreeEnter(p);
    rc = queryTableLock(p, iTab, lockType);
    if( rc==SQLITE_OK ){
      rc = lockTable(p, iTab, lockType);
    }
    sqlite3BtreeLeave(p);
  }
  return rc;
}
#endif

#ifndef SQLITE_OMIT_INCRBLOB
/*
** Argument pCsr must be a cursor opened for writing on an 
** INTKEY table currently pointing at a valid table entry. 
** This function modifies the data stored as part of that entry.
** Only the data content may only be modified, it is not possible
** to change the length of the data stored.
*/
int sqlite3BtreePutData(BtCursor *pCsr, u32 offset, u32 amt, void *z){
  assert( cursorHoldsMutex(pCsr) );
  assert( sqlite3_mutex_held(pCsr->pBtree->db->mutex) );
  assert(pCsr->isIncrblobHandle);

  restoreCursorPosition(pCsr);
  assert( pCsr->eState!=CURSOR_REQUIRESEEK );
  if( pCsr->eState!=CURSOR_VALID ){
    return SQLITE_ABORT;
  }

  /* Check some preconditions: 
  **   (a) the cursor is open for writing,
  **   (b) there is no read-lock on the table being modified and
  **   (c) the cursor points at a valid row of an intKey table.
  */
  if( !pCsr->wrFlag ){
    return SQLITE_READONLY;
  }
  assert( !pCsr->pBt->readOnly 
          && pCsr->pBt->inTransaction==TRANS_WRITE );
  if( checkReadLocks(pCsr->pBtree, pCsr->pgnoRoot, pCsr, 0) ){
    return SQLITE_LOCKED; /* The table pCur points to has a read lock */
  }
  if( pCsr->eState==CURSOR_INVALID || !pCsr->apPage[pCsr->iPage]->intKey ){
    return SQLITE_ERROR;
  }

  return accessPayload(pCsr, offset, amt, (unsigned char *)z, 0, 1);
}

/* 
** Set a flag on this cursor to cache the locations of pages from the 
** overflow list for the current row. This is used by cursors opened
** for incremental blob IO only.
**
** This function sets a flag only. The actual page location cache
** (stored in BtCursor.aOverflow[]) is allocated and used by function
** accessPayload() (the worker function for sqlite3BtreeData() and
** sqlite3BtreePutData()).
*/
void sqlite3BtreeCacheOverflow(BtCursor *pCur){
  assert( cursorHoldsMutex(pCur) );
  assert( sqlite3_mutex_held(pCur->pBtree->db->mutex) );
  assert(!pCur->isIncrblobHandle);
  assert(!pCur->aOverflow);
  pCur->isIncrblobHandle = 1;
}
#endif
