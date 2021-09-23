/*
 * Copyright (c) 2011, Google Inc.
 */
#ifndef BULK_CHECKIN_H
#define BULK_CHECKIN_H

#include "cache.h"

int fsync_and_close_loose_object_bulk_checkin(int fd, const char *tmpfile,
					      const char *filename, time_t mtime);

int index_bulk_checkin(struct object_id *oid,
		       int fd, size_t size, enum object_type type,
		       const char *path, unsigned flags);

void plug_bulk_checkin(void);
void unplug_bulk_checkin(struct lock_file *);

#endif
