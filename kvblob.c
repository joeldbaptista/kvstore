/*
 * kvblob: a key-value store on disk, where both key and value are byte
 * blobs the client owns.
 *
 * File layout, all integers little-endian:
 *
 *	0			header, 64 bytes
 *	bucketoff		nbucket 8-byte record offsets, 0 means empty
 *	...			records, allocated upwards to heapend
 *
 * Header:
 *	 0  u8[8]	magic "KVBLOB1\n"
 *	 8  u32		version
 *	12  u32		header size
 *	16  u64		nbucket, a power of two
 *	24  u64		npair
 *	32  u64		bucketoff
 *	40  u64		heapend, the next free heap offset
 *	48  u64[2]	reserved, zero
 *
 * Record:
 *	 0  u64		full hash of the key
 *	 8  u64		next record in this bucket's chain, 0 means end
 *	16  u32		key length
 *	20  u32		value length
 *	24  u32		value capacity, >= value length
 *	28  u32		reserved, zero
 *	32  u8[klen]	key bytes
 *	    u8[cap]	value area, the first vlen bytes are the value
 *
 * A record is reached only through its bucket chain, so removing one is
 * unlinking it, and replacing a value too long for its capacity is
 * linking a fresh record in its place. Neither reclaims the space the
 * old record held: the file grows until it is rebuilt. There is no
 * compaction in this iteration.
 *
 * Every put and delete ends in fsync, so the bytes of a call that
 * returned KVB_OK are on the disk. A crash in the middle of a call can
 * still leave the store inconsistent, because nothing here is a journal.
 */

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "blob.h"
#include "kvblob.h"

enum {
	Hdrsz		= 64,
	Recsz		= 32,		/* record header, without key or value */
	Version		= 1,
	Nbucket0	= 64,		/* buckets in a fresh store */
	Capgrain	= 16,		/* value capacity is a multiple of this */
	Iobuf		= 4096
};

static const unsigned char Magic[8] = "KVBLOB1\n";

struct KVblob {
	int		fd;
	uint64_t	nbucket;
	uint64_t	npair;
	uint64_t	bucketoff;
	uint64_t	heapend;
};

typedef struct Rec Rec;
struct Rec {
	uint64_t	hash;
	uint64_t	next;
	uint32_t	klen;
	uint32_t	vlen;
	uint32_t	cap;
};

/* Integer encoding. */

static void
put32(unsigned char *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static uint32_t
get32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
put64(unsigned char *p, uint64_t v)
{
	put32(p, (uint32_t)(v & 0xffffffffULL));
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t
get64(const unsigned char *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Unbuffered whole-request reads and writes at an absolute offset. */

static int
rdfull(int fd, void *buf, size_t n, uint64_t off)
{
	unsigned char *p = buf;
	ssize_t r;

	while(n > 0){
		r = pread(fd, p, n, (off_t)off);
		if(r < 0){
			if(errno == EINTR)
				continue;
			return KVB_EIO;
		}
		if(r == 0){
			errno = EIO;
			return KVB_ECORRUPT;	/* short file */
		}
		p += r;
		n -= (size_t)r;
		off += (uint64_t)r;
	}
	return KVB_OK;
}

static int
wrfull(int fd, const void *buf, size_t n, uint64_t off)
{
	const unsigned char *p = buf;
	ssize_t r;

	while(n > 0){
		r = pwrite(fd, p, n, (off_t)off);
		if(r < 0){
			if(errno == EINTR)
				continue;
			return KVB_EIO;
		}
		p += r;
		n -= (size_t)r;
		off += (uint64_t)r;
	}
	return KVB_OK;
}

static int
zerofill(int fd, uint64_t off, uint64_t n)
{
	static const unsigned char zero[Iobuf];
	size_t k;
	int e;

	while(n > 0){
		k = n < sizeof zero ? (size_t)n : sizeof zero;
		if((e = wrfull(fd, zero, k, off)) != KVB_OK)
			return e;
		off += k;
		n -= k;
	}
	return KVB_OK;
}

static int
syncfd(int fd)
{
	while(fsync(fd) < 0){
		if(errno == EINTR)
			continue;
		return KVB_EIO;
	}
	return KVB_OK;
}

/* Header, buckets, records. */

static int
hdrwrite(KVblob *kv)
{
	unsigned char b[Hdrsz];
	int e;

	memset(b, 0, sizeof b);
	memcpy(b, Magic, sizeof Magic);
	put32(b + 8, Version);
	put32(b + 12, Hdrsz);
	put64(b + 16, kv->nbucket);
	put64(b + 24, kv->npair);
	put64(b + 32, kv->bucketoff);
	put64(b + 40, kv->heapend);
	if((e = wrfull(kv->fd, b, sizeof b, 0)) != KVB_OK)
		return e;
	return syncfd(kv->fd);
}

static int
hdrread(KVblob *kv)
{
	unsigned char b[Hdrsz];
	int e;

	if((e = rdfull(kv->fd, b, sizeof b, 0)) != KVB_OK)
		return e;
	if(memcmp(b, Magic, sizeof Magic) != 0)
		return KVB_ECORRUPT;
	if(get32(b + 8) != Version || get32(b + 12) != Hdrsz)
		return KVB_ECORRUPT;
	kv->nbucket = get64(b + 16);
	kv->npair = get64(b + 24);
	kv->bucketoff = get64(b + 32);
	kv->heapend = get64(b + 40);
	if(kv->nbucket == 0 || (kv->nbucket & (kv->nbucket - 1)) != 0)
		return KVB_ECORRUPT;
	if(kv->bucketoff < Hdrsz)
		return KVB_ECORRUPT;
	if(kv->heapend < kv->bucketoff + kv->nbucket * 8)
		return KVB_ECORRUPT;
	return KVB_OK;
}

static int
slotread(KVblob *kv, uint64_t at, uint64_t *off)
{
	unsigned char b[8];
	int e;

	if((e = rdfull(kv->fd, b, sizeof b, at)) != KVB_OK)
		return e;
	*off = get64(b);
	return KVB_OK;
}

static int
slotwrite(KVblob *kv, uint64_t at, uint64_t off)
{
	unsigned char b[8];

	put64(b, off);
	return wrfull(kv->fd, b, sizeof b, at);
}

static int
recread(KVblob *kv, uint64_t off, Rec *r)
{
	unsigned char b[Recsz];
	int e;

	if(off < Hdrsz || off + Recsz > kv->heapend)
		return KVB_ECORRUPT;
	if((e = rdfull(kv->fd, b, sizeof b, off)) != KVB_OK)
		return e;
	r->hash = get64(b);
	r->next = get64(b + 8);
	r->klen = get32(b + 16);
	r->vlen = get32(b + 20);
	r->cap = get32(b + 24);
	if(r->vlen > r->cap)
		return KVB_ECORRUPT;
	if(off + Recsz + r->klen + r->cap > kv->heapend)
		return KVB_ECORRUPT;
	return KVB_OK;
}

static int
recwrite(KVblob *kv, uint64_t off, const Rec *r)
{
	unsigned char b[Recsz];

	memset(b, 0, sizeof b);
	put64(b, r->hash);
	put64(b + 8, r->next);
	put32(b + 16, r->klen);
	put32(b + 20, r->vlen);
	put32(b + 24, r->cap);
	return wrfull(kv->fd, b, sizeof b, off);
}

static uint64_t
hashblob(const Blob *k)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	size_t i;

	for(i = 0; i < k->len; i++){
		h ^= (uint64_t)k->buf[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* keyeq sets *eq to 1 if the key stored in the record at off equals k. */
static int
keyeq(KVblob *kv, uint64_t off, uint32_t klen, const Blob *k, int *eq)
{
	unsigned char b[Iobuf];
	const unsigned char *p;
	uint64_t at;
	uint32_t left;
	size_t n;
	int e;

	*eq = 0;
	if((uint64_t)klen != (uint64_t)k->len)
		return KVB_OK;
	at = off + Recsz;
	p = k->buf;
	left = klen;
	while(left > 0){
		n = left < sizeof b ? left : sizeof b;
		if((e = rdfull(kv->fd, b, n, at)) != KVB_OK)
			return e;
		if(memcmp(b, p, n) != 0)
			return KVB_OK;
		at += n;
		p += n;
		left -= (uint32_t)n;
	}
	*eq = 1;
	return KVB_OK;
}

/*
 * find locates the record holding key k.  On KVB_OK it fills *r, puts
 * the record's offset in *roff, and puts in *link the file offset of the
 * 8-byte slot pointing at that record, which is either the bucket slot
 * or the next field of the record before it.  Unlinking is then one
 * write to *link.
 */
static int
find(KVblob *kv, const Blob *k, uint64_t hash, Rec *r, uint64_t *roff,
	uint64_t *link)
{
	uint64_t off, at;
	int e, eq;

	at = kv->bucketoff + (hash & (kv->nbucket - 1)) * 8;
	if((e = slotread(kv, at, &off)) != KVB_OK)
		return e;
	while(off != 0){
		if((e = recread(kv, off, r)) != KVB_OK)
			return e;
		if(r->hash == hash){
			if((e = keyeq(kv, off, r->klen, k, &eq)) != KVB_OK)
				return e;
			if(eq){
				*roff = off;
				*link = at;
				return KVB_OK;
			}
		}
		at = off + 8;		/* the next field of this record */
		off = r->next;
	}
	return KVB_ENOTFOUND;
}

/* grow doubles the bucket array and rehashes every record into it. */
static int
grow(KVblob *kv)
{
	uint64_t newn, newoff, i, off, next, at, head;
	Rec r;
	int e;

	newn = kv->nbucket * 2;
	newoff = kv->heapend;
	if((e = zerofill(kv->fd, newoff, newn * 8)) != KVB_OK)
		return e;
	for(i = 0; i < kv->nbucket; i++){
		if((e = slotread(kv, kv->bucketoff + i * 8, &off)) != KVB_OK)
			return e;
		while(off != 0){
			if((e = recread(kv, off, &r)) != KVB_OK)
				return e;
			next = r.next;
			at = newoff + (r.hash & (newn - 1)) * 8;
			if((e = slotread(kv, at, &head)) != KVB_OK)
				return e;
			if((e = slotwrite(kv, off + 8, head)) != KVB_OK)
				return e;
			if((e = slotwrite(kv, at, off)) != KVB_OK)
				return e;
			off = next;
		}
	}
	if((e = syncfd(kv->fd)) != KVB_OK)
		return e;
	kv->bucketoff = newoff;
	kv->nbucket = newn;
	kv->heapend = newoff + newn * 8;	/* old array is left behind */
	return hdrwrite(kv);
}

/* valwrite copies the value bytes into the value area of a record. */
static int
valwrite(KVblob *kv, uint64_t off, uint32_t klen, const Blob *v)
{
	if(v->len == 0)
		return KVB_OK;
	return wrfull(kv->fd, v->buf, v->len, off + Recsz + klen);
}

/* Public interface. */

KVblob *
kvbopen(const char *path)
{
	KVblob *kv;
	struct stat st;
	int oerr;

	if(path == NULL){
		errno = EINVAL;
		return NULL;
	}
	if((kv = malloc(sizeof *kv)) == NULL)
		return NULL;
	memset(kv, 0, sizeof *kv);
	if((kv->fd = open(path, O_RDWR | O_CREAT, 0666)) < 0)
		goto fail;
	if(fstat(kv->fd, &st) < 0)
		goto fail;
	if(st.st_size == 0){
		kv->nbucket = Nbucket0;
		kv->npair = 0;
		kv->bucketoff = Hdrsz;
		kv->heapend = Hdrsz + Nbucket0 * 8;
		if(zerofill(kv->fd, kv->bucketoff, kv->nbucket * 8) != KVB_OK)
			goto fail;
		if(hdrwrite(kv) != KVB_OK)
			goto fail;
	}else if(hdrread(kv) != KVB_OK){
#ifdef EFTYPE
		errno = EFTYPE;
#else
		errno = EINVAL;
#endif
		goto fail;
	}
	return kv;

fail:
	oerr = errno;
	if(kv->fd >= 0)
		close(kv->fd);
	free(kv);
	errno = oerr;
	return NULL;
}

int
kvbclose(KVblob *kv)
{
	int e;

	if(kv == NULL)
		return KVB_EINVAL;
	e = syncfd(kv->fd);
	if(close(kv->fd) < 0 && e == KVB_OK)
		e = KVB_EIO;
	free(kv);
	return e;
}

int
kvbget(KVblob *kv, const Blob *k, Blob *v)
{
	Rec r;
	uint64_t roff, link;
	int e;

	if(kv == NULL || k == NULL || v == NULL)
		return KVB_EINVAL;
	if(k->len > 0 && k->buf == NULL)
		return KVB_EINVAL;
	if((e = find(kv, k, hashblob(k), &r, &roff, &link)) != KVB_OK)
		return e;
	if((uint64_t)r.vlen > (uint64_t)v->len){
		v->len = r.vlen;
		return KVB_ESMALL;
	}
	if(r.vlen > 0){
		if(v->buf == NULL)
			return KVB_EINVAL;
		if((e = rdfull(kv->fd, v->buf, r.vlen,
			roff + Recsz + r.klen)) != KVB_OK)
			return e;
	}
	v->len = r.vlen;
	return KVB_OK;
}

int
kvblen(KVblob *kv, const Blob *k, size_t *vlen)
{
	Rec r;
	uint64_t roff, link;
	int e;

	if(kv == NULL || k == NULL || vlen == NULL)
		return KVB_EINVAL;
	if(k->len > 0 && k->buf == NULL)
		return KVB_EINVAL;
	if((e = find(kv, k, hashblob(k), &r, &roff, &link)) != KVB_OK)
		return e;
	*vlen = r.vlen;
	return KVB_OK;
}

int
kvbput(KVblob *kv, const Blob *k, const Blob *v)
{
	Rec r, old;
	uint64_t hash, roff, link, off, cap;
	int e, found;

	if(kv == NULL || k == NULL || v == NULL)
		return KVB_EINVAL;
	if((k->len > 0 && k->buf == NULL) || (v->len > 0 && v->buf == NULL))
		return KVB_EINVAL;
	if(k->len > 0xffffffffUL || v->len > 0xffffffffUL)
		return KVB_ETOOBIG;

	hash = hashblob(k);
	e = find(kv, k, hash, &old, &roff, &link);
	if(e != KVB_OK && e != KVB_ENOTFOUND)
		return e;
	found = e == KVB_OK;

	if(found && (uint64_t)v->len <= (uint64_t)old.cap){
		/* the new value fits in the capacity of the old one */
		old.vlen = (uint32_t)v->len;
		if((e = recwrite(kv, roff, &old)) != KVB_OK)
			return e;
		if((e = valwrite(kv, roff, old.klen, v)) != KVB_OK)
			return e;
		return syncfd(kv->fd);
	}

	/* otherwise a fresh record takes the place of any old one */
	cap = ((uint64_t)v->len + Capgrain - 1) / Capgrain * Capgrain;
	if(cap > 0xffffffffULL)
		cap = v->len;			/* no room to round up */
	r.hash = hash;
	r.klen = (uint32_t)k->len;
	r.vlen = (uint32_t)v->len;
	r.cap = (uint32_t)cap;
	off = kv->heapend;
	if(found){
		r.next = old.next;
	}else{
		link = kv->bucketoff + (hash & (kv->nbucket - 1)) * 8;
		if((e = slotread(kv, link, &r.next)) != KVB_OK)
			return e;
	}
	if((e = recwrite(kv, off, &r)) != KVB_OK)
		return e;
	if(r.klen > 0 &&
		(e = wrfull(kv->fd, k->buf, k->len, off + Recsz)) != KVB_OK)
		return e;
	if((e = valwrite(kv, off, r.klen, v)) != KVB_OK)
		return e;
	/* the padding between vlen and cap is never read, so leave it */

	/*
	 * The header has to cover the new record before anything points at
	 * it, because a chain reaching past heapend reads as corruption.
	 * hdrwrite ends in fsync, so the record is on the disk too.
	 */
	kv->heapend = off + Recsz + r.klen + r.cap;
	if(!found)
		kv->npair++;
	if((e = hdrwrite(kv)) != KVB_OK)
		return e;
	if((e = slotwrite(kv, link, off)) != KVB_OK)
		return e;
	if((e = syncfd(kv->fd)) != KVB_OK)
		return e;

	if(!found && kv->npair > kv->nbucket / 4 * 3)
		return grow(kv);
	return KVB_OK;
}

int
kvbdel(KVblob *kv, const Blob *k)
{
	Rec r;
	uint64_t roff, link;
	int e;

	if(kv == NULL || k == NULL)
		return KVB_EINVAL;
	if(k->len > 0 && k->buf == NULL)
		return KVB_EINVAL;
	if((e = find(kv, k, hashblob(k), &r, &roff, &link)) != KVB_OK)
		return e;
	if((e = slotwrite(kv, link, r.next)) != KVB_OK)
		return e;
	if((e = syncfd(kv->fd)) != KVB_OK)
		return e;
	kv->npair--;				/* the record space is leaked */
	return hdrwrite(kv);
}

int
kvbcount(KVblob *kv, size_t *n)
{
	if(kv == NULL || n == NULL)
		return KVB_EINVAL;
	*n = (size_t)kv->npair;
	return KVB_OK;
}

const char *
kvberrstr(int err)
{
	switch(err){
	case KVB_OK:
		return "no error";
	case KVB_ENOTFOUND:
		return "key not found";
	case KVB_ESMALL:
		return "buffer too small";
	case KVB_EIO:
		return "input/output error";
	case KVB_ECORRUPT:
		return "store file corrupt";
	case KVB_EINVAL:
		return "invalid argument";
	case KVB_ENOMEM:
		return "out of memory";
	case KVB_ETOOBIG:
		return "key or value too long";
	}
	return "unknown error";
}
