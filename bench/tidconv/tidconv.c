#include "postgres.h"
#include "fmgr.h"
#include "storage/itemptr.h"
PG_MODULE_MAGIC;
/* encoding used by the demo: block*2048 + offset */
PG_FUNCTION_INFO_V1(int8_to_tid);
Datum int8_to_tid(PG_FUNCTION_ARGS)
{
	int64 v = PG_GETARG_INT64(0);
	ItemPointer t = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerSet(t, (BlockNumber) (v / 2048), (OffsetNumber) (v % 2048));
	PG_RETURN_ITEMPOINTER(t);
}
PG_FUNCTION_INFO_V1(tid_to_int8);
Datum tid_to_int8(PG_FUNCTION_ARGS)
{
	ItemPointer t = PG_GETARG_ITEMPOINTER(0);
	PG_RETURN_INT64((int64) ItemPointerGetBlockNumber(t) * 2048 + ItemPointerGetOffsetNumber(t));
}
