/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "iobuffer.h"
#include "temp-string.h"
#include "mmap-util.h"
#include "message-parser.h"
#include "message-part-serialize.h"
#include "message-size.h"
#include "imap-bodystructure.h"
#include "imap-envelope.h"
#include "imap-message-cache.h"

#include <unistd.h>

/* It's not very useful to cache lots of messages, as they're mostly wanted
   just once. The biggest reason for this cache to exist is to get just the
   latest message. */
#define MAX_CACHED_MESSAGES 16

#define DEFAULT_MESSAGE_POOL_SIZE 4096

typedef struct _CachedMessage CachedMessage;

struct _CachedMessage {
	CachedMessage *next;

	Pool pool;
	unsigned int uid;

	MessagePart *part;
	MessageSize *hdr_size;
	MessageSize *body_size;
	MessageSize *partial_size;

	char *cached_body;
	char *cached_bodystructure;
	char *cached_envelope;

	MessagePartEnvelopeData *envelope;
};

struct _ImapMessageCache {
	ImapMessageCacheIface *iface;

	CachedMessage *messages;
	int messages_count;

	CachedMessage *open_msg;
	IOBuffer *open_inbuf;

	void *context;
};

ImapMessageCache *imap_msgcache_alloc(ImapMessageCacheIface *iface)
{
	ImapMessageCache *cache;

	cache = i_new(ImapMessageCache, 1);
	cache->iface = iface;
	return cache;
}

static void cached_message_free(CachedMessage *msg)
{
	pool_unref(msg->pool);
}

void imap_msgcache_clear(ImapMessageCache *cache)
{
	CachedMessage *next;

	imap_msgcache_close(cache);

	while (cache->messages != NULL) {
		next = cache->messages->next;
		cached_message_free(cache->messages);
		cache->messages = next;
	}
}

void imap_msgcache_free(ImapMessageCache *cache)
{
	imap_msgcache_clear(cache);
	i_free(cache);
}

static CachedMessage *cache_new(ImapMessageCache *cache, unsigned int uid)
{
	CachedMessage *msg, **msgp;
	Pool pool;

	if (cache->messages_count < MAX_CACHED_MESSAGES)
		cache->messages_count++;
	else {
		/* remove the last message from cache */
                msgp = &cache->messages;
		while ((*msgp)->next != NULL)
			msgp = &(*msgp)->next;

		cached_message_free(*msgp);
		*msgp = NULL;
	}

	pool = pool_create("CachedMessage", DEFAULT_MESSAGE_POOL_SIZE, FALSE);

	msg = p_new(pool, CachedMessage, 1);
	msg->pool = pool;
	msg->uid = uid;

	msg->next = cache->messages;
	cache->messages = msg;
	return msg;
}

static CachedMessage *cache_open_or_create(ImapMessageCache *cache,
					   unsigned int uid)
{
	CachedMessage **pos, *msg;

	pos = &cache->messages;
	for (; *pos != NULL; pos = &(*pos)->next) {
		if ((*pos)->uid == uid)
			break;
	}

	if (*pos == NULL) {
		/* not found, add it */
		msg = cache_new(cache, uid);
	} else if (*pos != cache->messages) {
		/* move it to first in list */
		msg = *pos;
		*pos = msg->next;

		msg->next = cache->messages;
		cache->messages = msg;
	} else {
		msg = *pos;
	}

	return msg;
}

static void parse_envelope_header(MessagePart *part,
				  const char *name, size_t name_len,
				  const char *value, size_t value_len,
				  void *context)
{
	CachedMessage *msg = context;

	if (part == NULL || part->parent == NULL) {
		/* parse envelope headers if we're at the root message part */
		imap_envelope_parse_header(msg->pool, &msg->envelope,
					   t_strndup(name, name_len),
					   value, value_len);
	}
}

static int imap_msgcache_get_inbuf(ImapMessageCache *cache, uoff_t offset)
{
	if (cache->open_inbuf == NULL)
		cache->open_inbuf = cache->iface->open_mail(cache->context);
	else if (offset < cache->open_inbuf->offset) {
		/* need to rewind */
		cache->open_inbuf =
			cache->iface->inbuf_rewind(cache->open_inbuf,
						   cache->context);
	}

	if (cache->open_inbuf == NULL)
		return FALSE;

	i_assert(offset >= cache->open_inbuf->offset);

	io_buffer_skip(cache->open_inbuf, offset - cache->open_inbuf->offset);
	return TRUE;
}

static void msg_get_part(ImapMessageCache *cache)
{
	if (cache->open_msg->part == NULL) {
		cache->open_msg->part =
			cache->iface->get_cached_parts(cache->open_msg->pool,
						       cache->context);
	}
}

/* Caches the fields for given message if possible */
static void cache_fields(ImapMessageCache *cache, ImapCacheField fields)
{
        CachedMessage *msg;
	const char *value;

	msg = cache->open_msg;

	t_push();
	if ((fields & IMAP_CACHE_BODY) && msg->cached_body == NULL) {
		value = cache->iface->get_cached_field(IMAP_CACHE_BODY,
						       cache->context);
		if (value == NULL && imap_msgcache_get_inbuf(cache, 0)) {
			msg_get_part(cache);

			value = imap_part_get_bodystructure(msg->pool,
							    &msg->part,
							    cache->open_inbuf,
							    FALSE);
		}
		msg->cached_body = p_strdup(msg->pool, value);
	}

	if ((fields & IMAP_CACHE_BODYSTRUCTURE) &&
	    msg->cached_bodystructure == NULL) {
		value = cache->iface->get_cached_field(IMAP_CACHE_BODYSTRUCTURE,
						       cache->context);
		if (value == NULL && imap_msgcache_get_inbuf(cache, 0)) {
			msg_get_part(cache);

			value = imap_part_get_bodystructure(msg->pool,
							    &msg->part,
							    cache->open_inbuf,
							    TRUE);
		}
		msg->cached_bodystructure = p_strdup(msg->pool, value);
	}

	if ((fields & IMAP_CACHE_ENVELOPE) && msg->cached_envelope == NULL) {
		value = cache->iface->get_cached_field(IMAP_CACHE_ENVELOPE,
						       cache->context);
		if (value == NULL) {
			if (msg->envelope == NULL &&
			    imap_msgcache_get_inbuf(cache, 0)) {
				/* envelope isn't parsed yet, do it. header
				   size is calculated anyway so save it */
				if (msg->hdr_size == NULL) {
					msg->hdr_size = p_new(msg->pool,
							      MessageSize, 1);
				}

				message_parse_header(NULL, cache->open_inbuf,
						     msg->hdr_size,
						     parse_envelope_header,
						     msg);
			}

			value = imap_envelope_get_part_data(msg->envelope);
		}

		if (value != NULL)
			msg->cached_envelope = p_strdup(msg->pool, value);
	}

	if ((fields & IMAP_CACHE_MESSAGE_BODY_SIZE) && msg->body_size == NULL) {
		/* we don't have body size. and since we're already going
		   to scan the whole message body, we might as well build
		   the MessagePart. */
                fields |= IMAP_CACHE_MESSAGE_PART;
	}

	if (fields & IMAP_CACHE_MESSAGE_PART) {
		msg_get_part(cache);

		if (msg->part == NULL && imap_msgcache_get_inbuf(cache, 0)) {
			/* we need to parse the message */
			MessageHeaderFunc func;

			if ((fields & IMAP_CACHE_ENVELOPE) &&
			    msg->cached_envelope == NULL) {
				/* we need envelope too, fill the info
				   while parsing headers */
				func = parse_envelope_header;
			} else {
				func = NULL;
			}

			msg->part = message_parse(msg->pool, cache->open_inbuf,
						  func, msg);
		}
	}

	if ((fields & IMAP_CACHE_MESSAGE_BODY_SIZE) && msg->body_size == NULL) {
		i_assert(msg->part != NULL);

		msg->body_size = p_new(msg->pool, MessageSize, 1);
		if (msg->hdr_size == NULL)
			msg->hdr_size = p_new(msg->pool, MessageSize, 1);

		*msg->hdr_size = msg->part->header_size;
		*msg->body_size = msg->part->body_size;
	}

	if ((fields & IMAP_CACHE_MESSAGE_HDR_SIZE) && msg->hdr_size == NULL) {
		msg_get_part(cache);

		msg->hdr_size = p_new(msg->pool, MessageSize, 1);
		if (msg->part != NULL) {
			/* easy, get it from root part */
			*msg->hdr_size = msg->part->header_size;
		} else {
			/* need to do some light parsing */
			if (imap_msgcache_get_inbuf(cache, 0)) {
				message_get_header_size(cache->open_inbuf,
							msg->hdr_size);
			}
		}
	}

	t_pop();
}

void imap_msgcache_open(ImapMessageCache *cache, unsigned int uid,
			ImapCacheField fields,
			uoff_t virtual_header_size, uoff_t virtual_body_size,
			void *context)
{
	CachedMessage *msg;

	msg = cache_open_or_create(cache, uid);
	if (cache->open_msg != msg) {
		imap_msgcache_close(cache);

		cache->open_msg = msg;
		cache->context = context;
	}

	if (virtual_header_size != 0 && msg->hdr_size == NULL) {
		/* physical size == virtual size */
		msg->hdr_size = p_new(msg->pool, MessageSize, 1);
		msg->hdr_size->physical_size = msg->hdr_size->virtual_size =
			virtual_header_size;
	}

	if (virtual_body_size != 0 && msg->body_size == NULL) {
		/* physical size == virtual size */
		msg->body_size = p_new(msg->pool, MessageSize, 1);
		msg->body_size->physical_size = msg->body_size->virtual_size =
			virtual_body_size;
	}

	cache_fields(cache, fields);
}

void imap_msgcache_close(ImapMessageCache *cache)
{
	if (cache->open_inbuf != NULL) {
		io_buffer_unref(cache->open_inbuf);
		cache->open_inbuf = NULL;
	}

	cache->open_msg = NULL;
	cache->context = NULL;
}

const char *imap_msgcache_get(ImapMessageCache *cache, ImapCacheField field)
{
	CachedMessage *msg;

	i_assert(cache->open_msg != NULL);

	msg = cache->open_msg;
	switch (field) {
	case IMAP_CACHE_BODY:
		if (msg->cached_body == NULL)
			cache_fields(cache, field);
		return msg->cached_body;
	case IMAP_CACHE_BODYSTRUCTURE:
		if (msg->cached_bodystructure == NULL)
			cache_fields(cache, field);
		return msg->cached_bodystructure;
	case IMAP_CACHE_ENVELOPE:
		if (msg->cached_envelope == NULL)
			cache_fields(cache, field);
		return msg->cached_envelope;
	default:
		i_assert(0);
	}

	return NULL;
}

MessagePart *imap_msgcache_get_parts(ImapMessageCache *cache)
{
	i_assert(cache->open_msg != NULL);

	if (cache->open_msg->part == NULL)
		cache_fields(cache, IMAP_CACHE_MESSAGE_PART);
	return cache->open_msg->part;
}

int imap_msgcache_get_rfc822(ImapMessageCache *cache, IOBuffer **inbuf,
			     MessageSize *hdr_size, MessageSize *body_size)
{
	CachedMessage *msg;
	uoff_t offset;

	i_assert(cache->open_msg != NULL);

	msg = cache->open_msg;
	if (inbuf != NULL) {
		offset = hdr_size != NULL ? 0 :
			msg->hdr_size->physical_size;
		if (!imap_msgcache_get_inbuf(cache, offset))
			return FALSE;
                *inbuf = cache->open_inbuf;
	}

	if (body_size != NULL) {
		if (msg->body_size == NULL)
			cache_fields(cache, IMAP_CACHE_MESSAGE_BODY_SIZE);
		if (msg->body_size == NULL)
			return FALSE;
		*body_size = *msg->body_size;
	}

	if (hdr_size != NULL) {
		if (msg->hdr_size == NULL)
			cache_fields(cache, IMAP_CACHE_MESSAGE_HDR_SIZE);
		if (msg->hdr_size == NULL)
			return FALSE;
		*hdr_size = *msg->hdr_size;
	}

	return TRUE;
}

static void get_partial_size(IOBuffer *inbuf,
			     uoff_t virtual_skip, uoff_t max_virtual_size,
			     MessageSize *partial, MessageSize *dest)
{
	unsigned char *msg;
	size_t size;
	int cr_skipped;

	/* see if we can use the existing partial */
	if (partial->virtual_size > virtual_skip)
		memset(partial, 0, sizeof(MessageSize));
	else {
		io_buffer_skip(inbuf, partial->physical_size);
		virtual_skip -= partial->virtual_size;
	}

	message_skip_virtual(inbuf, virtual_skip, partial, &cr_skipped);

	if (!cr_skipped) {
		/* see if we need to add virtual CR */
		if (io_buffer_read_data_blocking(inbuf, &msg, &size, 0) > 0) {
			if (msg[0] == '\n')
				dest->virtual_size++;
		}
	}

	message_get_body_size(inbuf, dest, max_virtual_size);
}

int imap_msgcache_get_rfc822_partial(ImapMessageCache *cache,
				     uoff_t virtual_skip,
				     uoff_t max_virtual_size,
				     int get_header, MessageSize *size,
                                     IOBuffer **inbuf)
{
	CachedMessage *msg;
	uoff_t physical_skip;
	int size_got;

	i_assert(cache->open_msg != NULL);

	*inbuf = NULL;

	msg = cache->open_msg;
	if (msg->hdr_size == NULL) {
		cache_fields(cache, IMAP_CACHE_MESSAGE_HDR_SIZE);
		if (msg->hdr_size == NULL)
			return FALSE;
	}

	physical_skip = get_header ? 0 : msg->hdr_size->physical_size;

	/* see if we can do this easily */
	size_got = FALSE;
	if (virtual_skip == 0) {
		if (msg->body_size == NULL) {
			cache_fields(cache, IMAP_CACHE_MESSAGE_BODY_SIZE);
			if (msg->body_size == NULL)
				return FALSE;
		}

		if (max_virtual_size >= msg->body_size->virtual_size) {
			*size = *msg->body_size;
			size_got = TRUE;
		}
	}

	if (!size_got) {
		if (!imap_msgcache_get_inbuf(cache,
					     msg->hdr_size->physical_size))
			return FALSE;

		if (msg->partial_size == NULL)
			msg->partial_size = p_new(msg->pool, MessageSize, 1);
		get_partial_size(cache->open_inbuf, virtual_skip,
				 max_virtual_size, msg->partial_size, size);

		physical_skip += msg->partial_size->physical_size;
	}

	if (get_header)
		message_size_add(size, msg->hdr_size);

	/* seek to wanted position */
	if (!imap_msgcache_get_inbuf(cache, physical_skip))
		return FALSE;

        *inbuf = cache->open_inbuf;
	return TRUE;
}

int imap_msgcache_get_data(ImapMessageCache *cache, IOBuffer **inbuf)
{
	i_assert(cache->open_msg != NULL);

	if (!imap_msgcache_get_inbuf(cache, 0))
		return FALSE;

        *inbuf = cache->open_inbuf;
	return TRUE;
}
