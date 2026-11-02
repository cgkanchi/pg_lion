/*-------------------------------------------------------------------------
 * lion_simplehash.h
 *	  A helper for the extension's simplehash.h tables, for files with no
 *	  need of lion.h (the vector index's among them).
 *-------------------------------------------------------------------------
 */
#ifndef LION_SIMPLEHASH_H
#define LION_SIMPLEHASH_H

/*
 * Begin a full iteration of a simplehash table of prefix pfx, as
 * pfx_start_iterate() does: at the table's first unused slot, which there
 * always is, since simplehash grows before a table fills.  The same walk in
 * the same order; what differs is that the slot is found here, where it
 * starts at 0 rather than at simplehash's PG_UINT64_MAX, which a static
 * analyser takes for a value the 32-bit iterator cannot hold.
 */
#define LION_SH_START_ITERATE(pfx, tb, it) \
	do { \
		uint32		lion_sh_at_ = 0; \
		uint32		lion_sh_i_; \
		\
		for (lion_sh_i_ = 0; lion_sh_i_ < (tb)->size; lion_sh_i_++) \
		{ \
			if ((tb)->data[lion_sh_i_].status != pfx##_SH_IN_USE) \
			{ \
				lion_sh_at_ = lion_sh_i_; \
				break; \
			} \
		} \
		pfx##_start_iterate_at((tb), (it), lion_sh_at_); \
	} while (0)

#endif							/* LION_SIMPLEHASH_H */
