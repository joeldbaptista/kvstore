/*
 * example: a command line front end to kvblob, and a self-contained demo.
 *
 *	kvb FILE put KEY VALUE		store VALUE under KEY
 *	kvb FILE get KEY		write the value to the standard output
 *	kvb FILE len KEY		print the length of the value
 *	kvb FILE del KEY		remove the pair
 *	kvb FILE count			print how many pairs the store holds
 *	kvb FILE demo			run the demo described in demo() below
 *
 * Note where the memory comes from in getcmd(): the client asks for the
 * length, allocates its own buffer, has it filled, uses it and frees it.
 * The library allocates nothing on its behalf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blob.h"
#include "kvblob.h"

static Blob
str(const char *s)
{
	Blob b;

	b.len = strlen(s);
	b.buf = (unsigned char *)s;
	return b;
}

static Blob
bytes(const unsigned char *p, size_t n)
{
	Blob b;

	b.len = n;
	b.buf = (unsigned char *)p;
	return b;
}

static void
die(const char *what, int err)
{
	fprintf(stderr, "kvb: %s: %s\n", what, kvberrstr(err));
	exit(1);
}

static int
getcmd(KVblob *kv, const char *key)
{
	Blob k, v;
	size_t n;
	int e;

	k = str(key);
	if ((e = kvblen(kv, &k, &n)) != KVB_OK){
		if(e == KVB_ENOTFOUND){
			fprintf(stderr, "kvb: %s: %s\n", key, kvberrstr(e));
			return 1;
		}
		die("len", e);
	}
	v.len = n;
	v.buf = n > 0 ? malloc(n) : NULL;
	if (n > 0 && v.buf == NULL){
		fprintf(stderr, "kvb: out of memory\n");
		return 1;
	}
	if ((e = kvbget(kv, &k, &v)) != KVB_OK){
		free(v.buf);
		die("get", e);
	}
	if (v.len > 0)
		fwrite(v.buf, 1, v.len, stdout);
	putchar('\n');
	free(v.buf);
	return 0;
}

/* report prints one line about the value stored under key. */
static void
report(KVblob *kv, const char *key)
{
	unsigned char buf[64];
	Blob k, v;
	int e;

	k = str(key);
	v.len = sizeof buf;
	v.buf = buf;
	e = kvbget(kv, &k, &v);
	if (e == KVB_ENOTFOUND){
		printf("  %-10s absent\n", key);
		return;
	}
	if (e == KVB_ESMALL){
		printf("  %-10s %lu bytes, too long for a %lu byte buffer\n",
			key, (unsigned long)v.len, (unsigned long)sizeof buf);
		return;
	}
	if (e != KVB_OK)
		die("get", e);
	printf("  %-10s %lu bytes: ", key, (unsigned long)v.len);
	fwrite(buf, 1, v.len, stdout);
	putchar('\n');
}

static int
demo(const char *path)
{
	static unsigned char raw[] = { 0x00, 0xff, 0x41, 0x00, 0x42 };
	unsigned char big[5000];
	KVblob *kv;
	Blob k, v;
	size_t i, n;
	int e;

	printf("store %s\n\n", path);

	if ((kv = kvbopen(path)) == NULL){
		perror("kvbopen");
		return 1;
	}

	/* three pairs of plain text */
	printf("put three text pairs\n");
	k = str("colour"); v = str("blue");
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	k = str("city"); v = str("Lisbon");
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	k = str("empty"); v = bytes(NULL, 0);
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	report(kv, "colour");
	report(kv, "city");
	report(kv, "empty");

	/* a key and a value that are not text at all */
	printf("\nput a pair of raw bytes, key and value both holding NUL\n");
	k = bytes(raw, sizeof raw);
	v = bytes(raw, sizeof raw);
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	if ((e = kvblen(kv, &k, &n)) != KVB_OK) die("len", e);
	printf("  %lu byte key holds a %lu byte value\n",
		(unsigned long)sizeof raw, (unsigned long)n);

	/* a value longer than any stack buffer in report() */
	printf("\nput a 5000 byte value under \"big\"\n");
	for (i = 0; i < sizeof big; i++)
		big[i] = (unsigned char)('a' + i % 26);
	k = str("big");
	v = bytes(big, sizeof big);
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	report(kv, "big");

	/* replacing a value, shorter then longer */
	printf("\nreplace \"colour\" twice\n");
	k = str("colour"); v = str("red");
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	report(kv, "colour");
	k = str("colour"); v = str("a much longer colour name than before");
	if ((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	report(kv, "colour");

	/* enough pairs to force the bucket array to double */
	printf("\nput 200 numbered pairs, which grows the bucket array\n");
	for (i = 0; i < 200; i++){
		char kb[32], vb[32];

		sprintf(kb, "key%lu", (unsigned long)i);
		sprintf(vb, "value%lu", (unsigned long)i);
		k = str(kb); v = str(vb);
		if((e = kvbput(kv, &k, &v)) != KVB_OK) die("put", e);
	}
	if ((e = kvbcount(kv, &n)) != KVB_OK) die("count", e);
	printf("  %lu pairs stored\n", (unsigned long)n);
	report(kv, "key199");

	printf("\ndelete \"city\"\n");
	k = str("city");
	if ((e = kvbdel(kv, &k)) != KVB_OK) die("del", e);
	report(kv, "city");
	if ((e = kvbdel(kv, &k)) != KVB_ENOTFOUND){
		fprintf(stderr, "kvb: deleting twice should not succeed\n");
		return 1;
	}

	if ((e = kvbclose(kv)) != KVB_OK) die("close", e);

	/* the point of the library: it was all on the disk, not in memory */
	printf("\nclose, then open the same file again\n");
	if ((kv = kvbopen(path)) == NULL){
		perror("kvbopen");
		return 1;
	}
	if ((e = kvbcount(kv, &n)) != KVB_OK) die("count", e);
	printf("  %lu pairs still there\n", (unsigned long)n);
	report(kv, "colour");
	report(kv, "city");
	report(kv, "key42");
	k = bytes(raw, sizeof raw);
	if ((e = kvblen(kv, &k, &n)) != KVB_OK) die("len", e);
	printf("  raw key still holds %lu bytes\n", (unsigned long)n);
	if ((e = kvbclose(kv)) != KVB_OK) die("close", e);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: kvb FILE put KEY VALUE\n"
		"       kvb FILE get|len|del KEY\n"
		"       kvb FILE count\n"
		"       kvb FILE demo\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	KVblob *kv;
	Blob k, v;
	const char *path, *cmd;
	size_t n;
	int e, rc;

	if(argc < 3)
		usage();
	path = argv[1];
	cmd = argv[2];

	if (strcmp(cmd, "demo") == 0)
		return demo(path);

	if((kv = kvbopen(path)) == NULL){
		perror("kvbopen");
		return 1;
	}
	rc = 0;
	if (strcmp(cmd, "put") == 0 && argc == 5){
		k = str(argv[3]);
		v = str(argv[4]);
		if((e = kvbput(kv, &k, &v)) != KVB_OK)
			die("put", e);
	}else if (strcmp(cmd, "get") == 0 && argc == 4){
		rc = getcmd(kv, argv[3]);
	}else if (strcmp(cmd, "len") == 0 && argc == 4){
		k = str(argv[3]);
		if((e = kvblen(kv, &k, &n)) == KVB_ENOTFOUND){
			fprintf(stderr, "kvb: %s: %s\n", argv[3],
				kvberrstr(e));
			rc = 1;
		} else if (e != KVB_OK)
			die("len", e);
		else
			printf("%lu\n", (unsigned long)n);
	}else if (strcmp(cmd, "del") == 0 && argc == 4){
		k = str(argv[3]);
		if ((e = kvbdel(kv, &k)) == KVB_ENOTFOUND){
			fprintf(stderr, "kvb: %s: %s\n", argv[3],
				kvberrstr(e));
			rc = 1;
		} else if (e != KVB_OK)
			die("del", e);
	} else if (strcmp(cmd, "count") == 0 && argc == 3){
		if ((e = kvbcount(kv, &n)) != KVB_OK)
			die("count", e);
		printf("%lu\n", (unsigned long)n);
	} else {
		kvbclose(kv);
		usage();
	}
	if ((e = kvbclose(kv)) != KVB_OK)
		die("close", e);
	return rc;
}
