/*
 * Copyright (c) 2011, Google Inc.
 */
#include "cache.h"
#include "bulk-checkin.h"
#include "lockfile.h"
#include "repository.h"
#include "csum-file.h"
#include "pack.h"
#include "strbuf.h"
#include "string-list.h"
#include "packfile.h"
#include "object-store.h"

static int bulk_checkin_plugged;

static struct string_list bulk_fsync_state = STRING_LIST_INIT_DUP;

static struct bulk_checkin_state {
	char *pack_tmp_name;
	struct hashfile *f;
	off_t offset;
	struct pack_idx_option pack_idx_opts;

	struct pack_idx_entry **written;
	uint32_t alloc_written;
	uint32_t nr_written;
} bulk_checkin_state;

static void finish_tmp_packfile(struct strbuf *basename,
				const char *pack_tmp_name,
				struct pack_idx_entry **written_list,
				uint32_t nr_written,
				struct pack_idx_option *pack_idx_opts,
				unsigned char hash[])
{
	char *idx_tmp_name = NULL;

	stage_tmp_packfiles(basename, pack_tmp_name, written_list, nr_written,
			    pack_idx_opts, hash, &idx_tmp_name);
	rename_tmp_packfile_idx(basename, &idx_tmp_name);

	free(idx_tmp_name);
}

static void finish_bulk_checkin(struct bulk_checkin_state *state)
{
	unsigned char hash[GIT_MAX_RAWSZ];
	struct strbuf packname = STRBUF_INIT;
	int i;

	if (!state->f)
		return;

	if (state->nr_written == 0) {
		close(state->f->fd);
		unlink(state->pack_tmp_name);
		goto clear_exit;
	} else if (state->nr_written == 1) {
		finalize_hashfile(state->f, hash, CSUM_HASH_IN_STREAM | CSUM_FSYNC | CSUM_CLOSE);
	} else {
		int fd = finalize_hashfile(state->f, hash, 0);
		fixup_pack_header_footer(fd, hash, state->pack_tmp_name,
					 state->nr_written, hash,
					 state->offset);
		close(fd);
	}

	strbuf_addf(&packname, "%s/pack/pack-%s.", get_object_directory(),
		    hash_to_hex(hash));
	finish_tmp_packfile(&packname, state->pack_tmp_name,
			    state->written, state->nr_written,
			    &state->pack_idx_opts, hash);
	for (i = 0; i < state->nr_written; i++)
		free(state->written[i]);

clear_exit:
	free(state->written);
	memset(state, 0, sizeof(*state));

	strbuf_release(&packname);
	/* Make objects we just wrote available to ourselves */
	reprepare_packed_git(the_repository);
}

static void do_sync_and_rename(struct string_list *fsync_state, struct lock_file *lock_file)
{
	if (fsync_state->nr) {
		struct string_list_item *rename;

		/*
		 * Issue a full hardware flush against the lock file to ensure
		 * that all objects are durable before any renames occur.
		 * The code in fsync_and_close_loose_object_bulk_checkin has
		 * already ensured that writeout has occurred, but it has not
		 * flushed any writeback cache in the storage hardware.
		 */
		fsync_or_die(get_lock_file_fd(lock_file), get_lock_file_path(lock_file));

		for_each_string_list_item(rename, fsync_state) {
			const char *src = rename->string;
			const char *dst = rename->util;

			if (finalize_object_file(src, dst))
				die_errno(_("could not rename '%s' to '%s'"), src, dst);
		}

		string_list_clear(fsync_state, 1);
	}
}

static int already_written(struct bulk_checkin_state *state, struct object_id *oid)
{
	int i;

	/* The object may already exist in the repository */
	if (has_object_file(oid))
		return 1;

	/* Might want to keep the list sorted */
	for (i = 0; i < state->nr_written; i++)
		if (oideq(&state->written[i]->oid, oid))
			return 1;

	/* This is a new object we need to keep */
	return 0;
}

/*
 * Read the contents from fd for size bytes, streaming it to the
 * packfile in state while updating the hash in ctx. Signal a failure
 * by returning a negative value when the resulting pack would exceed
 * the pack size limit and this is not the first object in the pack,
 * so that the caller can discard what we wrote from the current pack
 * by truncating it and opening a new one. The caller will then call
 * us again after rewinding the input fd.
 *
 * The already_hashed_to pointer is kept untouched by the caller to
 * make sure we do not hash the same byte when we are called
 * again. This way, the caller does not have to checkpoint its hash
 * status before calling us just in case we ask it to call us again
 * with a new pack.
 */
static int stream_to_pack(struct bulk_checkin_state *state,
			  git_hash_ctx *ctx, off_t *already_hashed_to,
			  int fd, size_t size, enum object_type type,
			  const char *path, unsigned flags)
{
	git_zstream s;
	unsigned char ibuf[16384];
	unsigned char obuf[16384];
	unsigned hdrlen;
	int status = Z_OK;
	int write_object = (flags & HASH_WRITE_OBJECT);
	off_t offset = 0;

	git_deflate_init(&s, pack_compression_level);

	hdrlen = encode_in_pack_object_header(obuf, sizeof(obuf), type, size);
	s.next_out = obuf + hdrlen;
	s.avail_out = sizeof(obuf) - hdrlen;

	while (status != Z_STREAM_END) {
		if (size && !s.avail_in) {
			ssize_t rsize = size < sizeof(ibuf) ? size : sizeof(ibuf);
			ssize_t read_result = read_in_full(fd, ibuf, rsize);
			if (read_result < 0)
				die_errno("failed to read from '%s'", path);
			if (read_result != rsize)
				die("failed to read %d bytes from '%s'",
				    (int)rsize, path);
			offset += rsize;
			if (*already_hashed_to < offset) {
				size_t hsize = offset - *already_hashed_to;
				if (rsize < hsize)
					hsize = rsize;
				if (hsize)
					the_hash_algo->update_fn(ctx, ibuf, hsize);
				*already_hashed_to = offset;
			}
			s.next_in = ibuf;
			s.avail_in = rsize;
			size -= rsize;
		}

		status = git_deflate(&s, size ? 0 : Z_FINISH);

		if (!s.avail_out || status == Z_STREAM_END) {
			if (write_object) {
				size_t written = s.next_out - obuf;

				/* would we bust the size limit? */
				if (state->nr_written &&
				    pack_size_limit_cfg &&
				    pack_size_limit_cfg < state->offset + written) {
					git_deflate_abort(&s);
					return -1;
				}

				hashwrite(state->f, obuf, written);
				state->offset += written;
			}
			s.next_out = obuf;
			s.avail_out = sizeof(obuf);
		}

		switch (status) {
		case Z_OK:
		case Z_BUF_ERROR:
		case Z_STREAM_END:
			continue;
		default:
			die("unexpected deflate failure: %d", status);
		}
	}
	git_deflate_end(&s);
	return 0;
}

/* Lazily create backing packfile for the state */
static void prepare_to_stream(struct bulk_checkin_state *state,
			      unsigned flags)
{
	if (!(flags & HASH_WRITE_OBJECT) || state->f)
		return;

	state->f = create_tmp_packfile(&state->pack_tmp_name);
	reset_pack_idx_option(&state->pack_idx_opts);

	/* Pretend we are going to write only one object */
	state->offset = write_pack_header(state->f, 1);
	if (!state->offset)
		die_errno("unable to write pack header");
}

static int deflate_to_pack(struct bulk_checkin_state *state,
			   struct object_id *result_oid,
			   int fd, size_t size,
			   enum object_type type, const char *path,
			   unsigned flags)
{
	off_t seekback, already_hashed_to;
	git_hash_ctx ctx;
	unsigned char obuf[16384];
	unsigned header_len;
	struct hashfile_checkpoint checkpoint = {0};
	struct pack_idx_entry *idx = NULL;

	seekback = lseek(fd, 0, SEEK_CUR);
	if (seekback == (off_t) -1)
		return error("cannot find the current offset");

	header_len = xsnprintf((char *)obuf, sizeof(obuf), "%s %" PRIuMAX,
			       type_name(type), (uintmax_t)size) + 1;
	the_hash_algo->init_fn(&ctx);
	the_hash_algo->update_fn(&ctx, obuf, header_len);

	/* Note: idx is non-NULL when we are writing */
	if ((flags & HASH_WRITE_OBJECT) != 0)
		CALLOC_ARRAY(idx, 1);

	already_hashed_to = 0;

	while (1) {
		prepare_to_stream(state, flags);
		if (idx) {
			hashfile_checkpoint(state->f, &checkpoint);
			idx->offset = state->offset;
			crc32_begin(state->f);
		}
		if (!stream_to_pack(state, &ctx, &already_hashed_to,
				    fd, size, type, path, flags))
			break;
		/*
		 * Writing this object to the current pack will make
		 * it too big; we need to truncate it, start a new
		 * pack, and write into it.
		 */
		if (!idx)
			BUG("should not happen");
		hashfile_truncate(state->f, &checkpoint);
		state->offset = checkpoint.offset;
		finish_bulk_checkin(state);
		if (lseek(fd, seekback, SEEK_SET) == (off_t) -1)
			return error("cannot seek back");
	}
	the_hash_algo->final_oid_fn(result_oid, &ctx);
	if (!idx)
		return 0;

	idx->crc32 = crc32_end(state->f);
	if (already_written(state, result_oid)) {
		hashfile_truncate(state->f, &checkpoint);
		state->offset = checkpoint.offset;
		free(idx);
	} else {
		oidcpy(&idx->oid, result_oid);
		ALLOC_GROW(state->written,
			   state->nr_written + 1,
			   state->alloc_written);
		state->written[state->nr_written++] = idx;
	}
	return 0;
}

static void add_rename_bulk_checkin(struct string_list *fsync_state,
				    const char *src, const char *dst)
{
	string_list_insert(fsync_state, src)->util = xstrdup(dst);
}

int fsync_and_close_loose_object_bulk_checkin(int fd, const char *tmpfile,
					      const char *filename, time_t mtime)
{
	int do_finalize = 1;
	int ret = 0;

	if (fsync_object_files != FSYNC_OBJECT_FILES_OFF) {
		/*
		 * If we have a plugged bulk checkin, we issue a call that
		 * cleans the filesystem page cache but avoids a hardware flush
		 * command. Later on we will issue a single hardware flush
		 * before renaming files as part of do_sync_and_rename.
		 */
		if (bulk_checkin_plugged &&
		    fsync_object_files == FSYNC_OBJECT_FILES_BATCH &&
		    git_fsync(fd, FSYNC_WRITEOUT_ONLY) >= 0) {
			add_rename_bulk_checkin(&bulk_fsync_state, tmpfile, filename);
			do_finalize = 0;

		} else {
			fsync_or_die(fd, "loose object file");
		}
	}

	if (close(fd))
		die_errno(_("error when closing loose object file"));

	if (mtime) {
		struct utimbuf utb;
		utb.actime = mtime;
		utb.modtime = mtime;
		if (utime(tmpfile, &utb) < 0)
			warning_errno(_("failed utime() on %s"), tmpfile);
	}

	if (do_finalize)
		ret = finalize_object_file(tmpfile, filename);

	return ret;
}

int index_bulk_checkin(struct object_id *oid,
		       int fd, size_t size, enum object_type type,
		       const char *path, unsigned flags)
{
	int status = deflate_to_pack(&bulk_checkin_state, oid, fd, size, type,
				     path, flags);
	if (!bulk_checkin_plugged)
		finish_bulk_checkin(&bulk_checkin_state);
	return status;
}

void plug_bulk_checkin(void)
{
	assert(!bulk_checkin_plugged);
	bulk_checkin_plugged = 1;
}

void unplug_bulk_checkin(struct lock_file *lock_file)
{
	assert(bulk_checkin_plugged);
	bulk_checkin_plugged = 0;
	if (bulk_checkin_state.f)
		finish_bulk_checkin(&bulk_checkin_state);

	do_sync_and_rename(&bulk_fsync_state, lock_file);
}
