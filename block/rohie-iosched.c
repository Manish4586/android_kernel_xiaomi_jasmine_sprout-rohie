/*
 * Copyright (c) 2018-2020, Rohie Kernel
 * Author: Manish4586 <manish.n.manish45@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>

enum { ASYNC, SYNC };

static const int sync_read_expire  = 1 * HZ;
static const int sync_write_expire = 1 * HZ;
static const int async_read_expire  =  2 * HZ;
static const int async_write_expire = 2 * HZ;
static const int writes_starved = 1;
static const int fifo_batch     = 1;

struct rohie_data {

	struct list_head fifo_list[2][2];

	unsigned int batched;
	unsigned int starved;

	int fifo_expire[2][2];
	int fifo_batch;
	int writes_starved;
};

static void rohie_merged_requests(struct request_queue *q, struct request *rq,
		    struct request *next)
{
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(next->fifo_time, rq->fifo_time)) {
			list_move(&rq->queuelist, &next->queuelist);
			rq->fifo_time = next->fifo_time;
		}
	}

	rq_fifo_clear(next);
}

static void rohie_add_request(struct request_queue *q, struct request *rq)
{
	struct rohie_data *td = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);
	const int data_dir = rq_data_dir(rq);

	rq->fifo_time = jiffies + td->fifo_expire[sync][data_dir];
	list_add(&rq->queuelist, &td->fifo_list[sync][data_dir]);
}

static struct request *rohie_expired_request(struct rohie_data *td, int sync, int data_dir)
{
	struct list_head *list = &td->fifo_list[sync][data_dir];
	struct request *rq;

	if (list_empty(list))
		return NULL;

	rq = rq_entry_fifo(list->next);

	if (time_after(jiffies, rq->fifo_time))
		return rq;

	return NULL;
}

static struct request *rohie_choose_expired_request(struct rohie_data *td)
{
	struct request *rq;

	rq = rohie_expired_request(td, ASYNC, WRITE);
	if (rq)
		return rq;
	rq = rohie_expired_request(td, ASYNC, READ);
	if (rq)
		return rq;

	rq = rohie_expired_request(td, SYNC, WRITE);
	if (rq)
		return rq;
	rq = rohie_expired_request(td, SYNC, READ);
	if (rq)
		return rq;

	return NULL;
}

static struct request *rohie_choose_request(struct rohie_data *td, int data_dir)
{
	struct list_head *sync = td->fifo_list[SYNC];
	struct list_head *async = td->fifo_list[ASYNC];

	if (!list_empty(&sync[data_dir]))
		return rq_entry_fifo(sync[data_dir].next);
	if (!list_empty(&sync[!data_dir]))
		return rq_entry_fifo(sync[!data_dir].next);

	if (!list_empty(&async[data_dir]))
		return rq_entry_fifo(async[data_dir].next);
	if (!list_empty(&async[!data_dir]))
		return rq_entry_fifo(async[!data_dir].next);

	return NULL;
}

static inline void rohie_dispatch_request(struct rohie_data *td, struct request *rq)
{
	rq_fifo_clear(rq);
	elv_dispatch_add_tail(rq->q, rq);

	td->batched++;

	if (rq_data_dir(rq))
		td->starved = 0;
	else
		td->starved++;
}

static int rohie_dispatch_requests(struct request_queue *q, int force)
{
	struct rohie_data *td = q->elevator->elevator_data;
	struct request *rq = NULL;
	int data_dir = READ;

	if (td->batched > td->fifo_batch) {
		td->batched = 0;
		rq = rohie_choose_expired_request(td);
	}

	if (!rq) {
		if (td->starved > td->writes_starved)
			data_dir = WRITE;

		rq = rohie_choose_request(td, data_dir);
		if (!rq)
			return 0;
	}

	rohie_dispatch_request(td, rq);

	return 1;
}

static struct request *rohie_former_request(struct request_queue *q, struct request *rq)
{
	struct rohie_data *td = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);
	const int data_dir = rq_data_dir(rq);

	if (rq->queuelist.prev == &td->fifo_list[sync][data_dir])
		return NULL;

	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *rohie_latter_request(struct request_queue *q, struct request *rq)
{
	struct rohie_data *td = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);
	const int data_dir = rq_data_dir(rq);

	if (rq->queuelist.next == &td->fifo_list[sync][data_dir])
		return NULL;

	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static int rohie_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct rohie_data *td;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	td = kmalloc_node(sizeof(*td), GFP_KERNEL, q->node);
	if (!td) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = td;

	INIT_LIST_HEAD(&td->fifo_list[SYNC][READ]);
	INIT_LIST_HEAD(&td->fifo_list[SYNC][WRITE]);
	INIT_LIST_HEAD(&td->fifo_list[ASYNC][READ]);
	INIT_LIST_HEAD(&td->fifo_list[ASYNC][WRITE]);

	td->batched = 0;
	td->fifo_expire[SYNC][READ] = sync_read_expire;
	td->fifo_expire[SYNC][WRITE] = sync_write_expire;
	td->fifo_expire[ASYNC][READ] = async_read_expire;
	td->fifo_expire[ASYNC][WRITE] = async_write_expire;
	td->fifo_batch = fifo_batch;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void rohie_exit_queue(struct elevator_queue *e)
{
	struct rohie_data *td = e->elevator_data;

	BUG_ON(!list_empty(&td->fifo_list[SYNC][READ]));
	BUG_ON(!list_empty(&td->fifo_list[SYNC][WRITE]));
	BUG_ON(!list_empty(&td->fifo_list[ASYNC][READ]));
	BUG_ON(!list_empty(&td->fifo_list[ASYNC][WRITE]));

	kfree(td);
}

static struct elevator_type iosched_rohie = {
	.ops = {
		.elevator_merge_req_fn		= rohie_merged_requests,
		.elevator_dispatch_fn		= rohie_dispatch_requests,
		.elevator_add_req_fn		= rohie_add_request,
		.elevator_former_req_fn		= rohie_former_request,
		.elevator_latter_req_fn		= rohie_latter_request,
		.elevator_init_fn		= rohie_init_queue,
		.elevator_exit_fn		= rohie_exit_queue,
	},
	.elevator_name = "rohie",
	.elevator_owner = THIS_MODULE,
};

static int __init rohie_init(void)
{
	elv_register(&iosched_rohie);
	return 0;
}

static void __exit rohie_exit(void)
{
	elv_unregister(&iosched_rohie);
}

module_init(rohie_init);
module_exit(rohie_exit);

MODULE_AUTHOR("Manish4586");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rohie IO Scheduler");
