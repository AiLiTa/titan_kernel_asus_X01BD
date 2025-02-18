/*
 * Anxiety IO scheduler
 *
 * Anxiety is designed for mobile devices that are latency
 * sensitive. It is based on the no-op scheduler with additional
 * tweaks and changes. Using Anxiety on Android devices with
 * interactive and realtime workloads has proven significantly
 * faster R/W speeds in smaller buffer sizes (ie. databases,
 * journals, raw assets, etc).
 *
 * Copyright (C) 2018-2019 Tyler Nijmeh <tylernij@gmail.com>
 * Copyright (C) 2018 kdrag0n <dragon@khronodragon.com>
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/fb.h>

/* Default tunable values */
#define	DEFAULT_MAX_WRITES_STARVED		8	/* Max times reads can starve a write */
#define	DEFAULT_MAX_WRITES_STARVED_SUSPENDED	0	/* Ditto but during screen-off states */

struct anxiety_data {
	struct list_head queue[2] ____cacheline_aligned;
	uint16_t writes_starved;
	struct notifier_block fb_notifier;
	bool display_on;

	/* Tunables */
	uint8_t max_writes_starved;
	uint8_t max_writes_starved_suspended;
};

static void anxiety_merged_requests(struct request_queue *q, struct request *rq, struct request *next)
{
	rq_fifo_clear(next);
}

static __always_inline struct request *anxiety_choose_request(struct anxiety_data *adata)
{
	/* Prioritize reads unless writes are exceedingly starved */
	bool starved = adata->writes_starved > (adata->display_on ? adata->max_writes_starved_suspended : adata->max_writes_starved);

	/* Handle a read request */
	if (!starved && !list_empty(&adata->queue[READ])) {
		adata->writes_starved++;
		return rq_entry_fifo(adata->queue[READ].next);
	}

	/* Handle a write request */
	if (!list_empty(&adata->queue[WRITE])) {
		adata->writes_starved = 0;
		return rq_entry_fifo(adata->queue[WRITE].next);
	}

	/* If there are no requests, then there is nothing to starve */
	adata->writes_starved = 0;
	return NULL;
}

static int anxiety_dispatch(struct request_queue *q, int force)
{
	struct request *rq = anxiety_choose_request(q->elevator->elevator_data);

	if (!rq)
		return 0;

	rq_fifo_clear(rq);
	elv_dispatch_add_tail(rq->q, rq);

	return 1;
}

static void anxiety_add_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	list_add_tail(&rq->queuelist, &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir]);
}

static struct request *anxiety_former_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	if (rq->queuelist.prev == &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir])
		return NULL;

	return list_prev_entry(rq, queuelist);
}

static struct request *anxiety_latter_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	if (rq->queuelist.next == &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir])
		return NULL;

	return list_next_entry(rq, queuelist);
}

static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct anxiety_data *adata = container_of(self, struct anxiety_data, fb_notifier);
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
			case FB_BLANK_UNBLANK:
				adata->display_on = true;
				break;
			case FB_BLANK_POWERDOWN:
			case FB_BLANK_HSYNC_SUSPEND:
			case FB_BLANK_VSYNC_SUSPEND:
			case FB_BLANK_NORMAL:
				adata->display_on = false;
				break;
		}
	}

	return 0;
}

static int anxiety_init_queue(struct request_queue *q, struct elevator_type *elv)
{
	struct anxiety_data *adata;
	struct elevator_queue *eq = elevator_alloc(q, elv);

	if (!eq)
		return -ENOMEM;

	/* Allocate the data */
	adata = kmalloc_node(sizeof(*adata), GFP_KERNEL, q->node);
	if (!adata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	/* Set the elevator data */
	eq->elevator_data = adata;

	/* Initialize */
	INIT_LIST_HEAD(&adata->queue[READ]);
	INIT_LIST_HEAD(&adata->queue[WRITE]);
	adata->writes_starved = 0;
	adata->max_writes_starved = DEFAULT_MAX_WRITES_STARVED;
	adata->max_writes_starved_suspended = DEFAULT_MAX_WRITES_STARVED_SUSPENDED;

	adata->fb_notifier.notifier_call = fb_notifier_callback;
	fb_register_client(&adata->fb_notifier);

	/* Set elevator to Anxiety */
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void anxiety_exit_queue(struct elevator_queue *e)
{
	struct anxiety_data *adata = e->elevator_data;

	fb_unregister_client(&adata->fb_notifier);

	kfree(adata);
}

/* Sysfs access */
static ssize_t anxiety_max_writes_starved_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;

	return snprintf(page, PAGE_SIZE, "%u\n", adata->max_writes_starved);
}

static ssize_t anxiety_max_writes_starved_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	int ret;

	ret = kstrtou8(page, 0, &adata->max_writes_starved);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t anxiety_max_writes_starved_suspended_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;

	return snprintf(page, PAGE_SIZE, "%u\n", adata->max_writes_starved_suspended);
}

static ssize_t anxiety_max_writes_starved_suspended_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	int ret;

	ret = kstrtou8(page, 0, &adata->max_writes_starved_suspended);
	if (ret < 0)
		return ret;

	return count;
}

static struct elv_fs_entry anxiety_attrs[] = {
	__ATTR(max_writes_starved, 0644, anxiety_max_writes_starved_show, anxiety_max_writes_starved_store),
	__ATTR(max_writes_starved_suspended, 0644, anxiety_max_writes_starved_suspended_show, anxiety_max_writes_starved_suspended_store),
	__ATTR_NULL
};

static struct elevator_type elevator_anxiety = {
	.ops = {
		.elevator_merge_req_fn	= anxiety_merged_requests,
		.elevator_dispatch_fn	= anxiety_dispatch,
		.elevator_add_req_fn	= anxiety_add_request,
		.elevator_former_req_fn	= anxiety_former_request,
		.elevator_latter_req_fn	= anxiety_latter_request,
		.elevator_init_fn	= anxiety_init_queue,
		.elevator_exit_fn	= anxiety_exit_queue,
	},
	.elevator_name = "anxiety",
	.elevator_attrs = anxiety_attrs,
	.elevator_owner = THIS_MODULE,
};

static int __init anxiety_init(void)
{
	return elv_register(&elevator_anxiety);
}

static void __exit anxiety_exit(void)
{
	elv_unregister(&elevator_anxiety);
}

module_init(anxiety_init);
module_exit(anxiety_exit);

MODULE_AUTHOR("Tyler Nijmeh");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Anxiety IO scheduler");
