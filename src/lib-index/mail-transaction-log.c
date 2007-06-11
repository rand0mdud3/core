/* Copyright (C) 2003-2007 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "buffer.h"
#include "file-dotlock.h"
#include "nfs-workarounds.h"
#include "close-keep-errno.h"
#include "mmap-util.h"
#include "mail-index-private.h"
#include "mail-transaction-log-private.h"

#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

/* this lock should never exist for a long time.. */
#define LOG_DOTLOCK_TIMEOUT 60
#define LOG_DOTLOCK_STALE_TIMEOUT 60

#define MAIL_TRANSACTION_LOG_SUFFIX ".log"
#define LOG_NEW_DOTLOCK_SUFFIX ".newlock"

static void
mail_transaction_log_set_head(struct mail_transaction_log *log,
			      struct mail_transaction_log_file *file)
{
	i_assert(log->head != file);

	file->refcount++;
	log->head = file;
}

struct mail_transaction_log *
mail_transaction_log_alloc(struct mail_index *index)
{
	struct mail_transaction_log *log;

	log = i_new(struct mail_transaction_log, 1);
	log->index = index;

	log->dotlock_settings.use_excl_lock = index->use_excl_dotlocks;
	log->dotlock_settings.timeout = LOG_DOTLOCK_TIMEOUT;
	log->dotlock_settings.stale_timeout = LOG_DOTLOCK_STALE_TIMEOUT;

	log->new_dotlock_settings = log->dotlock_settings;
	log->new_dotlock_settings.lock_suffix = LOG_NEW_DOTLOCK_SUFFIX;

	return log;
}

int mail_transaction_log_open(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file;
	const char *path;
	int ret;

	if (log->open_file != NULL)
		mail_transaction_log_file_free(&log->open_file);

	if (MAIL_INDEX_IS_IN_MEMORY(log->index))
		return 0;

	path = t_strconcat(log->index->filepath,
			   MAIL_TRANSACTION_LOG_SUFFIX, NULL);

	file = mail_transaction_log_file_alloc(log, path);
	if ((ret = mail_transaction_log_file_open(file, FALSE)) <= 0) {
		/* leave the file for _create() */
		log->open_file = file;
		return ret;
	}

	mail_transaction_log_set_head(log, file);
	return 1;
}

int mail_transaction_log_create(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file;
	const char *path;

	if (MAIL_INDEX_IS_IN_MEMORY(log->index)) {
		file = mail_transaction_log_file_alloc_in_memory(log);
		mail_transaction_log_set_head(log, file);
		return 0;
	}

	path = t_strconcat(log->index->filepath,
			   MAIL_TRANSACTION_LOG_SUFFIX, NULL);

	file = mail_transaction_log_file_alloc(log, path);

	if (log->open_file != NULL) {
		/* remember what file we tried to open. if someone else created
		   a new file, use it instead of recreating it */
		file->st_ino = log->open_file->st_ino;
		file->st_dev = log->open_file->st_dev;
		file->last_size = log->open_file->last_size;
		file->last_mtime = log->open_file->last_mtime;
		mail_transaction_log_file_free(&log->open_file);
	}

	if (mail_transaction_log_file_create(file) < 0)
		mail_transaction_log_file_free(&file);

	mail_transaction_log_set_head(log, file);
	return 1;
}

void mail_transaction_log_close(struct mail_transaction_log *log)
{
	mail_transaction_log_views_close(log);

	if (log->open_file != NULL)
		mail_transaction_log_file_free(&log->open_file);
	if (log->head != NULL)
		log->head->refcount--;
	mail_transaction_logs_clean(log);
	i_assert(log->files == NULL);
}

void mail_transaction_log_free(struct mail_transaction_log **_log)
{
	struct mail_transaction_log *log = *_log;

	*_log = NULL;

	mail_transaction_log_close(log);
	log->index->log = NULL;
	i_free(log);
}

int mail_transaction_log_move_to_memory(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file = log->head;

	if (file == NULL || MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file))
		return 0;

	/* read the whole file to memory. we might currently be appending
	   data into it, so we want to read it up to end of file */
        file->buffer_offset = 0;

	if (file->buffer != NULL) {
		buffer_free(file->buffer);
		file->buffer = NULL;
	}

	if (file->mmap_base != NULL) {
		if (munmap(file->mmap_base, file->mmap_size) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "munmap()");
		}
		file->mmap_base = NULL;
	}

	if (mail_transaction_log_file_read(file, 0) <= 0)
		return -1;

	/* after we've read the file into memory, make it into in-memory
	   log file */
	if (close(file->fd) < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "close()");
	}
	file->fd = -1;
	return 0;
}

void mail_transaction_logs_clean(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file, *next;

	for (file = log->files; file != NULL; file = next) {
		next = file->next;

		i_assert(file->refcount >= 0);
		if (file->refcount == 0)
			mail_transaction_log_file_free(&file);
	}
}

#define LOG_WANT_ROTATE(file) \
	(((file)->sync_offset > MAIL_TRANSACTION_LOG_ROTATE_MIN_SIZE && \
	  (time_t)(file)->hdr.create_stamp < \
	   ioloop_time - MAIL_TRANSACTION_LOG_ROTATE_TIME) || \
	 ((file)->sync_offset > MAIL_TRANSACTION_LOG_ROTATE_MAX_SIZE))

bool mail_transaction_log_want_rotate(struct mail_transaction_log *log)
{
	return LOG_WANT_ROTATE(log->head);
}

int mail_transaction_log_rotate(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file;
	const char *path = log->head->filepath;
	struct stat st;

	i_assert(log->head->locked);

	if (MAIL_INDEX_IS_IN_MEMORY(log->index))
		file = mail_transaction_log_file_alloc_in_memory(log);
	else {
                /* we're locked, we shouldn't need to worry about ESTALE
                   problems in here. */
		if (fstat(log->head->fd, &st) < 0) {
			mail_index_file_set_syscall_error(log->index,
				file->filepath, "fstat()");
			return -1;
		}

		file = mail_transaction_log_file_alloc(log, path);

		file->st_dev = st.st_dev;
		file->st_ino = st.st_ino;
		file->last_mtime = st.st_mtime;
		file->last_size = st.st_size;

		if (mail_transaction_log_file_create(file) < 0) {
			mail_transaction_log_file_free(&file);
			return -1;
		}
	}

	if (--log->head->refcount == 0)
		mail_transaction_logs_clean(log);
	else
		mail_transaction_log_file_unlock(log->head);
	mail_transaction_log_set_head(log, file);
	return 0;
}

static int mail_transaction_log_refresh(struct mail_transaction_log *log)
{
        struct mail_transaction_log_file *file;
	struct stat st;
	const char *path;

	i_assert(log->head != NULL);

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(log->head))
		return 0;

	path = t_strconcat(log->index->filepath,
			   MAIL_TRANSACTION_LOG_SUFFIX, NULL);
	if (nfs_safe_stat(path, &st) < 0) {
		if (errno != ENOENT) {
			mail_index_file_set_syscall_error(log->index, path,
							  "stat()");
			return -1;
		}
		return -1;
	} else {
		if (log->head->st_ino == st.st_ino &&
		    CMP_DEV_T(log->head->st_dev, st.st_dev)) {
			/* same file */
			return 0;
		}
	}

	file = mail_transaction_log_file_alloc(log, path);
	if (mail_transaction_log_file_open(file, FALSE) <= 0) {
		mail_transaction_log_file_free(&file);
		return -1;
	}

	i_assert(!file->locked);

	if (--log->head->refcount == 0)
		mail_transaction_logs_clean(log);
	mail_transaction_log_set_head(log, file);
	return 0;
}

void mail_transaction_log_get_mailbox_sync_pos(struct mail_transaction_log *log,
					       uint32_t *file_seq_r,
					       uoff_t *file_offset_r)
{
	*file_seq_r = log->head->hdr.file_seq;
	*file_offset_r = log->head->mailbox_sync_max_offset;
}

void mail_transaction_log_set_mailbox_sync_pos(struct mail_transaction_log *log,
					       uint32_t file_seq,
					       uoff_t file_offset)
{
	i_assert(file_seq == log->head->hdr.file_seq);
	i_assert(file_offset >= log->head->mailbox_sync_saved_offset);

	if (file_offset >= log->head->mailbox_sync_max_offset)
		log->head->mailbox_sync_max_offset = file_offset;
}

int mail_transaction_log_find_file(struct mail_transaction_log *log,
				   uint32_t file_seq,
				   struct mail_transaction_log_file **file_r)
{
	struct mail_transaction_log_file *file;
	const char *path;
	int ret;

	if (file_seq > log->head->hdr.file_seq) {
		/* see if the .log file has been recreated */
		if (log->head->locked) {
			/* transaction log is locked. there's no way a newer
			   file exists. */
			return 0;
		}

		if (mail_transaction_log_refresh(log) < 0)
			return -1;
		if (file_seq > log->head->hdr.file_seq)
			return 0;
	}

	for (file = log->files; file != NULL; file = file->next) {
		if (file->hdr.file_seq == file_seq) {
			*file_r = file;
			return 1;
		}
	}

	if (MAIL_INDEX_IS_IN_MEMORY(log->index))
		return 0;

	/* see if we have it in log.2 file */
	path = t_strconcat(log->index->filepath,
			   MAIL_TRANSACTION_LOG_SUFFIX".2", NULL);
	file = mail_transaction_log_file_alloc(log, path);
	if ((ret = mail_transaction_log_file_open(file, TRUE)) <= 0)
		return ret;

	/* but is it what we expected? */
	if (file->hdr.file_seq != file_seq)
		return 0;

	*file_r = file;
	return 1;
}

int mail_transaction_log_lock_head(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file;
	int ret = 0;

	/* we want to get the head file locked. this is a bit racy,
	   since by the time we have it locked a new log file may have been
	   created.

	   creating new log file requires locking the head file, so if we
	   can lock it and don't see another file, we can be sure no-one is
	   creating a new log at the moment */

	for (;;) {
		file = log->head;
		if (mail_transaction_log_file_lock(file) < 0)
			return -1;

		file->refcount++;
		ret = mail_transaction_log_refresh(log);
		if (--file->refcount == 0) {
			mail_transaction_logs_clean(log);
			file = NULL;
		}

		if (ret == 0 && log->head == file) {
			/* success */
			break;
		}

		if (file != NULL)
			mail_transaction_log_file_unlock(file);

		if (ret < 0)
			break;

		/* try again */
	}

	return ret;
}

int mail_transaction_log_sync_lock(struct mail_transaction_log *log,
				   uint32_t *file_seq_r, uoff_t *file_offset_r)
{
	i_assert(!log->index->log_locked);

	if (mail_transaction_log_lock_head(log) < 0)
		return -1;

	/* update sync_offset */
	if (mail_transaction_log_file_map(log->head, log->head->sync_offset,
					  (uoff_t)-1) < 0) {
		mail_transaction_log_file_unlock(log->head);
		return -1;
	}

	log->index->log_locked = TRUE;
	*file_seq_r = log->head->hdr.file_seq;
	*file_offset_r = log->head->sync_offset;
	return 0;
}

void mail_transaction_log_sync_unlock(struct mail_transaction_log *log)
{
	i_assert(log->index->log_locked);

	log->index->log_locked = FALSE;
	mail_transaction_log_file_unlock(log->head);
}

void mail_transaction_log_get_head(struct mail_transaction_log *log,
				   uint32_t *file_seq_r, uoff_t *file_offset_r)
{
	i_assert(log->index->log_locked);

	*file_seq_r = log->head->hdr.file_seq;
	*file_offset_r = log->head->sync_offset;
}

bool mail_transaction_log_is_head_prev(struct mail_transaction_log *log,
				       uint32_t file_seq, uoff_t file_offset)
{
	return log->head->hdr.prev_file_seq == file_seq &&
		log->head->hdr.prev_file_offset == file_offset;
}
