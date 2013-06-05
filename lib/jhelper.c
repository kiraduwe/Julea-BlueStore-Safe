/*
 * Copyright (c) 2010-2013 Michael Kuhn
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 **/

#include <julea-config.h>

#include <glib.h>
#include <gio/gio.h>

#include <mongo.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <jhelper-internal.h>

#include <jsemantics.h>
#include <jtrace-internal.h>

/**
 * \defgroup JHelper Helper
 *
 * Helper data structures and functions.
 *
 * @{
 **/

void
j_helper_set_nodelay (GSocketConnection* connection, gboolean enable)
{
	gint const flag = (enable) ? 1 : 0;

	GSocket* socket_;
	gint fd;

	g_return_if_fail(connection != NULL);

	j_trace_enter(G_STRFUNC);

	socket_ = g_socket_connection_get_socket(connection);
	fd = g_socket_get_fd(socket_);

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(gint));

	j_trace_leave(G_STRFUNC);
}

guint
j_helper_get_processor_count (void)
{
	guint thread_count = 8;

#if GLIB_CHECK_VERSION(2,35,4)
	thread_count = g_get_num_processors();
#elif defined(_SC_NPROCESSORS_ONLN)
	thread_count = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
	thread_count = sysconf(_SC_NPROCESSORS_CONF);
#endif

	return thread_count;
}

gboolean
j_helper_insert_batch (mongo* connection, gchar const* ns, bson** obj, guint length, mongo_write_concern* write_concern)
{
	guint32 const max_obj_size = J_MIB(16);

	gboolean ret = TRUE;
	guint offset = 0;
	guint32 obj_size = 0;

	for (guint i = 0; i < length; i++)
	{
		guint32 size;

		size = bson_size(obj[i]);

		if (G_UNLIKELY(obj_size + size > max_obj_size))
		{
			ret = (mongo_insert_batch(connection, ns, (bson const**)obj + offset, i - offset, write_concern, MONGO_CONTINUE_ON_ERROR) == MONGO_OK) && ret;

			offset = i;
			obj_size = 0;
		}

		obj_size += size;
	}

	ret = (mongo_insert_batch(connection, ns, (bson const**)obj + offset, length - offset, write_concern, MONGO_CONTINUE_ON_ERROR) == MONGO_OK) && ret;

	return ret;
}

void
j_helper_set_write_concern (mongo_write_concern* write_concern, JSemantics* semantics)
{
	g_return_if_fail(write_concern != NULL);
	g_return_if_fail(semantics != NULL);

	j_trace_enter(G_STRFUNC);

	mongo_write_concern_init(write_concern);

	if (j_semantics_get(semantics, J_SEMANTICS_SAFETY) != J_SEMANTICS_SAFETY_NONE)
	{
		write_concern->w = 1;

		if (j_semantics_get(semantics, J_SEMANTICS_SAFETY) == J_SEMANTICS_SAFETY_STORAGE)
		{
			write_concern->j = 1;
		}
	}

	mongo_write_concern_finish(write_concern);

	j_trace_leave(G_STRFUNC);
}

JSemantics*
j_helper_parse_semantics (gchar const* template_str, gchar const* semantics_str)
{
	JSemantics* semantics;
	gchar** parts;
	guint parts_len;

	j_trace_enter(G_STRFUNC);

	if (template_str == NULL || g_strcmp0(template_str, "default") == 0)
	{
		semantics = j_semantics_new(J_SEMANTICS_TEMPLATE_DEFAULT);
	}
	else if (g_strcmp0(template_str, "posix") == 0)
	{
		semantics = j_semantics_new(J_SEMANTICS_TEMPLATE_POSIX);
	}
	else if (g_strcmp0(template_str, "checkpoint") == 0)
	{
		semantics = j_semantics_new(J_SEMANTICS_TEMPLATE_CHECKPOINT);
	}
	else if (g_strcmp0(template_str, "serial") == 0)
	{
		semantics = j_semantics_new(J_SEMANTICS_TEMPLATE_SERIAL);
	}
	else
	{
		g_assert_not_reached();
	}

	if (semantics_str == NULL)
	{
		goto end;
	}

	parts = g_strsplit(semantics_str, ",", 0);
	parts_len = g_strv_length(parts);

	for (guint i = 0; i < parts_len; i++)
	{
		gchar const* value;

		if ((value = strchr(parts[i], '=')) == NULL)
		{
			continue;
		}

		value++;

		if (g_str_has_prefix(parts[i], "atomicity="))
		{
			if (g_strcmp0(value, "batch") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_ATOMICITY, J_SEMANTICS_ATOMICITY_BATCH);
			}
			else if (g_strcmp0(value, "operation") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_ATOMICITY, J_SEMANTICS_ATOMICITY_OPERATION);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_ATOMICITY, J_SEMANTICS_ATOMICITY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else if (g_str_has_prefix(parts[i], "concurrency="))
		{
			if (g_strcmp0(value, "overlapping") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONCURRENCY, J_SEMANTICS_CONCURRENCY_OVERLAPPING);
			}
			else if (g_strcmp0(value, "non-overlapping") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONCURRENCY, J_SEMANTICS_CONCURRENCY_NON_OVERLAPPING);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONCURRENCY, J_SEMANTICS_CONCURRENCY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else if (g_str_has_prefix(parts[i], "consistency="))
		{
			if (g_strcmp0(value, "immediate") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONSISTENCY, J_SEMANTICS_CONSISTENCY_IMMEDIATE);
			}
			else if (g_strcmp0(value, "eventual") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONSISTENCY, J_SEMANTICS_CONSISTENCY_EVENTUAL);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_CONSISTENCY, J_SEMANTICS_CONSISTENCY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else if (g_str_has_prefix(parts[i], "persistency="))
		{
			if (g_strcmp0(value, "immediate") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_PERSISTENCY, J_SEMANTICS_PERSISTENCY_IMMEDIATE);
			}
			else if (g_strcmp0(value, "eventual") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_PERSISTENCY, J_SEMANTICS_PERSISTENCY_EVENTUAL);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_PERSISTENCY, J_SEMANTICS_PERSISTENCY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else if (g_str_has_prefix(parts[i], "safety="))
		{
			if (g_strcmp0(value, "storage") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_SAFETY, J_SEMANTICS_SAFETY_STORAGE);
			}
			else if (g_strcmp0(value, "network") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_SAFETY, J_SEMANTICS_SAFETY_NETWORK);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_SAFETY, J_SEMANTICS_SAFETY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else if (g_str_has_prefix(parts[i], "security="))
		{
			if (g_strcmp0(value, "strict") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_SECURITY, J_SEMANTICS_SECURITY_STRICT);
			}
			else if (g_strcmp0(value, "none") == 0)
			{
				j_semantics_set(semantics, J_SEMANTICS_SECURITY, J_SEMANTICS_SECURITY_NONE);
			}
			else
			{
				g_assert_not_reached();
			}
		}
		else
		{
			g_assert_not_reached();
		}
	}

	g_strfreev(parts);

end:
	j_trace_leave(G_STRFUNC);

	return semantics;
}

/**
 * @}
 **/
