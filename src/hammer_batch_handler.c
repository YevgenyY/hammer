#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

#include "hammer_connection.h"
#include "hammer_sched.h"
#include "hammer_epoll.h"
#include "hammer_config.h"
#include "hammer_socket.h"
#include "hammer_list.h"
#include "hammer_macros.h"
#include "hammer_batch.h"
#include "hammer.h"

int hammer_batch_init()
{
	uint32_t alloc_size = config->batch_buf_max_size +
		config->batch_job_max_num * AES_KEY_SIZE +
		config->batch_job_max_num * PKT_OFFSET_SIZE + // input buffer
		config->batch_job_max_num * AES_IV_SIZE;

	hammer_batch_t *batch = hammer_sched_get_batch_struct();
	
	batch->buf_A.output_buf = cuda_pinned_mem_alloc(config->batch_max_buf_size);
	batch->buf_A.intput_buf = cuda_pinned_mem_alloc(alloc_size);
	batch->buf_A.aes_keys_pos = config->batch_buf_size;
	batch->buf_A.pkt_offsets_pos = batch->bufA.aes_keys_pos + config->batch_job_max_num * AES_KEY_SIZE;
	batch->buf_A.ivs_pos = batch->buf_A.pkt_offsets_pos + config->batch_job_max_num * PKT_OFFSET_SIZE;

	batch->buf_A.job_list = hammer_mem_malloc(config->batch_job_max_num * sizeof(hammer_job_t));
	batch->buf_A.buf_size = config->batch_buf_max_size;
	batch->buf_A.buf_length = 0;
	batch->buf_A.job_num = 0;

	batch->buf_B.output_buf = cuda_pinned_mem_alloc(config->batch_max_buf_size);
	batch->buf_B.input_buf = cuda_pinned_mem_alloc(alloc_size);
	batch->buf_B.aes_keys_pos = config->batch_buf_size;
	batch->buf_B.pkt_offsets_pos = batch->buf_B.aes_keys_pos + config->batch_job_max_num * AES_KEY_SIZE;
	batch->buf_B.ivs_pos = batch->buf_B.pkt_offsets_pos + config->batch_job_max_num * PKT_OFFSET_SIZE;

	batch->buf_B.job_list = hammer_mem_malloc(config->batch_job_max_num * sizeof(hammer_job_t));
	batch->buf_B.buf_size = config->batch_buf_max_size;
	batch->buf_B.buf_length = 0;
	batch->buf_B.job_num = 0;

	batch->cur_buf = &(batch->buf_A);
	batch->cur_buf_id = 0;

	batch->processed_buf_id = -1;
	batch->buf_has_been_taken = -1;
	
	res = pthread_mutex_init(&(batch->mutex_batch_complete, NULL));
	if (res != 0) {
		perror("Mutex initialization failed");
		exit(EXIT_FAILURE);
	}

	res = pthread_mutex_init(&(batch->mutex_batch_launch, NULL));
	if (res != 0) {
		perror("Mutex initialization failed");
		exit(EXIT_FAILURE);
	}

	return 0;
}

int hammer_batch_job_add(hammer_batch_t *batch, hammer_connection_t *c, int length)
{
	int pad_length, i = batch->cur_buf->job_num;
	hammer_job_t *new_job = &(batch->cur_buf->job_list[i]);
	void *base;

	/* Point this job ptr to the output buffer
	 * Although it has not been generated by GPU yet =P
	 * pad 16 for AES
	 */
	pad_length = (length + SHA1_SIZE + 16) & (~0x0F);
	new_job->job_body_ptr = batch->cur_buf->output_buf + batch->cur_buf->buf_length;
	new_job->job_body_length = pad_length;
	new_job->job_actural_length = length;
	new_job->connection = c;

	/* Add the job to the connection job list, so that it can be 
	 * forwarded in hammer_handler_ssl_write
	 */
	hammer_list_add(&(new_job->_head), c->job_list);

	/* Add key, pkt_offset, and iv to the input buffer */
	base = batch->input_buf + aes_keys_pos;
	memcpy(base + AES_KEY_SIZE * job_num, c->key, AES_KEY_SIZE);
	base = batch->input_buf + pkt_offset_pos;
	((uint32_t *)base)[job_num] = batch->cur_buf->buf_length;
	base = batch->input_buf + ivs_pos;
	memcpy(base + AES_IV_SIZE * job_num, c->iv, AES_IV_SIZE);

	/* Update batch parameters */
	batch->cur_buf->buf_length += pad_length;
	batch->cur_buf->job_num ++;

	if (batch->cur_buf->buf_length >= batch->cur_buf->buf_size ||
			batch->cur_buf->job_num >= config->batch_job_max_num) {
		hammer_err("error in batch job add\n");
		exit(0);
	}

	return 0;
}

/* We don't batch read from clients, which all need decryption.
   And these are supposed to be only requests, therefore we do not bother GPU to handle this
   while CPU is competent for fast AES operation with small amount of data -- AES-NI 

   However, we batch the read from server, since they all need encryption
*/
int hammer_batch_handler_read(hammer_connection_t *c)
{
	int recv_len, available, if_gpu_fetched;
	hammer_connection_t *rc;
	hammer_sched_t *sched = hammer_sched_get_sched_struct();
	hammer_batch_t *batch = hammer_sched_get_batch_struct();

//			hammer_epoll_state_set(sched->epoll_fd, socket,
//					HAMMER_EPOLL_READ,
//					HAMMER_EPOLL_LEVEL_TRIGGERED,
//					(EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLIN));

	/* we batch ssl encryption */
	if (!c->ssl) {
		hammer_err("this should be an ssl connection\n");
		exit(0);
	}

	///////////////////////////////////////////////////////////
	pthread_mutex_lock(&(batch->mutex_batch_complete));
	/* If GPU worker has processed the data */
	if (hammer_batch_if_gpu_processed_new(batch)) {
		hammer_batch_forwarding(batch);
	}
	pthread_mutex_unlock(&(batch->mutex_batch_complete));

	/* Lock, we do not permit GPU worker to enter */
	///////////////////////////////////////////////////////////
	pthread_mutex_lock(&(batch->mutex_batch_launch));


	/* If GPU worker has fetched the data,
	 * we will switch our buffer, a two-buffer strategy. */ 
	if (hammer_batch_if_current_buf_taken(batch)) {
		hammer_batch_switch_buffer(batch);
	}
	
	available = batch->cur_buf->buf_size - batch->cur_buf->buf_length;
	if (available <= 0) {
		printf("small available buffer!\n");
		exit(0);
	}

	/* Read incomming data */
	recv_len = hammer_socket_read (
			c->socket,
			batch->cur_buf->input_buf + batch->cur_buf->buf_length,
			available);
	if (recv_len <= 0) {
		// FIXME
		//if (errno == EAGAIN) {
		//	return 1;
		//} else {
			//hammer_session_remove(socket);
			printf("read unencrypted, Hey!!!\n");
			return -1;
		//}

	}
	/* Batch this job */
	hammer_batch_job_add(batch, c, recv_len);


	/* Unlock, Now gpu worker has completed this read, GPU can launch this batch */
	///////////////////////////////////////////////////////////
	pthread_mutex_unlock(&(batch->mutex_batch_launch));

	/* check its r_conn  */
	if (c->r_conn == NULL) {
		hammer_err("This r_conn is considered to be existed \n");
		exit(0);
	}

	return 0;
}

/* This function trigger write event of all the jobs in this batch */
int hammer_batch_forwarding(hammer_batch_t *batch)
{
	int i;
	hammer_connection_t *rc;
	hammer_job_t *this_job;
	hammer_sched_t *sched = hammer_sched_get_sched_struct();
	hammer_batch_buf_t *buf;

	assert(batch->processed_buf_id == (batch->cur_buf_id ^ 0x1));
	/* Get the buf that has been processed by GPU */
	if (batch->processed_buf_id == 0) {
		buf = &(batch->buf_A);
	} else {
		buf = &(batch->buf_B);
	}

	/* Set each connection to forward */
	for (i = 0; i < buf->job_num; i ++) {
		this_job = &(buf->job_list[i]);
		rc = this_job->connection->r_conn;
		
		hammer_epoll_change_mode(sched->epoll_fd,
				rc->socket,
				HAMMER_EPOLL_WRITE,
				HAMMER_EPOLL_LEVEL_TRIGGERED);
	}

	/* Mark this event has been processed */
	batch->processed_buf_id = -1;

	return 0;
}

int hammer_batch_switch_buffer(hammer_batch_t *batch)
{
	if (batch->cur_buf_id == 0) {
		batch->cur_buf = &(batch->buf_B);
		batch->cur_buf_id = 1;
	} else {
		batch->cur_buf = &(batch->buf_A);
		batch->cur_buf_id = 0;
	}

	/* mark this event has been processed, and buf is switched*/
	batch->buf_has_been_taken = -1;

	/* refresh cur_buf  */
	batch->cur_buf->job_num = 0;
	batch->cur_buf->buf_length = 0;

	return 0;
}

int hammer_batch_if_gpu_processed_new(hammer_batch_t *batch)
{
	if (batch->processed_buf_id == -1) {
		return 0;
	} else if (batch->processed_buf_id == 0 || batch->processed_buf_id == 1) {
		return 1;
	} else {
		hammer_err("error processed_buf_id\n");
		exit(0);
	}
}

int hammer_batch_if_current_buf_taken(hammer_batch_t *batch)
{
	if (batch->buf_has_been_taken != -1) {
		/* This buf has been taken */
		assert(batch->buf_has_been_taken == batch->cur_buf_id);
		return 1;
	} else {
		return 0;
	}
}

