/* -------------------------------------------------------------------------
 *
 * spock_relcache.c
 *     Caching relation specific information
 *
 * Copyright (c) 2022-2023, pgEdge, Inc.
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"

#include "catalog/pg_trigger.h"

#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "spock_common.h"

#include "spock.h"
#include "spock_relcache.h"

#define SPOCKRELATIONHASH_INITIAL_SIZE 128
static HTAB *SpockRelationHash = NULL;


static void spock_relcache_init(void);
static int tupdesc_get_att_by_name(TupleDesc desc, const char *attname);

static void
relcache_free_entry(SpockRelation *entry)
{
	pfree(entry->nspname);
	pfree(entry->relname);

	if (entry->natts > 0)
	{
		int	i;

		for (i = 0; i < entry->natts; i++)
			pfree(entry->attnames[i]);

		pfree(entry->attnames);
	}

	if (entry->attmap)
		pfree(entry->attmap);

	entry->natts = 0;
	entry->reloid = InvalidOid;
	entry->rel = NULL;
}


SpockRelation *
spock_relation_open(uint32 remoteid, LOCKMODE lockmode)
{
	SpockRelation *entry;
	bool		found;

	if (SpockRelationHash == NULL)
		spock_relcache_init();

	/* Search for existing entry. */
	entry = hash_search(SpockRelationHash, (void *) &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "cache lookup failed for remote relation %u",
			 remoteid);

	/* Need to update the local cache? */
	if (!OidIsValid(entry->reloid))
	{
		RangeVar   *rv = makeNode(RangeVar);
		int			i;
		TupleDesc	desc;

		rv->schemaname = (char *) entry->nspname;
		rv->relname = (char *) entry->relname;
		entry->rel = table_openrv(rv, lockmode);

		desc = RelationGetDescr(entry->rel);
		for (i = 0; i < entry->natts; i++)
			entry->attmap[i] = tupdesc_get_att_by_name(desc, entry->attnames[i]);

		entry->reloid = RelationGetRelid(entry->rel);

		/* Cache trigger info. */
		entry->hasTriggers = false;
		if (entry->rel->trigdesc != NULL)
		{
			TriggerDesc	   *trigdesc = entry->rel->trigdesc;

			for (i = 0; i < trigdesc->numtriggers; i++)
			{
				Trigger *trigger = &trigdesc->triggers[i];

				/* We only fire replica triggers on rows */
				if (!(trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN ||
					  trigger->tgenabled == TRIGGER_DISABLED) &&
					TRIGGER_FOR_ROW(trigger->tgtype))
				{
					entry->hasTriggers = true;
					break;
				}
			}
		}
	}
	else if (!entry->rel)
		entry->rel = table_open(entry->reloid, lockmode);

	return entry;
}

void
spock_relation_cache_update(uint32 remoteid, char *schemaname,
								 char *relname, int natts, char **attnames,
								 Oid *attrtypes, Oid *attrtypmods)
{
	MemoryContext		oldcontext;
	SpockRelation  *entry;
	bool				found;
	int					i;

	if (SpockRelationHash == NULL)
		spock_relcache_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 */
	entry = hash_search(SpockRelationHash, (void *) &remoteid,
						HASH_ENTER, &found);

	if (found)
		relcache_free_entry(entry);

	/* Make cached copy of the data */
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	entry->nspname = pstrdup(schemaname);
	entry->relname = pstrdup(relname);
	entry->natts = natts;
	entry->attnames = palloc(natts * sizeof(char *));
	entry->attrtypes = (Oid *) palloc(natts * sizeof(Oid));
	entry->attrtypmods = (Oid *) palloc(natts * sizeof(Oid));
	for (i = 0; i < natts; i++)
	{
		entry->attnames[i] = pstrdup(attnames[i]);
		entry->attrtypes[i] = attrtypes[i];
		entry->attrtypmods[i] = attrtypmods[i];
	}
	entry->attmap = palloc(natts * sizeof(int));
	MemoryContextSwitchTo(oldcontext);

	/* XXX Should we validate the relation against local schema here? */

	entry->reloid = InvalidOid;
}

void
spock_relation_cache_updater(SpockRemoteRel *remoterel)
{
	MemoryContext		oldcontext;
	SpockRelation  *entry;
	bool				found;
	int					i;

	if (SpockRelationHash == NULL)
		spock_relcache_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 */
	entry = hash_search(SpockRelationHash, (void *) &remoterel->relid,
						HASH_ENTER, &found);

	if (found)
		relcache_free_entry(entry);

	/* Make cached copy of the data */
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	entry->nspname = pstrdup(remoterel->nspname);
	entry->relname = pstrdup(remoterel->relname);
	entry->natts = remoterel->natts;
	entry->attnames = palloc(remoterel->natts * sizeof(char *));
	for (i = 0; i < remoterel->natts; i++)
		entry->attnames[i] = pstrdup(remoterel->attnames[i]);
	entry->attmap = palloc(remoterel->natts * sizeof(int));
	MemoryContextSwitchTo(oldcontext);

	/* XXX Should we validate the relation against local schema here? */

	entry->reloid = InvalidOid;
}

void
spock_relation_close(SpockRelation * rel, LOCKMODE lockmode)
{
	table_close(rel->rel, lockmode);
	rel->rel = NULL;
}

static void
spock_relcache_invalidate_callback(Datum arg, Oid reloid)
{
	SpockRelation *entry;

	/* Just to be sure. */
	if (SpockRelationHash == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, SpockRelationHash);

		/* TODO, use inverse lookup hastable */
		while ((entry = (SpockRelation *) hash_seq_search(&status)) != NULL)
		{
			if (entry->reloid == reloid)
				entry->reloid = InvalidOid;
		}
	}
	else
	{
		/* invalidate all cache entries */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, SpockRelationHash);

		while ((entry = (SpockRelation *) hash_seq_search(&status)) != NULL)
			entry->reloid = InvalidOid;
	}
}

static void
spock_relcache_init(void)
{
	HASHCTL		ctl;
	int			hashflags;

	/* Make sure we've initialized CacheMemoryContext. */
	if (CacheMemoryContext == NULL)
		CreateCacheMemoryContext();

	/* Initialize the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(SpockRelation);
	ctl.hcxt = CacheMemoryContext;
	hashflags = HASH_ELEM | HASH_CONTEXT;
	hashflags |= HASH_BLOBS;

	SpockRelationHash = hash_create("spock relation cache",
                                            SPOCKRELATIONHASH_INITIAL_SIZE,
                                            &ctl, hashflags);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(spock_relcache_invalidate_callback,
								  (Datum) 0);
}


/*
 * Find attribute index in TupleDesc struct by attribute name.
 */
static int
tupdesc_get_att_by_name(TupleDesc desc, const char *attname)
{
	int		i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc,i);

		if (strcmp(NameStr(att->attname), attname) == 0)
			return i;
	}

	elog(ERROR, "unknown column name %s", attname);
}
