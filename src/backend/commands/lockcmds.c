/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/lockcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "parser/parse_clause.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"
#include "utils/syscache.h"
#include "rewrite/rewriteHandler.h"
#include "nodes/nodeFuncs.h"

static void LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait, Oid userid);
static AclResult LockTableAclCheck(Oid relid, LOCKMODE lockmode, Oid userid);
static void RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid,
										 Oid oldrelid, void *arg);
static void LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
							List *ancestor_views);

/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*---------
	 * During recovery we only accept these variations:
	 * LOCK TABLE foo IN ACCESS SHARE MODE
	 * LOCK TABLE foo IN ROW SHARE MODE
	 * LOCK TABLE foo IN ROW EXCLUSIVE MODE
	 * This test must match the restrictions defined in LockAcquireExtended()
	 *---------
	 */
	if (lockstmt->mode > RowExclusiveLock)
		PreventCommandDuringRecovery("LOCK TABLE");

	/*
	 * Iterate over the list and process the named relations one at a time
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(p);
		bool		recurse = rv->inh;
		Oid			reloid;

		reloid = RangeVarGetRelidExtended(rv, lockstmt->mode,
										  lockstmt->nowait ? RVR_NOWAIT : 0,
										  RangeVarCallbackForLockTable,
										  (void *) &lockstmt->mode);

		/*
		 * CDB: LOCK TABLE will not release the lock until end of transaction,
		 * set the holdTillEndXact flag for GDD to know about this.
		 */
		if (lockstmt->mode != NoLock)
		{
			LOCKTAG		tag;

			SET_LOCKTAG_RELATION(tag, MyDatabaseId, reloid);
			LockSetHoldTillEndXact(&tag);
		}

		if (get_rel_relkind(reloid) == RELKIND_VIEW)
			LockViewRecurse(reloid, lockstmt->mode, lockstmt->nowait, NIL);
		else if (recurse)
			LockTableRecurse(reloid, lockstmt->mode, lockstmt->nowait, GetUserId());
	}

	if (Gp_role == GP_ROLE_DISPATCH && !lockstmt->coordinatoronly)
	{
		CdbDispatchUtilityStatement((Node *) lockstmt,
									DF_CANCEL_ON_ERROR|
									DF_NEED_TWO_PHASE,
									NIL,
									NULL);
	}
}

/*
 * Before acquiring a table lock on the named table, check whether we have
 * permission to do so.
 */
static void
RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid, Oid oldrelid,
							 void *arg)
{
	LOCKMODE	lockmode = *(LOCKMODE *) arg;
	char		relkind;
	AclResult	aclresult;

	if (!OidIsValid(relid))
		return;					/* doesn't exist, so no permissions check */
	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;					/* woops, concurrently dropped; no permissions
								 * check */

	/* Currently, we only allow plain tables or views to be locked */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or view",
						rv->relname)));

#if 0 /* Upstream code not applicable to GPDB */
	/*
	 * Make note if a temporary relation has been accessed in this
	 * transaction.
	 */
	relpersistence = get_rel_persistence(relid);
	if (relpersistence == RELPERSISTENCE_TEMP)
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;
#endif

	/* Check permissions. */
	aclresult = LockTableAclCheck(relid, lockmode, GetUserId());
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 *
 * We use find_inheritance_children not find_all_inheritors to avoid taking
 * locks far in advance of checking privileges.  This means we'll visit
 * multiply-inheriting children more than once, but that's no problem.
 */
static void
LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait, Oid userid)
{
	List	   *children;
	ListCell   *lc;

	children = find_inheritance_children(reloid, NoLock);

	foreach(lc, children)
	{
		Oid			childreloid = lfirst_oid(lc);
		AclResult	aclresult;

		/* Check permissions before acquiring the lock. */
		aclresult = LockTableAclCheck(childreloid, lockmode, userid);
		if (aclresult != ACLCHECK_OK)
		{
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* child concurrently dropped, just skip it */
			aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(childreloid)), relname);
		}

		/* We have enough rights to lock the relation; do so. */
		if (!nowait)
		{
			LockRelationOid(childreloid, lockmode);

			/*
			 * CDB: LOCK TABLE will not release the lock until end of
			 * transaction, set the holdTillEndXact flag for GDD to know about
			 * this.
			 */
			if (lockmode != NoLock)
			{
				LOCKTAG		tag;

				SET_LOCKTAG_RELATION(tag, MyDatabaseId, childreloid);
				LockSetHoldTillEndXact(&tag);
			}
		}
		else if (!ConditionalLockRelationOid(childreloid, lockmode))
		{
			/* try to throw error by name; relation could be deleted... */
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* child concurrently dropped, just skip it */
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on relation \"%s\"",
							relname)));
		}

		/*
		 * Even if we got the lock, child might have been concurrently
		 * dropped. If so, we can skip it.
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(childreloid)))
		{
			/* Release useless lock */
			UnlockRelationOid(childreloid, lockmode);
			continue;
		}

		LockTableRecurse(childreloid, lockmode, nowait, userid);
	}
}

/*
 * Apply LOCK TABLE recursively over a view
 *
 * All tables and views appearing in the view definition query are locked
 * recursively with the same lock mode.
 */

typedef struct
{
	LOCKMODE	lockmode;		/* lock mode to use */
	bool		nowait;			/* no wait mode */
	Oid			viewowner;		/* view owner for checking the privilege */
	Oid			viewoid;		/* OID of the view to be locked */
	List	   *ancestor_views; /* OIDs of ancestor views */
} LockViewRecurse_context;

static bool
LockViewRecurse_walker(Node *node, LockViewRecurse_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);
			AclResult	aclresult;

			Oid			relid = rte->relid;
			char		relkind = rte->relkind;
			char	   *relname = get_rel_name(relid);

			/*
			 * The OLD and NEW placeholder entries in the view's rtable are
			 * skipped.
			 */
			if (relid == context->viewoid &&
				(strcmp(rte->eref->aliasname, "old") == 0 ||
				 strcmp(rte->eref->aliasname, "new") == 0))
				continue;

			/* Currently, we only allow plain tables or views to be locked. */
			if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
				relkind != RELKIND_VIEW)
				continue;

			/*
			 * We might be dealing with a self-referential view.  If so, we
			 * can just stop recursing, since we already locked it.
			 */
			if (list_member_oid(context->ancestor_views, relid))
				continue;

			/* Check permissions with the view owner's privilege. */
			aclresult = LockTableAclCheck(relid, context->lockmode, context->viewowner);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(relkind), relname);

			/* We have enough rights to lock the relation; do so. */
			if (!context->nowait)
				LockRelationOid(relid, context->lockmode);
			else if (!ConditionalLockRelationOid(relid, context->lockmode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));

			if (relkind == RELKIND_VIEW)
				LockViewRecurse(relid, context->lockmode, context->nowait,
								context->ancestor_views);
			else if (rte->inh)
				LockTableRecurse(relid, context->lockmode, context->nowait, context->viewowner);
		}

		return query_tree_walker(query,
								 LockViewRecurse_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  LockViewRecurse_walker,
								  context);
}

static void
LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
				List *ancestor_views)
{
	LockViewRecurse_context context;
	Relation	view;
	Query	   *viewquery;

	/* caller has already locked the view */
	view = table_open(reloid, NoLock);
	viewquery = get_view_query(view);

	context.lockmode = lockmode;
	context.nowait = nowait;
	context.viewowner = view->rd_rel->relowner;
	context.viewoid = reloid;
	context.ancestor_views = lcons_oid(reloid, ancestor_views);

	LockViewRecurse_walker((Node *) viewquery, &context);

	ancestor_views = list_delete_oid(ancestor_views, reloid);

	table_close(view, NoLock);
}

/*
 * Check whether the current user is permitted to lock this relation.
 */
static AclResult
LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid)
{
	AclResult	aclresult;
	AclMode		aclmask;

	/* Verify adequate privilege */
	if (lockmode == AccessShareLock)
		aclmask = ACL_SELECT;
	else if (lockmode == RowExclusiveLock)
		aclmask = ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE;
	else
		aclmask = ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE;

	aclresult = pg_class_aclcheck(reloid, userid, aclmask);

	return aclresult;
}
