#ifndef BLOB_H
#define BLOB_H

#include <stddef.h>

typedef struct Blob Blob;

struct Blob {
	size_t len;
	unsigned char *buf;
};

#endif
