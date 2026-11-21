# kvblob

kvblob stores byte blobs on disk, keyed by byte blobs. It does two
things: it puts a pair away, and it finds a value again from its key.
It does not interpret either blob, and it does not own either blob.

The store lives in one file. Nothing is kept in memory between calls
except the open handle, which holds a file descriptor and four numbers
copied from the file header.

A *blob* is a byte range the client owns:

```c
struct Blob {
	size_t		len;
	unsigned char  *buf;
};
```

The client allocates those bytes, and the client frees them. No
function in the library allocates or frees them on its behalf. The one
allocation the library makes is the `KVblob` handle itself, in
`kvbopen`, released by `kvbclose`.

## Building

```
make                       # produces kvb
./kvb demo.kvb demo        # runs the demo
make clean
```

The library is C99 and POSIX. It uses `open`, `pread`, `pwrite`,
`fsync`, `fstat` and `close`, so it needs no other dependency.

## Using it

```
kvb FILE put KEY VALUE     store VALUE under KEY
kvb FILE get KEY           write the value to the standard output
kvb FILE len KEY           print the length of the value
kvb FILE del KEY           remove the pair
kvb FILE count             print how many pairs the store holds
kvb FILE demo              run the demo
```

The demo is the better introduction, because it exercises binary keys,
an empty value, replacement, growth, deletion and reopening. The `get`
subcommand in `example.c` is the shortest illustration of the ownership
rule: it asks `kvblen` for the length, allocates a buffer of that size,
has `kvbget` fill it, uses it, and frees it.

## The interface

This table is the complete interface.

| Function | Meaning |
|---|---|
| `KVblob *kvbopen(const char *path)` | Open the store, creating and initialising the file if it does not exist. Returns `NULL` with `errno` set on failure. |
| `int kvbclose(KVblob *kv)` | Flush, close, and release the handle. |
| `int kvbget(KVblob *kv, const Blob *k, Blob *v)` | Copy the value stored under `k` into the buffer `v` describes. |
| `int kvblen(KVblob *kv, const Blob *k, size_t *vlen)` | Report the length of the value stored under `k`, without copying it. |
| `int kvbput(KVblob *kv, const Blob *k, const Blob *v)` | Store `v` under `k`, replacing any value already there. |
| `int kvbdel(KVblob *kv, const Blob *k)` | Remove the pair stored under `k`. |
| `int kvbcount(KVblob *kv, size_t *n)` | Report how many pairs the store holds. |
| `const char *kvberrstr(int err)` | Describe a return code. |

Every function returning `int` returns `KVB_OK`, which is zero, or one
of the negative codes in `kvblob.h`. That enumeration is also complete:
`KVB_ENOTFOUND`, `KVB_ESMALL`, `KVB_EIO`, `KVB_ECORRUPT`, `KVB_EINVAL`,
`KVB_ENOMEM` and `KVB_ETOOBIG`.

`kvbget` deserves one note. On entry `v->buf` points at a buffer of
`v->len` bytes. On return `v->len` holds the true length of the value,
whether or not it fitted. So a buffer too small yields `KVB_ESMALL`
with the needed length in `v->len`, and the caller can resize and call
again. A caller that does not want to guess calls `kvblen` first.

A key may be zero bytes long, and so may a value. When `len` is zero,
`buf` may be `NULL`. Neither length may exceed 4294967295, because the
format stores lengths in 32 bits; a longer blob yields `KVB_ETOOBIG`.

A handle is not thread-safe, and a store file must have at most one
open handle across the whole system. Nothing enforces either rule.

## The file format

All integers are little-endian and unsigned, so a store file moves
between machines of different byte order. Offsets are absolute byte
positions in the file. An offset of zero means "none", which is
unambiguous because no record can start at zero: the header is there.

The file has three parts, in this order:

```
0                   header, 64 bytes
bucketoff           bucket array, nbucket slots of 8 bytes
                    records, packed upwards, ending at heapend
```

### Header, 64 bytes at offset 0

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 8 | magic | the bytes `KVBLOB1\n` |
| 8 | 4 | version | format version, currently 1 |
| 12 | 4 | header size | 64, so a later version can grow the header |
| 16 | 8 | nbucket | number of bucket slots, always a power of two |
| 24 | 8 | npair | number of pairs stored |
| 32 | 8 | bucketoff | offset of the bucket array |
| 40 | 8 | heapend | offset just past the last record, the next free byte |
| 48 | 16 | reserved | zero |

`bucketoff` is not fixed, because growing the bucket array writes a new
and larger one at the end of the file rather than moving the records.

### Bucket array

`nbucket` slots of 8 bytes each. Slot *i* holds the offset of the first
record of chain *i*, or zero if that chain is empty. A fresh store has
64 slots, so it is 576 bytes long and empty.

### Record

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 8 | hash | the full 64-bit hash of the key |
| 8 | 8 | next | offset of the next record in this chain, or zero |
| 16 | 4 | klen | key length in bytes |
| 20 | 4 | vlen | value length in bytes |
| 24 | 4 | cap | capacity of the value area, at least `vlen` |
| 28 | 4 | reserved | zero |
| 32 | klen | key | the key bytes |
| 32+klen | cap | value area | the first `vlen` bytes are the value |

A record therefore occupies `32 + klen + cap` bytes. `cap` is `vlen`
rounded up to a multiple of 16, so a value that later grows a little
can be written where it already is.

Two fields exist purely to save work. The stored `hash` lets a chain
walk skip a key comparison whenever the hashes differ, and that is the
common case for the records that merely collided into the same bucket.
The `cap` field lets a replacement value avoid allocating a new record.

The bytes between `vlen` and `cap` are never read, so they are never
written either. As a result the file may be shorter than `heapend`
says, and the hole reads as zeros if a later write extends the file.

## How the code works

`kvblob.c` is about 650 lines in four layers: byte encoding,
whole-request I/O, the structures on disk, and the seven public
functions.

### The two lower layers

`put32`, `get32`, `put64` and `get64` move integers between memory and
a byte buffer in little-endian order. Nothing in the library casts a
struct onto file bytes, so padding and byte order of the host machine
never reach the disk.

`rdfull` and `wrfull` loop over `pread` and `pwrite` until the whole
request is done, retrying on `EINTR`. A short read means the file ends
where a record should be, so `rdfull` reports `KVB_ECORRUPT` rather
than `KVB_EIO`. `zerofill` writes a run of zeros, and `syncfd` loops
over `fsync`. Both lower layers take absolute offsets, so the library
keeps no file position and never calls `lseek`.

### Lookup

`hashblob` is FNV-1a, 64-bit. `find` is the heart of the library, and
every public function except `kvbcount` and `kvberrstr` goes through
it. Given a key it reports three things: the record, its offset, and
the offset of the 8-byte slot that points at the record.

That third value is why removal is cheap. The slot is either a bucket
slot, when the record is first in its chain, or the `next` field of the
record before it. Either way, unlinking the record is one 8-byte write
to that offset, and `find` has already computed it during the walk.

`keyeq` does the key comparison. It compares the lengths first, and
then reads the stored key in 4096-byte pieces and `memcmp`s each piece
against the caller's key, so a key of any size needs no allocation.

### Get and len

`kvbget` calls `find`, compares `vlen` against the buffer the caller
offered, and either reads `vlen` bytes straight from the value area
into that buffer or reports `KVB_ESMALL`. In both cases it writes the
true length into `v->len`. `kvblen` is the same without the copy.

### Put

`kvbput` hashes the key and calls `find`, then takes one of two paths.

If the key is present and the new value fits in the existing `cap`, it
rewrites the record header with the new `vlen`, writes the value where
the old one was, and calls `fsync`. Nothing else on the disk changes.

Otherwise it builds a fresh record at `heapend` and links it in. The
order of the writes matters, and it is this:

1. Write the record header, the key, and the value.
2. Raise `heapend` past the new record, raise `npair` if the key is
   new, and write the header. `hdrwrite` ends in `fsync`, so the
   record and the header that covers it are both on the disk.
3. Write the new record's offset into the pointing slot, which is the
   slot `find` reported when the key was present, and the bucket slot
   otherwise. Then `fsync` again.

The record becomes reachable only in step 3, and by then the header
already covers it. The reverse order would leave a window where a
chain points past `heapend`, which `recread` rejects as corruption. A
replaced record keeps its successor, because the new record inherits
the old record's `next` before the slot is overwritten.

### Delete

`kvbdel` calls `find`, writes the record's `next` into the pointing
slot, calls `fsync`, decrements `npair`, and writes the header. The
record itself is left where it is, unreachable.

### Growth

A chain walk stays short only while the number of pairs stays close to
the number of buckets. So after a put that adds a key, `kvbput` checks
whether `npair` has passed three quarters of `nbucket`, and calls
`grow` if it has.

`grow` writes a zeroed array of `2 * nbucket` slots at the end of the
file, then walks every old chain and relinks each record it finds into
the new array by `hash & (newn - 1)`. It saves each record's `next`
before overwriting it, because that field is the old chain link as
well as the new one. Then it points the header at the new array and
leaves the old array where it is. No record moves, and no key is
hashed again, because each record carries its own hash.

### Opening

`kvbopen` opens the path with `O_RDWR | O_CREAT` and mode 0666, then
asks `fstat` for the size. A size of zero means the file is new, so it
writes 64 zeroed bucket slots and a header describing them. Any other
size means the file should already be a store, so `hdrread` checks the
magic, the version, the header size, that `nbucket` is a power of two,
and that `heapend` covers the bucket array. A file that fails those
checks is not a store, and `kvbopen` fails with `errno` set to `EFTYPE`
where the platform defines it, or `EINVAL` where it does not.

`recread` applies the same suspicion to every record it loads: the
offset must lie inside the file, `vlen` must not exceed `cap`, and the
whole record must end at or before `heapend`. A corrupt chain therefore
reports `KVB_ECORRUPT` instead of looping or reading nonsense.

## Durability

Every put and every delete ends in `fsync`. So when one of them returns
`KVB_OK`, the bytes are on the disk and not merely in the page cache.
That costs one flush for a put that fits in place, and two for a put
that adds a record or for a delete.

But a crash in the middle of a call can still leave the store
inconsistent, because the library writes no journal. The window is
narrow in each case, and these are all of them: a crash between steps 2
and 3 of a new-record put leaves a counted but unreachable record, and
a crash inside `grow` leaves the two bucket arrays disagreeing. Neither
is repaired at the next open, because there is nothing to repair from.

## What this iteration does not do

This list is exhaustive.

1. **No space reclamation.** A delete unlinks a record, a replacement
   too long for its capacity abandons one, and growing the bucket array
   abandons the old array. Nothing is reused, so the file only grows.
   There is no compaction, and no free list.
2. **No crash atomicity.** As described under Durability.
3. **No locking.** One open handle per file, system-wide, enforced by
   nothing. There is no reader-writer sharing, and no `flock`.
4. **No iteration.** There is no way to walk the pairs, so a key is
   found only when the client already holds it. The format supports a
   walk, by reading the bucket array and following each chain, but no
   function exposes one.
5. **No ordering.** Hashing scatters the keys, so there is no sorted
   traversal and there are no range queries.

## Files

| File | Contents |
|---|---|
| `blob.h` | the `Blob` type, nothing else |
| `kvblob.h` | the interface, the return codes, and the rules a caller must obey |
| `kvblob.c` | the store |
| `example.c` | the command line front end and the demo |
| `Makefile` | `make`, `make clean` |
