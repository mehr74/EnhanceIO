/*
 * True Transparent Caching (TTC) code.
 *  eio_ttc.c
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 * 
 *  Made EIO fully transparent with respect to applications. A cache can be
 *  created or deleted while a filesystem or applications are online
 *   Amit Kale <akale@stec-inc.com>
 *   Ramprasad Chinthekindi <rchinthekindi@stec-inc.com>
 *   Akhil Bhansali <abhansali@stec-inc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include "os.h"
#include "eio_ttc.h"
/*
#define BIO_RW_BARRIER		REQ_FLUSH
#define BIO_RW_DISCARD 		REQ_DISCARD
#define WRITE_BARRIER   (WRITE | ((unsigned long)1 << BIO_RW_BARRIER))
#define BIO_RW_UNPLUG		REQ_FLUSH
#define BIO_RW_SYNCIO		REQ_SYNC
static inline bool bio_rw_flagged(struct bio *bio, int flag)
{
        return (bio->bi_rw & (1 << flag)) != 0;
}

#define bio_empty_barrier(bio)  (bio_rw_flagged(bio, BIO_RW_BARRIER) && !bio_has_data(bio) && !bio_rw_flagged(bio, BIO_RW_DISCARD))
*/
struct rw_semaphore	eio_ttc_lock[EIO_HASHTBL_SIZE];
static struct list_head	eio_ttc_list[EIO_HASHTBL_SIZE];

int eio_reboot_notified = 0;
extern int eio_force_warm_boot;

extern long eio_ioctl(struct file *filp, unsigned cmd, unsigned long arg);
extern long eio_compact_ioctl(struct file *filp, unsigned cmd, unsigned long arg);

extern mempool_t *_io_pool;
extern mempool_t *_dmc_bio_pool;
extern struct eio_control_s *eio_control;

static int eio_make_request_fn(struct request_queue *, struct bio *);
static void eio_cache_rec_fill(struct cache_c *, cache_rec_short_t *);
static void eio_enqueue_io(struct eio_barrier_q *, struct cache_c *, struct bio *, int);
static void eio_wq_work(struct work_struct *);
static void eio_process_barrier(struct eio_barrier_q *, struct cache_c *, struct bio *);
static void eio_io_uncached_partition(struct eio_barrier_q *, struct bio *);
static void eio_bio_end_empty_barrier(struct bio *, int);
static void eio_issue_empty_barrier_flush(struct block_device *, struct bio *,
	 			    int , struct eio_barrier_q *, int rw_flags);
static int eio_finish_nrdirty(struct cache_c *);
static int eio_mode_switch(struct cache_c *, u_int32_t);
static int eio_policy_switch(struct cache_c *, u_int32_t);
static struct eio_barrier_q * eio_barrier_q_alloc_init(void);
static void eio_barrier_q_free(struct eio_barrier_q *);

static int eio_overlap_split_bio(struct request_queue *, struct bio *);
static struct bio * eio_split_new_bio(struct bio *, struct bio_container *,
				      unsigned *, unsigned *, sector_t);
static void eio_split_endio(struct bio *, int);

static int
eio_open(struct inode *ip, struct file *filp)
{
	__module_get(THIS_MODULE);
	return 0;
}

static int
eio_release(struct inode *ip, struct file *filp)
{
	module_put(THIS_MODULE);
	return 0;
}

static struct file_operations eio_fops = {
	.open			= eio_open,
	.release		= eio_release,
	.unlocked_ioctl		= eio_ioctl,
	.compat_ioctl		= eio_compact_ioctl,
	.owner			= THIS_MODULE, 
};

static struct miscdevice eio_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= MISC_DEVICE,
	.fops		= &eio_fops,
};

int
eio_create_misc_device()
{
	return misc_register(&eio_misc);
}

int
eio_delete_misc_device()
{
	return misc_deregister(&eio_misc);
}

int
eio_ttc_get_device(const char *path, fmode_t mode, struct eio_bdev **result)
{
	struct block_device	*bdev;
	struct eio_bdev		*eio_bdev;
	unsigned int		major, minor;
	dev_t			uninitialized_var(dev);
	static char             *eio_holder = "ENHANCE IO"; 

	if (sscanf(path, "%u:%u", &major, &minor) == 2) {
		/* Extract the major/minor numbers */
		dev = MKDEV(major, minor);
		if (MAJOR(dev) != major || MINOR(dev) != minor)
			return -EOVERFLOW;
	} else {
		/* convert the path to a device */
		struct block_device *bdev = lookup_bdev(path);

		if (IS_ERR(bdev))
			return PTR_ERR(bdev);

		dev = bdev->bd_dev;
		bdput(bdev);
	}

	bdev = blkdev_get_by_dev(dev, mode, eio_holder);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	/*
	 * Do we need to claim the devices ??
	 * bd_claim_by_disk(bdev, charptr, gendisk)
	 */

	eio_bdev = (struct eio_bdev *)KZALLOC(sizeof(*eio_bdev), GFP_KERNEL);
	if (eio_bdev == NULL) {
		blkdev_put(bdev, mode);
		return -ENOMEM;
	}

	eio_bdev->bdev = bdev;
	eio_bdev->mode = mode;
	*result = eio_bdev;
	return 0;
}

void
eio_ttc_put_device(struct eio_bdev **d)
{
	struct eio_bdev	*eio_bdev;

	eio_bdev = *d;
	blkdev_put(eio_bdev->bdev, eio_bdev->mode);
	kfree(eio_bdev);
	*d = NULL;
	return;
}

struct cache_c *
eio_cache_lookup(char *name)
{
	struct cache_c	*dmc = NULL;
	int		i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_read(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {
			if (!strcmp(name, dmc->cache_name)) {
				up_read(&eio_ttc_lock[i]);
				return dmc;
			}
		}
		up_read(&eio_ttc_lock[i]);
	}
	return NULL;
}

int
eio_ttc_activate(struct cache_c *dmc)
{
	struct block_device	*bdev;
	struct request_queue	*rq;
	make_request_fn		*origmfn;
	struct cache_c		*dmc1;
	struct eio_barrier_q	*bq;
	int			wholedisk;
	int			error;
	int			index;
	int			rw_flags = 0;

	bdev = dmc->disk_dev->bdev;
	if (bdev == NULL) {
		EIOERR("cache_create: Source device not found\n");
		return (-ENODEV);
	}
	rq = bdev->bd_disk->queue;

	wholedisk = 0;
	if (bdev == bdev->bd_contains) {
		wholedisk = 1;
	}

	dmc->dev_start_sect = bdev->bd_part->start_sect;
	dmc->dev_end_sect =
		bdev->bd_part->start_sect + bdev->bd_part->nr_sects - 1;

	EIODEBUG("eio_ttc_activate: Device/Partition"
		 " sector_start: %llu, end: %llu\n",
		 (uint64_t)dmc->dev_start_sect, (uint64_t)dmc->dev_end_sect);

retry:
	error = 0;
	origmfn = NULL;
	bq = NULL;
	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	down_write(&eio_ttc_lock[index]);
	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
			continue;

		if ((wholedisk) || (dmc1->dev_info == EIO_DEV_WHOLE_DISK) ||
		    (dmc1->disk_dev->bdev == bdev)) {
			error = -EINVAL;
			up_write(&eio_ttc_lock[index]);
			goto out;
		}

		/* some partition of same device already cached */
		bq = dmc1->barrier_q;
		if (!list_empty(&bq->deferred_list)) {
			up_write(&eio_ttc_lock[index]);
			schedule_timeout(msecs_to_jiffies(100));
			EIODEBUG("ttc_activate: Device queue not empty, retrying...\n");
			goto retry;
		}
		VERIFY(dmc1->dev_info == EIO_DEV_PARTITION);
		origmfn = dmc1->origmfn;
		break;
	}

	/*
	 * Save original make_request_fn. Switch make_request_fn only once.
	 */

	if (origmfn) {
		bq->ref_count++;
		dmc->barrier_q = bq;
		dmc->origmfn = origmfn;
		dmc->dev_info = EIO_DEV_PARTITION;
		VERIFY(wholedisk == 0);
		VERIFY(origmfn == bq->origmfn);
	} else {
		/* Allocate eio_barrier_q */
		bq = eio_barrier_q_alloc_init();
		if (!bq) {
			error = -ENOMEM;
			up_write(&eio_ttc_lock[index]);
			goto out;
		}
		bq->ttc_lock_index = index;
		dmc->barrier_q = bq;
		bq->ref_count++;
		bq->origmfn = rq->make_request_fn;
		dmc->origmfn = rq->make_request_fn;
		rq->make_request_fn = eio_make_request_fn;
		dmc->dev_info = (wholedisk) ? EIO_DEV_WHOLE_DISK : EIO_DEV_PARTITION;
	}

	list_add_tail(&dmc->cachelist, &eio_ttc_list[index]);

	/*
	 * Sleep for sometime, to allow previous I/Os to hit 
	 * Issue a barrier I/O on Source device.
	 */

	msleep(1);
	SET_BARRIER_FLAGS(rw_flags);
	eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, NULL,
				EIO_HDD_DEVICE, dmc->barrier_q, rw_flags);
	up_write(&eio_ttc_lock[index]);

out:
	if (error == -EINVAL) {
		if (wholedisk)
			EIOERR("cache_create: A partition of this device is already cached.\n");
		else
			EIOERR("cache_create: Device is already cached.\n");
	}
	return error;
}

int
eio_ttc_deactivate(struct cache_c *dmc, int force)
{
	struct block_device	*bdev;
	struct request_queue	*rq;
	struct cache_c		*dmc1;
	struct eio_barrier_q	*bq;
	int			found_partitions;
	int			index;
	int			ret;

	ret = 0;
	bq = dmc->barrier_q;
	bdev = dmc->disk_dev->bdev;
	rq = bdev->bd_disk->queue;

	if (force)
		goto deactivate;

	/* Process and wait for nr_dirty to drop to zero */
	if (dmc->mode == CACHE_MODE_WB) {
		if (!CACHE_FAILED_IS_SET(dmc))  {
			ret = eio_finish_nrdirty(dmc);
			if (ret) {
				EIOERR("ttc_deactivate: nrdirty failed to finish for cache \"%s\".",
					dmc->cache_name);
				return ret;
			}
		} else {
			EIODEBUG("ttc_deactivate: Cache \"%s\" failed is already set. Continue with cache delete.",
					dmc->cache_name);
		}
	}

	/*
	 * Traverse the list and see if other partitions of this device are
	 * cached. Switch mfn if this is the only partition of the device
	 * in the list.
	 */
deactivate:
	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);
	found_partitions = 0;

	/* check if barrier QUEUE is empty or not */
retry:
	down_write(&eio_ttc_lock[index]);
	spin_lock_irq(&bq->deferred_lock);
	if (!list_empty(&bq->deferred_list)) {
		spin_unlock_irq(&bq->deferred_lock);
		up_write(&eio_ttc_lock[index]);
		EIOINFO("ttc_deactivate: Waiting for device barrier/flush queue to drain\n");
		schedule_timeout(msecs_to_jiffies(100));
		goto retry;
	}
	spin_unlock_irq(&bq->deferred_lock);

	if (dmc->dev_info != EIO_DEV_WHOLE_DISK) {
		list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
			if (dmc == dmc1)
				continue;

			if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
				continue;

			VERIFY(dmc1->dev_info == EIO_DEV_PARTITION);

			/*
			 * There are still other partitions which are cached.
			 * Do not switch the make_request_fn.
			 */

			found_partitions = 1;
			break;
		}
	}

	if ((dmc->dev_info == EIO_DEV_WHOLE_DISK) || (found_partitions == 0)) {
		rq->make_request_fn = dmc->origmfn;
		VERIFY(bq->ref_count == 1);
		bq->ref_count--;
		dmc->barrier_q = NULL;
		eio_barrier_q_free(bq);
	} else {
		bq->ref_count--;
		dmc->barrier_q = NULL;
	}

	list_del_init(&dmc->cachelist);
	up_write(&eio_ttc_lock[index]);

	/* wait for nr_ios to drain-out */
	while (ATOMIC_READ(&dmc->nr_ios) != 0)
		schedule_timeout(msecs_to_jiffies(100));

	return ret;
}

void
eio_ttc_init(void)
{
	int	i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		init_rwsem(&eio_ttc_lock[i]);
		INIT_LIST_HEAD(&eio_ttc_list[i]);
	}
}

/*
 * Cases:-
 * 1. Full device cached.
 *	if (ENQUEUE || barrier(bio))
 *		enqueue (dmc, bio) and return
 * 	else
 *		call eio_map(dmc, bio)
 * 2. Some partitions of the device cached.
 *	if (ENQUEUE || barrier(bio))
 *		All I/Os (both on cached and uncached partitions) are enqueued.
 * 	else
 *		if (I/O on cached partition)
 *			call eio_map(dmc, bio)
 *		else
 *			origmfn(bio);	// uncached partition
 * 3. q->mfn got switched back to original
 *	call origmfn(q, bio)
 * 4. Race condition:
 */

static void
eio_make_request_fn(struct request_queue *q, struct bio *bio)
{
	int			ret;
	int			overlap;
	int			barrier;
	int			enqueue;
	int			index;
	make_request_fn		*origmfn;
	struct cache_c		*dmc, *dmc1;
	struct eio_barrier_q	*bq;
	struct block_device	*bdev;

	barrier = 0;
	bdev = bio->bi_bdev;


re_lookup:
	dmc = NULL;
	origmfn = NULL;
	bq = NULL;
	overlap = enqueue = ret = 0;

	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	if (barrier)
		down_write(&eio_ttc_lock[index]);
	else
		down_read(&eio_ttc_lock[index]);

	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains) {
			continue;
		}

		/* For uncached partitions, check if IO needs to be enqueued */
		if (!enqueue) {
			if (barrier || (dmc1->barrier_q->queue_flags == EIO_QUEUE_IO_TO_THREAD)) {
				enqueue = 1;
				bq = dmc1->barrier_q;
			}
		}

		if (dmc1->dev_info == EIO_DEV_WHOLE_DISK) {
			dmc = dmc1;	/* found cached device */
			break;
		}

		/* Handle partitions */
		if (!origmfn)
			origmfn = dmc1->origmfn;

		/* I/O perfectly fit within cached partition */
		if ((bio->bi_sector >= dmc1->dev_start_sect) &&
		    ((bio->bi_sector + to_sector(bio->bi_size) - 1) <=
		     dmc1->dev_end_sect)) {
			VERIFY(overlap == 0);
			dmc = dmc1;	/* found cached partition */
			break;
		}

		/* Check if I/O is overlapping with cached partitions */
		if (((bio->bi_sector >= dmc1->dev_start_sect) &&
		     (bio->bi_sector <= dmc1->dev_end_sect)) ||
		    ((bio->bi_sector + to_sector(bio->bi_size) - 1 >=
		      dmc1->dev_start_sect) &&
		     (bio->bi_sector + to_sector(bio->bi_size) - 1 <=
		      dmc1->dev_end_sect))) {
			overlap = 1;
			EIOERR("Overlapping I/O detected on %s cache at sector: %llu, size: %u\n",
				dmc1->cache_name, (uint64_t)bio->bi_sector, bio->bi_size);
			break;
		}
	}

	if (unlikely(overlap)) {
		/* unlock ttc lock */
		if (barrier)
			up_write(&eio_ttc_lock[index]);
		else
			up_read(&eio_ttc_lock[index]);

		if (bio_rw_flagged(bio, BIO_RW_DISCARD)) {
			EIOERR("eio_mfn: Overlap I/O with Discard flag received."
				" Discard flag is not supported.\n");
			bio_endio(bio, -EOPNOTSUPP);
		} else {
			ret = eio_overlap_split_bio(q, bio);
		}
	} else if (enqueue || barrier) {
		VERIFY(bq != NULL);
		eio_enqueue_io(bq, dmc, bio, barrier);
	} else if (dmc) {	/* found cached partition or device */

		/*
		 * Start sector of cached partition may or may not be
		 * aligned with cache blocksize.
		 * Map start of the partition to zero reference.
		 */

		if (bio->bi_sector) {
			VERIFY(bio->bi_sector >= dmc->dev_start_sect);
			bio->bi_sector -= dmc->dev_start_sect;
		}
		ret = eio_map(dmc, q, bio);
		if (ret) {
			/* Error case: restore the start sector of bio */
			bio->bi_sector += dmc->dev_start_sect;
		}
	}

	if (!overlap) {
		if (barrier)
			up_write(&eio_ttc_lock[index]);
		else
			up_read(&eio_ttc_lock[index]);
	}

	if (overlap || enqueue || barrier || dmc)
		return;

	/*
	 * Race condition:-
	 * origmfn can be NULL if  all partitions or whole disk got uncached.
	 * We set origmfn = q->mfn if origmfn is NULL.
	 * The origmfn may now again be eio_make_request_fn because
	 * someone else switched the q->mfn because of a new
	 * partition or whole disk being cached.
	 * Since, we cannot protect q->make_request_fn() by any lock,
	 * this situation may occur. However, this is a very rare event.
	 * In this case restart the lookup.
	 */

	if (origmfn == NULL)
		origmfn = q->make_request_fn;
	if (origmfn == eio_make_request_fn)
		goto re_lookup;

	origmfn(q, bio);
	return;
}

uint64_t
eio_get_cache_count(void)
{
	struct cache_c		*dmc;
	uint64_t		cnt = 0;
	int			i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_read(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {
			cnt++;
		}
		up_read(&eio_ttc_lock[i]);
	}
	return cnt;
}

int
eio_get_cache_list(unsigned long *arg)
{
	int				error = 0;
	unsigned int			size, i, j;
	cache_list_t			reclist;
	cache_rec_short_t		*cache_recs;
	struct cache_c			*dmc;

	if (copy_from_user(&reclist, (cache_list_t *)arg,
			   sizeof (cache_list_t))) {
		error = -EFAULT;
		goto out;
	}

	size = reclist.ncaches * sizeof (cache_rec_short_t);
	cache_recs = vmalloc(size);
	if (!cache_recs) {
		error = -ENOMEM;
		goto out;
	}
	memset(cache_recs, 0, size);

	i = 0;
	for (j = 0; j < EIO_HASHTBL_SIZE; j++) {
		down_read(&eio_ttc_lock[j]);
		list_for_each_entry(dmc, &eio_ttc_list[j], cachelist) {
			eio_cache_rec_fill(dmc, &cache_recs[i]);
			i++;

			if (i == reclist.ncaches)
				break;
		}
		up_read(&eio_ttc_lock[j]);

		if (i == reclist.ncaches)
			break;
	}

	if (copy_to_user((char *)reclist.cachelist,
			 (char *)cache_recs, size)) {
		error = -EFAULT;
		goto out;
	}

	if (copy_to_user((cache_list_t *)arg, &reclist,
			 sizeof (cache_list_t))) {
		error = -EFAULT;
		goto out;
	}

out:
	return error;
}

static void
eio_cache_rec_fill(struct cache_c *dmc, cache_rec_short_t *rec)
{
	STRNCPY(rec->cr_name, dmc->cache_name,
		sizeof (rec->cr_name));
	STRNCPY(rec->cr_src_devname, dmc->disk_devname,
		sizeof (rec->cr_src_devname));
	STRNCPY(rec->cr_ssd_devname, dmc->cache_devname,
		sizeof (rec->cr_ssd_devname));
	rec->cr_src_dev_size = eio_get_device_size(dmc->disk_dev);
	rec->cr_ssd_dev_size = eio_get_device_size(dmc->cache_dev);
	rec->cr_src_sector_size = 0;	/* unused in userspace */
	rec->cr_ssd_sector_size = 0;	/* unused in userspace */
	rec->cr_flags = dmc->cache_flags;
	rec->cr_policy = dmc->req_policy;
	rec->cr_mode = dmc->mode;
	rec->cr_persistence = dmc->persistence;
	rec->cr_blksize = dmc->block_size;	/* In sectors */
	rec->cr_assoc = dmc->assoc;
	return;
}

/*
 * Few sanity checks before cache creation.
 */

int
eio_do_preliminary_checks(struct cache_c *dmc)
{
	struct block_device	*bdev, *ssd_bdev;
	struct cache_c		*dmc1;
	int			error;
	int			wholedisk;
	int			index;

	error = wholedisk = 0;
	bdev = dmc->disk_dev->bdev;
	ssd_bdev = dmc->cache_dev->bdev;

	/*
	 * Disallow cache creation if source and cache device
	 * belong to same device.
	 */

	if (bdev->bd_contains == ssd_bdev->bd_contains)
		return -EINVAL;

	/*
	 * Check if cache with same name exists.
	 */

	if (eio_cache_lookup(dmc->cache_name))
		return -EEXIST;

	if (bdev == bdev->bd_contains) {
		wholedisk = 1;
	}

	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	down_read(&eio_ttc_lock[index]);
	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
			continue;

		if ((wholedisk) || (dmc1->dev_info == EIO_DEV_WHOLE_DISK) ||
		    (dmc1->disk_dev->bdev == bdev)) {
			error = -EINVAL;
			break;
		}
	}
	up_read(&eio_ttc_lock[index]);
	return error;
}

/* Use mempool_alloc and free for io in sync_io as well */
void eio_dec_count(struct eio_context *io, int error)
{

	if (error)
		io->error = error;

	if (atomic_dec_and_test(&io->count)) {
		if (io->event) {
			complete(io->event);
		} else {
			int err = io->error;
			eio_notify_fn fn = io->callback;
			void *context = io->context;

			mempool_free(io, _io_pool);
			io = NULL;
			fn(err, context);
		}
	}
}

void eio_endio(struct bio *bio, int error)
{
	struct eio_context *io;

	io = bio->bi_private;
	VERIFY (io != NULL);

	bio_put(bio);

	eio_dec_count(io, error);
}

int eio_dispatch_io_pages(struct cache_c *dmc, struct eio_io_region *where, int rw, struct page **pagelist,
			struct eio_context *io, int hddio, int num_vecs, int sync)
{
	struct bio 	*bio;
	struct page 	*page;
	unsigned long 	len;
	unsigned 	offset;
	int 		num_bvecs;
	int		remaining_bvecs = num_vecs;
	int 		ret = 0;
	int		pindex = 0;

	sector_t remaining = where->count;

	do {
		/* Verify that num_vecs should not cross the threshhold */
		/* Check how many max bvecs bdev supports */
		num_bvecs = min_t(int, bio_get_nr_vecs(where->bdev), remaining_bvecs);
		bio = bio_alloc(GFP_NOIO, num_bvecs);
		bio->bi_bdev = where->bdev;
		bio->bi_sector = where->sector + (where->count - remaining);

		/* Remap the start sector of partition */
		if (hddio)
			bio->bi_sector += dmc->dev_start_sect;
		bio->bi_rw |= rw;
		bio->bi_end_io = eio_endio;
		bio->bi_private = io;

		while (remaining) {
			page = pagelist[pindex];
			len = min_t(unsigned long, PAGE_SIZE, to_bytes(remaining));
			offset = 0;

			if (!bio_add_page(bio, page, len, offset))
				break;

			remaining -= to_sector(len);
			pindex++;
			remaining_bvecs--;
		}

		atomic_inc(&io->count);
		if (hddio) {
			dmc->origmfn(bdev_get_queue(bio->bi_bdev), bio);

		} else {
			submit_bio(rw, bio);
		}

	} while (remaining);

	VERIFY(remaining_bvecs == 0);
	return ret;
}

/*
 * This function will dispatch the i/o. It also takes care of
 * splitting the large I/O requets to smaller I/Os which may not
 * fit into single bio.
 */

int eio_dispatch_io(struct cache_c *dmc, struct eio_io_region *where, int rw, struct bio_vec *bvec,
			struct eio_context *io, int hddio, int num_vecs, int sync)
{
	struct bio 	*bio;
	struct page 	*page;
	unsigned long 	len;
	unsigned 	offset;
	int 		num_bvecs;
	int		remaining_bvecs = num_vecs;
	int 		ret = 0;

	sector_t remaining = where->count;

	do {
		/* Verify that num_vecs should not cross the threshhold */
		/* Check how many max bvecs bdev supports */
		num_bvecs = min_t(int, bio_get_nr_vecs(where->bdev), remaining_bvecs);
		bio = bio_alloc(GFP_NOIO, num_bvecs);
		bio->bi_bdev = where->bdev;
		bio->bi_sector = where->sector + (where->count - remaining);

		/* Remap the start sector of partition */
		if (hddio)
			bio->bi_sector += dmc->dev_start_sect;
		bio->bi_rw |= rw;
		bio->bi_end_io = eio_endio;
		bio->bi_private = io;

		while (remaining) {
			page = bvec->bv_page;
			len = min_t(unsigned long, bvec->bv_len, to_bytes(remaining));
			offset = bvec->bv_offset;

			if (!bio_add_page(bio, page, len, offset))
				break;

			offset = 0;
			remaining -= to_sector(len);
			bvec = bvec + 1;
			remaining_bvecs--;
		}

		atomic_inc(&io->count);
		if (hddio) {
			dmc->origmfn(bdev_get_queue(bio->bi_bdev), bio);
			if (ret) {
			}
		} else {
			submit_bio(rw, bio);
		}


	} while (remaining);

	VERIFY(remaining_bvecs == 0);
	return ret;
}


int eio_async_io(struct cache_c *dmc, struct eio_io_region *where, int rw, struct eio_io_request *req)
{
	struct eio_context *io;
	int err = 0;

	io = mempool_alloc(_io_pool, GFP_NOIO);
	if (unlikely(io == NULL)) {
		EIOERR("eio_async_io: failed to allocate eio_context.\n");
		return -ENOMEM;
	}
	BZERO((char *)io, sizeof (struct eio_context));

	atomic_set(&io->count, 1);
	io->callback = req->notify;
	io->context = req->context;
	io->event = NULL;

	switch (req->mtype) {
	case EIO_BVECS:
		err = eio_dispatch_io(dmc, where, rw, req->dptr.pages, io, req->hddio, req->num_bvecs, 0);
		break;

	case EIO_PAGES:
		err = eio_dispatch_io_pages(dmc, where, rw, req->dptr.plist, io, req->hddio, req->num_bvecs, 0);
		break;
	}

	/* Check if i/o submission has returned any error */
	if (unlikely(err)) {
		/* Wait for any i/os which are submitted, to end. */
retry:
		if (atomic_read(&io->count) != 1) {
			schedule_timeout(msecs_to_jiffies(1));
			goto retry;
		}

		VERIFY(io != NULL);
		mempool_free(io, _io_pool);
		io = NULL;
		return err;
	}
		
	/* Drop the extra reference count here */
	eio_dec_count(io, err);
	return err;
}

int eio_sync_io(struct cache_c *dmc, struct eio_io_region *where,
			int rw, struct eio_io_request *req)
{
	int ret = 0;
	struct eio_context io;
	DECLARE_COMPLETION_ONSTACK(wait);

	BZERO((char *)&io, sizeof io);

	atomic_set(&io.count, 1);
	io.event = &wait;
	io.callback = NULL;
	io.context = NULL;

	/* For synchronous I/Os pass SYNC & UNPLUG. */
	rw |= REQ_SYNC;

	switch(req->mtype) {
	case EIO_BVECS:
		ret = eio_dispatch_io(dmc, where, rw, req->dptr.pages,
					&io, req->hddio, req->num_bvecs, 1);
		break;
	case EIO_PAGES:
		ret = eio_dispatch_io_pages(dmc, where, rw, req->dptr.plist,
					&io, req->hddio, req->num_bvecs, 1);
		break;
	}

	/* Check if i/o submission has returned any error */
	if (unlikely(ret)) {
		/* Wait for any i/os which are submitted, to end. */
retry:
		if (atomic_read(&(io.count)) != 1) {
			schedule_timeout(msecs_to_jiffies(1));
			goto retry;
		}

		return ret;
	}

	/* Drop extra reference count here */
	eio_dec_count(&io, ret);
	wait_for_completion(&wait);

	if (io.error)
		ret = io.error;

	return ret;
}

int eio_do_io(struct cache_c *dmc, struct eio_io_region *where, int rw,
		struct eio_io_request *io_req)
{
	if (!io_req->notify)
		return eio_sync_io(dmc, where, rw, io_req);

	return eio_async_io(dmc, where, rw, io_req);
}

/**********************************************************************************
		 ****** BARRIER I/O and Deferred bio Handling ******
 *********************************************************************************/

static struct eio_barrier_q *
eio_barrier_q_alloc_init(void)
{
	struct eio_barrier_q	*bq;

	bq = (struct eio_barrier_q *)KZALLOC(sizeof(*bq), GFP_KERNEL);
	if (bq == NULL) {
		EIOERR("wq_alloc: Failed to allocate memory for barrier/flush queue\n");
		return NULL;
	}

	/*
	 * BARRIER I/O handling through workqueue.
	 */

	INIT_LIST_HEAD(&bq->deferred_list);
	INIT_WORK(&bq->barrier_defer_work, eio_wq_work);
	spin_lock_init(&bq->deferred_lock);
	bq->barrier_wq = create_singlethread_workqueue("eio-queue");
	if (!bq->barrier_wq) {
		EIOERR("wq_alloc: Failed to create workqueue\n");
		kfree(bq);
		return NULL;
	}
	return bq;
}

static void
eio_barrier_q_free(struct eio_barrier_q *bq)
{

	VERIFY(bq->ref_count == 0);
	VERIFY(bq->queue_flags == 0);
	VERIFY(bq->barrier_cnt == 0);
	VERIFY(list_empty(&bq->deferred_list));
	destroy_workqueue(bq->barrier_wq);
	kfree(bq);
	return;
}

static void
eio_enqueue_io(struct eio_barrier_q *bq, struct cache_c *dmc,
		struct bio *bio, int barrier)
{
	struct eio_bio_item	*item;

	item = mempool_alloc(_dmc_bio_pool, GFP_NOIO);
	if (!item) {
		/* XXX: Error handling */
	}
	item->bio = bio;
	item->dmc = dmc;
	INIT_LIST_HEAD(&item->bio_list1);

	spin_lock_irq(&bq->deferred_lock);
	if (barrier)
		bq->barrier_cnt++;
	list_add_tail(&item->bio_list1, &bq->deferred_list);
	spin_unlock_irq(&bq->deferred_lock);

	if (bq->queue_flags != EIO_QUEUE_IO_TO_THREAD) {
		VERIFY(bq->queue_flags == 0);
		VERIFY(barrier == 1);
		bq->queue_flags = EIO_QUEUE_IO_TO_THREAD;
		EIODEBUG("eio_enqueue_io: set enqueue flag\n");
		queue_work(bq->barrier_wq, &bq->barrier_defer_work);
	}
	return;
}

/*
 * Process the deferred bios
 */

static void
eio_wq_work(struct work_struct *work)
{
	struct eio_barrier_q	*bq;
	struct eio_bio_item	*item;

	struct cache_c		*dmc;
	struct bio		*bio;
	int			ret;
	int			write_lock;
	int			index;

	bq = container_of(work, struct eio_barrier_q, barrier_defer_work);
	write_lock = 0;
	index = bq->ttc_lock_index;

retry:
	if (write_lock) {
		down_write(&eio_ttc_lock[index]);
		spin_lock_irq(&bq->deferred_lock);
		if ((!bq->barrier_cnt) &&
		    (bq->queue_flags == EIO_QUEUE_IO_TO_THREAD)) {
			bq->queue_flags = 0;
			EIODEBUG("eio_enqueue_io: reset enqueue flag\n");
		}
		spin_unlock_irq(&bq->deferred_lock);
		up_write(&eio_ttc_lock[index]);
	}

	while (1) {
		down_read(&eio_ttc_lock[index]);
		spin_lock_irq(&bq->deferred_lock);
		if ((!bq->barrier_cnt) &&
		    (bq->queue_flags == EIO_QUEUE_IO_TO_THREAD)) {
			spin_unlock_irq(&bq->deferred_lock);
			up_read(&eio_ttc_lock[index]);
			write_lock = 1;
			goto retry;
		}

		if (list_empty(&bq->deferred_list)) {
			VERIFY(bq->queue_flags == 0);
			VERIFY(bq->barrier_cnt == 0);
			spin_unlock_irq(&bq->deferred_lock);
			up_read(&eio_ttc_lock[index]);
			break;
		}

		item = list_entry(bq->deferred_list.next, struct eio_bio_item, bio_list1);
		list_del(&item->bio_list1);

		bio = item->bio;
		dmc = item->dmc;
		if (unlikely(bio_rw_flagged(bio, BIO_RW_BARRIER))) {
			bq->barrier_cnt--;
			VERIFY(bq->barrier_cnt >= 0);
		}
		spin_unlock_irq(&bq->deferred_lock);
		mempool_free(item, _dmc_bio_pool);

		if (bio_rw_flagged(bio, BIO_RW_BARRIER)) {
			eio_process_barrier(bq, dmc, bio);
		} else {
			if (dmc) {	/* I/O on cached partition or device */
				if (bio->bi_sector) {
					VERIFY(bio->bi_sector >= dmc->dev_start_sect);
					bio->bi_sector -= dmc->dev_start_sect;
				}

				do {
					ret = eio_map(dmc, bdev_get_queue(bio->bi_bdev), bio);
					VERIFY(ret == 0);
				} while (ret != 0);
			} else {	/* I/O on uncached partition */
				eio_io_uncached_partition(bq, bio);
			}
		}
		up_read(&eio_ttc_lock[index]);
	}
	return;
}

static void
eio_io_uncached_partition(struct eio_barrier_q *bq, struct bio *bio)
{
	struct block_device	*bdev;

	bdev = bio->bi_bdev;
	bq->origmfn(bdev_get_queue(bdev), bio);
	return;
}

void
eio_process_zero_size_bio(struct cache_c *dmc, struct bio *origbio)
{
	struct eio_barrier_q *bq;
	unsigned long rw_flags = 0;

	/* Extract bio flags from original bio */
	bq = dmc->barrier_q;
	rw_flags = origbio->bi_rw;

	VERIFY(origbio->bi_size == 0);
	VERIFY(bq && rw_flags != 0);

	eio_issue_empty_barrier_flush(dmc->cache_dev->bdev, NULL,
					EIO_SSD_DEVICE, NULL, rw_flags);
	eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, origbio,
					EIO_HDD_DEVICE, bq, rw_flags);
}

/*
 * Barrier I/O handling:-
 * Empty Barrier I/O:
 *	- Wait for pending I/Os (I/Os issued before this barrier)
 *	  to complete.
 *	- call eio_issue_empty_barrier_flush() on to issue
 *	  barrier I/O on both HDD and SSD.
 *
 * Non-Empty Barrier I/O:
 *	- Wait for pending I/Os (I/Os issued before this barrier)
 *	  to complete.
 *	- call eio_map().
 *	- call eio_issue_empty_barrier_flush() on both HDD and SSD.
 */

static void
eio_process_barrier(struct eio_barrier_q *bq, struct cache_c *dmc,
		    struct bio *bio)
{
	int			ret;
	int			index;
	struct cache_c		*dmc1;
	struct block_device	*ssd_bdev[16];
	int			i, cnt;
	int			rw_flags = 0;

	/* cache on entier device */
	if (dmc && (dmc->dev_info == EIO_DEV_WHOLE_DISK)) {
		while (ATOMIC_READ(&dmc->nr_ios) != 0) {
			schedule_timeout(msecs_to_jiffies(1));
		}
		if (!bio_empty_barrier(bio)) {
			if (bio->bi_sector) {
				VERIFY(bio->bi_sector >= dmc->dev_start_sect);
				bio->bi_sector -= dmc->dev_start_sect;
			}

			do {
				ret = eio_map(dmc, bdev_get_queue(bio->bi_bdev), bio);
				VERIFY(ret == 0);
			} while (ret != 0);
		}
		SET_BARRIER_FLAGS(rw_flags);
		eio_issue_empty_barrier_flush(dmc->cache_dev->bdev, NULL,
					EIO_SSD_DEVICE, NULL, rw_flags);
		if (bio_empty_barrier(bio))	// call bio_endio()
			eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, bio,
						EIO_HDD_DEVICE, bq, rw_flags);
		else
			eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, NULL,
						EIO_HDD_DEVICE, bq, rw_flags);
		return;
	}

	/* partition handlling */
	cnt = 0;
	index = bq->ttc_lock_index;
	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bio->bi_bdev->bd_contains)
			continue;
		while (ATOMIC_READ(&dmc1->nr_ios) != 0) {
			schedule_timeout(msecs_to_jiffies(1));
		}
		if (cnt < 16)
			ssd_bdev[cnt++] = dmc1->cache_dev->bdev;
	}

	if (!bio_empty_barrier(bio)) {
		if (dmc) {
			VERIFY((bio->bi_sector >= dmc->dev_start_sect) &&
				((bio->bi_sector + to_sector(bio->bi_size) - 1) <= dmc->dev_end_sect));

			if (bio->bi_sector) {
				VERIFY(bio->bi_sector >= dmc->dev_start_sect);
				bio->bi_sector -= dmc->dev_start_sect;
			}

			do {
				ret = eio_map(dmc, bdev_get_queue(bio->bi_bdev), bio);
				VERIFY(ret == 0);
			} while (ret != 0);
		} else {
			eio_io_uncached_partition(bq, bio);
		}
	}

	SET_BARRIER_FLAGS(rw_flags);
	/* Issue empty barrier on SSDs of all cached partitions of HDD device */
	for (i = 0; i < cnt; i++)
		eio_issue_empty_barrier_flush(ssd_bdev[i], NULL, EIO_SSD_DEVICE,
					NULL, rw_flags);

	if (bio_empty_barrier(bio))	// need to call bio_endio()
		eio_issue_empty_barrier_flush(bio->bi_bdev->bd_contains, bio,
					EIO_HDD_DEVICE, bq, rw_flags);
	else
		eio_issue_empty_barrier_flush(bio->bi_bdev->bd_contains, NULL,
					EIO_HDD_DEVICE, bq, rw_flags);
	return;
}

static void
eio_bio_end_empty_barrier(struct bio *bio, int err)
{
	if (bio->bi_private)
		bio_endio(bio->bi_private, err);
	bio_put(bio);
	return;
}

static void
eio_issue_empty_barrier_flush(struct block_device *bdev, struct bio *orig_bio,
			int device, struct eio_barrier_q *bq, int rw_flags)
{
	struct bio		*bio;

	bio = bio_alloc(GFP_KERNEL, 0);
	if (!bio) {
		if (orig_bio)
			bio_endio(orig_bio, -ENOMEM);
	}
	bio->bi_end_io = eio_bio_end_empty_barrier;
	bio->bi_private = orig_bio;
	bio->bi_bdev = bdev;
	bio->bi_rw |= rw_flags;

	bio_get(bio);
	if (device == EIO_HDD_DEVICE) {
		bq->origmfn(bdev_get_queue(bio->bi_bdev), bio);

	} else {
		submit_bio(0, bio);
	}
	bio_put(bio);
	return;
}

static int
eio_finish_nrdirty(struct cache_c *dmc)
{
	int			index;
	int			ret = 0;
	int			retry_count;

	/*
	 * Due to any transient errors, finish_nr_dirty may not drop
	 * to zero. Retry the clean operations for FINISH_NRDIRTY_RETRY_COUNT.
	 */
	retry_count = FINISH_NRDIRTY_RETRY_COUNT;

	index = EIO_HASH_BDEV(dmc->disk_dev->bdev->bd_contains->bd_dev);
	down_write(&eio_ttc_lock[index]);

	/* Wait for the in-flight I/Os to drain out */
	while (ATOMIC_READ(&dmc->nr_ios) != 0) {
		EIODEBUG("finish_nrdirty: Draining I/O inflight\n");
		schedule_timeout(msecs_to_jiffies(1));
	}
	VERIFY(!(dmc->sysctl_active.do_clean & EIO_CLEAN_START));

	dmc->sysctl_active.do_clean |= EIO_CLEAN_KEEP | EIO_CLEAN_START;
	up_write(&eio_ttc_lock[index]);

	/*
	 * In the process of cleaning CACHE if CACHE turns to FAILED state,
	 * its a severe error.
	 */
	do {
		if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
			EIOERR("finish_nrdirty: CACHE \"%s\" is in FAILED state.",
				dmc->cache_name);
			ret = -ENODEV;
			break;
		}
			
		if (!dmc->sysctl_active.fast_remove) {
			eio_clean_all(dmc);
		}
	} while (!dmc->sysctl_active.fast_remove && (ATOMIC_READ(&dmc->nr_dirty) > 0)
		 && (!(dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG)));
	dmc->sysctl_active.do_clean &= ~EIO_CLEAN_START;

	/*
	 * If all retry_count exhausted and nr_dirty is still not zero.
	 * Return error.
	 */
	if (((dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG) ||
		(retry_count == 0)) &&
		(ATOMIC_READ(&dmc->nr_dirty) > 0)) {
		ret = -EINVAL;
	}
	if (ret)
		EIOERR("finish_nrdirty: Failed to finish %lu dirty blocks for cache \"%s\".",
			ATOMIC_READ(&dmc->nr_dirty), dmc->cache_name);

	return ret;
}

int
eio_cache_edit(char *cache_name, u_int32_t mode, u_int32_t policy)
{
	int		error = 0;
	int		index;
	struct cache_c	*dmc;
	uint32_t        old_time_thresh = 0;
	int		restart_async_task = 0;
	int		ret;

	VERIFY((mode != 0) || (policy != 0));

	dmc = eio_cache_lookup(cache_name);
	if (NULL == dmc) {
		EIOERR("cache_edit: cache %s do not exist", cache_name);
		return -EINVAL;
	}

	if ((dmc->mode == mode) && (dmc->req_policy == policy))
		return 0;

	if (unlikely(CACHE_FAILED_IS_SET(dmc)) || unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		EIOERR("cache_edit: Cannot proceed with edit for cache \"%s\"."
			" Cache is in failed or degraded state.",
			dmc->cache_name);
		return -EINVAL;
	}

	SPIN_LOCK_IRQSAVE_FLAGS(&dmc->cache_spin_lock);
	if (dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG) {
		EIOERR("cache_edit: system shutdown in progress, cannot edit"
			" cache %s", cache_name);
		SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);
		return -EINVAL;
	}
	if (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
		EIOERR("cache_edit: simultaneous edit/delete operation on cache"
			" %s is not permitted", cache_name);
		SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);
		return -EINVAL;
	}
	dmc->cache_flags |= CACHE_FLAGS_MOD_INPROG;
	SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);
	old_time_thresh = dmc->sysctl_active.time_based_clean_interval;

	if (dmc->mode == CACHE_MODE_WB) {
		if (CACHE_FAILED_IS_SET(dmc)) {
			EIOERR("cache_edit:  Can not proceed with edit for Failed cache \"%s\".",
				dmc->cache_name);
			error = -EINVAL;
			goto out;
		}
		eio_stop_async_tasks(dmc);
		restart_async_task = 1;
	}

	/* Wait for nr_dirty to drop to zero */
	if (dmc->mode == CACHE_MODE_WB && mode != CACHE_MODE_WB) {
		if (CACHE_FAILED_IS_SET(dmc)) {
			EIOERR("cache_edit:  Can not proceed with edit for Failed cache \"%s\".",
				dmc->cache_name);
			error = -EINVAL;
			goto out;
		}

		error = eio_finish_nrdirty(dmc);
		/* This error can mostly occur due to Device removal */
		if (unlikely(error)) {
			EIOERR("cache_edit: nr_dirty FAILED to finish for cache \"%s\".",
				dmc->cache_name);
			goto out;
		}
		VERIFY((dmc->sysctl_active.do_clean & EIO_CLEAN_KEEP) &&
			!(dmc->sysctl_active.do_clean & EIO_CLEAN_START));
		VERIFY(dmc->sysctl_active.fast_remove || (ATOMIC_READ(&dmc->nr_dirty) == 0));
	}

	index = EIO_HASH_BDEV(dmc->disk_dev->bdev->bd_contains->bd_dev);
	down_write(&eio_ttc_lock[index]);

	/* Wait for the in-flight I/Os to drain out */
	while (ATOMIC_READ(&dmc->nr_ios) != 0) {
		EIODEBUG("cache_edit: Draining I/O inflight\n");
		schedule_timeout(msecs_to_jiffies(1));
	}

	EIODEBUG("cache_edit: Blocking application I/O\n");

	VERIFY(ATOMIC_READ(&dmc->nr_ios) == 0);

	/* policy change */
	if ((policy != 0) && (policy != dmc->req_policy)) {
		error = eio_policy_switch(dmc, policy);
		if (error) {

			up_write(&eio_ttc_lock[index]);
			goto out;
		}
	}

	/* mode change */
	if ((mode != 0) && (mode != dmc->mode)) {
		error = eio_mode_switch(dmc, mode);
		if (error) {

			up_write(&eio_ttc_lock[index]);
			goto out;
		}
	}

	dmc->sysctl_active.time_based_clean_interval = old_time_thresh;
	/* write updated superblock */
	error = eio_sb_store(dmc);
	if (error) {
		/* XXX: In case of error put the cache in degraded mode. */
		EIOERR("eio_cache_edit: superblock update failed(error %d)",
			error);
		goto out;
	}

	eio_procfs_dtr(dmc);
	eio_procfs_ctr(dmc);

	up_write(&eio_ttc_lock[index]);

out:
	dmc->sysctl_active.time_based_clean_interval = old_time_thresh;

	/*
	 * Resetting EIO_CLEAN_START and EIO_CLEAN_KEEP flags.
	 * EIO_CLEAN_START flag should be restored if eio_stop_async_tasks()
	 * is not called in future.
	 */

	dmc->sysctl_active.do_clean &= ~(EIO_CLEAN_START | EIO_CLEAN_KEEP);

	/* Restart async-task for "WB" cache. */
	if ((dmc->mode == CACHE_MODE_WB) && (restart_async_task == 1)) {
		EIODEBUG("cache_edit: Restarting the clean_thread.\n");
		VERIFY(dmc->clean_thread == NULL);
		ret = eio_start_clean_thread(dmc);
		if (ret) {
			error = ret;
			EIOERR("cache_edit: Failed to restart async tasks. error=%d.\n", ret);
		}
		if (dmc->sysctl_active.time_based_clean_interval &&
		    ATOMIC_READ(&dmc->nr_dirty)) {
			schedule_delayed_work(&dmc->clean_aged_sets_work,
					      dmc->sysctl_active.time_based_clean_interval * 60 * HZ);
			dmc->is_clean_aged_sets_sched = 1;
		}
	}
	SPIN_LOCK_IRQSAVE_FLAGS(&dmc->cache_spin_lock);
	dmc->cache_flags &= ~CACHE_FLAGS_MOD_INPROG;
	SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);
	EIODEBUG("eio_cache_edit: Allowing application I/O\n");
	return error;
}

static int
eio_mode_switch(struct cache_c *dmc, u_int32_t mode)
{
	int		error = 0;
	u_int32_t	orig_mode;

	VERIFY(dmc->mode != mode);
	EIODEBUG("eio_mode_switch: mode switch from %u to %u\n",
		 dmc->mode, mode);

	if (mode == CACHE_MODE_WB) {
		orig_mode = dmc->mode;
		dmc->mode = mode;

		error = eio_allocate_wb_resources(dmc);
		if (error) {
			dmc->mode = orig_mode;
			goto out;
		}
	} else if (dmc->mode == CACHE_MODE_WB) {
		eio_free_wb_resources(dmc);
		dmc->mode = mode;
	} else {	/* (RO -> WT) or (WT -> RO) */
		VERIFY(((dmc->mode == CACHE_MODE_RO) && (mode == CACHE_MODE_WT)) ||
		       ((dmc->mode == CACHE_MODE_WT) && (mode == CACHE_MODE_RO)));
		dmc->mode = mode;
	}

out:
	if (error) {
		EIOERR("mode_switch: Failed to switch mode, error: %d\n", error);
	}
	return error;
}

/*
 * XXX: Error handling.
 * In case of error put the cache in degraded mode.
 */

static int
eio_policy_switch(struct cache_c *dmc, u_int32_t policy)
{
	int		error;

	VERIFY(dmc->req_policy != policy);


	eio_policy_free(dmc);

	dmc->req_policy = policy;
	error = eio_policy_init(dmc);
	if (error) {
		goto out;
	}

	error = eio_repl_blk_init(dmc->policy_ops);
	if (error) {
		EIOERR("eio_policy_swtich: Unable to allocate memory for policy cache block");
		goto out;
	}

	error = eio_repl_sets_init(dmc->policy_ops);
	if (error) {
		EIOERR("eio_policy_switch: Failed to allocate memory for cache policy");
		goto out;
	}

	eio_policy_lru_pushblks(dmc->policy_ops);
	return 0;

out:
	eio_policy_free(dmc);
	dmc->req_policy = CACHE_REPL_RANDOM;
	(void)eio_policy_init(dmc);
	return error;
}

void
eio_free_wb_pages(struct page **pages, int allocated)
{
	/* Verify that allocated is never 0 or less that zero. */
	if (allocated <= 0) {
		return;
	}

	do {
		put_page(pages[--allocated]);
	} while (allocated);

	*pages = NULL;
}

void
eio_free_wb_bvecs(struct bio_vec *bvec, int allocated, int blksize)
{
	int  i;

	if (allocated <= 0)
		return;

	for (i = 0; i < allocated; i++) {

		switch(blksize) {
		case BLKSIZE_2K:
			/*
			 * For 2k blocksize, each page is shared between two
			 * bio_vecs. Hence make sure to put_page only for even
			 * indexes.
			 */
			if (((i % 2) == 0) && bvec[i].bv_page) {
				put_page(bvec[i].bv_page);
				bvec[i].bv_page = NULL;
				continue;
			}

			/* For odd index page should already have been freed. */
			if ((i % 2))
				bvec[i].bv_page = NULL;

			continue;

		case BLKSIZE_4K:
		case BLKSIZE_8K:
			if (bvec[i].bv_page) {
				put_page(bvec[i].bv_page);
				bvec[i].bv_page = NULL;
			}

			continue;
		}
	}

}

/*
 * This function allocates pages to array of bvecs allocated by caller.
 * It has special handling of blocksize of 2k where single page is
 * shared between two bio_vecs.
 */

int
eio_alloc_wb_bvecs(struct bio_vec *bvec, int max, int blksize)
{
	int		i, ret;
	struct bio_vec	*iovec;
	struct page	*page;

	ret = 0;
	iovec = bvec;
	page = NULL;

	for (i = 0; i < max; i++) {

		switch(blksize) {

		case BLKSIZE_2K:
			/*
			 * In case of 2k blocksize, two biovecs will be sharing
			 * same page address. This is handled below.
			 */

			if ((i % 2) == 0) {
				/* Allocate page only for even bio vector */
				page = alloc_page(GFP_KERNEL | __GFP_ZERO);
				if (unlikely(!page)) {
					EIOERR("eio_alloc_wb_bvecs: System memory too low.\n");
					goto err;
				}
				iovec[i].bv_page = page;
				iovec[i].bv_len = to_bytes(blksize);
				iovec[i].bv_offset = 0;
			} else {
				/* Let the odd biovec share page allocated earlier. */
				VERIFY(page != NULL);
				iovec[i].bv_page = page;
				iovec[i].bv_len = to_bytes(blksize);
				iovec[i].bv_offset = PAGE_SIZE - to_bytes(blksize);

				/* Mark page NULL here as it is not required anymore. */
				page = NULL;
			}

			continue;

		case BLKSIZE_4K:
		case BLKSIZE_8K:
				page = alloc_page(GFP_KERNEL | __GFP_ZERO);
				if (unlikely(!page)) {
					EIOERR("eio_alloc_wb_bvecs: System memory too low.\n");
					goto err;
				}
				iovec[i].bv_page = page;
				iovec[i].bv_offset = 0;
				iovec[i].bv_len = PAGE_SIZE;

				page = NULL;
				continue;
		}

	}
	
	goto out;

err:
	if (i !=  max) {
		if ( i > 0)
			eio_free_wb_bvecs(bvec, i, blksize);
		ret = -ENOMEM;
	}

out:
	return ret;
}


int
eio_alloc_wb_pages(struct page **pages, int max)
{
	int 		i, ret = 0;
	struct page 	*page;

	for (i = 0; i < max; i++) {

		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (unlikely(!page)) {
			EIOERR("alloc_wb_pages: System memory too low.\n");
			break;
		}
		pages[i] = page;
	}

	if (i != max) {
		if (i > 0)
			eio_free_wb_pages(pages, i);
		ret = -ENOMEM;
		goto out;
	}

out:
	return ret;
}

/*
 ****************************************************************************
 * struct bio_vec *eio_alloc_pages(int max_pages, int *page_count)
 * dmc : cache object
 * pages : bio_vec to be allocated for synchronous I/O.
 * page_count : total number of pages allocated.
 ****************************************************************************
 *
 * This function allocates pages capped to minimum of
 * MD_MAX_NR_PAGES OR maximun number of pages supported by
 * block device.
 * This is to ensure that the pages allocated should fit
 * into single bio request.
 */

struct bio_vec *
eio_alloc_pages(u_int32_t max_pages, int *page_count)
{
	int pcount, i;
	struct bio_vec *pages;
	int nr_pages;

	
	/*
	 * Find out no. of pages supported by block device max capped to
	 * MD_MAX_NR_PAGES;
	 */
	nr_pages = min_t(u_int32_t, max_pages, MD_MAX_NR_PAGES);
	
	pages = kzalloc(nr_pages * sizeof(struct bio_vec), GFP_NOIO);
	if (unlikely(!pages)) {
		EIOERR("eio_alloc_pages: System memory too low.\n");
		return NULL;
	}

	pcount = 0;
	for (i = 0; i < nr_pages; i++) {
		pages[i].bv_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (unlikely(!pages[i].bv_page)) {
			EIOERR("eio_alloc_pages: System memory too low.\n");
			break;
		} else {
			pages[i].bv_len = PAGE_SIZE;
			pages[i].bv_offset = 0;
			pcount++;
		}
	}

	if (pcount == 0) {
		EIOERR("Single page allocation failed. System memory too low.");
		if (pages)
			kfree(pages);
		
		return NULL;
	}

	/* following can be commented out later...
	 * we may have less pages allocated.
	 */
	VERIFY(pcount == nr_pages);

	/* Set the return values here */
	*page_count = pcount;
	return pages;
}

/*
 * As part of reboot handling, stop all activies and mark the devices as
 * read only.
 */

int
eio_reboot_handling(void)
{
	struct cache_c		*dmc, *tempdmc = NULL;
	int			i, error;
	uint32_t		old_time_thresh;

	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE) {
		return 0;
	}

	(void)wait_on_bit_lock((void *)&eio_control->synch_flags, EIO_HANDLE_REBOOT,
		eio_wait_schedule, TASK_UNINTERRUPTIBLE);
	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE) {
		clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
		smp_mb__after_clear_bit();
		wake_up_bit((void *)&eio_control->synch_flags, EIO_HANDLE_REBOOT);
		return 0;
	}
	VERIFY(eio_reboot_notified == 0);
	eio_reboot_notified = EIO_REBOOT_HANDLING_INPROG;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_write(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {
			if (tempdmc) {
				kfree(tempdmc);
			}
			tempdmc = NULL;
			if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
			    unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
				EIOERR("Cache \"%s\" is in failed/degraded mode."
					" Cannot mark cache read only.\n",
					dmc->cache_name);
				continue;
			}

			while (ATOMIC_READ(&dmc->nr_ios) != 0) {
				EIODEBUG("rdonly: Draining I/O inflight\n");
				schedule_timeout(msecs_to_jiffies(10));
			}

			VERIFY(ATOMIC_READ(&dmc->nr_ios) == 0);
			VERIFY(dmc->cache_rdonly == 0);

			/*
			 * Shutdown processing has the highest priority.
			 * Stop all ongoing activities.
			 */

			SPIN_LOCK_IRQSAVE_FLAGS(&dmc->cache_spin_lock);
			VERIFY(!(dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG));
			dmc->cache_flags |= CACHE_FLAGS_SHUTDOWN_INPROG;
			SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);

			/*
			 * Wait for ongoing edit/delete to complete.
			 */

			while (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
				up_write(&eio_ttc_lock[i]);
				schedule_timeout(msecs_to_jiffies(1));
				down_write(&eio_ttc_lock[i]);
			}
			if (dmc->cache_flags & CACHE_FLAGS_DELETED) {

				/*
				 * Cache got deleted. Free the dmc.
				 */

				tempdmc = dmc;
				continue;
			}
			old_time_thresh = dmc->sysctl_active.time_based_clean_interval;
			eio_stop_async_tasks(dmc);
			dmc->sysctl_active.time_based_clean_interval = old_time_thresh;

			dmc->cache_rdonly = 1;
			EIOINFO("Cache \"%s\" marked read only\n", dmc->cache_name);
			up_write(&eio_ttc_lock[i]);

			if (dmc->cold_boot && ATOMIC_READ(&dmc->nr_dirty) && !eio_force_warm_boot) {
				EIOINFO("Cold boot set for cache %s: Draining dirty blocks: %ld",
						dmc->cache_name, ATOMIC_READ(&dmc->nr_dirty));
				eio_clean_for_reboot(dmc);
			}

			error = eio_md_store(dmc);
			if (error) {
				EIOERR("Cannot mark cache \"%s\" read only\n",
					dmc->cache_name);
			}

			SPIN_LOCK_IRQSAVE_FLAGS(&dmc->cache_spin_lock);
			dmc->cache_flags &= ~CACHE_FLAGS_SHUTDOWN_INPROG;
			SPIN_UNLOCK_IRQRESTORE_FLAGS(&dmc->cache_spin_lock);

			down_write(&eio_ttc_lock[i]);
		}
		if (tempdmc) {
			kfree(tempdmc);
		}
		tempdmc = NULL;
		up_write(&eio_ttc_lock[i]);
	}

	eio_reboot_notified = EIO_REBOOT_HANDLING_DONE;
	clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_HANDLE_REBOOT);
	return 0;
}

static int
eio_overlap_split_bio(struct request_queue *q, struct bio *bio)
{
	int			i, nbios, ret;
	void			**bioptr;
	sector_t		snum;
	struct bio_container	*bc;
	unsigned		bvec_idx;
	unsigned		bvec_consumed;

	nbios = bio->bi_size >> SECTOR_SHIFT;
	snum = bio->bi_sector;

	bioptr = kmalloc(nbios * (sizeof (void *)), GFP_KERNEL);
	if (!bioptr) {
		bio_endio(bio, -ENOMEM);
		return 0;
	}
	bc = kmalloc(sizeof (struct bio_container), GFP_NOWAIT);
	if (!bc) {
		bio_endio(bio, -ENOMEM);
		kfree(bioptr);
		return 0;
	}

	atomic_set(&bc->bc_holdcount, nbios);
	bc->bc_bio = bio;
	bc->bc_error = 0;

	bvec_idx = bio->bi_idx;
	bvec_consumed = 0;
	for (i = 0; i < nbios; i++) {
		bioptr[i] = eio_split_new_bio(bio, bc, &bvec_idx, &bvec_consumed, snum);
		if (!bioptr[i]) {
			break;
		}
		snum++;
	}

	/* Error: cleanup */
	if (i < nbios) {
		for (i--; i >= 0; i--)
			bio_put(bioptr[i]);
		bio_endio(bio, -ENOMEM);
		kfree(bc);
		goto out;
	}

	for (i = 0; i < nbios; i++) {
		ret = eio_make_request_fn(q, bioptr[i]);
		if (ret)
			bio_endio(bioptr[i], -EIO);
	}

out:
	kfree(bioptr);
	return 0;
}

static struct bio *
eio_split_new_bio(struct bio *bio, struct bio_container *bc,
		  unsigned *bvec_idx, unsigned *bvec_consumed, sector_t snum)
{
	struct bio	*cbio;
	unsigned	iosize = 1 << SECTOR_SHIFT;

	cbio = bio_alloc(GFP_NOIO, 1);
	if (!cbio)
		return NULL;

	VERIFY(bio->bi_io_vec[*bvec_idx].bv_len >= iosize);

	if (bio->bi_io_vec[*bvec_idx].bv_len <= *bvec_consumed) {
		VERIFY(bio->bi_io_vec[*bvec_idx].bv_len == *bvec_consumed);
		(*bvec_idx)++;
		VERIFY(bio->bi_vcnt > *bvec_idx);
		*bvec_consumed = 0;
	}

	cbio->bi_io_vec[0].bv_page = bio->bi_io_vec[*bvec_idx].bv_page;
	cbio->bi_io_vec[0].bv_offset = bio->bi_io_vec[*bvec_idx].bv_offset + *bvec_consumed;
	cbio->bi_io_vec[0].bv_len = iosize;
	*bvec_consumed += iosize;

	cbio->bi_sector = snum;
	cbio->bi_size = iosize;
	cbio->bi_bdev = bio->bi_bdev;
	cbio->bi_rw = bio->bi_rw;
	cbio->bi_vcnt = 1;
	cbio->bi_idx = 0;
	cbio->bi_end_io = eio_split_endio;
	cbio->bi_private = bc;
	return cbio;
}

static void
eio_split_endio(struct bio *bio, int error)
{
	struct bio_container	*bc = bio->bi_private;
	if (error)
		bc->bc_error = error;
	bio_put(bio);
	if (atomic_dec_and_test(&bc->bc_holdcount)) {
		bio_endio(bc->bc_bio, bc->bc_error);
		kfree(bc);
	}
	return;
}

