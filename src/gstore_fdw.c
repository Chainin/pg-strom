/*
 * gstore_fdw.c
 *
 * On GPU column based data store as FDW provider.
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"

/*
 * GpuStorePlanInfo
 */
typedef struct
{
	List	   *host_quals;
	List	   *dev_quals;
	size_t		raw_nrows;		/* # of rows kept in GpuStoreFdw */
	size_t		dma_nrows;		/* # of rows to be backed from the device */
	Bitmapset  *outer_refs;		/* attributes to be backed to host */
	List	   *sort_keys;		/* list of Vars */
	List	   *sort_order;		/* BTXXXXStrategyNumber */
	List	   *sort_null_first;/* null-first? */
	/* table options */
	int			pinning;		/* GPU device number */
	int			format;			/* GSTORE_FDW_FORMAT__*  */
} GpuStoreFdwInfo;

/*
 *  GpuStoreExecState - state object for scan/insert/update/delete
 */
typedef struct
{
	GpuStoreBuffer *gs_buffer;
	cl_ulong		gs_index;
	AttrNumber		ctid_anum;	/* only UPDATE or DELETE */
} GpuStoreExecState;

/* ---- static variables ---- */
static Oid		reggstore_type_oid = InvalidOid;

Datum pgstrom_gstore_fdw_validator(PG_FUNCTION_ARGS);
Datum pgstrom_gstore_fdw_handler(PG_FUNCTION_ARGS);
Datum pgstrom_reggstore_in(PG_FUNCTION_ARGS);
Datum pgstrom_reggstore_out(PG_FUNCTION_ARGS);
Datum pgstrom_reggstore_recv(PG_FUNCTION_ARGS);
Datum pgstrom_reggstore_send(PG_FUNCTION_ARGS);

static inline void
form_gpustore_fdw_info(GpuStoreFdwInfo *gsf_info,
					   List **p_fdw_exprs, List **p_fdw_privs)
{
	List	   *exprs = NIL;
	List	   *privs = NIL;
	List	   *outer_refs_list = NIL;
	int			j;

	exprs = lappend(exprs, gsf_info->host_quals);
	exprs = lappend(exprs, gsf_info->dev_quals);
	privs = lappend(privs, makeInteger(gsf_info->raw_nrows));
	privs = lappend(privs, makeInteger(gsf_info->dma_nrows));
	j = -1;
	while ((j = bms_next_member(gsf_info->outer_refs, j)) >= 0)
		outer_refs_list = lappend_int(outer_refs_list, j);
	privs = lappend(privs, outer_refs_list);
	exprs = lappend(exprs, gsf_info->sort_keys);
	privs = lappend(privs, gsf_info->sort_order);
	privs = lappend(privs, gsf_info->sort_null_first);
	privs = lappend(privs, makeInteger(gsf_info->pinning));
	privs = lappend(privs, makeInteger(gsf_info->format));

	*p_fdw_exprs = exprs;
	*p_fdw_privs = privs;
}

static inline GpuStoreFdwInfo *
deform_gpustore_fdw_info(ForeignScan *fscan)
{
	GpuStoreFdwInfo *gsf_info = palloc0(sizeof(GpuStoreFdwInfo));
	List	   *exprs = fscan->fdw_exprs;
	List	   *privs = fscan->fdw_private;
	int			pindex = 0;
	int			eindex = 0;
	List	   *temp;
	ListCell   *lc;
	Bitmapset  *outer_refs = NULL;

	gsf_info->host_quals = list_nth(exprs, eindex++);
	gsf_info->dev_quals  = list_nth(exprs, eindex++);
	gsf_info->raw_nrows  = intVal(list_nth(privs, pindex++));
	gsf_info->dma_nrows  = intVal(list_nth(privs, pindex++));
	temp = list_nth(privs, pindex++);
	foreach (lc, temp)
		outer_refs = bms_add_member(outer_refs, lfirst_int(lc));
	gsf_info->outer_refs = outer_refs;
	gsf_info->sort_keys  = list_nth(exprs, eindex++);
	gsf_info->sort_order = list_nth(privs, pindex++);
	gsf_info->sort_null_first = list_nth(privs, pindex++);
	gsf_info->pinning    = intVal(list_nth(privs, pindex++));
	gsf_info->format     = intVal(list_nth(privs, pindex++));

	return gsf_info;
}

/*
 * gstoreGetForeignRelSize
 */
static void
gstoreGetForeignRelSize(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid ftable_oid)
{
	GpuStoreFdwInfo *gsf_info;
	Snapshot	snapshot;
	Size		rawsize;
	Size		nitems;
	double		selectivity;
	List	   *tmp_quals;
	List	   *dev_quals = NIL;
	List	   *host_quals = NIL;
	Bitmapset  *compressed = NULL;
	ListCell   *lc;
	int			anum;

	/* setup GpuStoreFdwInfo */
	gsf_info = palloc0(sizeof(GpuStoreFdwInfo));
	gstore_fdw_table_options(ftable_oid,
							 &gsf_info->pinning,
							 &gsf_info->format);
	for (anum=1; anum <= baserel->max_attr; anum++)
	{
		int		comp;

		gstore_fdw_column_options(ftable_oid, anum, &comp);
		if (comp != GSTORE_COMPRESSION__NONE)
			compressed = bms_add_member(compressed, anum -
										FirstLowInvalidHeapAttributeNumber);
	}
	/* pickup host/device quals */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);
		Bitmapset	   *varattnos = NULL;

		if (pgstrom_device_expression(root, rinfo->clause))
		{
			/*
			 * MEMO: Right now, we don't allow to reference compressed
			 * varlena datum by device-side SQL code.
			 */
			pull_varattnos((Node *)rinfo->clause,
						   baserel->relid,
						   &varattnos);
			if (!bms_overlap(varattnos, compressed))
				dev_quals = lappend(dev_quals, rinfo);
			else
				host_quals = lappend(dev_quals, rinfo);
		}
		else
			host_quals = lappend(dev_quals, rinfo);
	}
	/* estimate number of result rows */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	GpuStoreBufferGetSize(ftable_oid, snapshot, &rawsize, &nitems);
	UnregisterSnapshot(snapshot);

	tmp_quals = extract_actual_clauses(baserel->baserestrictinfo, false);
	selectivity = clauselist_selectivity(root,
										 tmp_quals,
										 baserel->relid,
										 JOIN_INNER,
										 NULL);
	baserel->rows  = selectivity * (double)nitems;
	baserel->pages = (rawsize + BLCKSZ - 1) / BLCKSZ;

	if (host_quals == NIL)
		gsf_info->dma_nrows = baserel->rows;
	else if (dev_quals != NIL)
	{
		tmp_quals = extract_actual_clauses(dev_quals, false);
		selectivity = clauselist_selectivity(root,
											 tmp_quals,
											 baserel->relid,
											 JOIN_INNER,
											 NULL);
		gsf_info->dma_nrows = selectivity * (double)nitems;
	}
	else
		gsf_info->dma_nrows = (double) nitems;

	gsf_info->raw_nrows  = nitems;
	gsf_info->host_quals = extract_actual_clauses(host_quals, false);
	gsf_info->dev_quals  = extract_actual_clauses(dev_quals, false);

	/* attributes to be referenced in the host code */
	pull_varattnos((Node *)baserel->reltarget->exprs,
				   baserel->relid,
				   &gsf_info->outer_refs);
	pull_varattnos((Node *)gsf_info->host_quals,
				   baserel->relid,
				   &gsf_info->outer_refs);
	baserel->fdw_private = gsf_info;
}

/*
 * gstoreCreateForeignPath
 */
static void
gstoreCreateForeignPath(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid ftable_oid,
						Bitmapset *outer_refs,
						List *host_quals,
						List *dev_quals,
						double raw_nrows,
						double dma_nrows,
						List *query_pathkeys)
{
	ForeignPath *fpath;
	ParamPathInfo *param_info;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	size_t		dma_size;
	size_t		tup_size;
	int			j, anum;
	QualCost	qcost;
	double		path_rows;
	List	   *useful_pathkeys = NIL;
	List	   *sort_keys = NIL;
	List	   *sort_order = NIL;
	List	   *sort_null_first = NIL;
	GpuStoreFdwInfo *gsf_info;

	/* Cost for GPU setup, if any */
	if (dev_quals != NIL || query_pathkeys != NIL)
		startup_cost += pgstrom_gpu_setup_cost;
	/* Cost for GPU qualifiers, if any */
	if (dev_quals)
	{
		cost_qual_eval_node(&qcost, (Node *)dev_quals, root);
		startup_cost += qcost.startup;
		run_cost += qcost.per_tuple * gpu_ratio * raw_nrows;
	}
	/* Cost for DMA (device-->host) */
	tup_size = MAXALIGN(offsetof(kern_tupitem, htup) +
						offsetof(HeapTupleHeaderData, t_bits) +
						BITMAPLEN(baserel->max_attr));
	j = -1;
	while ((j = bms_next_member(outer_refs, j)) >= 0)
	{
		anum = j + FirstLowInvalidHeapAttributeNumber;

		if (anum < InvalidAttrNumber)
			continue;
		if (anum == InvalidAttrNumber)
		{
			dma_size = baserel->pages * BLCKSZ;
			break;
		}
		if (anum < baserel->min_attr || anum > baserel->max_attr)
			elog(ERROR, "Bug? attribute number %d is out of range", anum);
		tup_size += baserel->attr_widths[anum - baserel->min_attr];
	}
	dma_size = (KDS_CALCULATE_HEAD_LENGTH(baserel->max_attr, true) +
				MAXALIGN(tup_size) * (size_t)dma_nrows);
	run_cost += pgstrom_gpu_dma_cost *
		((double)dma_size / (double)pgstrom_chunk_size());
	/* Cost for CPU qualifiers, if any */
	if (host_quals)
	{
		cost_qual_eval_node(&qcost, (Node *)host_quals, root);
		startup_cost += qcost.startup;
		run_cost += qcost.per_tuple * dma_nrows;
	}
	/* Cost for baserel parameters */
	param_info = get_baserel_parampathinfo(root, baserel, NULL);
	if (param_info)
	{
		cost_qual_eval(&qcost, param_info->ppi_clauses, root);
		startup_cost += qcost.startup;
		run_cost += qcost.per_tuple * dma_nrows;

		path_rows = param_info->ppi_rows;
	}
	else
		path_rows = baserel->rows;

	/* Cost for GpuSort */
	if (query_pathkeys != NIL)
	{
		ListCell   *lc1, *lc2;
		Cost		comparison_cost = 2.0 * pgstrom_gpu_operator_cost;

		foreach (lc1, query_pathkeys)
		{
			PathKey	   *pathkey = lfirst(lc1);
			EquivalenceClass *pathkey_ec = pathkey->pk_eclass;

			foreach (lc2, pathkey_ec->ec_members)
			{
				EquivalenceMember *em = lfirst(lc2);
				Var	   *var;

				/* reference to other table? */
				if (!bms_is_subset(em->em_relids, baserel->relids))
					continue;
				/* sort by constant? it makes no sense for GpuSort */
				if (bms_is_empty(em->em_relids))
					continue;
				/*
				 * GpuSort can support only simple variable reference,
				 * because sorting is earlier than projection.
				 */
				if (!IsA(em->em_expr, Var))
					continue;
				/* sanity checks */
				var = (Var *)em->em_expr;
				if (var->varno != baserel->relid ||
					var->varattno <= 0 ||
					var->varattno >  baserel->max_attr)
					continue;

				/*
				 * Varlena data types have special optimization - offset of
				 * values to extra buffer on KDS are preliminary sorted on
				 * GPU-size when GpuStore is constructed.
				 */
				if (get_typlen(var->vartype) == -1)
				{
					TypeCacheEntry *tcache
						= lookup_type_cache(var->vartype,
											TYPECACHE_CMP_PROC);
					if (!OidIsValid(tcache->cmp_proc))
						continue;
				}
				else
				{
					devtype_info *dtype = pgstrom_devtype_lookup(var->vartype);

					if (!dtype ||
						!pgstrom_devfunc_lookup_type_compare(dtype,
															 var->varcollid))
						continue;
				}
				/* OK, this is suitable key for GpuSort */
				sort_keys = lappend(sort_keys, copyObject(var));
				sort_order = lappend_int(sort_order, pathkey->pk_strategy);
				sort_null_first = lappend_int(sort_null_first,
											  (int)pathkey->pk_nulls_first);
				useful_pathkeys = lappend(useful_pathkeys, pathkey);
				break;
			}
		}
		if (useful_pathkeys == NIL)
			return;
		if (dma_nrows > 1.0)
		{
			double	log2 = log(dma_nrows) / 0.693147180559945;
			startup_cost += comparison_cost * dma_nrows * log2;
		}
	}

	/* setup GpuStoreFdwInfo with modification */
	gsf_info = palloc0(sizeof(GpuStoreFdwInfo));
	memcpy(gsf_info, baserel->fdw_private, sizeof(GpuStoreFdwInfo));
	gsf_info->host_quals = host_quals;
	gsf_info->dev_quals  = dev_quals;
	gsf_info->raw_nrows  = raw_nrows;
	gsf_info->dma_nrows  = dma_nrows;
	gsf_info->outer_refs = outer_refs;
	gsf_info->sort_keys  = sort_keys;
	gsf_info->sort_order = sort_order;
	gsf_info->sort_null_first = sort_null_first;

	fpath = create_foreignscan_path(root,
									baserel,
									NULL,	/* default pathtarget */
									path_rows,
									startup_cost,
									startup_cost + run_cost,
									useful_pathkeys,
									NULL,	/* no outer rel */
									NULL,	/* no extra plan */
									list_make1(gsf_info));
	add_path(baserel, (Path *)fpath);
}

/*
 * gstoreGetForeignPaths
 */
static void
gstoreGetForeignPaths(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	GpuStoreFdwInfo *gsf_info = (GpuStoreFdwInfo *)baserel->fdw_private;
	List		   *any_quals;
	Bitmapset	   *outer_refs_nodev;

	/* outer_refs when dev_quals are skipped */
	if (!gsf_info->dev_quals)
		outer_refs_nodev = gsf_info->outer_refs;
	else
	{
		outer_refs_nodev = bms_copy(gsf_info->outer_refs);
		pull_varattnos((Node *)gsf_info->dev_quals,
					   baserel->relid,
					   &outer_refs_nodev);
	}

	/* no device qual execution, no device side sorting */
	any_quals = extract_actual_clauses(baserel->baserestrictinfo, false);
	gstoreCreateForeignPath(root, baserel, foreigntableid,
							outer_refs_nodev,
							any_quals, NIL,
							gsf_info->raw_nrows,
							gsf_info->raw_nrows,
							NIL);

	/* device qual execution, but no device side sorting */
	if (gsf_info->dev_quals)
	{
		gstoreCreateForeignPath(root, baserel, foreigntableid,
								gsf_info->outer_refs,
								gsf_info->host_quals,
								gsf_info->dev_quals,
								gsf_info->raw_nrows,
								gsf_info->dma_nrows,
								NIL);
	}

	/* device side sorting */
	if (root->query_pathkeys)
	{
		/* without device qual execution */
		gstoreCreateForeignPath(root, baserel, foreigntableid,
								outer_refs_nodev,
								any_quals, NIL,
								gsf_info->raw_nrows,
								gsf_info->raw_nrows,
								root->query_pathkeys);
		/* with device qual execution */
		if (gsf_info->dev_quals)
		{
			gstoreCreateForeignPath(root, baserel, foreigntableid,
									gsf_info->outer_refs,
									gsf_info->host_quals,
									gsf_info->dev_quals,
									gsf_info->raw_nrows,
									gsf_info->dma_nrows,
									root->query_pathkeys);
		}
	}
}

/*
 * gstoreGetForeignPlan
 */
static ForeignScan *
gstoreGetForeignPlan(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid,
					 ForeignPath *best_path,
					 List *tlist,
					 List *scan_clauses,
					 Plan *outer_plan)
{
	GpuStoreFdwInfo *gsf_info = linitial(best_path->fdw_private);
	List	   *fdw_exprs;
	List	   *fdw_privs;

	form_gpustore_fdw_info(gsf_info, &fdw_exprs, &fdw_privs);
	return make_foreignscan(tlist,					/* plan.targetlist */
							gsf_info->host_quals,	/* plan.qual */
							baserel->relid,			/* scanrelid */
							fdw_exprs,				/* fdw_exprs */
							fdw_privs,				/* fdw_private */
							NIL,					/* fdw_scan_tlist */
							gsf_info->dev_quals,	/* fdw_recheck_quals */
							NULL);					/* outer_plan */
}

/*
 * gstoreAddForeignUpdateTargets
 */
static void
gstoreAddForeignUpdateTargets(Query *parsetree,
							  RangeTblEntry *target_rte,
							  Relation target_relation)
{
	Var			*var;
	TargetEntry *tle;

	/*
	 * We carry row_index as ctid system column
	 */

	/* Make a Var representing the desired value */
	var = makeVar(parsetree->resultRelation,
				  SelfItemPointerAttributeNumber,
				  TIDOID,
				  -1,
				  InvalidOid,
				  0);

	/* Wrap it in a resjunk TLE with the right name ... */
	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  "ctid",
						  true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * gstoreBeginForeignScan
 */
static void
gstoreBeginForeignScan(ForeignScanState *node, int eflags)
{
	EState	   *estate = node->ss.ps.state;
	GpuStoreExecState *gstate;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "cannot scan gstore_fdw table without MVCC snapshot");

	gstate = palloc0(sizeof(GpuStoreExecState));
	node->fdw_state = (void *) gstate;
}

/*
 * gstoreIterateForeignScan
 */
static TupleTableSlot *
gstoreIterateForeignScan(ForeignScanState *node)
{
	GpuStoreExecState *gstate = (GpuStoreExecState *) node->fdw_state;
	Relation		frel = node->ss.ss_currentRelation;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	EState		   *estate = node->ss.ps.state;
	ForeignScan	   *fscan = (ForeignScan *)node->ss.ps.plan;

	if (!gstate->gs_buffer)
		gstate->gs_buffer = GpuStoreBufferCreate(frel, estate->es_snapshot);
	if (GpuStoreBufferGetNext(frel,
							  estate->es_snapshot,
							  slot,
							  gstate->gs_buffer,
							  &gstate->gs_index,
							  fscan->fsSystemCol))
		return slot;

	return NULL;
}

/*
 * gstoreReScanForeignScan
 */
static void
gstoreReScanForeignScan(ForeignScanState *node)
{
	GpuStoreExecState *gstate = (GpuStoreExecState *) node->fdw_state;

	gstate->gs_index = 0;
}

/*
 * gstoreEndForeignScan
 */
static void
gstoreEndForeignScan(ForeignScanState *node)
{
	//GpuStoreExecState  *gstate = (GpuStoreExecState *) node->fdw_state;
}

/*
 * gstoreExplainForeignScan
 */
static void
gstoreExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	GpuStoreExecState  *gstate = (GpuStoreExecState *) node->fdw_state;
	GpuStoreFdwInfo	   *gsf_info;
	List			   *dcontext;
	char			   *temp;

	gsf_info = deform_gpustore_fdw_info((ForeignScan *)node->ss.ps.plan);

	/* setup deparsing context */
	dcontext = set_deparse_context_planstate(es->deparse_cxt,
											 (Node *)&node->ss.ps,
											 NIL); //XXX spec bug?
	/* device quelifiers, if any */
	if (gsf_info->dev_quals != NIL)
	{
		temp = deparse_expression((Node *)gsf_info->dev_quals,
								  dcontext, es->verbose, false);
		ExplainPropertyText("GPU Filter", temp, es);

		//Rows Removed by GPU Filter
	}

	/* sorting keys, if any */
	if (gsf_info->sort_keys != NIL)
	{
		StringInfoData buf;
		ListCell   *lc1, *lc2, *lc3;

		initStringInfo(&buf);
		forthree (lc1, gsf_info->sort_keys,
				  lc2, gsf_info->sort_order,
				  lc3, gsf_info->sort_null_first)
		{
			Node   *expr = lfirst(lc1);
			int		__order = lfirst_int(lc2);
			int		null_first = lfirst_int(lc3);
			const char *order;

			switch (__order)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					order = "asc";
					break;
				case BTGreaterStrategyNumber:
				case BTGreaterEqualStrategyNumber:
					order = "desc";
					break;
				default:
					order = "???";
					break;
			}
			temp = deparse_expression(expr, dcontext, es->verbose, false);
			if (es->verbose)
				appendStringInfo(&buf, "%s%s %s nulls %s",
								 buf.len > 0 ? ", " : "",
								 temp, order,
								 null_first ? "first" : "last");
			else
				appendStringInfo(&buf, "%s%s",
								 buf.len > 0 ? ", " : "",
								 temp);
		}
		ExplainPropertyText("Sort keys", buf.data, es);

		pfree(buf.data);
	}
}

/*
 * gstorePlanForeignModify
 */
static List *
gstorePlanForeignModify(PlannerInfo *root,
						ModifyTable *plan,
						Index resultRelation,
						int subplan_index)
{
	CmdType		operation = plan->operation;

	if (operation != CMD_INSERT &&
		operation != CMD_UPDATE &&
		operation != CMD_DELETE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gstore_fdw: not a supported operation")));
	return NIL;
}

/*
 * gstoreBeginForeignModify
 */
static void
gstoreBeginForeignModify(ModifyTableState *mtstate,
						 ResultRelInfo *rrinfo,
						 List *fdw_private,
						 int subplan_index,
						 int eflags)
{
	GpuStoreExecState *gstate = palloc0(sizeof(GpuStoreExecState));
	Relation	frel = rrinfo->ri_RelationDesc;
	CmdType		operation = mtstate->operation;

	/*
	 * NOTE: gstore_fdw does not support update operations by multiple
	 * concurrent transactions. So, we require stronger lock than usual
	 * INSERT/UPDATE/DELETE operations. It may lead unexpected deadlock,
	 * in spite of the per-tuple update capability.
	 */
	LockRelationOid(RelationGetRelid(frel), ShareUpdateExclusiveLock);

	/* Find the ctid resjunk column in the subplan's result */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		Plan	   *subplan = mtstate->mt_plans[subplan_index]->plan;
		AttrNumber	ctid_anum;

		ctid_anum = ExecFindJunkAttributeInTlist(subplan->targetlist, "ctid");
		if (!AttributeNumberIsValid(ctid_anum))
			elog(ERROR, "could not find junk ctid column");
		gstate->ctid_anum = ctid_anum;
	}
	rrinfo->ri_FdwState = gstate;
}

/*
 * gstoreExecForeignInsert
 */
static TupleTableSlot *
gstoreExecForeignInsert(EState *estate,
						ResultRelInfo *rrinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	GpuStoreExecState *gstate = (GpuStoreExecState *) rrinfo->ri_FdwState;
	Snapshot		snapshot = estate->es_snapshot;
	Relation		frel = rrinfo->ri_RelationDesc;

	if (snapshot->curcid > INT_MAX)
		elog(ERROR, "gstore_fdw: too much sub-transactions");

	if (!gstate->gs_buffer)
		gstate->gs_buffer = GpuStoreBufferCreate(frel, snapshot);

	GpuStoreBufferAppendRow(gstate->gs_buffer,
							RelationGetDescr(frel),
							snapshot,
							slot);
	return slot;
}

/*
 * gstoreExecForeignUpdate
 */
static TupleTableSlot *
gstoreExecForeignUpdate(EState *estate,
						ResultRelInfo *rrinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	GpuStoreExecState *gstate = (GpuStoreExecState *) rrinfo->ri_FdwState;
	Relation		frel = rrinfo->ri_RelationDesc;
	Snapshot		snapshot = estate->es_snapshot;
	Datum			datum;
	bool			isnull;
	ItemPointer		t_self;
	size_t			old_index;

	if (snapshot->curcid > INT_MAX)
		elog(ERROR, "gstore_fdw: too much sub-transactions");

	if (!gstate->gs_buffer)
		gstate->gs_buffer = GpuStoreBufferCreate(frel, snapshot);

	/* remove old version of the row */
	datum = ExecGetJunkAttribute(planSlot,
								 gstate->ctid_anum,
								 &isnull);
	if (isnull)
		elog(ERROR, "gstore_fdw: ctid is null");
	t_self = (ItemPointer)DatumGetPointer(datum);
	old_index = ((cl_ulong)t_self->ip_blkid.bi_hi << 32 |
				 (cl_ulong)t_self->ip_blkid.bi_lo << 16 |
				 (cl_ulong)t_self->ip_posid);
	GpuStoreBufferRemoveRow(gstate->gs_buffer,
							RelationGetDescr(frel),
							snapshot,
							old_index);

	/* insert new version of the row */
	GpuStoreBufferAppendRow(gstate->gs_buffer,
                            RelationGetDescr(frel),
							snapshot,
                            slot);
	return slot;
}

/*
 * gstoreExecForeignDelete
 */
static TupleTableSlot *
gstoreExecForeignDelete(EState *estate,
						ResultRelInfo *rrinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	GpuStoreExecState *gstate = (GpuStoreExecState *) rrinfo->ri_FdwState;
	Relation		frel = rrinfo->ri_RelationDesc;
	Snapshot		snapshot = estate->es_snapshot;
	Datum			datum;
	bool			isnull;
	ItemPointer		t_self;
	size_t			old_index;

	if (snapshot->curcid > INT_MAX)
		elog(ERROR, "gstore_fdw: too much sub-transactions");

	if (!gstate->gs_buffer)
		gstate->gs_buffer = GpuStoreBufferCreate(frel, snapshot);

	/* remove old version of the row */
	datum = ExecGetJunkAttribute(planSlot,
								 gstate->ctid_anum,
								 &isnull);
	if (isnull)
		elog(ERROR, "gstore_fdw: ctid is null");
	t_self = (ItemPointer)DatumGetPointer(datum);
	old_index = ((cl_ulong)t_self->ip_blkid.bi_hi << 32 |
				 (cl_ulong)t_self->ip_blkid.bi_lo << 16 |
				 (cl_ulong)t_self->ip_posid);
	GpuStoreBufferRemoveRow(gstate->gs_buffer,
							RelationGetDescr(frel),
							snapshot,
							old_index);
	return slot;
}

/*
 * gstoreEndForeignModify
 */
static void
gstoreEndForeignModify(EState *estate,
					   ResultRelInfo *rrinfo)
{
	//GpuStoreExecState *gstate = (GpuStoreExecState *) rrinfo->ri_FdwState;
}

/*
 * relation_is_gstore_fdw
 */
bool
relation_is_gstore_fdw(Oid table_oid)
{
	HeapTuple	tup;
	Oid			fserv_oid;
	Oid			fdw_oid;
	Oid			handler_oid;
	PGFunction	handler_fn;
	Datum		datum;
	char	   *prosrc;
	char	   *probin;
	bool		isnull;
	/* it should be foreign table, of course */
	if (get_rel_relkind(table_oid) != RELKIND_FOREIGN_TABLE)
		return false;
	/* pull OID of foreign-server */
	tup = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign table %u", table_oid);
	fserv_oid = ((Form_pg_foreign_table) GETSTRUCT(tup))->ftserver;
	ReleaseSysCache(tup);

	/* pull OID of foreign-data-wrapper */
	tup = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(fserv_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "foreign server with OID %u does not exist", fserv_oid);
	fdw_oid = ((Form_pg_foreign_server) GETSTRUCT(tup))->srvfdw;
	ReleaseSysCache(tup);

	/* pull OID of FDW handler function */
	tup = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdw_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u",fdw_oid);
	handler_oid = ((Form_pg_foreign_data_wrapper) GETSTRUCT(tup))->fdwhandler;
	ReleaseSysCache(tup);
	/* pull library path & function name */
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", handler_oid);
	if (((Form_pg_proc) GETSTRUCT(tup))->prolang != ClanguageId)
		elog(ERROR, "FDW handler function is not written with C-language");

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", handler_oid);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", handler_oid);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);
	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_gstore_fdw_handler)
		return false;
	/* OK, it is GpuStore foreign table */
	return true;
}

/*
 * gstore_fdw_table_options
 */
static void
__gstore_fdw_table_options(List *options,
						   int *p_pinning,
						   int *p_format)
{
	ListCell   *lc;
	int			pinning = -1;
	int			format = -1;

	foreach (lc, options)
	{
		DefElem	   *defel = lfirst(lc);

		if (strcmp(defel->defname, "pinning") == 0)
		{
			if (pinning >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"pinning\" option appears twice")));
			pinning = atoi(defGetString(defel));
			if (pinning < 0 || pinning >= numDevAttrs)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"pinning\" on unavailable GPU device")));
		}
		else if (strcmp(defel->defname, "format") == 0)
		{
			char   *format_name;

			if (format >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"format\" option appears twice")));
			format_name = defGetString(defel);
			if (strcmp(format_name, "pgstrom") == 0 ||
				strcmp(format_name, "default") == 0)
				format = GSTORE_FDW_FORMAT__PGSTROM;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("gstore_fdw: format \"%s\" is unknown",
								format_name)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("gstore_fdw: unknown option \"%s\"",
							defel->defname)));
		}
	}
	if (pinning < 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("gstore_fdw: No pinning GPU device"),
				 errhint("use 'pinning' option to specify GPU device")));

	/* put default if not specified */
	if (format < 0)
		format = GSTORE_FDW_FORMAT__PGSTROM;
	/* set results */
	if (p_pinning)
		*p_pinning = pinning;
	if (p_format)
		*p_format = format;
}

void
gstore_fdw_table_options(Oid gstore_oid, int *p_pinning, int *p_format)
{
	HeapTuple	tup;
	Datum		datum;
	bool		isnull;
	List	   *options = NIL;

	tup = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(gstore_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign table %u", gstore_oid);
	datum = SysCacheGetAttr(FOREIGNTABLEREL, tup,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (!isnull)
		options = untransformRelOptions(datum);
	__gstore_fdw_table_options(options, p_pinning, p_format);
	ReleaseSysCache(tup);
}

/*
 * gstore_fdw_column_options
 */
static void
__gstore_fdw_column_options(List *options, int *p_compression)
{
	ListCell   *lc;
	char	   *temp;
	int			compression = -1;

	foreach (lc, options)
	{
		DefElem	   *defel = lfirst(lc);

		if (strcmp(defel->defname, "compression") == 0)
		{
			if (compression >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"compression\" option appears twice")));
			temp = defGetString(defel);
			if (pg_strcasecmp(temp, "none") == 0)
				compression = GSTORE_COMPRESSION__NONE;
			else if (pg_strcasecmp(temp, "pglz") == 0)
				compression = GSTORE_COMPRESSION__PGLZ;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unknown compression logic: %s", temp)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("gstore_fdw: unknown option \"%s\"",
							defel->defname)));
		}
	}
	/* set default, if no valid options were supplied */
	if (compression < 0)
		compression = GSTORE_COMPRESSION__NONE;
	/* set results */
	if (p_compression)
		*p_compression = compression;
}

void
gstore_fdw_column_options(Oid gstore_oid, AttrNumber attnum,
						  int *p_compression)
{
	List	   *options = GetForeignColumnOptions(gstore_oid, attnum);

	__gstore_fdw_column_options(options, p_compression);
}

/*
 * pgstrom_gstore_fdw_validator
 */
Datum
pgstrom_gstore_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	switch (catalog)
	{
		case ForeignTableRelationId:
			__gstore_fdw_table_options(options, NULL, NULL);
			break;

		case AttributeRelationId:
			__gstore_fdw_column_options(options, NULL);
			break;

		case ForeignServerRelationId:
			if (options)
				elog(ERROR, "gstore_fdw: no options are supported on SERVER");
			break;

		case ForeignDataWrapperRelationId:
			if (options)
				elog(ERROR, "gstore_fdw: no options are supported on FOREIGN DATA WRAPPER");
			break;

		default:
			elog(ERROR, "gstore_fdw: no options are supported on catalog %s",
				 get_rel_name(catalog));
			break;
	}
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_validator);

/*
 * pgstrom_gstore_fdw_handler
 */
Datum
pgstrom_gstore_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* functions for scanning foreign tables */
	routine->GetForeignRelSize	= gstoreGetForeignRelSize;
	routine->GetForeignPaths	= gstoreGetForeignPaths;
	routine->GetForeignPlan		= gstoreGetForeignPlan;
	routine->AddForeignUpdateTargets = gstoreAddForeignUpdateTargets;
	routine->BeginForeignScan	= gstoreBeginForeignScan;
	routine->IterateForeignScan	= gstoreIterateForeignScan;
	routine->ReScanForeignScan	= gstoreReScanForeignScan;
	routine->EndForeignScan		= gstoreEndForeignScan;
	routine->ExplainForeignScan = gstoreExplainForeignScan;

	/* functions for INSERT/UPDATE/DELETE foreign tables */

	routine->PlanForeignModify	= gstorePlanForeignModify;
	routine->BeginForeignModify	= gstoreBeginForeignModify;
	routine->ExecForeignInsert	= gstoreExecForeignInsert;
	routine->ExecForeignUpdate  = gstoreExecForeignUpdate;
	routine->ExecForeignDelete	= gstoreExecForeignDelete;
	routine->EndForeignModify	= gstoreEndForeignModify;

	PG_RETURN_POINTER(routine);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_handler);

/*
 * pgstrom_reggstore_in
 */
Datum
pgstrom_reggstore_in(PG_FUNCTION_ARGS)
{
	Datum	datum = regclassin(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_in);

/*
 * pgstrom_reggstore_out
 */
Datum
pgstrom_reggstore_out(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	return regclassout(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_out);

/*
 * pgstrom_reggstore_recv
 */
Datum
pgstrom_reggstore_recv(PG_FUNCTION_ARGS)
{
	/* exactly the same as oidrecv, so share code */
	Datum	datum = oidrecv(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_recv);

/*
 * pgstrom_reggstore_send
 */
Datum
pgstrom_reggstore_send(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_send);

/*
 * get_reggstore_type_oid
 */
Oid
get_reggstore_type_oid(void)
{
	if (!OidIsValid(reggstore_type_oid))
	{
		Oid		temp_oid;

		temp_oid = GetSysCacheOid2(TYPENAMENSP,
								   CStringGetDatum("reggstore"),
								   ObjectIdGetDatum(PG_PUBLIC_NAMESPACE));
		if (!OidIsValid(temp_oid) ||
			!type_is_reggstore(temp_oid))
			elog(ERROR, "type \"reggstore\" is not defined");
		reggstore_type_oid = temp_oid;
	}
	return reggstore_type_oid;
}

/*
 * reset_reggstore_type_oid
 */
static void
reset_reggstore_type_oid(Datum arg, int cacheid, uint32 hashvalue)
{
	reggstore_type_oid = InvalidOid;
}

/*
 * type_is_reggstore
 */
bool
type_is_reggstore(Oid type_oid)
{
	Oid			typinput;
	HeapTuple	tup;
	char	   *prosrc;
	char	   *probin;
	Datum		datum;
	bool		isnull;
	PGFunction	handler_fn;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type_oid);
	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	ReleaseSysCache(tup);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(typinput));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", typinput);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", typinput);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", typinput);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);

	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_reggstore_in)
		return false;
	/* ok, it is reggstore type */
	return true;
}

/*
 * pgstrom_init_gstore_fdw
 */
void
pgstrom_init_gstore_fdw(void)
{
	/* invalidation of reggstore_oid variable */
	CacheRegisterSyscacheCallback(TYPEOID, reset_reggstore_type_oid, 0);
}
