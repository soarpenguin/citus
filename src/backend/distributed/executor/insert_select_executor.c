/*-------------------------------------------------------------------------
 *
 * insert_select_executor.c
 *
 * Executor logic for INSERT..SELECT.
 *
 * Copyright (c) 2017, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "distributed/insert_select_executor.h"
#include "distributed/insert_select_planner.h"
#include "distributed/multi_copy.h"
#include "distributed/multi_executor.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_router_executor.h"
#include "distributed/distributed_planner.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_management.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"


static HTAB * ExecuteSelectIntoRelation(Oid targetRelationId, List *insertTargetList,
										Query *selectQuery, EState *executorState,
										char *intermediateResultIdPrefix);


/*
 * CoordinatorInsertSelectExecScan executes an INSERT INTO distributed_table
 * SELECT .. query by setting up a DestReceiver that copies tuples into the
 * distributed table and then executing the SELECT query using that DestReceiver
 * as the tuple destination.
 */
TupleTableSlot *
CoordinatorInsertSelectExecScan(CustomScanState *node)
{
	CitusScanState *scanState = (CitusScanState *) node;
	TupleTableSlot *resultSlot = NULL;

	if (!scanState->finishedRemoteScan)
	{
		EState *executorState = scanState->customScanState.ss.ps.state;
		DistributedPlan *distributedPlan = scanState->distributedPlan;
		Query *selectQuery = distributedPlan->insertSelectSubquery;
		List *insertTargetList = distributedPlan->insertTargetList;
		Oid targetRelationId = distributedPlan->targetRelationId;
		char *intermediateResultIdPrefix = distributedPlan->intermediateResultIdPrefix;
		HTAB *shardConnectionsHash = NULL;

		ereport(DEBUG1, (errmsg("Collecting INSERT ... SELECT results on coordinator")));

		/*
		 * If we are dealing with partitioned table, we also need to lock its
		 * partitions. Here we only lock targetRelation, we acquire necessary
		 * locks on selected tables during execution of those select queries.
		 */
		if (PartitionedTable(targetRelationId))
		{
			LockPartitionRelations(targetRelationId, RowExclusiveLock);
		}

		shardConnectionsHash = ExecuteSelectIntoRelation(targetRelationId,
														 insertTargetList, selectQuery,
														 executorState,
														 intermediateResultIdPrefix);

		if (distributedPlan->workerJob != NULL)
		{
			/*
			 * If we also have a workerJob that means there is a second step
			 * to the INSERT...SELECT. This happens when there is a RETURNING
			 * or ON CONFLICT clause which is implemented as a separate
			 * distributed INSERT...SELECT from a set of intermediate results
			 * to the target relation.
			 */
			Job *workerJob = distributedPlan->workerJob;
			ListCell *taskCell = NULL;
			List *taskList = workerJob->taskList;
			List *prunedTaskList = NIL;
			bool hasReturning = distributedPlan->hasReturning;
			bool isModificationQuery = true;
 			/*
			 * We cannot actually execute INSERT...SELECT tasks that read from
			 * intermediate results that weren't created because no rows were
			 * written to them. Prune those tasks out by only including tasks
			 * on shards with connections.
			 */
			foreach(taskCell, taskList)
			{
				Task *task = (Task *) lfirst(taskCell);
				uint64 shardId = task->anchorShardId;
				bool shardModified = false;
 				hash_search(shardConnectionsHash, &shardId, HASH_FIND, &shardModified);
				if (shardModified)
				{
					prunedTaskList = lappend(prunedTaskList, task);
				}
			}
 			if (prunedTaskList != NIL)
			{
				ExecuteMultipleTasks(scanState, prunedTaskList, isModificationQuery,
									 hasReturning);
			}
		}

		scanState->finishedRemoteScan = true;
	}

	resultSlot = ReturnTupleFromTuplestore(scanState);

	return resultSlot;
}


/*
 * ExecuteSelectIntoRelation executes given SELECT query and inserts the
 * results into the target relation, which is assumed to be a distributed
 * table.
 *
 * If intermediateResultIdPrefix is not NULL, the data is instead written
 * to a set of intermediate results that are co-located with the table
 * for further processing (ON CONFLICT).
 *
 * ExecuteSelectIntoRelation returns the hash of connections that were
 * used to insert into the target relation.
 */
static HTAB *
ExecuteSelectIntoRelation(Oid targetRelationId, List *insertTargetList,
						  Query *selectQuery, EState *executorState,
						  char *intermediateResultIdPrefix)
{
	ParamListInfo paramListInfo = executorState->es_param_list_info;

	ListCell *insertTargetCell = NULL;
	List *columnNameList = NIL;
	bool stopOnFailure = false;
	char partitionMethod = 0;
	Var *partitionColumn = NULL;
	int partitionColumnIndex = -1;

	CitusCopyDestReceiver *copyDest = NULL;
	Query *queryCopy = NULL;

	partitionMethod = PartitionMethod(targetRelationId);
	if (partitionMethod == DISTRIBUTE_BY_NONE)
	{
		stopOnFailure = true;
	}

	partitionColumn = PartitionColumn(targetRelationId, 0);

	/* build the list of column names for the COPY statement */
	foreach(insertTargetCell, insertTargetList)
	{
		TargetEntry *insertTargetEntry = (TargetEntry *) lfirst(insertTargetCell);
		char *columnName = insertTargetEntry->resname;

		/* load the column information from pg_attribute */
		AttrNumber attrNumber = get_attnum(targetRelationId, columnName);

		/* check whether this is the partition column */
		if (partitionColumn != NULL && attrNumber == partitionColumn->varattno)
		{
			Assert(partitionColumnIndex == -1);

			partitionColumnIndex = list_length(columnNameList);
		}

		columnNameList = lappend(columnNameList, insertTargetEntry->resname);
	}

	/* set up a DestReceiver that copies into the distributed table */
	copyDest = CreateCitusCopyDestReceiver(targetRelationId, columnNameList,
										   partitionColumnIndex, executorState,
										   stopOnFailure, intermediateResultIdPrefix);

	/*
	 * Make a copy of the query, since ExecuteQueryIntoDestReceiver may scribble on it
	 * and we want it to be replanned every time if it is stored in a prepared
	 * statement.
	 */
	queryCopy = copyObject(selectQuery);

	ExecuteQueryIntoDestReceiver(queryCopy, paramListInfo, (DestReceiver *) copyDest);

	executorState->es_processed = copyDest->tuplesSent;

	XactModificationLevel = XACT_MODIFICATION_DATA;

	return copyDest->shardConnectionHash;
}
