#ifndef KVBLOB_H
#define KVBLOB_H

#include <stddef.h>

#include "blob.h"

/*
 * `kvblob` stores byte blobs on disk, keyed by byte blobs. The store is a
 * single file: a header, an array of hash buckets, and a heap of records.
 * A lookup is one hash, one bucket read, and a walk of that bucket's
 * chain, so it costs a small constant number of reads.
 *
 * The client owns every byte it hands over and every buffer it gets back
 * filled.The library allocates nothing but its own handle.
 *
 * A handle is not thread-safe, and a store file must have at most one
 * open handle across the whole system.Nothing enforces either rule.
 */

typedef struct KVblob KVblob;

enum {
	KVB_OK		=  0,
	KVB_ENOTFOUND	= -1,	/* no pair with that key */
	KVB_ESMALL	= -2,	/* caller buffer too small; needed size reported */
	KVB_EIO		= -3,	/* read, write or fsync failed; see errno */
	KVB_ECORRUPT	= -4,	/* the file is not a consistent kvblob store */
	KVB_EINVAL	= -5,	/* malformed argument */
	KVB_ENOMEM	= -6,	/* out of memory */
	KVB_ETOOBIG	= -7	/* key or value longer than 4294967295 bytes */
};

/*
 * kvbopen opens the store in the file named by path, creating and
 * initialising it if it does not exist. It returns a handle, or NULL on
 * failure with errno set; a file that exists but is not a kvblob store
 * fails with errno EFTYPE, or EINVAL where EFTYPE is absent.
 */
KVblob *kvbopen(const char *path);

/* kvbclose flushes and closes the store and releases the handle. */
int kvbclose(KVblob *kv);

/*
 * kvbget copies the value stored under k into the buffer v describes.
 * On entry v->buf points at a buffer of v->len bytes. On return v->len
 * is the true length of the value, whether or not it fitted: a buffer
 * too small yields KVB_ESMALL with v->len set, so the caller can resize
 * and call again. v->buf may be NULL when v->len is 0.
 */
int kvbget(KVblob *kv, const Blob *k, Blob *v);

/*
 * kvblen reports in *vlen the length of the value stored under k,
 * without copying it. It returns KVB_ENOTFOUND if k is absent, so it
 * doubles as an existence test.
 */
int kvblen(KVblob *kv, const Blob *k, size_t *vlen);

/*
 * kvbput stores v under k, replacing any value already stored under k.
 * The bytes are flushed to the disk before it returns.
 */
int kvbput(KVblob *kv, const Blob *k, const Blob *v);

/* kvbdel removes the pair stored under k. */
int	kvbdel(KVblob *kv, const Blob *k);

/* kvbcount reports in *n how many pairs the store holds. */
int	kvbcount(KVblob *kv, size_t *n);

/* kvberrstr returns a fixed description of a kvblob return code. */
const char *kvberrstr(int err);

#endif
