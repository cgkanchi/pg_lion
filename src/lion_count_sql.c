/*-------------------------------------------------------------------------
 *
 * lion_count_sql.c
 *		The SQL-callable count functions.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

PG_FUNCTION_INFO_V1(lion_index_count);
PG_FUNCTION_INFO_V1(lion_index_count2);
PG_FUNCTION_INFO_V1(lion_index_count_stats);
PG_FUNCTION_INFO_V1(lion_index_count_group_stats);
PG_FUNCTION_INFO_V1(lion_index_count_any);

/* ---------------------------------------------------------------------
 * SQL interface
 * --------------------------------------------------------------------- */

/*
 * Common argument validation for the lion_index_count* functions.
 * Everything is opened here and closed by lion_count_sql_close().
 */
typedef struct LionCountCall
{
	int			nkeys;
	Relation	heap;
	Relation	index[2];
	Datum		key[2];
	Oid			keytype[2];
} LionCountCall;

/*
 * An index's expressions or predicate AS STORED in pg_index, NIL when it has
 * none.  RelationGetIndexExpressions()/RelationGetIndexPredicate() hand back
 * the planner's simplified form, in which an inlinable SQL function has
 * already been replaced by its body - which is right for evaluating it and
 * wrong for asking which functions the query calls.
 */
static List *
lion_index_stored_exprs(Relation index, int attnum)
{
	HeapTuple	tup;
	Datum		d;
	bool		isnull;
	List	   *result = NIL;

	tup = SearchSysCache1(INDEXRELID,
						  ObjectIdGetDatum(RelationGetRelid(index)));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u",
			 RelationGetRelid(index));
	d = SysCacheGetAttr(INDEXRELID, tup, attnum, &isnull);
	if (!isnull)
	{
		void	   *node = stringToNode(TextDatumGetCString(d));

		result = (attnum == Anum_pg_index_indpred) ?
			make_ands_implicit((Expr *) node) : (List *) node;
	}
	ReleaseSysCache(tup);

	return result;
}

/*
 * EXECUTE on a function the query a count stands for would call, asked as
 * ExecInitFunc() asks it: of the current user, failing with core's own
 * "permission denied for function", then the object-access hook core fires
 * for every function it is about to run (DESIGN.md §9, "Privileges").  The
 * pushdown asks at executor startup and the SQL functions when called, never
 * at plan time: a cached plan outlives both a REVOKE and a SET ROLE.
 */
void
lion_check_execute(Oid funcid)
{
	AclResult	aclresult;

	aclresult = object_aclcheck(ProcedureRelationId, funcid, GetUserId(),
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);
}

/*
 * The same for an aggregate, as ExecInitAgg() asks it: EXECUTE on the
 * aggregate for the current user ("permission denied for aggregate"), then
 * EXECUTE on its final and transition functions for the aggregate's OWNER.
 * count()'s are the bootstrap superuser's, so the second half cannot fail for
 * it; it is here so that the node asks what the Agg it replaces asks, hooks
 * included.
 */
void
lion_check_aggregate_execute(Oid aggfnoid)
{
	AclResult	aclresult;
	HeapTuple	tup;
	Oid			transfn;
	Oid			finalfn;
	Oid			owner;

	aclresult = object_aclcheck(ProcedureRelationId, aggfnoid, GetUserId(),
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_AGGREGATE, get_func_name(aggfnoid));
	InvokeFunctionExecuteHook(aggfnoid);

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", aggfnoid);
	transfn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtransfn;
	finalfn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggfinalfn;
	ReleaseSysCache(tup);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", aggfnoid);
	owner = ((Form_pg_proc) GETSTRUCT(tup))->proowner;
	ReleaseSysCache(tup);

	if (OidIsValid(finalfn))
	{
		aclresult = object_aclcheck(ProcedureRelationId, finalfn, owner,
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(finalfn));
		InvokeFunctionExecuteHook(finalfn);
	}
	aclresult = object_aclcheck(ProcedureRelationId, transfn, owner,
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(transfn));
	InvokeFunctionExecuteHook(transfn);
}

/*
 * EXECUTE on every function an index expression or predicate calls
 * (lion_count_open_indexes()): the same question the executor asks of the
 * query the count stands for, in the same walk core uses to find the
 * functions of an expression.
 */
static bool
lion_check_function_acl(Oid funcid, void *context)
{
	lion_check_execute(funcid);
	return false;
}

static bool
lion_check_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* the stored form has not been through the planner's fix_opfuncids() */
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	(void) check_functions_in_node(node, lion_check_function_acl, context);
	return expression_tree_walker(node, lion_check_functions_walker, context);
}

/*
 * The error for a relation named by an OID that has no relation behind it:
 * one dropped since the caller named it, or one that never existed (a
 * regclass argument accepts any number).
 */
static void
lion_count_no_relation(Oid relid)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_TABLE),
			 errmsg("relation with OID %u does not exist", relid)));
}

/*
 * The type a SQL count's key is looked up as: what `col = key` in the query
 * the count stands for would compare it as, or an ERROR where that query
 * would find no operator.  The function's argument is polymorphic, so its
 * type is whatever the caller wrote, and it has to be brought to one of the
 * column's opclass members by the rules the parser would apply:
 *
 *	- a domain is its base type, as it is to operator resolution;
 *	- a class declared on a POLYMORPHIC type (enum_ops, FOR TYPE anyenum)
 *	  takes values of the column's own type and nothing else: its members
 *	  are (anyenum, anyenum), which the parser only lets two values of ONE
 *	  enum meet at.  A different enum would pass for "an enum", and its OIDs
 *	  would be looked up in this column's directory as if they meant
 *	  something there.  The resolved type is the class's own, opcintype.
 *	- otherwise the class's own type, or a type the family has a strategy-1
 *	  member for with it (int8 on an int4 column: int48eq, exactly as in the
 *	  query), which lion_probe_init() resolves further;
 *	- or, lacking such a member, a BINARY coercion to the class's type -
 *	  varchar to text - where the parser makes it with `col = key` too:
 *	  there is no `text = varchar`, so it relabels the key and calls the
 *	  class's own `text = text`.  The bytes are the same, so the key is then
 *	  simply one of the column's own values.  A cast FUNCTION is not taken,
 *	  for §21's reason (implicit does not mean lossless), and neither is a
 *	  coercion the parser would not make (bpchar and text, below).
 *
 * This is the whole of the key type check: until 2026-09-25 it compared the
 * key with rd_opcintype exactly, so enum_ops, a DEFAULT class, could never be
 * counted at all ("the index is on type anyenum" for the column's own enum),
 * and neither could a varchar key on the varchar column it indexes.
 */
static Oid
lion_count_key_type(Relation index, AttrNumber col, Oid keytype)
{
	LionState  *state = lion_index_column_state(index, col);
	Oid			opfamily = index->rd_opfamily[col - 1];
	Oid			opcintype = index->rd_opcintype[col - 1];
	Oid			basetype = getBaseType(keytype);

	if (IsPolymorphicType(opcintype))
	{
		if (lion_type_is_column(state, basetype))
			return opcintype;
	}
	else
	{
		Oid			eqopr;
		Oid			lefttype;
		Oid			righttype;

		if (basetype == opcintype ||
			OidIsValid(get_opfamily_member(opfamily, opcintype, basetype, 1)))
			return basetype;

		/*
		 * The binary coercion only where `col = key` makes it: the operator
		 * the parser picks for (the column's type, the key's type) has to be
		 * the class's own (opcintype, opcintype), reached without a cast
		 * function (compatible_oper()).  That a coercion EXISTS is not
		 * enough.  text is binary-coercible to bpchar, but `bpcharcol =
		 * 'x '::text` resolves to `text = text` - text is the preferred type
		 * of its category - and casts the COLUMN with rtrim1(), so it matches
		 * no row that bpchar's own equality, which ignores trailing blanks,
		 * would count.
		 */
		eqopr = compatible_oper_opid(list_make1(makeString(pstrdup("="))),
									 state->typid, basetype, true);
		if (OidIsValid(eqopr))
		{
			op_input_types(eqopr, &lefttype, &righttype);
			if (lefttype == opcintype && righttype == opcintype)
				return opcintype;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("type %s cannot be compared with index \"%s\"",
					format_type_be(keytype),
					RelationGetRelationName(index)),
			 errdetail("The index is on type %s.",
					   format_type_be(lion_column_type(state, opcintype)))));
	return InvalidOid;			/* keep the compiler quiet */
}

/*
 * The collation a SQL count's argument argnum brings to `col = key`: that of
 * its expression in the call, or - called with none, from C - the call's own.
 * The two-key form has one call collation for both keys, where the query it
 * stands for compares each under its own; so each is taken from its own
 * argument.
 */
static Oid
lion_count_arg_collation(FunctionCallInfo fcinfo, int argnum)
{
	Node	   *expr = (fcinfo->flinfo != NULL) ? fcinfo->flinfo->fn_expr : NULL;

	if (expr != NULL && IsA(expr, FuncExpr))
	{
		List	   *args = ((FuncExpr *) expr)->args;

		if (argnum < list_length(args))
			return exprCollation((Node *) list_nth(args, argnum));
	}
	return PG_GET_COLLATION();
}

/*
 * The collation key column col's own values bring to `col = key`: the table
 * column's, or its expression's when the column is an expression.  The
 * index's may be another - `(c COLLATE "x")` keeps c, and compares under x.
 */
static Oid
lion_count_column_collation(Relation heap, Relation index, AttrNumber col)
{
	AttrNumber	attnum = index->rd_index->indkey.values[col - 1];
	List	   *exprs;
	int			nth = 0;
	int			c;

	if (attnum != 0)
		return TupleDescAttr(RelationGetDescr(heap), attnum - 1)->attcollation;

	for (c = 0; c < col - 1; c++)
	{
		if (index->rd_index->indkey.values[c] == 0)
			nth++;
	}
	exprs = lion_index_stored_exprs(index, Anum_pg_index_indexprs);
	if (nth >= list_length(exprs))
		elog(ERROR, "index \"%s\" has too few expressions",
			 RelationGetRelationName(index));
	return exprCollation((Node *) list_nth(exprs, nth));
}

/*
 * Would the index answer `col = key` - or, with no key, `GROUP BY col` - as
 * the query does, collation and all?  The index hashed and compared its keys
 * under its own collation; the query compares under the key's, when the key
 * brings one of its own (an explicit COLLATE, or a column of another
 * collation), and otherwise under the column's: a literal or a parameter
 * brings the default collation, which gives way to the column's in the
 * parser's rule for an operator's inputs.  Two deterministic collations agree
 * on which values are equal - each calls them equal when their bytes are -
 * and a nondeterministic one agrees with no other: `c COLLATE
 * case_insensitive = 'abc'` counts 'ABC' and `c = 'abc'` does not.  So a
 * count under another collation than the index's is refused when either is
 * nondeterministic, and made otherwise.
 *
 * A key of the default collation is the one the call cannot read: an explicit
 * COLLATE "default" does not give way to the column's, and the planner folds
 * it into the literal or the parameter, where nothing is left to tell it
 * from a key that brings no collation of its own.  Both readings are taken:
 * the count is made only when both would make it.  They part only on a
 * column of a nondeterministic collation, which one reading compares under
 * and the other does not, and there the call must name the collation.  Taken
 * for no COLLATE at all, `lion_index_count('i', 'abc'::text COLLATE
 * "default")` counted 'ABC' on a case-insensitive column, where the query
 * under "default" does not (2026-09-29 review).
 */
static void
lion_count_check_collation(Relation heap, Relation index, AttrNumber col,
						   Oid keycoll)
{
	Oid			idxcoll = index->rd_indcollation[col - 1];
	Oid			collation = keycoll;

	if (!OidIsValid(idxcoll))
		return;					/* the key type is not collatable */
	if (!OidIsValid(collation) || collation == DEFAULT_COLLATION_OID)
		collation = lion_count_column_collation(heap, index, col);
	if (!OidIsValid(collation))
		return;
	if (collation == idxcoll)
	{
		if (keycoll == DEFAULT_COLLATION_OID &&
			collation != DEFAULT_COLLATION_OID &&	/* always deterministic */
			!get_collation_isdeterministic(collation))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_COLLATION),
					 errmsg("could not determine which collation to use for a count through index \"%s\"",
							RelationGetRelationName(index)),
					 errdetail("The key is of the default collation, which stands for the column's nondeterministic collation \"%s\" without a COLLATE clause and for \"default\" with COLLATE \"default\"; the two do not agree on which values are equal, and the call cannot tell them apart.",
							   get_collation_name(collation)),
					 errhint("Give the key the column's collation, as in COLLATE \"%s\", or count with the query itself.",
							 get_collation_name(collation))));
		return;
	}
	if (get_collation_isdeterministic(collation) &&
		get_collation_isdeterministic(idxcoll))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_COLLATION_MISMATCH),
			 errmsg("cannot count through index \"%s\" under collation \"%s\"",
					RelationGetRelationName(index),
					get_collation_name(collation)),
			 errdetail("Key column %d of the index is under collation \"%s\", and a nondeterministic collation does not agree with any other on which values are equal.",
					   col, get_collation_name(idxcoll)),
			 errhint("Count with the query itself, or through an index built under the collation it compares with.")));
}

/*
 * Open and vet the indexes of one SQL count: relkind, access method, key
 * type, collation, privileges, row-level security and snapshot eligibility.
 * keytype and keycoll may be NULL, which means the caller has no search key
 * at all (the grouped form below, which walks every entry instead of looking
 * one up); keycoll[i] is the collation key i brings
 * (lion_count_arg_collation()).
 */
static void
lion_count_open_indexes(Snapshot snapshot, int nidx, const Oid *idxoid,
					   const Oid *keytype, const Oid *keycoll,
					   AttrNumber wantcol, LionCountCall *call)
{
	Oid			heapoid = InvalidOid;
	char	   *heapname;
	int			i;

	/* index[] and keytype[] hold two; every caller opens one or two */
	Assert(nidx >= 1 && nidx <= (int) lengthof(call->index));

	call->nkeys = nidx;
	call->heap = NULL;
	for (i = 0; i < 2; i++)
	{
		call->index[i] = NULL;
		call->keytype[i] = InvalidOid;
	}

	/*
	 * Nothing is locked yet, so every catalog answer below may be about a
	 * relation that is being dropped: a relation that is gone is reported by
	 * its OID, never as "(null)" or as a failed cache lookup.
	 */
	for (i = 0; i < nidx; i++)
	{
		char	   *idxname = get_rel_name(idxoid[i]);
		char		relkind = get_rel_relkind(idxoid[i]);
		Oid			hoid;

		if (keytype != NULL)
			call->keytype[i] = keytype[i];

		if (idxname == NULL || relkind == '\0')
			lion_count_no_relation(idxoid[i]);
		if (relkind != RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index", idxname)));

		hoid = IndexGetRelation(idxoid[i], true);
		if (!OidIsValid(hoid))
			lion_count_no_relation(idxoid[i]);
		if (i == 0)
			heapoid = hoid;
		else if (hoid != heapoid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("indexes \"%s\" and \"%s\" are not on the same table",
							get_rel_name(idxoid[0]), idxname)));
	}

	/*
	 * The cheap half of the privilege check, BEFORE any lock (2026-09-23
	 * review): the caller must hold SELECT on the table or on at least one of
	 * its columns, which is the least any count through any of its indexes
	 * needs.  Without it a role with no privilege at all could take - or
	 * queue for - a lock on any table that has a lion index, and hold up
	 * everything that queues behind it.  The exact check, which reads the
	 * index definition, follows once the locks are held, because only then
	 * can that definition be trusted.  A table dropped meanwhile makes
	 * pg_class_aclcheck() raise "does not exist".
	 */
	heapname = get_rel_name(heapoid);
	if (heapname == NULL)
		lion_count_no_relation(heapoid);
	if (pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		pg_attribute_aclcheck_all(heapoid, GetUserId(), ACL_SELECT,
								  ACLMASK_ANY) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(get_rel_relkind(heapoid)),
					   heapname);

	call->heap = try_table_open(heapoid, AccessShareLock);
	if (call->heap == NULL)
		lion_count_no_relation(heapoid);
	lion_check_table_am(call->heap);	/* the visibility map is the heap's */

	for (i = 0; i < nidx; i++)
	{
		Relation	index = try_index_open(idxoid[i], AccessShareLock);

		if (index == NULL)
			lion_count_no_relation(idxoid[i]);
		call->index[i] = index;

		if (index->rd_rel->relam != lion_get_am_oid())
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a lion index",
							RelationGetRelationName(index))));
		/*
		 * wantcol = 0 means the caller names an index and a key but no COLUMN,
		 * so a multicolumn index (DESIGN.md §24) has nothing to tell it which
		 * key set is meant.  The planner's pushdown has the column from the
		 * clause and is not restricted this way.
		 */
		if (wantcol == 0)
		{
			if (IndexRelationGetNumberOfKeyAttributes(index) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lion index \"%s\" has %d key columns, and this function needs exactly one",
								RelationGetRelationName(index),
								IndexRelationGetNumberOfKeyAttributes(index))));
		}
		else if (wantcol < 1 ||
				 wantcol > IndexRelationGetNumberOfKeyAttributes(index))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lion index \"%s\" has no key column %d",
							RelationGetRelationName(index), wantcol)));

		/*
		 * The key's type, resolved the way `col = key` would resolve it
		 * (lion_count_key_type()); what lion_probe_init() is handed from here
		 * on, and what the EXECUTE check below names the equality by.
		 * Raising the errors here keeps them out of the middle of the count.
		 */
		if (OidIsValid(call->keytype[i]))
			call->keytype[i] = lion_count_key_type(index,
												   (wantcol == 0) ? 1 : wantcol,
												   call->keytype[i]);

		/*
		 * A multi-key opclass (DESIGN.md §17) stores one entry per extracted
		 * key, so its entries are not column values: a search key of the
		 * column's own type - a whole tsvector - is not what any entry holds,
		 * and hashing and comparing it as if it were answered a meaningless
		 * count; and a row appears under several entries, so the sum over
		 * them is not a row count and no single entry is a group either.  The
		 * keyed functions and the grouped one refuse such a column alike;
		 * the LionCount planner refuses to drive a GROUP BY from one for the same
		 * reason, and answers `@>` or `@@` through lion_extract_query().
		 */
		{
			AttrNumber	col = (wantcol == 0) ? 1 : wantcol;

			if (lion_index_column_state(index, col)->multikey)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("key column %d of index \"%s\" has a multi-key operator class, whose entries are not column values",
								col, RelationGetRelationName(index))));

			lion_count_check_collation(call->heap, index, col,
									   (keycoll != NULL) ? keycoll[i] :
									   InvalidOid);
		}
	}

	/*
	 * Privileges: exactly what the equivalent query needs.  SELECT count(*)
	 * FROM t WHERE col = key references col, so SELECT on the table or on
	 * every column the index reads is required; with less than that the
	 * count would let a caller probe values it is not allowed to read.
	 *
	 * "Every column the index reads" is more than its key columns.  The count
	 * of a PARTIAL index is the count of the rows that satisfy its predicate,
	 * so the query it stands for is `... WHERE pred AND col = key` and reads
	 * the predicate's columns too - with SELECT(id) alone, a partial index
	 * `(id) WHERE secret` answers 1 or 0 and so reveals `secret` a row at a
	 * time (2026-09-23 review).  An expression column reads whatever its
	 * expression does, and a whole-row reference reads every column, which
	 * only a table-level grant covers.  An index that reads NO column at all
	 * - one on a constant - stands for `SELECT count(*) FROM t`, which needs
	 * SELECT on the table or on at least one of its columns, and the same
	 * rule applies here.
	 *
	 * And the query CALLS every function in the index's expressions and
	 * predicate, which needs EXECUTE on each of them whatever the table
	 * grants say; so does the count, or it would let a caller probe the
	 * results of a function it may not run.
	 */
	{
		bool		tablesel = pg_class_aclcheck(heapoid, GetUserId(),
												 ACL_SELECT) == ACLCHECK_OK;

		for (i = 0; i < nidx; i++)
		{
			Relation	index = call->index[i];
			List	   *exprs = lion_index_stored_exprs(index, Anum_pg_index_indexprs);
			List	   *pred = lion_index_stored_exprs(index, Anum_pg_index_indpred);
			Bitmapset  *cols = NULL;
			int			c;
			int			m;

			(void) lion_check_functions_walker((Node *) exprs, NULL);
			(void) lion_check_functions_walker((Node *) pred, NULL);

			if (tablesel)
				continue;

			/* EVERY key column is referenced, not just the first (§24). */
			for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
			{
				AttrNumber	attnum = index->rd_index->indkey.values[c];

				if (attnum != 0)
					cols = bms_add_member(cols,
										  attnum - FirstLowInvalidHeapAttributeNumber);
			}
			pull_varattnos((Node *) exprs, 1, &cols);
			pull_varattnos((Node *) pred, 1, &cols);

			if (bms_is_empty(cols))
			{
				if (pg_attribute_aclcheck_all(heapoid, GetUserId(), ACL_SELECT,
											  ACLMASK_ANY) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV,
								   get_relkind_objtype(call->heap->rd_rel->relkind),
								   RelationGetRelationName(call->heap));
				continue;
			}

			m = -1;
			while ((m = bms_next_member(cols, m)) >= 0)
			{
				AttrNumber	attnum = m + FirstLowInvalidHeapAttributeNumber;

				if (attnum == InvalidAttrNumber ||
					pg_attribute_aclcheck(heapoid, attnum, GetUserId(),
										  ACL_SELECT) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV,
								   get_relkind_objtype(call->heap->rd_rel->relkind),
								   RelationGetRelationName(call->heap));
			}
		}
	}

	/*
	 * A materialized view created WITH NO DATA has an empty heap and empty
	 * indexes, so every count through them was 0 - where the query the count
	 * stands for refuses to run at all (2026-09-25 review).  Refuse as
	 * ExecOpenScanRelation() does, and where the executor does: after the
	 * range table's privileges, before the scan's quals and the aggregate are
	 * initialised and their functions checked.  The pushdown node makes the
	 * same check (lion_begin_custom_scan()).
	 */
	if (!RelationIsScannable(call->heap))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view \"%s\" has not been populated",
						RelationGetRelationName(call->heap)),
				 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

	/*
	 * The query also CALLS count() and the equality the key is looked up
	 * with, so the count asks for EXECUTE on both, as the executor would of
	 * that query (2026-09-23 review).  `col = key` is strategy 1 of the key
	 * column's opfamily for (opcintype, the key's type as
	 * lion_count_key_type() resolved it) - int48eq for an int8 key on an int4
	 * column, texteq for a varchar key on a text_ops column (the parser
	 * relabels it), enum_eq for the column's own enum, exactly as in the
	 * query - and `col = ANY (keys)` calls the same function per element.
	 * The resolved type is also what the lookup is made as, so the function
	 * checked here is the one whose meaning the count reproduces.  The
	 * grouped form stands for
	 * `SELECT col, count(*) ... GROUP BY col`, whose Agg compares groups with
	 * the type's equality; the pushdown only groups by an index whose
	 * strategy 1 IS that equality (DESIGN.md §10), so strategy 1 for
	 * (opcintype, opcintype) is the function there.
	 */
	for (i = 0; i < nidx; i++)
	{
		Relation	index = call->index[i];
		AttrNumber	col = (wantcol == 0) ? 1 : wantcol;
		Oid			opfamily = index->rd_opfamily[col - 1];
		Oid			opcintype = index->rd_opcintype[col - 1];
		Oid			eqop = InvalidOid;

		if (OidIsValid(call->keytype[i]))
			eqop = get_opfamily_member(opfamily, opcintype,
									   call->keytype[i], 1);
		if (!OidIsValid(eqop))
			eqop = get_opfamily_member(opfamily, opcintype, opcintype, 1);
		if (!OidIsValid(eqop))
			elog(ERROR, "missing equality operator for type %u in opfamily %u",
				 opcintype, opfamily);
		lion_check_execute(get_opcode(eqop));
	}
	lion_check_aggregate_execute(F_COUNT_);

	/*
	 * Row-level security: the policies would have to be evaluated per row,
	 * and the whole point of this count is not to look at rows.  Refuse.
	 * The same query through the planner still works: the pushdown declines
	 * relations with security quals and the ordinary plan applies them.
	 */
	if (check_enable_rls(heapoid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot count through index \"%s\" because row-level security is enabled on table \"%s\"",
						RelationGetRelationName(call->index[0]),
						RelationGetRelationName(call->heap))));

	/*
	 * Snapshot eligibility.  The planner decides this for a query that
	 * mentions the table; a direct SQL count was handed an index nobody
	 * vetted, so it asks the same question itself - once the snapshot is
	 * known, which is why this is the last thing lion_count_sql() does before
	 * the lookup.  An index that indcheckxmin makes unusable does not contain
	 * the HOT-chain versions an old snapshot still sees, and rechecking
	 * cannot invent a TID that is not in the posting set (DESIGN.md §9).
	 */
	for (i = 0; i < nidx; i++)
	{
		const char *why;

		if (!lion_index_usable(call->index[i], snapshot, &why))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot count through index \"%s\" because %s",
							RelationGetRelationName(call->index[i]), why)));

		/*
		 * And none at all under PostgreSQL 16's old_snapshot_threshold, which
		 * the planner's pushdown declines for the same reason (DESIGN.md §9).
		 */
		lion_check_old_snapshot(call->index[i], snapshot);
	}
}

/*
 * The same for the lion_index_count(idx, key [, idx2, key2]) functions,
 * whose arguments alternate index and key.
 */
static void
lion_count_sql_open(FunctionCallInfo fcinfo, int nkeys, Snapshot snapshot,
				   LionCountCall *call)
{
	Oid			idxoid[2];
	Oid			keytype[2];
	Oid			keycoll[2];
	Datum		key[2];
	int			i;

	Assert(nkeys >= 1 && nkeys <= 2);

	for (i = 0; i < nkeys; i++)
	{
		idxoid[i] = PG_GETARG_OID(2 * i);
		key[i] = PG_GETARG_DATUM(2 * i + 1);
		keytype[i] = get_fn_expr_argtype(fcinfo->flinfo, 2 * i + 1);
		if (!OidIsValid(keytype[i]))
			elog(ERROR, "could not determine the type of the search key");
		keycoll[i] = lion_count_arg_collation(fcinfo, 2 * i + 1);
	}

	lion_count_open_indexes(snapshot, nkeys, idxoid, keytype, keycoll, 0,
						   call);

	for (i = 0; i < nkeys; i++)
		call->key[i] = key[i];
}

static void
lion_count_sql_close(LionCountCall *call)
{
	int			i;

	for (i = 0; i < call->nkeys; i++)
	{
		if (call->index[i] != NULL)
			index_close(call->index[i], AccessShareLock);
	}
	if (call->heap != NULL)
		table_close(call->heap, AccessShareLock);
}

static int64
lion_count_sql(FunctionCallInfo fcinfo, int nkeys, LionCountStats *stats)
{
	LionCountCall call;
	Snapshot	snapshot;
	int64		result;
	int			i;

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	lion_count_sql_open(fcinfo, nkeys, snapshot, &call);

	/*
	 * index_beginscan() takes a relation-level predicate lock on an index
	 * whose AM has no ampredlocks (indexam.c).  We read the index without a
	 * scan, so take the same lock ourselves - before looking, so that an
	 * absent key is covered too: otherwise two SERIALIZABLE transactions could
	 * each count an absent key, insert it, and both commit.
	 */
	for (i = 0; i < nkeys; i++)
		PredicateLockRelation(call.index[i], snapshot);

	result = lion_count_keys(call.heap, snapshot, nkeys, call.index,
							call.key, call.keytype, stats);

	lion_count_sql_close(&call);
	return result;
}

Datum
lion_index_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(lion_count_sql(fcinfo, 1, NULL));
}

Datum
lion_index_count2(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(lion_count_sql(fcinfo, 2, NULL));
}

Datum
lion_index_count_stats(PG_FUNCTION_ARGS)
{
	LionCountStats stats;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	int64		count;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	memset(&stats, 0, sizeof(stats));
	count = lion_count_sql(fcinfo, 1, &stats);

	values[0] = Int64GetDatum(count);
	values[1] = Int64GetDatum(stats.blocks_skipped_via_vm);
	values[2] = Int64GetDatum(stats.tids_rechecked);
	values[3] = Int64GetDatum(stats.blocks_rechecked);
	/* one count never revisits a heap block, so this is always 0 here */
	values[4] = Int64GetDatum(stats.cache_hits);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * The count of a list longer than lion_array_batch_size(), located and counted
 * a batch at a time as the pushdown's plain count locates one (DESIGN.md §15,
 * "A list too long to locate at once"; lion_count_batched()).  The values are
 * sorted into the order a lookup locates them in, with their hashes, and the
 * byte-for-byte repeats dropped; each batch - run on to where the hash
 * changes, so that no equality class, and no entry, is in two - is located,
 * counted as the disjoint list it is and released, pins and all, before the
 * next one is located.  The entries of one scalar index are disjoint (a
 * multi-key class is refused by lion_count_open_indexes()), so the count of
 * the list is the sum of the batches' counts.  What is held for the whole
 * call is the values - the array, and a Datum and a hash each - one batch of
 * sets, and the visibility cache the batches share, as the node's do.  elems
 * and nulls, deconstruct_array()'s, are freed once the values are sorted.
 */
static int64
lion_count_any_batched(LionCountCall *call, Snapshot snapshot, int nelems,
					   Datum *elems, bool *nulls, bool elmbyval, int16 elmlen)
{
	Relation	index = call->index[0];
	int			batch = lion_array_batch_size();
	LionVisCache *cache;
	MemoryContext batchcxt;
	MemoryContext oldcxt;
	Datum	   *vals;
	uint32	   *hashes;
	int			nvals;
	int			start = 0;
	int64		count = 0;

	vals = (Datum *) palloc_extended(sizeof(Datum) * Max(nelems, 1),
									 MCXT_ALLOC_HUGE);
	hashes = (uint32 *) palloc_extended(sizeof(uint32) * Max(nelems, 1),
										MCXT_ALLOC_HUGE);
	nvals = lion_probe_sort(index, 1, call->keytype[0], nelems, elems, nulls,
							vals, hashes);
	nvals = lion_probe_sort_unique(vals, hashes, nvals, elmbyval, elmlen);
	/* a by-reference value points into the array, which the caller keeps */
	pfree(elems);
	pfree(nulls);

	cache = lion_vis_cache_create(CurrentMemoryContext);
	batchcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "lion index count batch",
									 ALLOCSET_DEFAULT_SIZES);
	while (start < nvals)
	{
		int			end = start + Min(batch, nvals - start);
		LionPostingSet *sets;
		int			nsets;
		int			nfound;
		int			i;

		while (end < nvals && hashes[end] == hashes[end - 1])
			end++;

		oldcxt = MemoryContextSwitchTo(batchcxt);
		sets = (LionPostingSet *)
			palloc_extended(sizeof(LionPostingSet) * (end - start),
							MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		nsets = lion_posting_set_lookup_many_col(index, 1, call->keytype[0],
												 end - start, &vals[start],
												 NULL, sets, &nfound);
		if (nfound > 0)
		{
			LionCountSource src;

			memset(&src, 0, sizeof(src));
			src.nsets = nsets;
			src.sets = sets;
			src.disjoint = true;
			count += lion_count_sources_cached(call->heap, snapshot, 1, &src,
											   NULL, cache, false);
		}
		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(batchcxt);

		start = end;
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextDelete(batchcxt);
	lion_vis_cache_destroy(cache);
	pfree(vals);
	pfree(hashes);

	return count;
}

/*
 * lion_index_count_any(idx, keys) - count(*) WHERE col = ANY (keys), the
 * SQL form of the IN list of DESIGN.md §15: the union of the listed values'
 * posting sets, counted against the visibility map like any other count.
 *
 * The values are located with lion_posting_set_lookup_many(), so the bucket
 * pages are read in order and duplicates cost nothing, and the union is the
 * k-way merge of lion_ecursor_build() or the disjoint sum.  Unlike the
 * pushdown's literal lists, this has no limit on the number of values: the
 * pins the lookup keeps are budgeted instead (DESIGN.md §15), and the sets
 * past the budget are counted one at a time.  And a list longer than a
 * work_mem of located sets is located a batch at a time
 * (lion_count_any_batched()): every value's set used to be held at once, and
 * past some nine million values the array of them was larger than an
 * allocation may be (2026-09-29 review).
 */
Datum
lion_index_count_any(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	Oid			keycoll;
	LionCountCall call;
	LionCountSource src;
	LionPostingSet *sets;
	Snapshot	snapshot;
	Datum	   *elems;
	bool	   *nulls;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			nelems;
	int			nsets;
	int			nfound;
	int64		count = 0;
	int			i;

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	keycoll = lion_count_arg_collation(fcinfo, 1);
	lion_count_open_indexes(snapshot, 1, &idxoid, &elemtype, &keycoll, 0,
						   &call);
	PredicateLockRelation(call.index[0], snapshot);

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	/*
	 * The elements are deconstructed as what the array holds, and looked up
	 * as the type lion_count_open_indexes() resolved that to: a varchar[] is
	 * looked up as text on a text_ops column, the same bytes.
	 */
	if (nelems > lion_array_batch_size())
	{
		count = lion_count_any_batched(&call, snapshot, nelems, elems, nulls,
									   elmbyval, elmlen);
		lion_count_sql_close(&call);
		PG_RETURN_INT64(count);
	}

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(nelems, 1));
	nsets = lion_posting_set_lookup_many_col(call.index[0], 1, call.keytype[0],
											nelems, elems, nulls, sets,
											&nfound);

	/* An empty array, an all-NULL one, or no listed value with an entry. */
	if (nfound > 0)
	{
		memset(&src, 0, sizeof(src));
		src.nsets = nsets;
		src.sets = sets;

		/*
		 * lion_posting_set_lookup_many() dropped duplicate ENTRIES, so the sets
		 * are distinct entries of one index and their union is their sum
		 * whenever the opclass is scalar: the disjoint-sum short-circuit of
		 * DESIGN.md §15, which lion_count_sources() takes from here.
		 */
		src.disjoint = true;
		count = lion_count_sources(call.heap, snapshot, 1, &src, NULL);
	}

	for (i = 0; i < nsets; i++)
		lion_posting_set_release(&sets[i]);

	lion_count_sql_close(&call);

	PG_RETURN_INT64(count);
}

/*
 * lion_index_count_group_stats(idx) - count every key of one index under
 * one snapshot, the way the GROUP BY path of DESIGN.md §10 does, sharing one
 * visibility cache across the groups.
 *
 * This is the SQL image of lion_next_group() in lion_exec_count.c: same entry
 * scan, same one lion_count_sources_cached() call per group, same single cache
 * for the whole run.  It exists because the cache can only pay off across
 * counts, so nothing a single lion_index_count() does can exercise it -
 * and a regression test should not have to go through the planner to prove
 * that the dirty pages of a grouped count are visited once instead of once
 * per group.  use_cache = false runs the very same loop with no cache at all,
 * which is what every caller did before the cache existed.
 */
Datum
lion_index_count_group_stats(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	bool		usecache = PG_GETARG_BOOL(1);
	AttrNumber	attno = (AttrNumber) PG_GETARG_INT16(2);
	LionCountCall call;
	Snapshot	snapshot;
	LionCountStats stats;
	LionVisCache *cache;
	LionEntryScan es;
	MemoryContext percxt;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};
	int64		groups = 0;
	int64		total = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	/*
	 * Column 0 is no column, and it is refused before anything else happens.
	 * lion_count_open_indexes() takes wantcol = 0 to mean "the caller names
	 * none", which a one-column index satisfies, so attno = 0 used to go on
	 * to read the operator class of column -1 (2026-09-23 review).
	 */
	if (attno < 1)
	{
		char	   *idxname = get_rel_name(idxoid);

		if (idxname == NULL)
			lion_count_no_relation(idxoid);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lion index \"%s\" has no key column %d",
						idxname, attno)));
	}

	lion_count_open_indexes(snapshot, 1, &idxoid, NULL, NULL, attno, &call);

	PredicateLockRelation(call.index[0], snapshot);

	memset(&stats, 0, sizeof(stats));
	/* use_cache = false is the pre-cache behaviour, for an A/B in one query */
	cache = usecache ? lion_vis_cache_create(CurrentMemoryContext) : NULL;
	percxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index group count",
								   ALLOCSET_SMALL_SIZES);

	lion_entry_scan_begin_col(&es, call.index[0], attno);

	for (;;)
	{
		LionCountSource src;
		LionPostingSet ps;
		Datum		key;
		int64		n;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(percxt);
		oldcxt = MemoryContextSwitchTo(percxt);

		if (!lion_entry_scan_next(&es, &key, &ps))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}

		memset(&src, 0, sizeof(src));
		src.nsets = 1;
		src.sets = &ps;

		n = lion_count_sources_cached(call.heap, snapshot, 1, &src, &stats,
									 cache, false);
		lion_posting_set_release(&ps);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if one of its rows is visible (§10). */
		if (n > 0)
		{
			groups++;
			total += n;
		}
	}

	lion_entry_scan_end(&es);
	lion_vis_cache_destroy(cache);
	MemoryContextDelete(percxt);
	lion_count_sql_close(&call);

	values[0] = Int64GetDatum(groups);
	values[1] = Int64GetDatum(total);
	values[2] = Int64GetDatum(stats.blocks_skipped_via_vm);
	values[3] = Int64GetDatum(stats.tids_rechecked);
	values[4] = Int64GetDatum(stats.blocks_rechecked);
	values[5] = Int64GetDatum(stats.cache_hits);
	values[6] = Int64GetDatum(stats.cache_full);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
