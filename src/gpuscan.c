/*
 * gpuscan.c
 *
 * Sequential scan accelerated by GPU processors
 * ----
 * Copyright 2011-2015 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2015 (C) The PG-Strom Development Team
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
#include "postgres.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/spccache.h"
#include "pg_strom.h"
#include "cuda_numeric.h"
#include "cuda_gpuscan.h"

static set_rel_pathlist_hook_type	set_rel_pathlist_next;
static CustomPathMethods		gpuscan_path_methods;
static CustomScanMethods		gpuscan_plan_methods;
static PGStromExecMethods		gpuscan_exec_methods;
static CustomScanMethods		bulkscan_plan_methods;
static PGStromExecMethods		bulkscan_exec_methods;
static bool						enable_gpuscan;

/*
 * Path information of GpuScan
 */
typedef struct {
	CustomPath	cpath;
	List	   *host_quals;		/* RestrictInfo run on host */
	List	   *dev_quals;		/* RestrictInfo run on device */
} GpuScanPath;

/*
 * form/deform interface of private field of CustomScan(GpuScan)
 */
typedef struct {
	char	   *kern_source;	/* source of opencl kernel */
	int32		extra_flags;	/* extra libraries to be included */
	List	   *func_defs;		/* list of declared functions */
	List	   *used_params;	/* list of Const/Param in use */
	List	   *used_vars;		/* list of Var in use */
	List	   *dev_quals;		/* qualifiers to be run on device */
	cl_int      base_fixed_width; /* width of fixed fields on base rel */
    cl_int      proj_fixed_width; /* width of fixed fields on projection */
    cl_int      proj_extra_width; /* width of extra buffer on projection */
} GpuScanInfo;

static inline void
form_gpuscan_info(CustomScan *cscan, GpuScanInfo *gs_info)
{
	List	   *privs = NIL;
	List	   *exprs = NIL;

	privs = lappend(privs, makeString(gs_info->kern_source ?
									  pstrdup(gs_info->kern_source) :
									  NULL));
	privs = lappend(privs, makeInteger(gs_info->extra_flags));
	privs = lappend(privs, gs_info->func_defs);
	exprs = lappend(exprs, gs_info->used_params);
	exprs = lappend(exprs, gs_info->used_vars);
	exprs = lappend(exprs, gs_info->dev_quals);
	privs = lappend(privs, makeInteger(gs_info->base_fixed_width));
	privs = lappend(privs, makeInteger(gs_info->proj_fixed_width));
	privs = lappend(privs, makeInteger(gs_info->proj_extra_width));

	cscan->custom_private = privs;
    cscan->custom_exprs = exprs;
}

static GpuScanInfo *
deform_gpuscan_info(CustomScan *cscan)
{
	GpuScanInfo	   *gs_info = palloc0(sizeof(GpuScanInfo));
	List		   *privs = cscan->custom_private;
	List		   *exprs = cscan->custom_exprs;
	int				pindex = 0;
	int				eindex = 0;

	Assert(IsA(cscan, CustomScan));
	gs_info->kern_source = strVal(list_nth(privs, pindex++));
	gs_info->extra_flags = intVal(list_nth(privs, pindex++));
	gs_info->func_defs = list_nth(privs, pindex++);
	gs_info->used_params = list_nth(exprs, eindex++);
	gs_info->used_vars = list_nth(exprs, eindex++);
	gs_info->dev_quals = list_nth(exprs, eindex++);
	gs_info->base_fixed_width = intVal(list_nth(privs, pindex++));
	gs_info->proj_fixed_width = intVal(list_nth(privs, pindex++));
	gs_info->proj_extra_width = intVal(list_nth(privs, pindex++));

	return gs_info;
}

typedef struct
{
	GpuTask			task;
	dlist_node		chain;
	CUfunction		kern_exec_quals;
	CUfunction		kern_dev_proj;
	CUdeviceptr		m_gpuscan;
	CUdeviceptr		m_kds_src;
	CUdeviceptr		m_kds_dst;
	CUevent 		ev_dma_send_start;
	CUevent			ev_dma_send_stop;	/* also, start kernel exec */
	CUevent			ev_dma_recv_start;	/* also, stop kernel exec */
	CUevent			ev_dma_recv_stop;
	pgstrom_data_store *pds_src;
	pgstrom_data_store *pds_dst;
	kern_resultbuf *kresults;
	kern_gpuscan	kern;
} pgstrom_gpuscan;

typedef struct {
	GpuTaskState	gts;

	BlockNumber		curr_blknum;
	BlockNumber		last_blknum;
	HeapTupleData	scan_tuple;
	List		   *dev_quals;
	bool			dev_projection;	/* true, if device projection is valid */
	cl_int			base_fixed_width; /* width of fixed fields on base rel */
	cl_int			proj_fixed_width; /* width of fixed fields on projection */
	cl_int			proj_extra_width; /* width of extra buffer on projection */
	cl_uint			num_rechecked;
} GpuScanState;

/* forward declarations */
static bool pgstrom_process_gpuscan(GpuTask *gtask);
static bool pgstrom_complete_gpuscan(GpuTask *gtask);
static void pgstrom_release_gpuscan(GpuTask *gtask);
static GpuTask *gpuscan_next_chunk(GpuTaskState *gts);
static TupleTableSlot *gpuscan_next_tuple(GpuTaskState *gts);

/*
 * cost_gpuscan
 *
 * cost estimation for GpuScan
 */
static void
cost_gpuscan(CustomPath *pathnode, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info,
			 List *host_quals, List *dev_quals, bool is_bulkload)
{
	Path	   *path = &pathnode->path;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		startup_delay = 0.0;
	double		spc_seq_page_cost;
	QualCost	dev_cost;
	QualCost	host_cost;
	Cost		gpu_per_tuple;
	Cost		cpu_per_tuple;
	cl_uint		num_chunks;
	Selectivity	dev_sel;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;
	num_chunks = estimate_num_chunks(path);

	/* fetch estimated page cost for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);
	/* Disk costs */
    run_cost += spc_seq_page_cost * baserel->pages;

	/* GPU costs */
	cost_qual_eval(&dev_cost, dev_quals, root);
	dev_sel = clauselist_selectivity(root, dev_quals, 0, JOIN_INNER, NULL);
	dev_cost.startup += pgstrom_gpu_setup_cost;
	if (cpu_tuple_cost > 0.0)
		dev_cost.per_tuple *= pgstrom_gpu_tuple_cost / cpu_tuple_cost;
	else
		dev_cost.per_tuple += disable_cost;

	/* CPU costs */
	cost_qual_eval(&host_cost, host_quals, root);

	/* Adjustment for param info */
	if (param_info)
	{
		QualCost	param_cost;

		/* Include costs of pushed-down clauses */
		cost_qual_eval(&param_cost, param_info->ppi_clauses, root);
		host_cost.startup += param_cost.startup;
		host_cost.per_tuple += param_cost.per_tuple;
	}

	/* total path cost */
	startup_cost += dev_cost.startup + host_cost.startup;
	if (!is_bulkload)
		cpu_per_tuple = host_cost.per_tuple + cpu_tuple_cost;
	else
		cpu_per_tuple = host_cost.per_tuple;
	gpu_per_tuple = dev_cost.per_tuple;
	run_cost += gpu_per_tuple * baserel->tuples;
	if (dev_quals != NIL)
		startup_delay = run_cost * (1.0 / (double)num_chunks);
	run_cost += cpu_per_tuple * dev_sel * baserel->tuples;

	path->startup_cost = startup_cost + startup_delay;
    path->total_cost = startup_cost + run_cost;
}

static void
gpuscan_add_scan_path(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Index rtindex,
					  RangeTblEntry *rte)
{
	CustomPath	   *pathnode;
	List		   *dev_quals = NIL;
	List		   *host_quals = NIL;
	ListCell	   *cell;
	codegen_context	context;

	/* call the secondary hook */
	if (set_rel_pathlist_next)
		set_rel_pathlist_next(root, baserel, rtindex, rte);

	/* nothing to do, if either PG-Strom or GpuScan is not enabled */
	if (!pgstrom_enabled || !enable_gpuscan)
		return;

	/* only base relation we can handle */
	if (baserel->rtekind != RTE_RELATION || baserel->relid == 0)
		return;

	/* system catalog is not supported */
	if (get_rel_namespace(rte->relid) == PG_CATALOG_NAMESPACE)
		return;

	/*
	 * check whether the qualifier can run on GPU device, or not
	 */
	pgstrom_init_codegen_context(&context);
	foreach (cell, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (pgstrom_codegen_available_expression(rinfo->clause))
			dev_quals = lappend(dev_quals, rinfo);
		else
			host_quals = lappend(host_quals, rinfo);
	}

	/* GpuScan does not make sense if no device qualifiers */
	if (dev_quals == NIL)
		return;

	/*
	 * Construction of a custom-plan node.
	 */
	pathnode = makeNode(CustomPath);
	pathnode->path.pathtype = T_CustomScan;
	pathnode->path.parent = baserel;
	pathnode->path.param_info
		= get_baserel_parampathinfo(root, baserel, baserel->lateral_relids);
	pathnode->path.pathkeys = NIL;	/* unsorted result */
	pathnode->flags = 0;
	pathnode->custom_private = NIL;	/* we don't use private field */
	pathnode->methods = &gpuscan_path_methods;

	/* cost estimation */
	cost_gpuscan(pathnode, root, baserel,
				 pathnode->path.param_info,
				 host_quals, dev_quals, false);
	/*
	 * FIXME: needs to pay attention for projection cost.
	 */

	/* check bulk-load capability */
	if (host_quals == NIL)
	{
		bool		support_bulkload = true;

		foreach (cell, baserel->reltargetlist)
		{
			Expr	   *expr = lfirst(cell);

			if (!IsA(expr, Var) &&
				!pgstrom_codegen_available_expression(expr))
			{
				support_bulkload = false;
				break;
			}
		}
		if (support_bulkload)
			pathnode->flags |= CUSTOMPATH_SUPPORT_BULKLOAD;
	}
	add_path(baserel, &pathnode->path);
}

/*
 * assign_bare_ntuples
 *
 * It assigns original number of tuples of the target relation when BulkScan
 * is built, because original plan_rows introduces the number of tuples
 * already filtered out, however, BulkScan will pull-up device executable
 * qualifiers.
 */
static void
assign_bare_ntuples(Plan *plannode, RangeTblEntry *rte)
{
	Relation		rel = heap_open(rte->relid, NoLock);
	BlockNumber		num_pages;
	double			num_tuples;
	double			allvisfrac;

	estimate_rel_size(rel, NULL, &num_pages, &num_tuples, &allvisfrac);
	plannode->plan_rows = num_tuples;

	heap_close(rel, NoLock);
}

/*
 * gpuscan_try_replace_relscan
 *
 * It tries to replace the supplied SeqScan plan by GpuScan, if it is
 * enough simple. Even though seq_path didn't involve any qualifiers,
 * it makes sense if parent path is managed by PG-Strom because of bulk-
 * loading functionality.
 */
Plan *
gpuscan_try_replace_seqscan(SeqScan *seqscan,
							List *range_tables,
							List **p_outer_quals,
							double *p_outer_ratio)
{
	CustomScan	   *cscan;
	GpuScanInfo		gs_info;
	RangeTblEntry  *rte;
	ListCell	   *lc;

	if (!enable_gpuscan || !pgstrom_bulkload_enabled)
		return NULL;

	Assert(IsA(seqscan, SeqScan));
	rte = rt_fetch(seqscan->scanrelid, range_tables);
	if (rte->rtekind != RTE_RELATION)
		return NULL;	/* usually, shouldn't happen */
	if (rte->relkind != RELKIND_RELATION &&
		rte->relkind != RELKIND_TOASTVALUE &&
		rte->relkind != RELKIND_MATVIEW)
		return NULL;	/* usually, shouldn't happen */

	/*
	 * Target-entry must be a simle varnode or device executable
	 * expression because it shall be calculated on device-side
	 * if it takes projection
	 */
	foreach (lc, seqscan->plan.targetlist)
	{
		TargetEntry	   *tle = lfirst(lc);

		if (!IsA(tle->expr, Var) &&
			!pgstrom_codegen_available_expression(tle->expr))
			return NULL;
	}

	/*
	 * Check whether the plan qualifiers is executable on device.
	 * Any host-only qualifier prevents bulk-loading.
	 */
	if (!pgstrom_codegen_available_expression((Expr *) seqscan->plan.qual))
		return NULL;

	/*
	 * OK, SeqScan with all device executable (or no) qualifiers, and
	 * no projection problems. So, GpuScan with bulk-load will ba a better
	 * choice than SeqScan.
	 */
	cscan = makeNode(CustomScan);
	cscan->scan.plan.startup_cost = seqscan->plan.startup_cost;
	cscan->scan.plan.total_cost = seqscan->plan.total_cost;
	cscan->scan.plan.plan_width = seqscan->plan.plan_width;
	assign_bare_ntuples(&cscan->scan.plan, rte);
	cscan->scan.plan.targetlist = copyObject(seqscan->plan.targetlist);
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.extParam = bms_copy(seqscan->plan.extParam);
	cscan->scan.plan.allParam = bms_copy(seqscan->plan.allParam);
	cscan->scan.scanrelid = seqscan->scanrelid;
	cscan->flags = CUSTOMPATH_SUPPORT_BULKLOAD;
	memset(&gs_info, 0, sizeof(GpuScanInfo));
	form_gpuscan_info(cscan, &gs_info);
	cscan->custom_relids = bms_make_singleton(seqscan->scanrelid);
	cscan->methods = &bulkscan_plan_methods;

	*p_outer_quals = copyObject(seqscan->plan.qual);
	*p_outer_ratio = Min(seqscan->plan.plan_rows /
						 cscan->scan.plan.plan_rows, 1.0);

	return &cscan->scan.plan;
}

/*
 * gpuscan_pullup_devquals - construct an equivalen GpuScan node, but
 * no device qualifiers which is pulled-up. In case of bulk-loading,
 * it is more reasonable to run device qualifier on upper node, than
 * individually.
 */
Plan *
gpuscan_pullup_devquals(Plan *plannode,
						List *range_tables,
						List **p_outer_quals,
						double *p_outer_ratio)
{
	CustomScan	   *cscan_old = (CustomScan *) plannode;
	CustomScan	   *cscan_new;
	GpuScanInfo	   *gs_info;
	RangeTblEntry  *rte = rt_fetch(cscan_old->scan.scanrelid, range_tables);
	List		   *outer_quals;

	Assert(pgstrom_plan_is_gpuscan(plannode));
	gs_info = deform_gpuscan_info(cscan_old);

	/* in case of nothing to be changed */
	if (gs_info->dev_quals == NIL)
	{
		Assert(cscan_old->methods == &bulkscan_plan_methods);
		Assert(gs_info->kern_source == NULL);
		*p_outer_quals = NIL;
		*p_outer_ratio = 1.0;
		return &cscan_old->scan.plan;
	}
	outer_quals = copyObject(gs_info->dev_quals);

	cscan_new = copyObject(cscan_old);
	assign_bare_ntuples(&cscan_new->scan.plan, rte);
	cscan_new->methods = &bulkscan_plan_methods;
	memset(gs_info, 0, sizeof(GpuScanInfo));
	form_gpuscan_info(cscan_new, gs_info);

	*p_outer_quals = outer_quals;
	*p_outer_ratio = Min(cscan_old->scan.plan.plan_rows /
						 cscan_new->scan.plan.plan_rows, 1.0);
	return &cscan_new->scan.plan;
}

/*
 * OpenCL code generation that can run on GPU/MIC device
 */
static char *
gpuscan_codegen_exec_quals(PlannerInfo *root,
						   List *dev_quals,
						   codegen_context *context)
{
	StringInfoData	body;
	char		   *expr_code;

	initStringInfo(&body);

	appendStringInfo(
		&body,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpuscan_quals_eval(kern_context *kcxt,\n"
		"                   kern_data_store *kds,\n"
		"                   size_t kds_index)\n"
		"{\n");

	if (dev_quals != NIL)
	{
		/* OK, let's walk on the device expression tree */
		expr_code = pgstrom_codegen_expression((Node *)dev_quals, context);

		/* add parameter declarations */
		pgstrom_codegen_param_declarations(&body, context);
		/* add variables declarations */
		pgstrom_codegen_var_declarations(&body, context);

		appendStringInfo(
			&body,
			"\n"
			"  return EVAL(%s);\n",
			expr_code);
	}
	else
	{
		appendStringInfo(
			&body,
			"  return true;\n");
	}
	appendStringInfo(
		&body,
		"}\n");

	return body.data;
}

static Plan *
create_gpuscan_plan(PlannerInfo *root,
					RelOptInfo *rel,
					CustomPath *best_path,
					List *tlist,
					List *clauses,
					List *custom_children)
{
	CustomScan	   *cscan;
	GpuScanInfo		gs_info;
	List		   *host_quals = NIL;
	List		   *dev_quals = NIL;
	ListCell	   *cell;
	char		   *kern_source;
	codegen_context	context;

	/* It should be a base relation */
	Assert(rel->relid > 0);
	Assert(rel->rtekind == RTE_RELATION);
	Assert(custom_children == NIL);

	/*
	 * Distribution of clauses into device executable and others.
	 *
	 * NOTE: Why we don't sort out on Path construction stage is,
	 * create_scan_plan() may add parameterized scan clause, thus
	 * we have to delay the final decision until this point.
	 */
	foreach (cell, clauses)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (!pgstrom_codegen_available_expression(rinfo->clause))
			host_quals = lappend(host_quals, rinfo);
		else
			dev_quals = lappend(dev_quals, rinfo);
	}
	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	host_quals = extract_actual_clauses(host_quals, false);
    dev_quals = extract_actual_clauses(dev_quals, false);

	/*
	 * Construct OpenCL kernel code
	 */
	pgstrom_init_codegen_context(&context);
	kern_source = gpuscan_codegen_exec_quals(root, dev_quals, &context);

	/*
	 * Construction of GpuScanPlan node; on top of CustomPlan node
	 */
	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = host_quals;
	cscan->scan.plan.lefttree = NULL;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.scanrelid = rel->relid;

	memset(&gs_info, 0, sizeof(GpuScanInfo));
	gs_info.kern_source = kern_source;
	gs_info.extra_flags = context.extra_flags | DEVKERNEL_NEEDS_GPUSCAN;
	gs_info.func_defs = context.func_defs;
	gs_info.used_params = context.used_params;
	gs_info.used_vars = context.used_vars;
	gs_info.dev_quals = dev_quals;
	form_gpuscan_info(cscan, &gs_info);
	cscan->flags = best_path->flags;
	cscan->methods = &gpuscan_plan_methods;

	return &cscan->scan.plan;
}

/*
 * pgstrom_path_is_gpuscan
 *
 * It returns true, if supplied path node is gpuscan.
 */
bool
pgstrom_path_is_gpuscan(const Path *path)
{
	if (IsA(path, CustomPath) &&
		path->pathtype == T_CustomScan &&
		((CustomPath *) path)->methods == &gpuscan_path_methods)
		return true;
	return false;
}

/*
 * pgstrom_plan_is_gpuscan
 *
 * It returns true, if supplied plan node is gpuscan.
 */
bool
pgstrom_plan_is_gpuscan(const Plan *plan)
{
	CustomScan	   *cscan = (CustomScan *) plan;

	if (IsA(cscan, CustomScan) &&
		(cscan->methods == &gpuscan_plan_methods ||
		 cscan->methods == &bulkscan_plan_methods))
		return true;
	return false;
}









/*
 * gpuscan_codegen_projection
 *
 * It makes GPU code to transform baserel => scan_tlist
 * Job of scan_tlist => targetlist by CPu
 *
 * Job of GPUSCAN_DEVICE_PROJECTION_(ROW|SLOT) macro:
 * - define tup_values[] / tup_isnull[] array (only ROW)
 * - define KVAR variables if referenced within expression node
 * - extract tuple and load data to KVAR/tup_values
 * - execute expression node, then store tup_values[]
 */
static char *
gpuscan_codegen_projection(CustomScan *cscan, TupleDesc tupdesc,
						   codegen_context *context)
{
	Index			scanrelid = cscan->scan.scanrelid;
	List		   *scan_tlist = cscan->custom_scan_tlist;
	AttrNumber	   *varremaps;
	Bitmapset	   *varattnos;
	ListCell	   *lc;
	int				prev;
	int				i, j, k;
	bool			needs_vlbuf = false;
	devtype_info   *dtype;
	StringInfoData	decl;
	StringInfoData	body;
	StringInfoData	temp;

	initStringInfo(&decl);
	initStringInfo(&body);
	initStringInfo(&temp);

	/*
	 * step.1 - declaration of function and KVAR_xx for expressions
	 */
	appendStringInfo(
		&decl,
		"STATIC_FUNCTION(void)\n"
		"gpuscan_projection(kern_context *kcxt,\n"
		"                   kern_data_store *kds,\n"
		"                   kern_tupitem *tupitem,\n"
		"                   cl_int format,\n"
		"                   Datum *tup_values,\n"
		"                   cl_bool *tup_isnull,\n"
		"                   cl_bool *tup_internal)\n"
		"{\n"
		"  HeapTupleHeaderData *htup;\n");

	varremaps = palloc0(sizeof(AttrNumber) * list_length(scan_tlist));
	varattnos = NULL;
	foreach (lc, scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);

		/*
		 * NOTE: If expression of TargetEntry is a simple Var-node,
		 * we can load the value into tup_values[]/tup_isnull[]
		 * array regardless of the data type. We have to track which
		 * column is the source of this TargetEntry.
		 * Elsewhere, we will construct device side expression using
		 * KVAR_xx variables.
		 */
		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			Assert(var->varno == scanrelid);
			Assert(var->varattno > 0 && var->varattno <= tupdesc->natts);
			varremaps[tle->resno - 1] = var->varattno;
		}
		else
		{
			pull_varattnos((Node *) tle->expr, scanrelid, &varattnos);
		}
	}

	prev = -1;
	while ((prev = bms_next_member(varattnos, prev)) >= 0)
	{
		Form_pg_attribute attr;
		AttrNumber	anum = prev + FirstLowInvalidHeapAttributeNumber;

		/* system column should not appear within device expression */
		Assert(anum > 0);
		attr = tupdesc->attrs[anum - 1];

		dtype = pgstrom_devtype_lookup(attr->atttypid);
		if (!dtype)
			elog(ERROR, "Bug? failed to lookup device supported type: %s",
				 format_type_be(attr->atttypid));
		appendStringInfo(&decl,
						 "  pg_%s_t KVAR_%u;\n",
						 dtype->type_name, anum);
	}

	/*
	 * step.2 - extract tuples and load values to KVAR or values/isnull array
	 * (only if tupitem_src is valid, of course)
	 */
	appendStringInfo(
		&body,
		"  htup = (!tupitem ? NULL : &tupitem->htup);\n"
		"  if (htup)\n"
		"  {\n"
		"    char    *curr = (char *)htup + htup->t_hoff;\n"
		"    cl_bool  heap_hasnull\n"
		"        = ((htup->t_infomask & HEAP_HASNULL) != 0);\n"
		"\n"
		"    assert((devptr_t)curr == MAXALIGN(curr));\n");

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];
		bool		referenced = false;

		dtype = pgstrom_devtype_lookup(attr->atttypid);
		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;

		appendStringInfo(
			&temp,
			"    /* attribute %d */\n"
			"    if (!heap_hasnull || !att_isnull(%d, htup->t_bits))\n"
			"    {\n",
			attr->attnum,
			attr->attnum - 1);

		/* current offset alignment */
		if (attr->attlen > 0)
			appendStringInfo(
				&temp,
				"      curr = (char *)TYPEALIGN(%d, curr);\n",
				typealign_get_width(attr->attalign));
		else
			appendStringInfo(
				&temp,
				"      if (!VARATT_NOT_PAD_BYTE(curr))\n"
				"        curr = (char *)TYPEALIGN(%d, curr);\n",
				typealign_get_width(attr->attalign));

		/* Put values on tup_values/tup_isnull and KVAR_xx if referenced */
		for (j=0; j < list_length(scan_tlist); j++)
		{
			if (varremaps[j] != attr->attnum)
				continue;

			appendStringInfo(
				&temp,
				"      tup_isnull[%d] = false;\n", j);
			if (attr->attbyval)
			{
				appendStringInfo(
					&temp,
					"      tup_values[%d] = *((%s *) curr);\n",
					j,
					(attr->attlen == sizeof(cl_long) ? "cl_long"
					 : attr->attlen == sizeof(cl_int) ? "cl_int"
					 : attr->attlen == sizeof(cl_short) ? "cl_short"
					 : "cl_char"));
			}
			else
			{
				appendStringInfo(
					&temp,
					"      tup_values[%d] = devptr_to_host(kds,curr);\n",
					j);
			}
			referenced = true;
		}

		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;
		if (bms_is_member(k, varattnos))
		{
			appendStringInfo(
				&temp,
				"      KVAR_%u = pg_%s_datum_ref(kcxt, curr, false);\n",
				attr->attnum,
				dtype->type_name);
			referenced = true;
		}
		/* make advance the offset */
		if (attr->attlen > 0)
			appendStringInfo(
				&temp,
				"      curr += %d;\n",
				attr->attlen);
		else
			appendStringInfo(
				&temp,
				"      curr += VARSIZE_ANY(curr);\n");

		appendStringInfo(
			&temp,
			"    }\n");

		/* Put NULL on tup_isnull and KVAR_xx, if needed */
		if (referenced)
		{
			appendStringInfo(
				&temp,
				"    else\n"
				"    {\n");

			for (j=0; j < list_length(scan_tlist); j++)
			{
				if (varremaps[j] == attr->attnum)
					appendStringInfo(
						&temp,
						"      tup_isnull[%d] = true;\n", j);
			}
			k = attr->attnum - FirstLowInvalidHeapAttributeNumber;
			if (bms_is_member(k, varattnos))
			{
				appendStringInfo(
					&temp,
					"      KVAR_%u = pg_%s_datum_ref(kcxt, NULL, false);\n",
					attr->attnum,
					dtype->type_name);
			}
			appendStringInfo(
				&temp,
				"    }\n");
		}

		if (referenced)
		{
			appendStringInfo(&body, "%s", temp.data);
			resetStringInfo(&temp);
		}
	}

	/*
	 * step.3 - execute expression node, then store the result onto KVAR_xx
	 */
    foreach (lc, scan_tlist)
    {
        TargetEntry    *tle = lfirst(lc);
		Oid				type_oid;

		if (IsA(tle->expr, Var))
			continue;

		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %u", type_oid);

		appendStringInfo(
			&decl,
			"  pg_%s_t temp_%u_v;\n",
			dtype->type_name,
			tle->resno);
		appendStringInfo(
			&body,
			"    temp_%u_v = %s;\n",
			tle->resno,
			pgstrom_codegen_expression((Node *) tle->expr, context));
	}

	appendStringInfo(
		&body,
		"  }\n"
		"\n");

	/*
	 * step.4 (only FDW_FORMAT_SLOT) - We have to acquire variable length
	 * buffer for indirect or numeric data type.
	 */
	appendStringInfo(
		&body,
		"  if (format == KDS_FORMAT_SLOT)\n"
		"  {\n");

	resetStringInfo(&temp);
	appendStringInfo(
		&temp,
		"    cl_uint vl_len = 0;\n"
		"    char   *vl_buf = NULL;\n"
		"\n"
		"    if (htup)\n"
		"    {\n");

	foreach (lc, scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);
		Oid				type_oid;

		if (IsA(tle->expr, Var))
			continue;

		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %u", type_oid);

		if (type_oid == NUMERICOID)
		{
			appendStringInfo(
				&temp,
				"    if (!temp_%u_v.isnull)\n"
				"      vl_len = TYPEALIGN(sizeof(cl_uint), vl_len)\n"
				"             + pg_numeric_to_varlena(kcxt,NULL,\n"
				"                                     temp_%u_v.value,\n"
				"                                     temp_%u_v.isnull);\n",
				tle->resno,
				tle->resno,
				tle->resno);
		}
		else if (!dtype->type_byval)
		{
			/* varlena is not supported yet */
			Assert(dtype->type_length > 0);

			appendStringInfo(
				&temp,
				"    if (!temp_%u_v.isnull)\n"
				"      vl_len = TYPEALIGN(%u, vl_len) + %u;\n",
				tle->resno,
				dtype->type_align,
				dtype->type_length);
		}
	}

	appendStringInfo(
		&temp,
		"    }\n"
		"    /* allocation of variable length buffer */\n "
		"    vl_len = MAXALIGN(vl_len);\n"
		"    offset = arithmetic_stairlike_add(vl_len, &count);\n"
		"    if (get_local_id() == 0)\n"
		"    {\n"
		"      if (count > 0)\n"
		"        base = atomicAdd(&kds_dst->usage, count);\n"
		"      else\n"
		"        base = 0;\n"
		"    }\n"
		"    __syncthreads();\n"
		"\n"
		"    if (KERN_DATA_STORE_SLOT_LENGTH(kds_dst, dst_limit) +\n"
		"        base + count > kds_dst->length)\n"
		"    {\n"
		"      STROM_SET_ERROR(&kcxt->e, StromError_DataStoreNoSpace);\n"
		"      return;\n"
		"    }\n"
		"    vl_buf = (char *)kds_dst + kds_dst->length\n"
		"           - (base + offset + vl_len);\n");

	if (needs_vlbuf)
		appendStringInfo(&body, "%s", temp.data);

	/*
	 * step.5 (only FDW_FORMAT_SLOT) - Store the KVAR_xx on the slot.
	 * pointer types must be host pointer
	 */
	appendStringInfo(
		&body,
		"    if (htup)\n"
		"    {\n");

	foreach (lc, scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);
		Oid				type_oid;

		if (IsA(tle->expr, Var))
			continue;

		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %u", type_oid);

		appendStringInfo(
			&body,
			"    tup_isnull[%d] = temp_%u_v.isnull;\n",
			tle->resno - 1, tle->resno);

		if (type_oid == NUMERICOID)
		{
			appendStringInfo(
				&body,
				"    if (!temp_%u_v.isnull)\n"
				"    {\n"
				"      vl_buf = (char *)TYPEALIGN(sizeof(cl_int), vl_buf);\n"
				"      tup_values[%d] = devptr_to_host(kds_dst, vl_buf);\n"
				"      vl_buf += pg_numeric_to_varlena(kcxt, vl_buf,\n"
				"                                      temp_%u_v.value,\n"
				"                                      temp_%u_v.isnull);\n",
				tle->resno,
				tle->resno - 1,
				tle->resno,
				tle->resno);
		}
		else if (dtype->type_byval)
		{
			/* FIXME: How to transform to the datum? */
			appendStringInfo(
				&body,
				"    if (!temp_%u_v.isnull)\n"
				"      tup_values[%d] = pg_%s_to_datum(temp_%u_v.value);\n",
				tle->resno,
				tle->resno - 1,
				dtype->type_name,
				tle->resno);
		}
		else
		{
			Assert(dtype->type_length > 0);
			appendStringInfo(
				&body,
				"    if (!temp_%u_v.isnull)\n"
				"    {\n"
				"      vl_buf = (char *)TYPEALIGN(%u, vl_buf);\n"
				"      tup_values[%d] = devptr_to_host(kds_dst, vl_buf);\n"
				"      memcpy(vl_buf, &temp_%u_v.value, %d);\n"
				"      vl_buf += %d;\n"
				"    }\n",
				tle->resno,
				dtype->type_align,
				tle->resno - 1,
				tle->resno, dtype->type_length,
				dtype->type_length);
		}
	}
	appendStringInfo(
		&body,
		"    }\n"
		"  }\n");

	/*
	 * step.6 (only FDW_FORMAT_ROW) - Stora the KVAR_xx on the slot.
	 * pointer types must be device pointer.
	 */
	appendStringInfo(
		&body,
		"  else\n"
		"  {\n"
		"    if (htup)\n"
		"    {\n");

	foreach (lc, scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);
		Oid				type_oid;

		if (IsA(tle->expr, Var))
			continue;

		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %u", type_oid);

		appendStringInfo(
			&body,
			"      tup_isnull[%d] = temp_%u_v.isnull;\n",
			tle->resno - 1, tle->resno);

		if (type_oid == NUMERICOID)
		{
			appendStringInfo(
				&body,
				"      tup_internal[%d] = true;\n"
				"      if (!temp_%u_v.isnull)\n"
				"        tup_values[%d] = temp_%u_v.value;\n",
				tle->resno - 1,
				tle->resno,
				tle->resno - 1, tle->resno);
		}
		else if (dtype->type_byval)
		{
			appendStringInfo(
				&body,
				"      if (!temp_%u_v.isnull)\n"
				"        tup_values[%d] = temp_%u_v.value;\n",
				tle->resno,
				tle->resno - 1, tle->resno);
		}
		else
		{
			Assert(dtype->type_length > 0);
			appendStringInfo(
				&body,
				"      if (!temp_%u_v.isnull)\n"
				"      {\n"
				"        vl_buf = (char *)TYPEALIGN(%u, vl_buf);\n"
				"        tup_values[%d] = devptr_to_host(kds_dst, vl_buf);\n"
				"        memcpy(vl_buf, &temp_%u_v.value, %u);\n"
				"        vl_buf += %u;\n"
				"      }\n",
				tle->resno,
				dtype->type_align,
				tle->resno - 1,
				tle->resno, dtype->type_length,
				dtype->type_length);
		}
	}
	appendStringInfo(
		&body,
		"    }\n"
		"  }\n"
		"}\n");

	/* parameter references */
	pgstrom_codegen_param_declarations(&decl, context);

	/* merge declaration and body */
	appendStringInfo(&decl, "\n%s", body.data);
	pfree(body.data);

	return decl.data;
}

/*
 * build_device_projection
 *
 *
 *
 *
 */
static Node *
replace_varnode_with_scan_tlist(Node *node, List *scan_tlist)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		ListCell   *lc;

		Assert(var->varlevelsup == 0);

		foreach (lc, scan_tlist)
		{
			TargetEntry	   *tle = lfirst(lc);
			Var			   *scan_var;

			if (!IsA(tle->expr, Var))
				continue;
			scan_var = (Var *) tle->expr;

			Assert(scan_var->varno == var->varno);
			if (scan_var->varattno == var->varattno)
			{
				Assert(scan_var->vartype == var->vartype &&
					   scan_var->vartypmod == var->vartypmod &&
					   scan_var->varcollid == var->varcollid);

				return (Node *) makeVar(INDEX_VAR,
										tle->resno,
										var->vartype,
										var->vartypmod,
										var->varcollid,
										0);
			}
		}
		elog(ERROR, "Bug? referenced Var-node not in scan_tlist");
	}
	return expression_tree_mutator(node, replace_varnode_with_scan_tlist,
								   scan_tlist);
}

static AttrNumber
insert_unique_expression(Expr *expr, List **p_scan_tlist)
{
	TargetEntry	   *tle;
	ListCell	   *lc;

	foreach (lc, *p_scan_tlist)
	{
		tle = lfirst(lc);
		if (equal(expr, tle->expr))
			return tle->resno;
	}

	/*
	 * Not found, so append this expression 
	 */
	tle = makeTargetEntry(copyObject(expr),
						  list_length(*p_scan_tlist) + 1,
						  NULL,
						  false);
	*p_scan_tlist = lappend(*p_scan_tlist, tle);

	return list_length(*p_scan_tlist);
}

static bool
build_device_projection(TupleDesc tupdesc,
						CustomScan *cscan,
						GpuScanInfo *gs_info)
{
	List	   *tlist_old = cscan->scan.plan.targetlist;
	Index		varno = cscan->scan.scanrelid;
	AttrNumber	attnum;
	ListCell   *lc;
	List	   *tlist_new = NIL;
	List	   *scan_tlist = NIL;
	cl_int		base_fixed_width = 0;
	cl_int		proj_fixed_width = 0;
	cl_int		proj_extra_width = 0;
	bool		with_gpu_projection = false;

#if NOT_USED
	/*
	 * XXX - Do we actually need projection if tlist is shorter than
	 * definition of the base relation but it is compatible with?
	 */
	if (list_length(tlist_old) != tupdesc->natts)
		with_gpu_projection = true;
#endif

	attnum = 1;
	foreach (lc, tlist_old)
	{
		TargetEntry	   *tle = lfirst(lc);
		TargetEntry	   *tle_new;
		AttrNumber		varattno;

		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			/* if these Asserts fail, planner messed up */
			Assert(var->varno == varno);
			Assert(var->varlevelsup == 0);

			/* GPU projection cannot contain whole-row reference */
			if (var->varattno == InvalidAttrNumber)
				return false;

			/*
			 * check whether the original tlist matches the physical layout
			 * of the base relation. GPU can reorder the var reference
			 * regardless of the data-type support.
			 */
			if (var->varattno != attnum || attnum > tupdesc->natts)
				with_gpu_projection = true;
			else
			{
				Form_pg_attribute	attr = tupdesc->attrs[attnum - 1];

				/* How to reference the dropped column? */
				Assert(!attr->attisdropped);
				/* See the logic in tlist_matches_tupdesc */
				if (var->vartype != attr->atttypid ||
					(var->vartypmod != attr->atttypmod &&
					 var->vartypmod != -1))
					with_gpu_projection = true;
			}
			/* add primitive Var-node on the scan_tlist */
			varattno = insert_unique_expression((Expr *) var, &scan_tlist);

			/* add pseudo Var-node on the tlist_new */
			tle_new = makeTargetEntry((Expr *) makeVar(INDEX_VAR,
													   varattno,
													   var->vartype,
													   var->vartypmod,
													   var->varcollid,
													   0),
									  list_length(tlist_new) + 1,
									  tle->resname,
									  tle->resjunk);
			tlist_new = lappend(tlist_new, tle_new);
		}
		else if (pgstrom_codegen_available_expression(tle->expr))
		{
			Oid		type_oid = exprType((Node *)tle->expr);
			Oid		type_mod = exprTypmod((Node *)tle->expr);
			Oid		coll_oid = exprCollation((Node *)tle->expr);

			/* Add device executable expression onto the scan_tlist */
			varattno = insert_unique_expression(tle->expr, &scan_tlist);

			/* Then, CPU just referenced the calculation result */
			tle_new = makeTargetEntry((Expr *) makeVar(INDEX_VAR,
													   varattno,
													   type_oid,
													   type_mod,
													   coll_oid,
													   0),
									  list_length(tlist_new) + 1,
									  tle->resname,
									  tle->resjunk);
			tlist_new = lappend(tlist_new, tle_new);

			/* obviously, we need GPU projection */
			with_gpu_projection = true;
		}
		else
		{
			/* Elsewhere, expression is not device executable */
			Bitmapset  *varattnos = NULL;
			int			prev = -1;
			Node	   *expr_new;

			/* 1. Pull varnodes within the expression*/
			pull_varattnos((Node *) tle->expr, varno, &varattnos);

			/* 2. Add varnodes to scan_tlist */
			while ((prev = bms_next_member(varattnos, prev)) >= 0)
			{
				Form_pg_attribute	attr;
				int		anum = prev - FirstLowInvalidHeapAttributeNumber;

				/* GPU projection cannot contain whole-row reference */
				if (anum == InvalidAttrNumber)
					return false;

				/* Add varnodes to scan_tlist */
				if (anum > 0)
					attr = tupdesc->attrs[anum - 1];
				else
					attr = SystemAttributeDefinition(anum, true);

				insert_unique_expression((Expr *) makeVar(varno,
														  attr->attnum,
														  attr->atttypid,
														  attr->atttypmod,
														  attr->attcollation,
														  0),
										 &scan_tlist);
			}
			/* 3. replace varnode of the expression */
			expr_new = replace_varnode_with_scan_tlist((Node *) tle->expr,
													   scan_tlist);
			/* 4. add modified expression to tlist_new */
			tle_new = makeTargetEntry((Expr *)expr_new,
									  list_length(tlist_new) + 1,
									  tle->resname,
									  tle->resjunk);
			tlist_new = lappend(tlist_new, tle_new);

			/* obviously, we need GPU projection */
			with_gpu_projection = true;
		}
		attnum++;
	}

	/*
	 * Hmm, it seems to me device projection is not necessary
	 */
	if (!with_gpu_projection)
		return false;

	/*
	 * Track width of the fixed-length fields for simple/indirect types,
	 * and width of the fixed-length fields of the base relation also.
	 */
	for (attnum=1; attnum < tupdesc->natts; attnum++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[attnum - 1];

		if (attr->attlen < 0)
			continue;
		base_fixed_width += attr->attlen;
	}

	foreach (lc, scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);
		Oid				type_oid = exprType((Node *) tle->expr);
		int16				type_len;
		bool			type_byval;

		if (type_oid == NUMERICOID)
		{
			proj_fixed_width += 32;	// XXX - TO BE FIXED LATER
			proj_extra_width += 32;	// XXX - TO BE FIXED LATER
		}
		else
		{
			get_typlenbyval(type_oid, &type_len, &type_byval);
			if (type_len < 0)
				continue;
			proj_fixed_width += type_len;
			if (!type_byval)
				proj_extra_width += type_len;
		}
	}

	/* update plan node */
	cscan->scan.plan.targetlist = tlist_new;
	cscan->custom_scan_tlist = scan_tlist;
	gs_info->base_fixed_width = base_fixed_width;
	gs_info->proj_fixed_width = proj_fixed_width;
	gs_info->proj_extra_width = proj_extra_width;

	return true;
}

/*
 * pgstrom_post_planner_gpuscan
 *
 * Applies projection of GpuScan if needed.
 */
void
pgstrom_post_planner_gpuscan(PlannedStmt *pstmt, Plan **p_curr_plan)
{
//	GpuScan		   *gscan = (GpuScan *) cscan;
	CustomScan	   *cscan = (CustomScan *) (*p_curr_plan);
	GpuScanInfo	   *gs_info = deform_gpuscan_info(cscan);
	List		   *rtables = pstmt->rtable;
	RangeTblEntry  *rte;
	Relation		baserel;
	TupleDesc		tupdesc;
	codegen_context	context;

	pgstrom_init_codegen_context(&context);
	context.func_defs = gs_info->func_defs;	/* restore */

	/*
	 * TODO: We may need to replace SeqScan by GpuScan with device
	 * projection, if it is enough cost effective.
	 */
	rte = rt_fetch(cscan->scan.scanrelid, rtables);
	Assert(rte->rtekind == RTE_RELATION);
	baserel = heap_open(rte->relid, NoLock);
	tupdesc = RelationGetDescr(baserel);

	if (build_device_projection(tupdesc, cscan, gs_info))
	{
		StringInfoData	kern;

		initStringInfo(&kern);
		appendStringInfo(&kern, "%s\n%s\n",
						 gs_info->kern_source,
						 gpuscan_codegen_projection(cscan,
													tupdesc,
													&context));
		gs_info->kern_source = kern.data;
	}

	/*
	 * Declaration of functions
	 */
	if (gs_info->func_defs != NULL)
	{
		StringInfoData	kern;

		initStringInfo(&kern);
		pgstrom_codegen_func_declarations(&kern, &context);
		appendStringInfo(&kern, "%s", gs_info->kern_source);

		gs_info->kern_source = kern.data;
	}
	form_gpuscan_info(cscan, gs_info);

	heap_close(baserel, NoLock);
}

/*
 * pgstrom_gpuscan_setup_bulkslot
 *
 * It setup tuple-slot for bulk-loading and projection-info to transform
 * the tuple into expected form.
 * (Once CustomPlan become CustomScan, no need to be a API)
 */
void
pgstrom_gpuscan_setup_bulkslot(PlanState *outer_planstate,
							   ProjectionInfo **p_bulk_proj,
							   TupleTableSlot **p_bulk_slot)
{
	CustomScanState *css = (CustomScanState *) outer_planstate;

	if (!IsA(css, CustomScanState) ||
		css->methods != &gpuscan_exec_methods.c)
		elog(ERROR, "Bug? PlanState node is not GpuScanState");

	*p_bulk_proj = css->ss.ps.ps_ProjInfo;
	*p_bulk_slot = css->ss.ss_ScanTupleSlot;
}

/*
 * gpuscan_create_scan_state
 *
 * allocation of GpuScanState, rather than CustomScanState
 */
static Node *
gpuscan_create_scan_state(CustomScan *cscan)
{
	GpuScanState   *gss = palloc0(sizeof(GpuScanState));

	/* Set tag and executor callbacks */
	NodeSetTag(gss, T_CustomScanState);
	gss->gts.css.flags = cscan->flags;
	if (cscan->methods == &gpuscan_plan_methods)
		gss->gts.css.methods = &gpuscan_exec_methods.c;
	else if (cscan->methods == &bulkscan_plan_methods)
		gss->gts.css.methods = &bulkscan_exec_methods.c;
	else
		elog(ERROR, "Bug? unexpected CustomPlanMethods");

	return (Node *) gss;
}

static void
gpuscan_begin(CustomScanState *node, EState *estate, int eflags)
{
	Relation		scan_rel = node->ss.ss_currentRelation;
	GpuContext	   *gcontext = NULL;
	GpuScanState   *gss = (GpuScanState *) node;
	CustomScan	   *cscan = (CustomScan *)node->ss.ps.plan;
	GpuScanInfo	   *gs_info = deform_gpuscan_info(cscan);

	/* gpuscan should not have inner/outer plan right now */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/* activate GpuContext for device execution */
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		gcontext = pgstrom_get_gpucontext();
	/* setup common GpuTaskState fields */
	pgstrom_init_gputaskstate(gcontext, &gss->gts);
	gss->gts.cb_task_process = pgstrom_process_gpuscan;
	gss->gts.cb_task_complete = pgstrom_complete_gpuscan;
	gss->gts.cb_task_release = pgstrom_release_gpuscan;
	gss->gts.cb_next_chunk = gpuscan_next_chunk;
	gss->gts.cb_next_tuple = gpuscan_next_tuple;

	/* initialize the start/end position */
	gss->curr_blknum = 0;
	gss->last_blknum = RelationGetNumberOfBlocks(scan_rel);
	/* initialize device qualifiers also, for fallback */
	gss->dev_quals = (List *)
		ExecInitExpr((Expr *) gs_info->dev_quals, &gss->gts.css.ss.ps);
	/* true, if device projection is needed */
	gss->dev_projection = (cscan->custom_scan_tlist != NIL);
	/* 'tableoid' should not change during relation scan */
	gss->scan_tuple.t_tableOid = RelationGetRelid(scan_rel);
	/* assign kernel source and flags */
	pgstrom_assign_cuda_program(&gss->gts,
								gs_info->used_params,
								gs_info->kern_source,
								gs_info->extra_flags);
	/* preload the CUDA program, if actually executed */
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		pgstrom_preload_cuda_program(&gss->gts);

	/* other run-time parameters */
    gss->num_rechecked = 0;
}

/*
 * pgstrom_release_gpuscan
 *
 * Callback handler when reference counter of pgstrom_gpuscan object
 * reached to zero, due to pgstrom_put_message.
 * It also unlinks associated device program and release row-store.
 * Note that this callback shall never be invoked under the OpenCL
 * server context, because some resources (like shared-buffer) are
 * assumed to be released by the backend process.
 */
static void
pgstrom_release_gpuscan(GpuTask *gputask)
{
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *) gputask;

	if (gpuscan->pds_src)
		pgstrom_release_data_store(gpuscan->pds_src);
	if (gpuscan->pds_dst)
		pgstrom_release_data_store(gpuscan->pds_dst);
	pgstrom_complete_gpuscan(&gpuscan->task);

	pfree(gpuscan);
}

static pgstrom_gpuscan *
create_pgstrom_gpuscan_task(GpuScanState *gss, pgstrom_data_store *pds_src)
{
	TupleDesc			scan_tupdesc = GTS_GET_SCAN_TUPDESC(gss);
	GpuContext		   *gcontext = gss->gts.gcontext;
	pgstrom_gpuscan    *gpuscan;
	kern_resultbuf	   *kresults;
	kern_data_store	   *kds_src = pds_src->kds;
	pgstrom_data_store *pds_dst;
	Size				length;

	/*
	 * allocation of the destination buffer
	 */
	if (gss->gts.be_row_format)
	{
		/*
		 * NOTE: When we have no device projection and row-format
		 * is required, we don't need to have destination buffer.
		 * kern_resultbuf will have offset of the visible rows,
		 * so we can reference pds_src as original PG-Strom did.
		 */
		if (!gss->dev_projection)
			pds_dst = NULL;
		else
		{
			length = (kds_src->length +
					  Max(gss->proj_fixed_width -
						  gss->base_fixed_width, 0) * kds_src->nitems);
			pds_dst = pgstrom_create_data_store_row(gcontext,
													scan_tupdesc,
													length,
													false);
		}
	}
	else
	{
		length = gss->proj_extra_width * kds_src->nitems;
		pds_dst = pgstrom_create_data_store_slot(gcontext,
												 scan_tupdesc,
												 kds_src->nitems,
												 false,
												 length,
												 NULL);
	}

	/*
	 * allocation of pgstrom_gpuscan
	 */
	length = (STROMALIGN(offsetof(pgstrom_gpuscan, kern.kparams)) +
			  STROMALIGN(gss->gts.kern_params->length) +
			  STROMALIGN(offsetof(kern_resultbuf,
								  results[pds_dst ? 0 : kds_src->nitems])));
	gpuscan = MemoryContextAllocZero(gcontext->memcxt, length);
	/* setting up */
	pgstrom_init_gputask(&gss->gts, &gpuscan->task);

	gpuscan->pds_src = pds_src;
	gpuscan->pds_dst = pds_dst;

	/* setting up kern_parambuf */
	memcpy(KERN_GPUSCAN_PARAMBUF(&gpuscan->kern),
		   gss->gts.kern_params,
		   gss->gts.kern_params->length);
	/* setting up kern_resultbuf */
	kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
    memset(kresults, 0, sizeof(kern_resultbuf));
    kresults->nrels = 1;
	if (gss->dev_quals != NIL)
		kresults->nrooms = kds_src->nitems;
	else
		kresults->all_visible = true;
	gpuscan->kresults = kresults;

	return gpuscan;
}

static GpuTask *
gpuscan_next_chunk(GpuTaskState *gts)
{
	pgstrom_gpuscan	   *gpuscan = NULL;
	GpuScanState	   *gss = (GpuScanState *) gts;
	Relation			rel = gss->gts.css.ss.ss_currentRelation;
	TupleDesc			tupdesc = RelationGetDescr(rel);
	Snapshot			snapshot = gss->gts.css.ss.ps.state->es_snapshot;
	bool				end_of_scan = false;
	pgstrom_data_store *pds;
	struct timeval tv1, tv2;

	/* no more blocks to read */
	if (gss->curr_blknum > gss->last_blknum)
		return NULL;

	PERFMON_BEGIN(&gss->gts.pfm_accum, &tv1);

	while (!gpuscan && !end_of_scan)
	{
		pds = pgstrom_create_data_store_row(gss->gts.gcontext,
											tupdesc,
											pgstrom_chunk_size(),
											false);
		pds->kds->table_oid = RelationGetRelid(rel);

		/*
		 * TODO: We have to stop block insert if and when device projection
		 * will increase the buffer consumption than threshold.
		 */

		/* fill up this data-store */
		while (gss->curr_blknum < gss->last_blknum &&
			   pgstrom_data_store_insert_block(pds, rel,
											   gss->curr_blknum,
											   snapshot, true) >= 0)
			gss->curr_blknum++;

		if (pds->kds->nitems > 0)
			gpuscan = create_pgstrom_gpuscan_task(gss, pds);
		else
		{
			pgstrom_release_data_store(pds);

			/* NOTE: In case when it scans on a large hole (that is
			 * continuous blocks contain invisible tuples only; may
			 * be created by DELETE with relaxed condition),
			 * pgstrom_data_store_insert_block() may return negative
			 * value without valid tuples, even though we don't reach
			 * either end of relation or chunk.
			 * So, we need to check whether we actually touched on
			 * the end-of-relation. If not, retry scanning.
			 *
			 * XXX - Is the above behavior still right?
			 */
			if (gss->curr_blknum >= gss->last_blknum)
				end_of_scan = true;
		}
	}
	PERFMON_END(&gss->gts.pfm_accum, time_outer_load, &tv1, &tv2);

	return &gpuscan->task;
}

static TupleTableSlot *
gpuscan_next_tuple(GpuTaskState *gts)
{
	GpuScanState	   *gss = (GpuScanState *) gts;
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *) gts->curr_task;
	pgstrom_data_store *pds_dst = gpuscan->pds_dst;
	TupleTableSlot	   *slot = NULL;
	struct timeval		tv1, tv2;

	PERFMON_BEGIN(&gss->gts.pfm_accum, &tv1);
	if (gss->gts.curr_index < pds_dst->kds->nitems)
	{
		cl_uint		index = gss->gts.curr_index++;

		slot = gss->gts.css.ss.ss_ScanTupleSlot;
		if (!pgstrom_fetch_data_store(slot, pds_dst, index,
									  &gss->scan_tuple))
			elog(ERROR, "failed to fetch a record from pds");
	}

#if 0
	/*
	 * XXX - old implementation. If CpuReCheck, we run dev_quals on the
	 * pds_src first, then make projection towards custom_scan_tlist by
	 * CPU.
	 */
	while (gss->gts.curr_index < kds_dst->nitems)
	{
		slot = gss->gts.css.ss.ss_ScanTupleSlot;
		if (!pgstrom_fetch_data_store(slot, pds, gss->gts.curr_index,
									  &gss->scan_tuple))
			elog(ERROR, "failed to fetch a record from pds: %d", i_result);
		Assert(slot->tts_tuple == &gss->scan_tuple);

		if (do_recheck)
		{
			ExprContext *econtext = gss->gts.css.ss.ps.ps_ExprContext;

			Assert(gss->dev_quals != NULL);
			econtext->ecxt_scantuple = slot;
			if (!ExecQual(gss->dev_quals, econtext, false))
			{
				slot = NULL;
				continue;
			}
		}
		break;
	}
#endif
	PERFMON_END(&gss->gts.pfm_accum, time_materialize, &tv1, &tv2);

	return slot;
}

static TupleTableSlot *
gpuscan_exec(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) pgstrom_exec_gputask,
					(ExecScanRecheckMtd) pgstrom_recheck_gputask);
}

static void *
gpuscan_exec_bulk(CustomScanState *node)
{
	GpuScanState	   *gss = (GpuScanState *) node;
	Relation			rel = node->ss.ss_currentRelation;
	TupleTableSlot	   *slot = node->ss.ss_ScanTupleSlot;
	TupleDesc			tupdesc = slot->tts_tupleDescriptor;
	Snapshot			snapshot = node->ss.ps.state->es_snapshot;
	pgstrom_data_store *pds = NULL;
	struct timeval		tv1, tv2;

	Assert(!gss->gts.kern_source);

	PERFMON_BEGIN(&gss->gts.pfm_accum, &tv1);

	while (gss->curr_blknum < gss->last_blknum)
	{
		pds = pgstrom_create_data_store_row(gss->gts.gcontext,
											tupdesc,
											pgstrom_chunk_size(),
											false);
		/* fill up this data store */
		while (gss->curr_blknum < gss->last_blknum &&
			   pgstrom_data_store_insert_block(pds, rel,
											   gss->curr_blknum,
											   snapshot, true) >= 0)
			gss->curr_blknum++;

		if (pds->kds->nitems > 0)
			break;
		pgstrom_release_data_store(pds);
        pds = NULL;
	}

	PERFMON_END(&gss->gts.pfm_accum, time_outer_load, &tv1, &tv2);

	return pds;
}

static void
gpuscan_end(CustomScanState *node)
{
	GpuScanState	   *gss = (GpuScanState *)node;

	pgstrom_release_gputaskstate(&gss->gts);
}

static void
gpuscan_rescan(CustomScanState *node)
{
	GpuScanState	   *gss = (GpuScanState *) node;

	/* clean-up and release any concurrent tasks */
    pgstrom_cleanup_gputaskstate(&gss->gts);

	/* OK, rewind the position to read */
	gss->curr_blknum = 0;
}

static void
gpuscan_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpuScanState   *gss = (GpuScanState *) node;
	CustomScan	   *cscan = (CustomScan *) gss->gts.css.ss.ps.plan;
	GpuScanInfo	   *gsinfo = deform_gpuscan_info(cscan);
	List		   *context;
	List		   *dev_proj = NIL;
	char		   *temp;
	ListCell	   *lc;

	/* Set up deparsing context */
	context = set_deparse_context_planstate(es->deparse_cxt,
											(Node *)&gss->gts.css.ss.ps,
											ancestors);
	/* Show device projection */
	foreach (lc, cscan->custom_scan_tlist)
		dev_proj = lappend(dev_proj, ((TargetEntry *) lfirst(lc))->expr);
	temp = deparse_expression((Node *)dev_proj, context, es->verbose, false);
	ExplainPropertyText("Device Projection", temp, es);

	/* Show device filter */
	if (gsinfo->dev_quals != NIL)
	{
		Node   *dev_quals = (Node *) make_ands_explicit(gsinfo->dev_quals);

		temp = deparse_expression(dev_quals, context, es->verbose, false);
		ExplainPropertyText("Device Filter", temp, es);
		show_instrumentation_count("Rows Removed by Device Fileter",
								   2, &gss->gts.css.ss.ps, es);
	}
	pgstrom_explain_gputaskstate(&gss->gts, es);
}

void
pgstrom_init_gpuscan(void)
{
	/* enable_gpuscan */
	DefineCustomBoolVariable("pg_strom.enable_gpuscan",
							 "Enables the use of GPU accelerated full-scan",
							 NULL,
							 &enable_gpuscan,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* setup path methods */
	memset(&gpuscan_path_methods, 0, sizeof(gpuscan_path_methods));
	gpuscan_path_methods.CustomName			= "GpuScan";
	gpuscan_path_methods.PlanCustomPath		= create_gpuscan_plan;

	/* setup plan methods */
	memset(&gpuscan_plan_methods, 0, sizeof(gpuscan_plan_methods));
	gpuscan_plan_methods.CustomName			= "GpuScan";
	gpuscan_plan_methods.CreateCustomScanState = gpuscan_create_scan_state;

	memset(&bulkscan_plan_methods, 0, sizeof(bulkscan_plan_methods));
	bulkscan_plan_methods.CustomName		= "BulkScan";
	bulkscan_plan_methods.CreateCustomScanState = gpuscan_create_scan_state;

	/* setup exec methods */
	memset(&gpuscan_exec_methods, 0, sizeof(gpuscan_exec_methods));
	gpuscan_exec_methods.c.CustomName         = "GpuScan";
	gpuscan_exec_methods.c.BeginCustomScan    = gpuscan_begin;
	gpuscan_exec_methods.c.ExecCustomScan     = gpuscan_exec;
	gpuscan_exec_methods.c.EndCustomScan      = gpuscan_end;
	gpuscan_exec_methods.c.ReScanCustomScan   = gpuscan_rescan;
	gpuscan_exec_methods.c.ExplainCustomScan  = gpuscan_explain;
	gpuscan_exec_methods.ExecCustomBulk       = gpuscan_exec_bulk;

	bulkscan_exec_methods.c.CustomName        = "BulkScan";
	bulkscan_exec_methods.c.BeginCustomScan   = gpuscan_begin;
	bulkscan_exec_methods.c.ExecCustomScan    = gpuscan_exec;
	bulkscan_exec_methods.c.EndCustomScan     = gpuscan_end;
	bulkscan_exec_methods.c.ReScanCustomScan  = gpuscan_rescan;
	bulkscan_exec_methods.c.ExplainCustomScan = gpuscan_explain;
	bulkscan_exec_methods.ExecCustomBulk      = gpuscan_exec_bulk;

	/* hook registration */
	set_rel_pathlist_next = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gpuscan_add_scan_path;
}

static void
gpuscan_cleanup_cuda_resources(pgstrom_gpuscan *gpuscan)
{
	CUDA_EVENT_DESTROY(gpuscan,ev_dma_recv_stop);
	CUDA_EVENT_DESTROY(gpuscan,ev_dma_recv_start);
	CUDA_EVENT_DESTROY(gpuscan,ev_dma_send_stop);
	CUDA_EVENT_DESTROY(gpuscan,ev_dma_send_start);

	if (gpuscan->m_gpuscan)
		gpuMemFree(&gpuscan->task, gpuscan->m_gpuscan);

	/* ensure pointers being NULL */
	gpuscan->kern_exec_quals = NULL;
	gpuscan->kern_dev_proj = NULL;
	gpuscan->m_gpuscan = 0UL;
	gpuscan->m_kds_src = 0UL;
	gpuscan->m_kds_dst = 0UL;
}

/*
 * pgstrom_complete_gpuscan
 *
 *
 *
 *
 */
static bool
pgstrom_complete_gpuscan(GpuTask *gtask)
{
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *) gtask;
	GpuTaskState	   *gts = gtask->gts;

	if (gts->pfm_accum.enabled)
	{
		CUDA_EVENT_ELAPSED(gpuscan, time_dma_send,
						   ev_dma_send_start,
						   ev_dma_send_stop);
		CUDA_EVENT_ELAPSED(gpuscan, time_kern_qual,
						   ev_dma_send_stop,
						   ev_dma_recv_start);
		CUDA_EVENT_ELAPSED(gpuscan, time_dma_recv,
						   ev_dma_recv_start,
						   ev_dma_recv_stop);
		pgstrom_accum_perfmon(&gts->pfm_accum, &gpuscan->task.pfm);
	}
	gpuscan_cleanup_cuda_resources(gpuscan);

	return true;
}

static void
pgstrom_respond_gpuscan(CUstream stream, CUresult status, void *private)
{
	pgstrom_gpuscan	   *gpuscan = private;
	GpuTaskState	   *gts = gpuscan->task.gts;

	/*
	 * NOTE: We need to pay careful attention for invocation timing of
	 * the callback registered via cuStreamAddCallback(). This routine
	 * shall be called on the non-master thread which is managed by CUDA
	 * runtime, so here is no guarantee resources are available.
	 * Once a transaction gets aborted, PostgreSQL backend takes a long-
	 * junk to the point where sigsetjmp(), then releases resources that
	 * is allocated for each transaction.
	 * Per-query memory context (estate->es_query_cxt) shall be released
	 * during AbortTransaction(), then CUDA context shall be also destroyed
	 * on the ResourceReleaseCallback().
	 * It means, this respond callback may be kicked, by CUDA runtime,
	 * concurrently, however, either/both of GpuTaskState or/and CUDA context
	 * may be already gone.
	 * So, prior to touch these resources, we need to ensure the resources
	 * are still valid.
	 *
	 * FIXME: Once IsTransactionState() returned 'true', transaction may be
	 * aborted during the rest of tasks. We need more investigation to
	 * ensure GpuTaskState is not released here...
	 *
	 * If CUDA runtime gives CUDA_ERROR_INVALID_CONTEXT, it implies CUDA
	 * context is already released. So, we should bail-out immediately.
	 * Also, once transaction state gets turned off from TRANS_INPROGRESS,
	 * it implies per-query memory context will be released very soon.
	 * So, we also need to bail-out immediately.
	 */
	if (status == CUDA_ERROR_INVALID_CONTEXT || !IsTransactionState())
		return;

	/* OK, routine is called back in the usual context */
	if (status == CUDA_SUCCESS)
		gpuscan->task.kerror = gpuscan->kern.kerror;
	else
	{
		gpuscan->task.kerror.errcode = status;
		gpuscan->task.kerror.kernel = StromKernel_CudaRuntime;
		gpuscan->task.kerror.lineno = 0;
	}

	/*
	 * Remove the GpuTask from the running_tasks list, and attach it
	 * on the completed_tasks list again. Note that this routine may
	 * be called by CUDA runtime, prior to attachment of GpuTask on
	 * the running_tasks by cuda_control.c.
	 */
	SpinLockAcquire(&gts->lock);
	if (gpuscan->task.chain.prev && gpuscan->task.chain.next)
	{
		dlist_delete(&gpuscan->task.chain);
		gts->num_running_tasks--;
	}
	if (gpuscan->task.kerror.errcode == StromError_Success)
		dlist_push_tail(&gts->completed_tasks, &gpuscan->task.chain);
	else
		dlist_push_head(&gts->completed_tasks, &gpuscan->task.chain);
	gts->num_completed_tasks++;
	SpinLockRelease(&gts->lock);

	SetLatch(&MyProc->procLatch);
}

static bool
__pgstrom_process_gpuscan(pgstrom_gpuscan *gpuscan)
{
	GpuScanState	   *gss = (GpuScanState *) gpuscan->task.gts;
	kern_resultbuf	   *kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
	pgstrom_data_store *pds_src = gpuscan->pds_src;
	pgstrom_data_store *pds_dst = gpuscan->pds_dst;
	cl_uint				src_nitems = pds_src->kds->nitems;
	void			   *kern_args[5];
	size_t				offset;
	size_t				length;
	size_t				grid_size;
	size_t				block_size;
	CUresult			rc;

	/*
	 * GPU kernel function lookup
	 */
	rc = cuModuleGetFunction(&gpuscan->kern_exec_quals,
							 gpuscan->task.cuda_module,
							 "gpuscan_exec_quals");
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuModuleGetFunction: %s", errorText(rc));

	rc = cuModuleGetFunction(&gpuscan->kern_dev_proj,
							 gpuscan->task.cuda_module,
							 gss->gts.be_row_format
							 ? "gpuscan_projection_row"
							 : "gpuscan_projection_slot");
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuModuleGetFunction: %s", errorText(rc));

	/*
	 * Allocation of device memory
	 */
	length = (GPUMEMALIGN(KERN_GPUSCAN_LENGTH(&gpuscan->kern)) +
			  GPUMEMALIGN(KERN_DATA_STORE_LENGTH(pds_src->kds)));
	if (pds_dst)
		length += GPUMEMALIGN(KERN_DATA_STORE_LENGTH(pds_dst->kds));

	gpuscan->m_gpuscan = gpuMemAlloc(&gpuscan->task, length);
	if (!gpuscan->m_gpuscan)
		goto out_of_resource;

	gpuscan->m_kds_src = gpuscan->m_gpuscan +
		GPUMEMALIGN(KERN_GPUSCAN_LENGTH(&gpuscan->kern));

	if (pds_dst)
		gpuscan->m_kds_dst = gpuscan->m_kds_src +
			GPUMEMALIGN(KERN_DATA_STORE_LENGTH(pds_src->kds));
	else
		gpuscan->m_kds_dst = 0UL;

	/*
	 * Creation of event objects, if any
	 */
	if (gpuscan->task.pfm.enabled)
	{
		rc = cuEventCreate(&gpuscan->ev_dma_send_start, CU_EVENT_DEFAULT);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuEventCreate: %s", errorText(rc));

		rc = cuEventCreate(&gpuscan->ev_dma_send_stop, CU_EVENT_DEFAULT);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuEventCreate: %s", errorText(rc));

		rc = cuEventCreate(&gpuscan->ev_dma_recv_start, CU_EVENT_DEFAULT);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuEventCreate: %s", errorText(rc));

		rc = cuEventCreate(&gpuscan->ev_dma_recv_stop, CU_EVENT_DEFAULT);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuEventCreate: %s", errorText(rc));
	}

	/*
	 * OK, enqueue a series of requests
	 */
	CUDA_EVENT_RECORD(gpuscan, ev_dma_send_start);

	offset = KERN_GPUSCAN_DMASEND_OFFSET(&gpuscan->kern);
	length = KERN_GPUSCAN_DMASEND_LENGTH(&gpuscan->kern);
	rc = cuMemcpyHtoDAsync(gpuscan->m_gpuscan,
						   (char *)&gpuscan->kern + offset,
						   length,
						   gpuscan->task.cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
	gpuscan->task.pfm.bytes_dma_send += length;
	gpuscan->task.pfm.num_dma_send++;

	/* kern_data_store *kds_src */
	length = KERN_DATA_STORE_LENGTH(pds_src->kds);
	rc = cuMemcpyHtoDAsync(gpuscan->m_kds_src,
						   pds_src->kds,
						   length,
						   gpuscan->task.cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
	gpuscan->task.pfm.bytes_dma_send += length;
	gpuscan->task.pfm.num_dma_send++;

	/* kern_data_store *kds_dst, if any */
	if (pds_dst)
	{
		length = KERN_DATA_STORE_HEAD_LENGTH(pds_dst->kds);
		rc = cuMemcpyHtoDAsync(gpuscan->m_kds_dst,
							   pds_dst->kds,
							   length,
							   gpuscan->task.cuda_stream);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
		gpuscan->task.pfm.bytes_dma_send += length;
		gpuscan->task.pfm.num_dma_send++;
	}
	CUDA_EVENT_RECORD(gpuscan, ev_dma_send_stop);

	/*
	 * Launch kernel function
	 */
	if (gss->dev_quals != NIL)
	{
		pgstrom_compute_workgroup_size(&grid_size,
									   &block_size,
									   gpuscan->kern_exec_quals,
									   gpuscan->task.cuda_device,
									   false,
									   src_nitems,
									   sizeof(kern_errorbuf));
		kern_args[0] = &gpuscan->m_gpuscan;
		kern_args[1] = &gpuscan->m_kds_src;

		rc = cuLaunchKernel(gpuscan->kern_exec_quals,
							grid_size, 1, 1,
							block_size, 1, 1,
							sizeof(kern_errorbuf) * block_size,
							gpuscan->task.cuda_stream,
							kern_args,
							NULL);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuLaunchKernel: %s", errorText(rc));
		gpuscan->task.pfm.num_kern_qual++;
	}
	else
	{
		/* no device qualifiers, thus, all rows are visible to projection */
		Assert(kresults->all_visible);
	}

	if (pds_dst != NULL)
	{
		pgstrom_compute_workgroup_size(&grid_size,
									   &block_size,
									   gpuscan->kern_dev_proj, 
									   gpuscan->task.cuda_device,
									   false,
									   src_nitems,
									   sizeof(kern_errorbuf));
		kern_args[0] = &gpuscan->m_gpuscan;
		kern_args[1] = &gpuscan->m_kds_src;
		kern_args[2] = &gpuscan->m_kds_dst;

		rc = cuLaunchKernel(gpuscan->kern_dev_proj,
							grid_size, 1, 1,
							block_size, 1, 1,
							sizeof(kern_errorbuf) * block_size,
							gpuscan->task.cuda_stream,
							kern_args,
							NULL);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuLaunchKernel: %s", errorText(rc));
	}

	/*
	 * Recv DMA call
	 */
	CUDA_EVENT_RECORD(gpuscan, ev_dma_recv_start);

	offset = KERN_GPUSCAN_DMARECV_OFFSET(&gpuscan->kern);
	length = KERN_GPUSCAN_DMARECV_LENGTH(&gpuscan->kern,
										 pds_dst ? 0 : pds_src->kds->nitems);
	rc = cuMemcpyDtoHAsync((char *)&gpuscan->kern + offset,
						   gpuscan->m_gpuscan + offset,
						   length,
						   gpuscan->task.cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "cuMemcpyDtoHAsync: %s", errorText(rc));
	gpuscan->task.pfm.bytes_dma_recv += length;
	gpuscan->task.pfm.num_dma_recv++;

	if (pds_dst)
	{
		length = KERN_DATA_STORE_LENGTH(pds_dst->kds);
		rc = cuMemcpyDtoHAsync(pds_dst->kds,
							   gpuscan->m_kds_dst,
							   length,
							   gpuscan->task.cuda_stream);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "cuMemcpyDtoHAsync: %s", errorText(rc));
		gpuscan->task.pfm.bytes_dma_recv += length;
		gpuscan->task.pfm.num_dma_recv++;
	}
	CUDA_EVENT_RECORD(gpuscan, ev_dma_recv_stop);

	/*
	 * Register callback
	 */
	rc = cuStreamAddCallback(gpuscan->task.cuda_stream,
							 pgstrom_respond_gpuscan,
							 gpuscan, 0);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "cuStreamAddCallback: %s", errorText(rc));

	return true;

out_of_resource:
	gpuscan_cleanup_cuda_resources(gpuscan);
	return false;
}

/*
 * clserv_process_gpuscan
 *
 * entrypoint of kernel gpuscan implementation
 */
static bool
pgstrom_process_gpuscan(GpuTask *task)
{
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *) task;
	bool				status;
	CUresult			rc;

	/* Switch CUDA Context */
	rc = cuCtxPushCurrent(gpuscan->task.cuda_context);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuCtxPushCurrent: %s", errorText(rc));

	PG_TRY();
	{
		status = __pgstrom_process_gpuscan(gpuscan);
	}
	PG_CATCH();
	{
		gpuscan_cleanup_cuda_resources(gpuscan);
		rc = cuCtxPopCurrent(NULL);
		if (rc != CUDA_SUCCESS)
			elog(WARNING, "failed on cuCtxPopCurrent: %s", errorText(rc));
		PG_RE_THROW();
	}
	PG_END_TRY();

	rc = cuCtxPopCurrent(NULL);
	if (rc != CUDA_SUCCESS)
		elog(WARNING, "failed on cuCtxPopCurrent: %s", errorText(rc));

	return status;
}
