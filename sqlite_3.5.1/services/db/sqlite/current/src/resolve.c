/*
** 2008 August 18
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains routines used for walking the parser tree and
** resolve all identifiers by associating them with a particular
** table and column.
**
** $Id: resolve.c,v 1.1 2009/11/30 11:51:01 erik Exp $
*/
#include "sqliteInt.h"
#include <stdlib.h>
#include <string.h>

/*
** Turn the pExpr expression into an alias for the iCol-th column of the
** result set in pEList.
**
** If the result set column is a simple column reference, then this routine
** makes an exact copy.  But for any other kind of expression, this
** routine make a copy of the result set column as the argument to the
** TK_AS operator.  The TK_AS operator causes the expression to be
** evaluated just once and then reused for each alias.
**
** The reason for suppressing the TK_AS term when the expression is a simple
** column reference is so that the column reference will be recognized as
** usable by indices within the WHERE clause processing logic. 
**
** Hack:  The TK_AS operator is inhibited if zType[0]=='G'.  This means
** that in a GROUP BY clause, the expression is evaluated twice.  Hence:
**
**     SELECT random()%5 AS x, count(*) FROM tab GROUP BY x
**
** Is equivalent to:
**
**     SELECT random()%5 AS x, count(*) FROM tab GROUP BY random()%5
**
** The result of random()%5 in the GROUP BY clause is probably different
** from the result in the result-set.  We might fix this someday.  Or
** then again, we might not...
*/
static void resolveAlias(
  Parse *pParse,         /* Parsing context */
  ExprList *pEList,      /* A result set */
  int iCol,              /* A column in the result set.  0..pEList->nExpr-1 */
  Expr *pExpr,           /* Transform this into an alias to the result set */
  const char *zType      /* "GROUP" or "ORDER" or "" */
){
  Expr *pOrig;           /* The iCol-th column of the result set */
  Expr *pDup;            /* Copy of pOrig */
  sqlite3 *db;           /* The database connection */

  assert( iCol>=0 && iCol<pEList->nExpr );
  pOrig = pEList->a[iCol].pExpr;
  assert( pOrig!=0 );
  assert( pOrig->flags & EP_Resolved );
  db = pParse->db;
  pDup = sqlite3ExprDup(db, pOrig);
  if( pDup==0 ) return;
  if( pDup->op!=TK_COLUMN && zType[0]!='G' ){
    pDup = sqlite3PExpr(pParse, TK_AS, pDup, 0, 0);
    if( pDup==0 ) return;
    if( pEList->a[iCol].iAlias==0 ){
      pEList->a[iCol].iAlias = (u16)(++pParse->nAlias);
    }
    pDup->iTable = pEList->a[iCol].iAlias;
  }
  if( pExpr->flags & EP_ExpCollate ){
    pDup->pColl = pExpr->pColl;
    pDup->flags |= EP_ExpCollate;
  }
  sqlite3ExprClear(db, pExpr);
  memcpy(pExpr, pDup, sizeof(*pExpr));
  sqlite3DbFree(db, pDup);
}

/*
** Given the name of a column of the form X.Y.Z or Y.Z or just Z, look up
** that name in the set of source tables in pSrcList and make the pExpr 
** expression node refer back to that source column.  The following changes
** are made to pExpr:
**
**    pExpr->iDb           Set the index in db->aDb[] of the database X
**                         (even if X is implied).
**    pExpr->iTable        Set to the cursor number for the table obtained
**                         from pSrcList.
**    pExpr->pTab          Points to the Table structure of X.Y (even if
**                         X and/or Y are implied.)
**    pExpr->iColumn       Set to the column number within the table.
**    pExpr->op            Set to TK_COLUMN.
**    pExpr->pLeft         Any expression this points to is deleted
**    pExpr->pRight        Any expression this points to is deleted.
**
** The pDbToken is the name of the database (the "X").  This value may be
** NULL meaning that name is of the form Y.Z or Z.  Any available database
** can be used.  The pTableToken is the name of the table (the "Y").  This
** value can be NULL if pDbToken is also NULL.  If pTableToken is NULL it
** means that the form of the name is Z and that columns from any table
** can be used.
**
** If the name cannot be resolved unambiguously, leave an error message
** in pParse and return non-zero.  Return zero on success.
*/
static int lookupName(
  Parse *pParse,       /* The parsing context */
  Token *pDbToken,     /* Name of the database containing table, or NULL */
  Token *pTableToken,  /* Name of table containing column, or NULL */
  Token *pColumnToken, /* Name of the column. */
  NameContext *pNC,    /* The name context used to resolve the name */
  Expr *pExpr          /* Make this EXPR node point to the selected column */
){
  char *zDb = 0;       /* Name of the database.  The "X" in X.Y.Z */
  char *zTab = 0;      /* Name of the table.  The "Y" in X.Y.Z or Y.Z */
  char *zCol = 0;      /* Name of the column.  The "Z" */
  int i, j;            /* Loop counters */
  int cnt = 0;                      /* Number of matching column names */
  int cntTab = 0;                   /* Number of matching table names */
  sqlite3 *db = pParse->db;         /* The database connection */
  struct SrcList_item *pItem;       /* Use for looping over pSrcList items */
  struct SrcList_item *pMatch = 0;  /* The matching pSrcList item */
  NameContext *pTopNC = pNC;        /* First namecontext in the list */
  Schema *pSchema = 0;              /* Schema of the expression */

  assert( pColumnToken && pColumnToken->z ); /* The Z in X.Y.Z cannot be NULL */

  /* Dequote and zero-terminate the names */
  zDb = sqlite3NameFromToken(db, pDbToken);
  zTab = sqlite3NameFromToken(db, pTableToken);
  zCol = sqlite3NameFromToken(db, pColumnToken);
  if( db->mallocFailed ){
    goto lookupname_end;
  }

  /* Initialize the node to no-match */
  pExpr->iTable = -1;
  pExpr->pTab = 0;

  /* Start at the inner-most context and move outward until a match is found */
  while( pNC && cnt==0 ){
    ExprList *pEList;
    SrcList *pSrcList = pNC->pSrcList;

    if( pSrcList ){
      for(i=0, pItem=pSrcList->a; i<pSrcList->nSrc; i++, pItem++){
        Table *pTab;
        int iDb;
        Column *pCol;
  
        pTab = pItem->pTab;
        assert( pTab!=0 && pTab->zName!=0 );
        iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
        assert( pTab->nCol>0 );
        if( zTab ){
          if( pItem->zAlias ){
            char *zTabName = pItem->zAlias;
            if( sqlite3StrICmp(zTabName, zTab)!=0 ) continue;
          }else{
            char *zTabName = pTab->zName;
            if( zTabName==0 || sqlite3StrICmp(zTabName, zTab)!=0 ) continue;
            if( zDb!=0 && sqlite3StrICmp(db->aDb[iDb].zName, zDb)!=0 ){
              continue;
            }
          }
        }
        if( 0==(cntTab++) ){
          pExpr->iTable = pItem->iCursor;
          pExpr->pTab = pTab;
          pSchema = pTab->pSchema;
          pMatch = pItem;
        }
        for(j=0, pCol=pTab->aCol; j<pTab->nCol; j++, pCol++){
          if( sqlite3StrICmp(pCol->zName, zCol)==0 ){
            IdList *pUsing;
            cnt++;
            pExpr->iTable = pItem->iCursor;
            pExpr->pTab = pTab;
            pMatch = pItem;
            pSchema = pTab->pSchema;
            /* Substitute the rowid (column -1) for the INTEGER PRIMARY KEY */
            pExpr->iColumn = j==pTab->iPKey ? -1 : j;
            if( i<pSrcList->nSrc-1 ){
              if( pItem[1].jointype & JT_NATURAL ){
                /* If this match occurred in the left table of a natural join,
                ** then skip the right table to avoid a duplicate match */
                pItem++;
                i++;
              }else if( (pUsing = pItem[1].pUsing)!=0 ){
                /* If this match occurs on a column that is in the USING clause
                ** of a join, skip the search of the right table of the join
                ** to avoid a duplicate match there. */
                int k;
                for(k=0; k<pUsing->nId; k++){
                  if( sqlite3StrICmp(pUsing->a[k].zName, zCol)==0 ){
                    pItem++;
                    i++;
                    break;
                  }
                }
              }
            }
            break;
          }
        }
      }
    }

#ifndef SQLITE_OMIT_TRIGGER
    /* If we have not already resolved the name, then maybe 
    ** it is a new.* or old.* trigger argument reference
    */
    if( zDb==0 && zTab!=0 && cnt==0 && pParse->trigStack!=0 ){
      TriggerStack *pTriggerStack = pParse->trigStack;
      Table *pTab = 0;
      u32 *piColMask = 0;
      if( pTriggerStack->newIdx != -1 && sqlite3StrICmp("new", zTab) == 0 ){
        pExpr->iTable = pTriggerStack->newIdx;
        assert( pTriggerStack->pTab );
        pTab = pTriggerStack->pTab;
        piColMask = &(pTriggerStack->newColMask);
      }else if( pTriggerStack->oldIdx != -1 && sqlite3StrICmp("old", zTab)==0 ){
        pExpr->iTable = pTriggerStack->oldIdx;
        assert( pTriggerStack->pTab );
        pTab = pTriggerStack->pTab;
        piColMask = &(pTriggerStack->oldColMask);
      }

      if( pTab ){ 
        int iCol;
        Column *pCol = pTab->aCol;

        pSchema = pTab->pSchema;
        cntTab++;
        for(iCol=0; iCol < pTab->nCol; iCol++, pCol++) {
          if( sqlite3StrICmp(pCol->zName, zCol)==0 ){
            cnt++;
            pExpr->iColumn = iCol==pTab->iPKey ? -1 : iCol;
            pExpr->pTab = pTab;
            if( iCol>=0 ){
              testcase( iCol==31 );
              testcase( iCol==32 );
              *piColMask |= ((u32)1<<iCol) | (iCol>=32?0xffffffff:0);
            }
            break;
          }
        }
      }
    }
#endif /* !defined(SQLITE_OMIT_TRIGGER) */

    /*
    ** Perhaps the name is a reference to the ROWID
    */
    if( cnt==0 && cntTab==1 && sqlite3IsRowid(zCol) ){
      cnt = 1;
      pExpr->iColumn = -1;
      pExpr->affinity = SQLITE_AFF_INTEGER;
    }

    /*
    ** If the input is of the form Z (not Y.Z or X.Y.Z) then the name Z
    ** might refer to an result-set alias.  This happens, for example, when
    ** we are resolving names in the WHERE clause of the following command:
    **
    **     SELECT a+b AS x FROM table WHERE x<10;
    **
    ** In cases like this, replace pExpr with a copy of the expression that
    ** forms the result set entry ("a+b" in the example) and return immediately.
    ** Note that the expression in the result set should have already been
    ** resolved by the time the WHERE clause is resolved.
    */
    if( cnt==0 && (pEList = pNC->pEList)!=0 && zTab==0 ){
      for(j=0; j<pEList->nExpr; j++){
        char *zAs = pEList->a[j].zName;
        if( zAs!=0 && sqlite3StrICmp(zAs, zCol)==0 ){
          Expr *pOrig;
          assert( pExpr->pLeft==0 && pExpr->pRight==0 );
          assert( pExpr->pList==0 );
          assert( pExpr->pSelect==0 );
          pOrig = pEList->a[j].pExpr;
          if( !pNC->allowAgg && ExprHasProperty(pOrig, EP_Agg) ){
            sqlite3ErrorMsg(pParse, "misuse of aliased aggregate %s", zAs);
            sqlite3DbFree(db, zCol);
            return 2;
          }
          resolveAlias(pParse, pEList, j, pExpr, "");
          cnt = 1;
          pMatch = 0;
          assert( zTab==0 && zDb==0 );
          goto lookupname_end_2;
        }
      } 
    }

    /* Advance to the next name context.  The loop will exit when either
    ** we have a match (cnt>0) or when we run out of name contexts.
    */
    if( cnt==0 ){
      pNC = pNC->pNext;
    }
  }

  /*
  ** If X and Y are NULL (in other words if only the column name Z is
  ** supplied) and the value of Z is enclosed in double-quotes, then
  ** Z is a string literal if it doesn't match any column names.  In that
  ** case, we need to return right away and not make any changes to
  ** pExpr.
  **
  ** Because no reference was made to outer contexts, the pNC->nRef
  ** fields are not changed in any context.
  */
  if( cnt==0 && zTab==0 && pColumnToken->z[0]=='"' ){
    sqlite3DbFree(db, zCol);
    pExpr->op = TK_STRING;
    pExpr->pTab = 0;
    return 0;
  }

  /*
  ** cnt==0 means there was not match.  cnt>1 means there were two or
  ** more matches.  Either way, we have an error.
  */
  if( cnt!=1 ){
    const char *zErr;
    zErr = cnt==0 ? "no such column" : "ambiguous column name";
    if( zDb ){
      sqlite3ErrorMsg(pParse, "%s: %s.%s.%s", zErr, zDb, zTab, zCol);
    }else if( zTab ){
      sqlite3ErrorMsg(pParse, "%s: %s.%s", zErr, zTab, zCol);
    }else{
      sqlite3ErrorMsg(pParse, "%s: %s", zErr, zCol);
    }
    pTopNC->nErr++;
  }

  /* If a column from a table in pSrcList is referenced, then record
  ** this fact in the pSrcList.a[].colUsed bitmask.  Column 0 causes
  ** bit 0 to be set.  Column 1 sets bit 1.  And so forth.  If the
  ** column number is greater than the number of bits in the bitmask
  ** then set the high-order bit of the bitmask.
  */
  if( pExpr->iColumn>=0 && pMatch!=0 ){
    int n = pExpr->iColumn;
    testcase( n==BMS-1 );
    if( n>=BMS ){
      n = BMS-1;
    }
    assert( pMatch->iCursor==pExpr->iTable );
    pMatch->colUsed |= ((Bitmask)1)<<n;
  }

lookupname_end:
  /* Clean up and return
  */
  sqlite3DbFree(db, zDb);
  sqlite3DbFree(db, zTab);
  sqlite3ExprDelete(db, pExpr->pLeft);
  pExpr->pLeft = 0;
  sqlite3ExprDelete(db, pExpr->pRight);
  pExpr->pRight = 0;
  pExpr->op = TK_COLUMN;
lookupname_end_2:
  sqlite3DbFree(db, zCol);
  if( cnt==1 ){
    assert( pNC!=0 );
    sqlite3AuthRead(pParse, pExpr, pSchema, pNC->pSrcList);
    /* Increment the nRef value on all name contexts from TopNC up to
    ** the point where the name matched. */
    for(;;){
      assert( pTopNC!=0 );
      pTopNC->nRef++;
      if( pTopNC==pNC ) break;
      pTopNC = pTopNC->pNext;
    }
    return 0;
  } else {
    return 1;
  }
}

/*
** This routine is callback for sqlite3WalkExpr().
**
** Resolve symbolic names into TK_COLUMN operators for the current
** node in the expression tree.  Return 0 to continue the search down
** the tree or 2 to abort the tree walk.
**
** This routine also does error checking and name resolution for
** function names.  The operator for aggregate functions is changed
** to TK_AGG_FUNCTION.
*/
static int resolveExprStep(Walker *pWalker, Expr *pExpr){
  NameContext *pNC;
  Parse *pParse;

  pNC = pWalker->u.pNC;
  assert( pNC!=0 );
  pParse = pNC->pParse;
  assert( pParse==pWalker->pParse );

  if( ExprHasAnyProperty(pExpr, EP_Resolved) ) return WRC_Prune;
  ExprSetProperty(pExpr, EP_Resolved);
#ifndef NDEBUG
  if( pNC->pSrcList && pNC->pSrcList->nAlloc>0 ){
    SrcList *pSrcList = pNC->pSrcList;
    int i;
    for(i=0; i<pNC->pSrcList->nSrc; i++){
      assert( pSrcList->a[i].iCursor>=0 && pSrcList->a[i].iCursor<pParse->nTab);
    }
  }
#endif
  switch( pExpr->op ){

#if defined(SQLITE_ENABLE_UPDATE_DELETE_LIMIT) && !defined(SQLITE_OMIT_SUBQUERY)
    /* The special operator TK_ROW means use the rowid for the first
    ** column in the FROM clause.  This is used by the LIMIT and ORDER BY
    ** clause processing on UPDATE and DELETE statements.
    */
    case TK_ROW: {
      SrcList *pSrcList = pNC->pSrcList;
      struct SrcList_item *pItem;
      assert( pSrcList && pSrcList->nSrc==1 );
      pItem = pSrcList->a; 
      pExpr->op = TK_COLUMN;
      pExpr->pTab = pItem->pTab;
      pExpr->iTable = pItem->iCursor;
      pExpr->iColumn = -1;
      pExpr->affinity = SQLITE_AFF_INTEGER;
      break;
    }
#endif /* defined(SQLITE_ENABLE_UPDATE_DELETE_LIMIT) && !defined(SQLITE_OMIT_SUBQUERY) */

    /* A lone identifier is the name of a column.
    */
    case TK_ID: {
      lookupName(pParse, 0, 0, &pExpr->token, pNC, pExpr);
      return WRC_Prune;
    }
  
    /* A table name and column name:     ID.ID
    ** Or a database, table and column:  ID.ID.ID
    */
    case TK_DOT: {
      Token *pColumn;
      Token *pTable;
      Token *pDb;
      Expr *pRight;

      /* if( pSrcList==0 ) break; */
      pRight = pExpr->pRight;
      if( pRight->op==TK_ID ){
        pDb = 0;
        pTable = &pExpr->pLeft->token;
        pColumn = &pRight->token;
      }else{
        assert( pRight->op==TK_DOT );
        pDb = &pExpr->pLeft->token;
        pTable = &pRight->pLeft->token;
        pColumn = &pRight->pRight->token;
      }
      lookupName(pParse, pDb, pTable, pColumn, pNC, pExpr);
      return WRC_Prune;
    }

    /* Resolve function names
    */
    case TK_CONST_FUNC:
    case TK_FUNCTION: {
      ExprList *pList = pExpr->pList;    /* The argument list */
      int n = pList ? pList->nExpr : 0;  /* Number of arguments */
      int no_such_func = 0;       /* True if no such function exists */
      int wrong_num_args = 0;     /* True if wrong number of arguments */
      int is_agg = 0;             /* True if is an aggregate function */
      int auth;                   /* Authorization to use the function */
      int nId;                    /* Number of characters in function name */
      const char *zId;            /* The function name. */
      FuncDef *pDef;              /* Information about the function */
      u8 enc = ENC(pParse->db);   /* The database encoding */

      zId = (char*)pExpr->token.z;
      nId = pExpr->token.n;
      pDef = sqlite3FindFunction(pParse->db, zId, nId, n, enc, 0);
      if( pDef==0 ){
        pDef = sqlite3FindFunction(pParse->db, zId, nId, -1, enc, 0);
        if( pDef==0 ){
          no_such_func = 1;
        }else{
          wrong_num_args = 1;
        }
      }else{
        is_agg = pDef->xFunc==0;
      }
#ifndef SQLITE_OMIT_AUTHORIZATION
      if( pDef ){
        auth = sqlite3AuthCheck(pParse, SQLITE_FUNCTION, 0, pDef->zName, 0);
        if( auth!=SQLITE_OK ){
          if( auth==SQLITE_DENY ){
            sqlite3ErrorMsg(pParse, "not authorized to use function: %s",
                                    pDef->zName);
            pNC->nErr++;
          }
          pExpr->op = TK_NULL;
          return WRC_Prune;
        }
      }
#endif
      if( is_agg && !pNC->allowAgg ){
        sqlite3ErrorMsg(pParse, "misuse of aggregate function %.*s()", nId,zId);
        pNC->nErr++;
        is_agg = 0;
      }else if( no_such_func ){
        sqlite3ErrorMsg(pParse, "no such function: %.*s", nId, zId);
        pNC->nErr++;
      }else if( wrong_num_args ){
        sqlite3ErrorMsg(pParse,"wrong number of arguments to function %.*s()",
             nId, zId);
        pNC->nErr++;
      }
      if( is_agg ){
        pExpr->op = TK_AGG_FUNCTION;
        pNC->hasAgg = 1;
      }
      if( is_agg ) pNC->allowAgg = 0;
      sqlite3WalkExprList(pWalker, pList);
      if( is_agg ) pNC->allowAgg = 1;
      /* FIX ME:  Compute pExpr->affinity based on the expected return
      ** type of the function 
      */
      return WRC_Prune;
    }
#ifndef SQLITE_OMIT_SUBQUERY
    case TK_SELECT:
    case TK_EXISTS:
#endif
    case TK_IN: {
      if( pExpr->pSelect ){
        int nRef = pNC->nRef;
#ifndef SQLITE_OMIT_CHECK
        if( pNC->isCheck ){
          sqlite3ErrorMsg(pParse,"subqueries prohibited in CHECK constraints");
        }
#endif
        sqlite3WalkSelect(pWalker, pExpr->pSelect);
        assert( pNC->nRef>=nRef );
        if( nRef!=pNC->nRef ){
          ExprSetProperty(pExpr, EP_VarSelect);
        }
      }
      break;
    }
#ifndef SQLITE_OMIT_CHECK
    case TK_VARIABLE: {
      if( pNC->isCheck ){
        sqlite3ErrorMsg(pParse,"parameters prohibited in CHECK constraints");
      }
      break;
    }
#endif
  }
  return (pParse->nErr || pParse->db->mallocFailed) ? WRC_Abort : WRC_Continue;
}

/*
** pEList is a list of expressions which are really the result set of the
** a SELECT statement.  pE is a term in an ORDER BY or GROUP BY clause.
** This routine checks to see if pE is a simple identifier which corresponds
** to the AS-name of one of the terms of the expression list.  If it is,
** this routine return an integer between 1 and N where N is the number of
** elements in pEList, corresponding to the matching entry.  If there is
** no match, or if pE is not a simple identifier, then this routine
** return 0.
**
** pEList has been resolved.  pE has not.
*/
static int resolveAsName(
  Parse *pParse,     /* Parsing context for error messages */
  ExprList *pEList,  /* List of expressions to scan */
  Expr *pE           /* Expression we are trying to match */
){
  int i;             /* Loop counter */

  if( pE->op==TK_ID || (pE->op==TK_STRING && pE->token.z[0]!='\'') ){
    sqlite3 *db = pParse->db;
    char *zCol = sqlite3NameFromToken(db, &pE->token);
    if( zCol==0 ){
      return -1;
    }
    for(i=0; i<pEList->nExpr; i++){
      char *zAs = pEList->a[i].zName;
      if( zAs!=0 && sqlite3StrICmp(zAs, zCol)==0 ){
        sqlite3DbFree(db, zCol);
        return i+1;
      }
    }
    sqlite3DbFree(db, zCol);
  }
  return 0;
}

/*
** pE is a pointer to an expression which is a single term in the
** ORDER BY of a compound SELECT.  The expression has not been
** name resolved.
**
** At the point this routine is called, we already know that the
** ORDER BY term is not an integer index into the result set.  That
** case is handled by the calling routine.
**
** Attempt to match pE against result set columns in the left-most
** SELECT statement.  Return the index i of the matching column,
** as an indication to the caller that it should sort by the i-th column.
** The left-most column is 1.  In other words, the value returned is the
** same integer value that would be used in the SQL statement to indicate
** the column.
**
** If there is no match, return 0.  Return -1 if an error occurs.
*/
static int resolveOrderByTermToExprList(
  Parse *pParse,     /* Parsing context for error messages */
  Select *pSelect,   /* The SELECT statement with the ORDER BY clause */
  Expr *pE           /* The specific ORDER BY term */
){
  int i;             /* Loop counter */
  ExprList *pEList;  /* The columns of the result set */
  NameContext nc;    /* Name context for resolving pE */

  assert( sqlite3ExprIsInteger(pE, &i)==0 );
  pEList = pSelect->pEList;

  /* Resolve all names in the ORDER BY term expression
  */
  memset(&nc, 0, sizeof(nc));
  nc.pParse = pParse;
  nc.pSrcList = pSelect->pSrc;
  nc.pEList = pEList;
  nc.allowAgg = 1;
  nc.nErr = 0;
  if( sqlite3ResolveExprNames(&nc, pE) ){
    sqlite3ErrorClear(pParse);
    return 0;
  }

  /* Try to match the ORDER BY expression against an expression
  ** in the result set.  Return an 1-based index of the matching
  ** result-set entry.
  */
  for(i=0; i<pEList->nExpr; i++){
    if( sqlite3ExprCompare(pEList->a[i].pExpr, pE) ){
      return i+1;
    }
  }

  /* If no match, return 0. */
  return 0;
}

/*
** Generate an ORDER BY or GROUP BY term out-of-range error.
*/
static void resolveOutOfRangeError(
  Parse *pParse,         /* The error context into which to write the error */
  const char *zType,     /* "ORDER" or "GROUP" */
  int i,                 /* The index (1-based) of the term out of range */
  int mx                 /* Largest permissible value of i */
){
  sqlite3ErrorMsg(pParse, 
    "%r %s BY term out of range - should be "
    "between 1 and %d", i, zType, mx);
}

/*
** Analyze the ORDER BY clause in a compound SELECT statement.   Modify
** each term of the ORDER BY clause is a constant integer between 1
** and N where N is the number of columns in the compound SELECT.
**
** ORDER BY terms that are already an integer between 1 and N are
** unmodified.  ORDER BY terms that are integers outside the range of
** 1 through N generate an error.  ORDER BY terms that are expressions
** are matched against result set expressions of compound SELECT
** beginning with the left-most SELECT and working toward the right.
** At the first match, the ORDER BY expression is transformed into
** the integer column number.
**
** Return the number of errors seen.
*/
static int resolveCompoundOrderBy(
  Parse *pParse,        /* Parsing context.  Leave error messages here */
  Select *pSelect       /* The SELECT statement containing the ORDER BY */
){
  int i;
  ExprList *pOrderBy;
  ExprList *pEList;
  sqlite3 *db;
  int moreToDo = 1;

  pOrderBy = pSelect->pOrderBy;
  if( pOrderBy==0 ) return 0;
  db = pParse->db;
#if SQLITE_MAX_COLUMN
  if( pOrderBy->nExpr>db->aLimit[SQLITE_LIMIT_COLUMN] ){
    sqlite3ErrorMsg(pParse, "too many terms in ORDER BY clause");
    return 1;
  }
#endif
  for(i=0; i<pOrderBy->nExpr; i++){
    pOrderBy->a[i].done = 0;
  }
  pSelect->pNext = 0;
  while( pSelect->pPrior ){
    pSelect->pPrior->pNext = pSelect;
    pSelect = pSelect->pPrior;
  }
  while( pSelect && moreToDo ){
    struct ExprList_item *pItem;
    moreToDo = 0;
    pEList = pSelect->pEList;
    assert( pEList!=0 );
    for(i=0, pItem=pOrderBy->a; i<pOrderBy->nExpr; i++, pItem++){
      int iCol = -1;
      Expr *pE, *pDup;
      if( pItem->done ) continue;
      pE = pItem->pExpr;
      if( sqlite3ExprIsInteger(pE, &iCol) ){
        if( iCol<0 || iCol>pEList->nExpr ){
          resolveOutOfRangeError(pParse, "ORDER", i+1, pEList->nExpr);
          return 1;
        }
      }else{
        iCol = resolveAsName(pParse, pEList, pE);
        if( iCol==0 ){
          pDup = sqlite3ExprDup(db, pE);
          if( !db->mallocFailed ){
            assert(pDup);
            iCol = resolveOrderByTermToExprList(pParse, pSelect, pDup);
          }
          sqlite3ExprDelete(db, pDup);
        }
        if( iCol<0 ){
          return 1;
        }
      }
      if( iCol>0 ){
        CollSeq *pColl = pE->pColl;
        int flags = pE->flags & EP_ExpCollate;
        sqlite3ExprDelete(db, pE);
        pItem->pExpr = pE = sqlite3Expr(db, TK_INTEGER, 0, 0, 0);
        if( pE==0 ) return 1;
        pE->pColl = pColl;
        pE->flags |= EP_IntValue | flags;
        pE->iTable = iCol;
        pItem->iCol = (u16)iCol;
        pItem->done = 1;
      }else{
        moreToDo = 1;
      }
    }
    pSelect = pSelect->pNext;
  }
  for(i=0; i<pOrderBy->nExpr; i++){
    if( pOrderBy->a[i].done==0 ){
      sqlite3ErrorMsg(pParse, "%r ORDER BY term does not match any "
            "column in the result set", i+1);
      return 1;
    }
  }
  return 0;
}

/*
** Check every term in the ORDER BY or GROUP BY clause pOrderBy of
** the SELECT statement pSelect.  If any term is reference to a
** result set expression (as determined by the ExprList.a.iCol field)
** then convert that term into a copy of the corresponding result set
** column.
**
** If any errors are detected, add an error message to pParse and
** return non-zero.  Return zero if no errors are seen.
*/
int sqlite3ResolveOrderGroupBy(
  Parse *pParse,        /* Parsing context.  Leave error messages here */
  Select *pSelect,      /* The SELECT statement containing the clause */
  ExprList *pOrderBy,   /* The ORDER BY or GROUP BY clause to be processed */
  const char *zType     /* "ORDER" or "GROUP" */
){
  int i;
  sqlite3 *db = pParse->db;
  ExprList *pEList;
  struct ExprList_item *pItem;

  if( pOrderBy==0 || pParse->db->mallocFailed ) return 0;
#if SQLITE_MAX_COLUMN
  if( pOrderBy->nExpr>db->aLimit[SQLITE_LIMIT_COLUMN] ){
    sqlite3ErrorMsg(pParse, "too many terms in %s BY clause", zType);
    return 1;
  }
#endif
  pEList = pSelect->pEList;
  assert( pEList!=0 );  /* sqlite3SelectNew() guarantees this */
  for(i=0, pItem=pOrderBy->a; i<pOrderBy->nExpr; i++, pItem++){
    if( pItem->iCol ){
      if( pItem->iCol>pEList->nExpr ){
        resolveOutOfRangeError(pParse, zType, i+1, pEList->nExpr);
        return 1;
      }
      resolveAlias(pParse, pEList, pItem->iCol-1, pItem->pExpr, zType);
    }
  }
  return 0;
}

/*
** pOrderBy is an ORDER BY or GROUP BY clause in SELECT statement pSelect.
** The Name context of the SELECT statement is pNC.  zType is either
** "ORDER" or "GROUP" depending on which type of clause pOrderBy is.
**
** This routine resolves each term of the clause into an expression.
** If the order-by term is an integer I between 1 and N (where N is the
** number of columns in the result set of the SELECT) then the expression
** in the resolution is a copy of the I-th result-set expression.  If
** the order-by term is an identify that corresponds to the AS-name of
** a result-set expression, then the term resolves to a copy of the
** result-set expression.  Otherwise, the expression is resolved in
** the usual way - using sqlite3ResolveExprNames().
**
** This routine returns the number of errors.  If errors occur, then
** an appropriate error message might be left in pParse.  (OOM errors
** excepted.)
*/
static int resolveOrderGroupBy(
  NameContext *pNC,     /* The name context of the SELECT statement */
  Select *pSelect,      /* The SELECT statement holding pOrderBy */
  ExprList *pOrderBy,   /* An ORDER BY or GROUP BY clause to resolve */
  const char *zType     /* Either "ORDER" or "GROUP", as appropriate */
){
  int i;                         /* Loop counter */
  int iCol;                      /* Column number */
  struct ExprList_item *pItem;   /* A term of the ORDER BY clause */
  Parse *pParse;                 /* Parsing context */
  int nResult;                   /* Number of terms in the result set */

  if( pOrderBy==0 ) return 0;
  nResult = pSelect->pEList->nExpr;
  pParse = pNC->pParse;
  for(i=0, pItem=pOrderBy->a; i<pOrderBy->nExpr; i++, pItem++){
    Expr *pE = pItem->pExpr;
    iCol = resolveAsName(pParse, pSelect->pEList, pE);
    if( iCol<0 ){
      return 1;  /* OOM error */
    }
    if( iCol>0 ){
      /* If an AS-name match is found, mark this ORDER BY column as being
      ** a copy of the iCol-th result-set column.  The subsequent call to
      ** sqlite3ResolveOrderGroupBy() will convert the expression to a
      ** copy of the iCol-th result-set expression. */
      pItem->iCol = (u16)iCol;
      continue;
    }
    if( sqlite3ExprIsInteger(pE, &iCol) ){
      /* The ORDER BY term is an integer constant.  Again, set the column
      ** number so that sqlite3ResolveOrderGroupBy() will convert the
      ** order-by term to a copy of the result-set expression */
      if( iCol<1 ){
        resolveOutOfRangeError(pParse, zType, i+1, nResult);
        return 1;
      }
      pItem->iCol = (u16)iCol;
      continue;
    }

    /* Otherwise, treat the ORDER BY term as an ordinary expression */
    pItem->iCol = 0;
    if( sqlite3ResolveExprNames(pNC, pE) ){
      return 1;
    }
  }
  return sqlite3ResolveOrderGroupBy(pParse, pSelect, pOrderBy, zType);
}

/*
** Resolve names in the SELECT statement p and all of its descendents.
*/
static int resolveSelectStep(Walker *pWalker, Select *p){
  NameContext *pOuterNC;  /* Context that contains this SELECT */
  NameContext sNC;        /* Name context of this SELECT */
  int isCompound;         /* True if p is a compound select */
  int nCompound;          /* Number of compound terms processed so far */
  Parse *pParse;          /* Parsing context */
  ExprList *pEList;       /* Result set expression list */
  int i;                  /* Loop counter */
  ExprList *pGroupBy;     /* The GROUP BY clause */
  Select *pLeftmost;      /* Left-most of SELECT of a compound */
  sqlite3 *db;            /* Database connection */
  

  assert( p!=0 );
  if( p->selFlags & SF_Resolved ){
    return WRC_Prune;
  }
  pOuterNC = pWalker->u.pNC;
  pParse = pWalker->pParse;
  db = pParse->db;

  /* Normally sqlite3SelectExpand() will be called first and will have
  ** already expanded this SELECT.  However, if this is a subquery within
  ** an expression, sqlite3ResolveExprNames() will be called without a
  ** prior call to sqlite3SelectExpand().  When that happens, let
  ** sqlite3SelectPrep() do all of the processing for this SELECT.
  ** sqlite3SelectPrep() will invoke both sqlite3SelectExpand() and
  ** this routine in the correct order.
  */
  if( (p->selFlags & SF_Expanded)==0 ){
    sqlite3SelectPrep(pParse, p, pOuterNC);
    return (pParse->nErr || db->mallocFailed) ? WRC_Abort : WRC_Prune;
  }

  isCompound = p->pPrior!=0;
  nCompound = 0;
  pLeftmost = p;
  while( p ){
    assert( (p->selFlags & SF_Expanded)!=0 );
    assert( (p->selFlags & SF_Resolved)==0 );
    p->selFlags |= SF_Resolved;

    /* Resolve the expressions in the LIMIT and OFFSET clauses. These
    ** are not allowed to refer to any names, so pass an empty NameContext.
    */
    memset(&sNC, 0, sizeof(sNC));
    sNC.pParse = pParse;
    if( sqlite3ResolveExprNames(&sNC, p->pLimit) ||
        sqlite3ResolveExprNames(&sNC, p->pOffset) ){
      return WRC_Abort;
    }
  
    /* Set up the local name-context to pass to sqlite3ResolveExprNames() to
    ** resolve the result-set expression list.
    */
    sNC.allowAgg = 1;
    sNC.pSrcList = p->pSrc;
    sNC.pNext = pOuterNC;
  
    /* Resolve names in the result set. */
    pEList = p->pEList;
    assert( pEList!=0 );
    for(i=0; i<pEList->nExpr; i++){
      Expr *pX = pEList->a[i].pExpr;
      if( sqlite3ResolveExprNames(&sNC, pX) ){
        return WRC_Abort;
      }
    }
  
    /* Recursively resolve names in all subqueries
    */
    for(i=0; i<p->pSrc->nSrc; i++){
      struct SrcList_item *pItem = &p->pSrc->a[i];
      if( pItem->pSelect ){
        const char *zSavedContext = pParse->zAuthContext;
        if( pItem->zName ) pParse->zAuthContext = pItem->zName;
        sqlite3ResolveSelectNames(pParse, pItem->pSelect, pOuterNC);
        pParse->zAuthContext = zSavedContext;
        if( pParse->nErr || db->mallocFailed ) return WRC_Abort;
      }
    }
  
    /* If there are no aggregate functions in the result-set, and no GROUP BY 
    ** expression, do not allow aggregates in any of the other expressions.
    */
    assert( (p->selFlags & SF_Aggregate)==0 );
    pGroupBy = p->pGroupBy;
    if( pGroupBy || sNC.hasAgg ){
      p->selFlags |= SF_Aggregate;
    }else{
      sNC.allowAgg = 0;
    }
  
    /* If a HAVING clause is present, then there must be a GROUP BY clause.
    */
    if( p->pHaving && !pGroupBy ){
      sqlite3ErrorMsg(pParse, "a GROUP BY clause is required before HAVING");
      return WRC_Abort;
    }
  
    /* Add the expression list to the name-context before parsing the
    ** other expressions in the SELECT statement. This is so that
    ** expressions in the WHERE clause (etc.) can refer to expressions by
    ** aliases in the result set.
    **
    ** Minor point: If this is the case, then the expression will be
    ** re-evaluated for each reference to it.
    */
    sNC.pEList = p->pEList;
    if( sqlite3ResolveExprNames(&sNC, p->pWhere) ||
       sqlite3ResolveExprNames(&sNC, p->pHaving)
    ){
      return WRC_Abort;
    }

    /* The ORDER BY and GROUP BY clauses may not refer to terms in
    ** outer queries 
    */
    sNC.pNext = 0;
    sNC.allowAgg = 1;

    /* Process the ORDER BY clause for singleton SELECT statements.
    ** The ORDER BY clause for compounds SELECT statements is handled
    ** below, after all of the result-sets for all of the elements of
    ** the compound have been resolved.
    */
    if( !isCompound && resolveOrderGroupBy(&sNC, p, p->pOrderBy, "ORDER") ){
      return WRC_Abort;
    }
    if( db->mallocFailed ){
      return WRC_Abort;
    }
  
    /* Resolve the GROUP BY clause.  At the same time, make sure 
    ** the GROUP BY clause does not contain aggregate functions.
    */
    if( pGroupBy ){
      struct ExprList_item *pItem;
    
      if( resolveOrderGroupBy(&sNC, p, pGroupBy, "GROUP") || db->mallocFailed ){
        return WRC_Abort;
      }
      for(i=0, pItem=pGroupBy->a; i<pGroupBy->nExpr; i++, pItem++){
        if( ExprHasProperty(pItem->pExpr, EP_Agg) ){
          sqlite3ErrorMsg(pParse, "aggregate functions are not allowed in "
              "the GROUP BY clause");
          return WRC_Abort;
        }
      }
    }

    /* Advance to the next term of the compound
    */
    p = p->pPrior;
    nCompound++;
  }

  /* Resolve the ORDER BY on a compound SELECT after all terms of
  ** the compound have been resolved.
  */
  if( isCompound && resolveCompoundOrderBy(pParse, pLeftmost) ){
    return WRC_Abort;
  }

  return WRC_Prune;
}

/*
** This routine walks an expression tree and resolves references to
** table columns and result-set columns.  At the same time, do error
** checking on function usage and set a flag if any aggregate functions
** are seen.
**
** To resolve table columns references we look for nodes (or subtrees) of the 
** form X.Y.Z or Y.Z or just Z where
**
**      X:   The name of a database.  Ex:  "main" or "temp" or
**           the symbolic name assigned to an ATTACH-ed database.
**
**      Y:   The name of a table in a FROM clause.  Or in a trigger
**           one of the special names "old" or "new".
**
**      Z:   The name of a column in table Y.
**
** The node at the root of the subtree is modified as follows:
**
**    Expr.op        Changed to TK_COLUMN
**    Expr.pTab      Points to the Table object for X.Y
**    Expr.iColumn   The column index in X.Y.  -1 for the rowid.
**    Expr.iTable    The VDBE cursor number for X.Y
**
**
** To resolve result-set references, look for expression nodes of the
** form Z (with no X and Y prefix) where the Z matches the right-hand
** size of an AS clause in the result-set of a SELECT.  The Z expression
** is replaced by a copy of the left-hand side of the result-set expression.
** Table-name and function resolution occurs on the substituted expression
** tree.  For example, in:
**
**      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY x;
**
** The "x" term of the order by is replaced by "a+b" to render:
**
**      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY a+b;
**
** Function calls are checked to make sure that the function is 
** defined and that the correct number of arguments are specified.
** If the function is an aggregate function, then the pNC->hasAgg is
** set and the opcode is changed from TK_FUNCTION to TK_AGG_FUNCTION.
** If an expression contains aggregate functions then the EP_Agg
** property on the expression is set.
**
** An error message is left in pParse if anything is amiss.  The number
** if errors is returned.
*/
int sqlite3ResolveExprNames( 
  NameContext *pNC,       /* Namespace to resolve expressions in. */
  Expr *pExpr             /* The expression to be analyzed. */
){
  int savedHasAgg;
  Walker w;

  if( pExpr==0 ) return 0;
#if SQLITE_MAX_EXPR_DEPTH>0
  {
    Parse *pParse = pNC->pParse;
    if( sqlite3ExprCheckHeight(pParse, pExpr->nHeight+pNC->pParse->nHeight) ){
      return 1;
    }
    pParse->nHeight += pExpr->nHeight;
  }
#endif
  savedHasAgg = pNC->hasAgg;
  pNC->hasAgg = 0;
  w.xExprCallback = resolveExprStep;
  w.xSelectCallback = resolveSelectStep;
  w.pParse = pNC->pParse;
  w.u.pNC = pNC;
  sqlite3WalkExpr(&w, pExpr);
#if SQLITE_MAX_EXPR_DEPTH>0
  pNC->pParse->nHeight -= pExpr->nHeight;
#endif
  if( pNC->nErr>0 ){
    ExprSetProperty(pExpr, EP_Error);
  }
  if( pNC->hasAgg ){
    ExprSetProperty(pExpr, EP_Agg);
  }else if( savedHasAgg ){
    pNC->hasAgg = 1;
  }
  return ExprHasProperty(pExpr, EP_Error);
}


/*
** Resolve all names in all expressions of a SELECT and in all
** decendents of the SELECT, including compounds off of p->pPrior,
** subqueries in expressions, and subqueries used as FROM clause
** terms.
**
** See sqlite3ResolveExprNames() for a description of the kinds of
** transformations that occur.
**
** All SELECT statements should have been expanded using
** sqlite3SelectExpand() prior to invoking this routine.
*/
void sqlite3ResolveSelectNames(
  Parse *pParse,         /* The parser context */
  Select *p,             /* The SELECT statement being coded. */
  NameContext *pOuterNC  /* Name context for parent SELECT statement */
){
  Walker w;

  assert( p!=0 );
  w.xExprCallback = resolveExprStep;
  w.xSelectCallback = resolveSelectStep;
  w.pParse = pParse;
  w.u.pNC = pOuterNC;
  sqlite3WalkSelect(&w, p);
}
