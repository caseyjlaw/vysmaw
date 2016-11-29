//
// Copyright © 2016 Associated Universities, Inc. Washington DC, USA.
//
// This file is part of vysmaw.
//
// vysmaw is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// vysmaw is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// vysmaw.  If not, see <http://www.gnu.org/licenses/>.
//
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <spectrum_reader.h>

#define READY(gate) G_STMT_START {                                      \
		MUTEX_LOCK((gate)->mtx); \
		(gate)->spectrum_reader_ready = true; \
		COND_SIGNAL((gate)->cond); \
		MUTEX_UNLOCK((gate)->mtx); \
	} G_STMT_END

#define CM_EVENT_FD_INDEX 0
#define INACTIVITY_TIMER_FD_INDEX 1
#define NUM_FIXED_FDS 2

enum run_state {
	STATE_INIT,
	STATE_RUN,
	STATE_QUIT,
	STATE_DONE
};

struct spectrum_reader_context_ {
	struct spectrum_reader_context *shared;
	enum run_state state;
	struct data_path_message *quit_msg;
	struct data_path_message *end_msg;
	GArray *restrict pollfds;
	GArray *restrict new_pollfds;
	struct rdma_event_channel *event_channel;
	GHashTable *connections;
	GSequence *fd_connections;
	GChecksum *checksum;
};

struct server_connection_context {
	struct rdma_cm_id *id;
	struct ibv_wc *wcs;
	GHashTable *mrs;
	uint32_t rkey;
	bool established;
	unsigned max_posted_wr;
	unsigned num_posted_wr;
	GQueue *reqs;

	unsigned num_not_ack;
	unsigned min_ack;

	GTimer *last_access;
};

enum rdma_req_result {
	RDMA_REQ_SUCCESS, RDMA_REQ_READ_FAILURE,
	RDMA_REQ_DIGEST_VERIFICATION_FAILURE
};

struct rdma_req {
	struct vysmaw_data_info data_info;
	struct vysmaw_spectrum_info spectrum_info;
	enum rdma_req_result result;
	enum ibv_wc_status status;
	GSList *consumers;
	struct vysmaw_message *message;
};

static bool verify_digest(
	GChecksum *checksum, const float *buff, size_t buffer_size, uint8_t *digest)
	__attribute__((nonnull));
static struct rdma_req *new_rdma_req(
	GSList *consumers, const struct signal_msg_payload *payload,
	const struct vysmaw_spectrum_info *spectrum_info)
	__attribute__((nonnull,returns_nonnull,malloc));
static void free_rdma_req(struct rdma_req *req)
	__attribute__((nonnull));
static int compare_server_comp_ch_fd(
	const struct server_connection_context *c1,
	const struct server_connection_context *c2,
	void *unused __attribute__((unused)))
	__attribute__((nonnull,pure));
static int start_rdma_cm(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int stop_rdma_cm(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static void set_max_posted_wr(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, unsigned max_posted_wr)
	__attribute__((nonnull));
static struct server_connection_context *initiate_server_connection(
	struct spectrum_reader_context_ *context, const struct sockaddr_in *sockaddr,
	GSList **error_records)
	__attribute__((nonnull,returns_nonnull,malloc));
static int find_connection(
	struct spectrum_reader_context_ *context, struct sockaddr_in *sockaddr,
	struct server_connection_context **conn_ctx, GQueue **req_queue,
	GSList **error_records)
	__attribute__((nonnull));
static int on_signal_message(
	struct spectrum_reader_context_ *context, struct signal_msg *msg,
	GSList **consumers, GSList **error_records)
	__attribute__((nonnull));
static int on_data_path_message(
	struct spectrum_reader_context_ *context, struct data_path_message *msg,
	GSList **error_records)
	__attribute__((nonnull));
static int on_server_addr_resolved(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, GSList **error_records)
	__attribute__((nonnull));
static int on_server_route_resolved(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, GSList **error_records)
	__attribute__((nonnull));
static int post_server_reads(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, GSList **error_records)
	__attribute__((nonnull));
static int on_server_established(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, uint32_t rkey,
	unsigned initiator_depth, GSList **error_records)
	__attribute__((nonnull));
static GSequenceIter *find_server_connection_context_iter(
	const struct spectrum_reader_context_ *context, int fd)
	__attribute__((nonnull,returns_nonnull));
static struct server_connection_context *find_server_connection_context(
	const struct spectrum_reader_context_ *context, int fd)
	__attribute__((nonnull));
static int begin_server_disconnect(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, GSList **error_records)
	__attribute__((nonnull));
static int complete_server_disconnect(
	struct spectrum_reader_context_ *context,
	struct server_connection_context *conn_ctx, GSList **error_records)
	__attribute__((nonnull));
static int on_cm_event(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int poll_completions(
	GChecksum *checksum, struct server_connection_context *conn_ctx,
	GSList **reqs, GSList **error_records)
	__attribute__((nonnull));
static void ack_completions(
	struct server_connection_context *conn_ctx, unsigned min_ack)
	__attribute__((nonnull));
static int get_completed_requests(
	struct spectrum_reader_context_ *context, int fd,
	struct server_connection_context **conn_ctx, GSList **reqs,
	GSList **error_records)
	__attribute__((nonnull));
static int on_read_completion(
	struct spectrum_reader_context_ *context, unsigned pfd,
	GSList **error_records)
	__attribute__((nonnull));
static int on_inactivity_timer_event(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int on_poll_events(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int to_quit_state(struct spectrum_reader_context_ *context,
                         struct data_path_message *msg, GSList **error_records)
	__attribute__((nonnull(1,3)));
static int spectrum_reader_loop(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int start_inactivity_timer(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int stop_inactivity_timer(
	struct spectrum_reader_context_ *context, GSList **error_records)
	__attribute__((nonnull));
static int loopback_msg(
	struct spectrum_reader_context_ *context,
	struct data_path_message *msg, GSList **error_records)
	__attribute__((nonnull));

static bool
verify_digest(GChecksum *checksum, const float *buff, size_t buffer_size,
              uint8_t *digest)
{
	g_checksum_reset(checksum);
	g_checksum_update(checksum, (guchar *)buff, buffer_size);
	uint8_t buff_digest[VYSMAW_DATA_DIGEST_SIZE];
	gsize digest_len = VYSMAW_DATA_DIGEST_SIZE;
	g_checksum_get_digest(checksum, buff_digest, &digest_len);
	return memcmp(buff_digest, digest, digest_len) == 0;
}

static struct rdma_req *
new_rdma_req(GSList *consumers, const struct signal_msg_payload *payload,
             const struct vysmaw_spectrum_info *spectrum_info)
{
	struct rdma_req *result = g_slice_new(struct rdma_req);
	memcpy(&(result->spectrum_info), spectrum_info,
	       sizeof(result->spectrum_info));
	result->data_info.num_channels = payload->num_channels;
	result->data_info.stations[0] = payload->stations[0];
	result->data_info.stations[1] = payload->stations[1];
	result->data_info.spectral_window_index = payload->spectral_window_index;
	result->data_info.stokes_index = payload->stokes_index;
	result->data_info.timestamp = spectrum_info->timestamp;
	result->consumers = consumers;
	return result;
}

static void
free_rdma_req(struct rdma_req *req)
{
	g_slist_free(req->consumers);
	g_slice_free(struct rdma_req, req);
}

static int
compare_server_comp_ch_fd(const struct server_connection_context *c1,
                          const struct server_connection_context *c2,
                          void *unused)
{
	return c1->id->send_cq_channel->fd - c2->id->send_cq_channel->fd;
}

static int
start_rdma_cm(struct spectrum_reader_context_ *context, GSList **error_records)
{
	/* rdma cm event channel */
	context->event_channel = rdma_create_event_channel();
	if (G_UNLIKELY(context->event_channel == NULL)) {
		VERB_ERR(error_records, errno, "rdma_create_event_channel");
		return -1;
	}
	int rc = set_nonblocking(context->event_channel->fd);
	if (G_UNLIKELY(rc != 0)) {
		MSG_ERROR(error_records, errno,
		          "failed to set rdma cm event channel to non-blocking: %s",
		          strerror(errno));
		return rc;
	}
	struct pollfd *pfd =
		&g_array_index(context->pollfds, struct pollfd, CM_EVENT_FD_INDEX);
	pfd->fd = context->event_channel->fd;
	pfd->events = POLLIN;

	context->connections = g_hash_table_new_full(
		(GHashFunc)sockaddr_hash, (GEqualFunc)sockaddr_equal,
		(GDestroyNotify)free_sockaddr_key, NULL);
	context->fd_connections = g_sequence_new(NULL);

	return rc;
}

static int
stop_rdma_cm(struct spectrum_reader_context_ *context, GSList **error_records)
{
	if (context->fd_connections != NULL)
		g_sequence_free(context->fd_connections);

	if (context->connections != NULL)
		g_hash_table_destroy(context->connections);

	if (context->event_channel != NULL)
		rdma_destroy_event_channel(context->event_channel);

	return 0;
}

static void
set_max_posted_wr(struct spectrum_reader_context_ *context,
                  struct server_connection_context *conn_ctx,
                  unsigned max_posted_wr)
{
	conn_ctx->max_posted_wr = max_posted_wr;
	conn_ctx->min_ack =
		max_posted_wr
		/ context->shared->handle->config.rdma_read_min_ack_part;
}

static struct server_connection_context *
initiate_server_connection(struct spectrum_reader_context_ *context,
                           const struct sockaddr_in *sockaddr,
                           GSList **error_records)
{
	struct rdma_cm_id *id;
	int rc = rdma_create_id(context->event_channel, &id, NULL, RDMA_PS_TCP);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "rdma_create_id");
		return NULL;
	}

	struct sockaddr_in *key = new_sockaddr_key(sockaddr);
	rc = rdma_resolve_addr(
		id, NULL, (struct sockaddr *)key,
		context->shared->handle->config.resolve_addr_timeout_ms);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "rdma_resolve_addr");
		rdma_destroy_id(id);
		free_sockaddr_key(key);
		return NULL;
	}

	struct server_connection_context *result =
		g_slice_new0(struct server_connection_context);
	result->id = id;
	result->established = false;
	result->reqs = g_queue_new();
	set_max_posted_wr(context, result,
	                  context->shared->handle->config.rdma_read_max_posted);
	result->num_posted_wr = 0;
	result->num_not_ack = 0;
	/* set wcs field later, after final value of max_posted_wr has been
	 * computed */
	result->last_access = g_timer_new();
	g_hash_table_insert(context->connections, key, result);
	return result;
}

static int
find_connection(struct spectrum_reader_context_ *context,
                struct sockaddr_in *sockaddr,
                struct server_connection_context **conn_ctx,
                GQueue **req_queue, GSList **error_records)
{
	*conn_ctx = g_hash_table_lookup(context->connections, sockaddr);
	if (*conn_ctx == NULL) {
		*conn_ctx =
			initiate_server_connection(context, sockaddr, error_records);
		if (G_UNLIKELY(*conn_ctx == NULL))
			return -1;
	}
	if ((*conn_ctx)->established
	    || context->shared->handle->config.preconnect_backlog)
		*req_queue = (*conn_ctx)->reqs;
	else
		*req_queue = NULL;
	return 0;
}

static int
on_signal_message(struct spectrum_reader_context_ *context,
                  struct signal_msg *msg, GSList **consumers,
                  GSList **error_records)
{
	struct signal_msg_payload *payload = &(msg->payload);
	GQueue *reqs = NULL;
	struct server_connection_context *conn_ctx = NULL;

	int rc = find_connection(context, &payload->sockaddr, &conn_ctx, &reqs,
	                         error_records);

	if (G_LIKELY(rc == 0 && reqs != NULL)) {
		struct vysmaw_spectrum_info *info = payload->infos;
		for (unsigned i = payload->num_spectra; i > 0; --i) {
			if (*consumers != NULL)
				g_queue_push_tail(reqs,
				                  new_rdma_req(*consumers, payload, info));
			*consumers = NULL; //rdma req takes list
			++consumers;
			++info;
		}
	}

	return rc;
}

static int
on_data_path_message(struct spectrum_reader_context_ *context,
                     struct data_path_message *msg, GSList **error_records)
{
	int rc = 0;
	switch (msg->typ) {
	case DATA_PATH_SIGNAL_MSG:
		if (G_LIKELY(context->state == STATE_RUN))
			rc = on_signal_message(
				context, msg->signal_msg, msg->consumers, error_records);
		buffer_pool_push(context->shared->signal_msg_buffers, msg->signal_msg);
		data_path_message_free(msg);
		break;

	case DATA_PATH_RECEIVE_FAIL:
		mark_signal_receive_failure(context->shared->handle, msg->wc_status);
		data_path_message_free(msg);
		break;

	case DATA_PATH_BUFFER_STARVATION:
		mark_signal_buffer_starvation(context->shared->handle);
		data_path_message_free(msg);
		break;

	case DATA_PATH_QUIT:
		if (context->quit_msg == NULL) {
			rc = to_quit_state(context, msg, error_records);
		} else {
			if (context->quit_msg == msg) {
				context->end_msg = data_path_message_new(
					context->shared->signal_msg_num_spectra);
				context->end_msg->typ = DATA_PATH_END;
				rc = loopback_msg(context, context->end_msg, error_records);
				context->quit_msg = NULL;
			}
			data_path_message_free(msg);
		}
		break;

	case DATA_PATH_END:
		context->state = STATE_DONE;
		break;

	default:
		g_assert_not_reached();
		break;
	}
	return rc;
}

static int
on_server_addr_resolved(struct spectrum_reader_context_ *context,
                        struct server_connection_context *conn_ctx,
                        GSList **error_records)
{
	/* query ib device */
	struct ibv_device_attr dev_attr;
	int rc = ibv_query_device(conn_ctx->id->verbs, &dev_attr);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, rc, "ibv_query_device");
		return -1;
	}
	set_max_posted_wr(
		context, conn_ctx,
		MIN(conn_ctx->max_posted_wr, dev_attr.max_qp_init_rd_atom));

	/* create qp */
	struct ibv_qp_init_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.sq_sig_all = 1;
	attr.cap.max_send_wr = conn_ctx->max_posted_wr;
	attr.cap.max_recv_wr = 1;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	rc = rdma_create_qp(conn_ctx->id, NULL, &attr);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "rdma_create_qp");
		return -1;
	}

	/* record maximum number of work requests we can post at one time */
	set_max_posted_wr(context, conn_ctx,
	                  MIN(conn_ctx->max_posted_wr, attr.cap.max_send_wr));

	/* resize cq for the maximum number of posted work requests */
	rc = ibv_resize_cq(conn_ctx->id->send_cq, conn_ctx->max_posted_wr);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "ibv_resize_cq");
		return -1;
	}

	/* now may insert the connection into the list sorted by completion fd */
	g_sequence_insert_sorted(
		context->fd_connections, conn_ctx,
		(GCompareDataFunc)compare_server_comp_ch_fd, NULL);

	rc = rdma_resolve_route(
		conn_ctx->id,
		context->shared->handle->config.resolve_route_timeout_ms);
	if (G_UNLIKELY(rc != 0))
		VERB_ERR(error_records, errno, "rdma_resolve_route");

	return rc;

}

static int
on_server_route_resolved(struct spectrum_reader_context_ *context,
                         struct server_connection_context *conn_ctx,
                         GSList **error_records)
{
	/* register memory for receiving spectra */
	conn_ctx->mrs = register_spectrum_buffer_pools(
		context->shared->handle, conn_ctx->id, error_records);
	if (G_UNLIKELY(conn_ctx->mrs == NULL))
		return -1;

	/* set up send completion queue event channel */
	int fd = conn_ctx->id->send_cq_channel->fd;
	set_nonblocking(fd);
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN
	};
	if (context->new_pollfds->len == 0)
		g_array_append_vals(context->new_pollfds, context->pollfds->data,
		                    context->pollfds->len);
	g_array_append_val(context->new_pollfds, pfd);

	/* post first request for notification from completion queue */
	int rc = ibv_req_notify_cq(conn_ctx->id->send_cq, 0);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, rc, "ibv_req_notify_cq");
		return rc;
	}

	/* connect to server */
	struct rdma_conn_param conn_param;
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.initiator_depth = conn_ctx->max_posted_wr;
	rc = rdma_connect(conn_ctx->id, &conn_param);
	if (G_UNLIKELY(rc != 0))
		VERB_ERR(error_records, errno, "rdma_connect");
	return rc;
}

static int
post_server_reads(struct spectrum_reader_context_ *context,
                  struct server_connection_context *conn_ctx,
                  GSList **error_records)
{
	int rc = 0;
	struct ibv_mr *mr = NULL;
	pool_id_t pool_id = NULL;
	while (rc == 0
	       && conn_ctx->num_posted_wr < conn_ctx->max_posted_wr
	       && !g_queue_is_empty(conn_ctx->reqs)) {
		struct rdma_req *req = g_queue_pop_head(conn_ctx->reqs);
		pool_id_t buff_pool_id;
		req->message = valid_buffer_message_new(
			context->shared->handle, &req->data_info, &buff_pool_id);
		if (req->message != NULL) {
			if (G_UNLIKELY(mr == NULL || buff_pool_id != pool_id)) {
				pool_id = buff_pool_id;
				mr = g_hash_table_lookup(conn_ctx->mrs, pool_id);
			}
			rc = rdma_post_read(
				conn_ctx->id, req, req->message->content.valid_buffer.buffer,
				req->message->content.valid_buffer.buffer_size, mr, 0,
				req->spectrum_info.data_addr, conn_ctx->rkey);
			if (G_LIKELY(rc == 0))
				conn_ctx->num_posted_wr++;
			else
				VERB_ERR(error_records, errno, "rdma_post_read");
		} else {
			free_rdma_req(req);
		}
	}
	return rc;
}

static int
on_server_established(struct spectrum_reader_context_ *context,
                      struct server_connection_context *conn_ctx,
                      uint32_t rkey, unsigned initiator_depth,
                      GSList **error_records)
{
	conn_ctx->rkey = rkey;
	set_max_posted_wr(context, conn_ctx,
	                  MIN(conn_ctx->max_posted_wr, initiator_depth));
	conn_ctx->wcs = g_new(struct ibv_wc, conn_ctx->max_posted_wr);
	conn_ctx->established = true;
	return post_server_reads(context, conn_ctx, error_records);
}

static GSequenceIter *
find_server_connection_context_iter(
	const struct spectrum_reader_context_ *context, int fd)
{
	struct ibv_comp_channel fd_ch = {
		.fd = fd
	};
	struct rdma_cm_id fd_id = {
		.send_cq_channel = &fd_ch
	};
	struct server_connection_context fd_ctx = {
		.id = &fd_id
	};
	GSequenceIter *iter =
		g_sequence_search(
			context->fd_connections, &fd_ctx,
			(GCompareDataFunc)compare_server_comp_ch_fd, NULL);
	if (!g_sequence_iter_is_begin(iter))
		iter = g_sequence_iter_prev(iter);
	return iter;
}

static struct server_connection_context *
find_server_connection_context(const struct spectrum_reader_context_ *context,
                               int fd)
{
	GSequenceIter *iter = find_server_connection_context_iter(context, fd);
	if (g_sequence_iter_is_end(iter))
		return NULL;
	return g_sequence_get(iter);
}

static int
begin_server_disconnect(struct spectrum_reader_context_ *context,
                        struct server_connection_context *conn_ctx,
                        GSList **error_records)
{
	int rc = 0;
	while (!g_queue_is_empty(conn_ctx->reqs))
		free_rdma_req(g_queue_pop_head(conn_ctx->reqs));

	if (conn_ctx->established) {
		conn_ctx->established = false;
		rc = rdma_disconnect(conn_ctx->id);
		if (G_UNLIKELY(rc != 0))
			VERB_ERR(error_records, errno, "rdma_disconnect");
	}
	return rc;
}

static int
complete_server_disconnect(struct spectrum_reader_context_ *context,
                           struct server_connection_context *conn_ctx,
                           GSList **error_records)
{
	if (conn_ctx->reqs != NULL)
		g_queue_free(conn_ctx->reqs);

	bool removed = g_hash_table_remove(
		context->connections, &(conn_ctx->id->route.addr.dst_sin));
	if (G_UNLIKELY(!removed))
		MSG_ERROR(error_records, -1, "%s",
		          "failed to remove server connection record from client");

	g_sequence_remove(
		find_server_connection_context_iter(
			context, conn_ctx->id->send_cq_channel->fd));

	if (context->new_pollfds->len == 0)
		g_array_append_vals(context->new_pollfds, context->pollfds->data,
		                    context->pollfds->len);

	for (unsigned i = 0; i < context->new_pollfds->len; ++i) {
		struct pollfd *pfd =
			&(g_array_index(context->new_pollfds, struct pollfd, i));
		if (pfd->fd == conn_ctx->id->send_cq_channel->fd) {
			g_array_remove_index(context->new_pollfds, i);
			break;
		}
	}

	ack_completions(conn_ctx, 1);
	// TODO: exit can hang here!
	//rdma_destroy_qp(conn_ctx->id);

	if (conn_ctx->mrs != NULL) {
		void dereg_mr(struct spectrum_buffer_pool *unused, struct ibv_mr *mr,
		              void *unused1) {
			int rc = rdma_dereg_mr(mr);
			if (G_UNLIKELY(rc != 0))
				VERB_ERR(error_records, errno, "rdma_dereg_mr");
		}
		g_hash_table_foreach(conn_ctx->mrs, (GHFunc)dereg_mr, NULL);
		g_hash_table_destroy(conn_ctx->mrs);
	}

	int rc = rdma_destroy_id(conn_ctx->id);
	if (G_UNLIKELY(rc != 0))
		VERB_ERR(error_records, errno, "rdma_destroy_id");

	if (conn_ctx->wcs != NULL)
		g_free(conn_ctx->wcs);

	if (conn_ctx->last_access != NULL)
		g_timer_destroy(conn_ctx->last_access);

	g_slice_free(struct server_connection_context, conn_ctx);

	return ((*error_records != NULL) ? -1 : 0);
}

static int
on_cm_event(struct spectrum_reader_context_ *context, GSList **error_records)
{
	/* get event */
	struct rdma_cm_event *event = NULL;
	int rc = rdma_get_cm_event(context->event_channel, &event);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "rdma_get_cm_event");
		return rc;
	}

	/* look up server connection */
	struct server_connection_context *conn_ctx =
		g_hash_table_lookup(context->connections,
		                    &event->id->route.addr.dst_sin);
	if (G_UNLIKELY(conn_ctx == NULL))
		MSG_ERROR(
			error_records, -1,
			"failed to find connection for event %s",
			rdma_event_str(event->event));

	/* Optimistically read some values from the event, even if they might only
	 * be valid for some event types. */
	enum rdma_cm_event_type ev_type = event->event;
	void *private_data = g_alloca(event->param.conn.private_data_len);
	memcpy(private_data, event->param.conn.private_data,
	       event->param.conn.private_data_len);
	unsigned initiator_depth = event->param.conn.initiator_depth;

	/* ack event */
	rc = rdma_ack_cm_event(event);
	if (G_UNLIKELY(rc != 0))
		VERB_ERR(error_records, errno, "rdma_ack_cm_event");

	if (G_UNLIKELY(*error_records != NULL))
		return -1;

	/* dispatch event based on type */
	switch (ev_type) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rc = on_server_addr_resolved(context, conn_ctx, error_records);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		rc = on_server_route_resolved(context, conn_ctx, error_records);
		break;

	case RDMA_CM_EVENT_ESTABLISHED: {
		uint32_t rkey = *(uint32_t *)private_data;
		rc = on_server_established(context, conn_ctx, rkey, initiator_depth,
		                           error_records);
		break;
	}
	case RDMA_CM_EVENT_DISCONNECTED:
		rc = begin_server_disconnect(context, conn_ctx, error_records);
		if (rc == 0 && conn_ctx->num_posted_wr == 0)
			rc = complete_server_disconnect(context, conn_ctx, error_records);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED: {
		char addr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &(conn_ctx->id->route.addr.dst_sin),
		          addr, sizeof(addr));
		MSG_ERROR(error_records, -1, "%s on %s", rdma_event_str(ev_type), addr);
		rc = -1;
		break;
	}
	default:
		break;
	}

	return rc;
}

static int
poll_completions(GChecksum *checksum, struct server_connection_context *conn_ctx,
                 GSList **reqs, GSList **error_records)
{
	*reqs = NULL;
	int nc = ibv_poll_cq(conn_ctx->id->send_cq, conn_ctx->max_posted_wr,
	                     conn_ctx->wcs);
	if (G_UNLIKELY(nc < 0)) {
		VERB_ERR(error_records, errno, "ibv_poll_cq");
		return errno;
	}
	if (G_LIKELY(nc > 0)) {
		g_assert(conn_ctx->num_posted_wr >= nc);
		conn_ctx->num_posted_wr -= nc;
		for (unsigned i = 0; i < nc; ++i) {
			struct rdma_req *req = (struct rdma_req *)conn_ctx->wcs[i].wr_id;
			req->status = conn_ctx->wcs[i].status;
			if (G_LIKELY(req->status == IBV_WC_SUCCESS)) {
				if (verify_digest(
					    checksum,
					    req->message->content.valid_buffer.buffer,
					    req->message->content.valid_buffer.buffer_size,
					    req->spectrum_info.digest))
					req->result = RDMA_REQ_SUCCESS;
				else
					req->result = RDMA_REQ_DIGEST_VERIFICATION_FAILURE;
			} else {
				req->result = RDMA_REQ_READ_FAILURE;
			}
			*reqs = g_slist_prepend(*reqs, req);
		}
	}
	return 0;
}

static void
ack_completions(struct server_connection_context *conn_ctx, unsigned min_ack)
{
	if (conn_ctx->num_not_ack >= min_ack) {
		if (conn_ctx->id != NULL)
			ibv_ack_cq_events(conn_ctx->id->send_cq, conn_ctx->num_not_ack);
		conn_ctx->num_not_ack = 0;
	}
}

static int
get_completed_requests(struct spectrum_reader_context_ *context, int fd,
                       struct server_connection_context **conn_ctx,
                       GSList **reqs, GSList **error_records)
{
	/* look up server_connection_context instance for given fd */
	*conn_ctx = find_server_connection_context(context, fd);
	if (G_UNLIKELY(*conn_ctx == NULL)) {
		MSG_ERROR(error_records, -1,
		          "failed to find server context for read completion on %d",
		          fd);
		return -1;
	}

	struct ibv_cq *ev_cq;
	void *cq_ctx = NULL;
	int rc =
		ibv_get_cq_event((*conn_ctx)->id->send_cq_channel, &ev_cq, &cq_ctx);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, errno, "ibv_get_cq_event");
		return rc;
	}

	(*conn_ctx)->num_not_ack++;
	ack_completions(*conn_ctx, (*conn_ctx)->min_ack);

	rc = ibv_req_notify_cq(ev_cq, 0);
	if (G_UNLIKELY(rc != 0)) {
		VERB_ERR(error_records, rc, "ibv_req_notify_cq");
		return rc;
	}

	return poll_completions(context->checksum, *conn_ctx, reqs, error_records);
}

static int
on_read_completion(struct spectrum_reader_context_ *context, unsigned pfd,
                   GSList **error_records)
{
	struct pollfd *pollfd =
		&g_array_index(context->pollfds, struct pollfd, pfd);

	GSList *reqs = NULL;
	struct server_connection_context *conn_ctx;
	int rc = get_completed_requests(context, pollfd->fd, &conn_ctx, &reqs,
	                                error_records);
	if (G_UNLIKELY(rc != 0)) return rc;

	g_timer_start(conn_ctx->last_access);

	rc = post_server_reads(context, conn_ctx, error_records);
	if (G_UNLIKELY(rc != 0)) return rc;

	while (reqs != NULL) {
		struct rdma_req *req = reqs->data;
		switch (req->result) {
		case RDMA_REQ_DIGEST_VERIFICATION_FAILURE:
			convert_valid_to_digest_failure(req->message);
			break;
		case RDMA_REQ_READ_FAILURE:
			convert_valid_to_rdma_read_failure(req->message, req->status);
			break;
		default:
			break;
		}
		message_queues_push(req->message, req->consumers);
		free_rdma_req(req);
		reqs = g_slist_delete_link(reqs, reqs);
	}

	if (!conn_ctx->established && conn_ctx->num_posted_wr == 0)
		rc = complete_server_disconnect(context, conn_ctx, error_records);

	return rc;
}

static int
on_inactivity_timer_event(struct spectrum_reader_context_ *context,
                          GSList **error_records)
{
	struct pollfd *pollfd = &g_array_index(
		context->pollfds, struct pollfd, INACTIVITY_TIMER_FD_INDEX);

	uint64_t n;
	read(pollfd->fd, &n, sizeof(n));

	double inactive_server_timeout_sec =
		context->shared->handle->config.inactive_server_timeout_sec;

	void disconnect_inactive(struct sockaddr_in *unused,
	                         struct server_connection_context *conn_ctx,
	                         void *unused1) {
		if (g_timer_elapsed(conn_ctx->last_access, NULL)
		    >= inactive_server_timeout_sec)
			begin_server_disconnect(context, conn_ctx, error_records);
	}

	g_hash_table_foreach(
		context->connections, (GHFunc)disconnect_inactive, NULL);

	if (*error_records != NULL) return -1;
	return 0;
}

static int
on_poll_events(struct spectrum_reader_context_ *context, GSList **error_records)
{
	int rc = 0;

	/* cm events */
	struct pollfd *cm_pollfd =
		&g_array_index(context->pollfds, struct pollfd, CM_EVENT_FD_INDEX);
	if (cm_pollfd->revents & POLLIN) {
		rc = on_cm_event(context, error_records);
	} else if (cm_pollfd->revents & (POLLERR | POLLHUP)) {
		MSG_ERROR(error_records, -1, "%s",
		          "rdma cm event channel ERR or HUP");
		rc = -1;
	}

	/* inactivity timer events */
	struct pollfd *tm_pollfd = &g_array_index(context->pollfds, struct pollfd,
	                                          INACTIVITY_TIMER_FD_INDEX);
	if (tm_pollfd->revents & POLLIN)
		rc = on_inactivity_timer_event(context, error_records);

	/* read completion events */
	for (unsigned i = NUM_FIXED_FDS; i < context->pollfds->len; ++i) {
		struct pollfd *ev_pollfd =
			&g_array_index(context->pollfds, struct pollfd, i);
		int rc1;
		if (ev_pollfd->revents & POLLIN) {
			rc1 = on_read_completion(context, i, error_records);
		} else if (ev_pollfd->revents & (POLLERR | POLLHUP)) {
			MSG_ERROR(error_records, -1, "%s",
			          "connection event channel ERR or HUP");
			rc1 = -1;
		}
		if (rc == 0 && rc1 != 0) rc = rc1;
	}
	if (context->new_pollfds->len > 0) {
		GArray *tmp = context->pollfds;
		context->pollfds = context->new_pollfds;
		context->new_pollfds = tmp;
		g_array_set_size(context->new_pollfds, 0);
	}
	return rc;
}

static int
to_quit_state(struct spectrum_reader_context_ *context,
              struct data_path_message *quit_msg, GSList **error_records)
{
	g_assert(context->state != STATE_DONE);

	int rc = 0;
	if (context->state != STATE_QUIT) {
		if (context->fd_connections != NULL) {
			GSequenceIter *iter =
				g_sequence_get_begin_iter(context->fd_connections);
			while (!g_sequence_iter_is_end(iter)) {
				struct server_connection_context *conn_ctx = g_sequence_get(iter);
				begin_server_disconnect(context, conn_ctx, error_records);
				iter = g_sequence_iter_next(iter);
			}
		}
		if (quit_msg == NULL) {
			quit_msg =
				data_path_message_new(context->shared->signal_msg_num_spectra);
			quit_msg->typ = DATA_PATH_QUIT;
		}
		rc = loopback_msg(context, quit_msg, error_records);
		context->state = STATE_QUIT;
		context->quit_msg = quit_msg;
	}
	return rc;
}

static int
spectrum_reader_loop(struct spectrum_reader_context_ *context,
                     GSList **error_records)
{
	int result = 0;
	bool quit = false;
	while (!quit) {
		int rc = 0;
		int nfd = poll((struct pollfd *)(context->pollfds->data),
		               context->pollfds->len, 0);
		if (G_LIKELY(nfd > 0)) {
			rc = on_poll_events(context, error_records);
		} else if (G_UNLIKELY(nfd < 0 && errno != EINTR)) {
			MSG_ERROR(error_records, errno,
			          "spectrum_reader poll failed: %s", strerror(errno));
			rc = -1;
		}
		int rc1 = 0;
		struct data_path_message *msg =
			g_async_queue_try_pop(context->shared->read_request_queue);
		if (msg != NULL)
			rc1 = on_data_path_message(context, msg, error_records);
		if (G_UNLIKELY(rc != 0 || rc1 != 0)) {
			to_quit_state(context, NULL, error_records);
			result = -1;
		}
		quit = (context->state == STATE_DONE
		        && (context->connections == NULL
		            || g_hash_table_size(context->connections) == 0));
	}
	return result;
}

static int
start_inactivity_timer(struct spectrum_reader_context_ *context,
                       GSList **error_records)
{
	struct pollfd *tm_pollfd = &g_array_index(context->pollfds, struct pollfd,
	                                          INACTIVITY_TIMER_FD_INDEX);

	tm_pollfd->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	tm_pollfd->events = POLLIN;
	if (tm_pollfd->fd >= 0) {
		time_t sec =
			context->shared->handle->config.inactive_server_timeout_sec / 2;
		struct itimerspec itimerspec = {
			.it_interval = { .tv_sec = sec, .tv_nsec = 0 },
			.it_value = { .tv_sec = sec, .tv_nsec = 0 }
		};
		int rc = timerfd_settime(tm_pollfd->fd, 0, &itimerspec, NULL);
		if (rc < 0) {
			MSG_ERROR(error_records, errno,
			          "Failed to start inactivity timer: %s",
			          strerror(errno));
			stop_inactivity_timer(context, error_records);
			return rc;
		}
	} else {
		MSG_ERROR(error_records, errno, "Failed to create inactivity timer: %s",
		          strerror(errno));
	}
	return tm_pollfd->fd;
}

static int
stop_inactivity_timer(struct spectrum_reader_context_ *context,
                      GSList **error_records)
{
	struct pollfd *tm_pollfd = &g_array_index(context->pollfds, struct pollfd,
	                                          INACTIVITY_TIMER_FD_INDEX);
	int rc = 0;
	if (tm_pollfd->fd >= 0) {
		rc = close(tm_pollfd->fd);
		if (rc < 0)
			MSG_ERROR(error_records, errno,
			          "Failed to close inactivity timer: %s", strerror(errno));
	}
	return rc;
}

static int
loopback_msg(struct spectrum_reader_context_ *context,
             struct data_path_message *msg, GSList **error_records)
{
	int result = 0;
	ssize_t num_written = 0;
	do {
		ssize_t n = write(context->shared->loop_fd, (void *)&msg + num_written,
		                  sizeof(msg) - num_written);
		if (G_LIKELY(n >= 0)) {
			num_written += n;
		} else if (errno != EINTR && n < 0) {
			MSG_ERROR(error_records, errno, "Failed to write to loop pipe: %s",
			          strerror(errno));
			result = -1;
		}
	} while (num_written < sizeof(msg) && result == 0);

	return result;
}

void *
spectrum_reader(struct spectrum_reader_context *shared)
{
	GSList *error_records = NULL;

	struct spectrum_reader_context_ context;
	memset(&context, 0, sizeof(context));
	context.shared = shared;
	context.state = STATE_INIT;
	context.pollfds = g_array_new(false, false, sizeof(struct pollfd));
	context.new_pollfds = g_array_new(false, false, sizeof(struct pollfd));
	context.checksum = g_checksum_new(G_CHECKSUM_MD5);

	g_array_set_size(context.pollfds, NUM_FIXED_FDS);
	for (unsigned i = 0; i < NUM_FIXED_FDS; ++i) {
		struct pollfd *pollfd =
			&g_array_index(context.pollfds, struct pollfd, i);
		pollfd->fd = -1;
		pollfd->events = 0;
	}

	int rc = start_rdma_cm(&context, &error_records);
	if (rc < 0)
		goto cleanup_and_return;

	rc = start_inactivity_timer(&context, &error_records);
	if (rc < 0)
		goto cleanup_and_return;

	READY(&shared->handle->gate);

	context.state = STATE_RUN;
	rc = spectrum_reader_loop(&context, &error_records);

 cleanup_and_return:
	READY(&shared->handle->gate);

	/* initialization failures may result in not being in STATE_DONE state */
	if (context.state != STATE_DONE) {
		to_quit_state(&context, NULL, &error_records);
		/* polling loop will only poll the loop fd, as the others are now
		 * closed */
		spectrum_reader_loop(&context, &error_records);
	}

	stop_inactivity_timer(&context, &error_records);

	stop_rdma_cm(&context, &error_records);

	rc = close(shared->loop_fd);
	if (rc != 0)
		MSG_ERROR(&error_records, errno, "Failed to close loop fd: %s",
		          strerror(errno));

	g_assert(context.end_msg != NULL && context.end_msg->typ == DATA_PATH_END);
	context.end_msg->error_records =
		g_slist_concat(error_records, context.end_msg->error_records);

	/* create vysmaw_message for end result */
	struct vysmaw_result result;
	if (context.end_msg->error_records == NULL) {
		result.code = VYSMAW_NO_ERROR;
		result.syserr_desc = NULL;
	} else {
		result.code = VYSMAW_SYSERR;
		GString *str = g_string_new("");
		GSList *er = context.end_msg->error_records;
		while (er != NULL) {
			g_string_append_printf(str,
			                       "%s\n",
			                       ((struct error_record *)(er->data))->desc);
			er = g_slist_next(er);
		}
		result.syserr_desc = g_string_free(str, FALSE);
	}
	struct vysmaw_message *msg = end_message_new(shared->handle, &result);

	/* post result message to all consumer queues */
	post_msg(shared->handle, msg);
	data_path_message_free(context.end_msg);

	g_checksum_free(context.checksum);
	g_array_free(context.pollfds, TRUE);
	g_array_free(context.new_pollfds, TRUE);
	g_async_queue_unref(shared->read_request_queue);
	handle_unref(shared->handle);
	g_free(shared);
	return NULL;
}