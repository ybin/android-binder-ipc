/* binder.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/cacheflush.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nsproxy.h>
#include <linux/poll.h>
#include <linux/debugfs.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/pid_namespace.h>

#include "binder.h"
#include "binder_trace.h"

static DEFINE_MUTEX(binder_main_lock);
static DEFINE_MUTEX(binder_deferred_lock);
static DEFINE_MUTEX(binder_mmap_lock);

static HLIST_HEAD(binder_procs);
static HLIST_HEAD(binder_deferred_list);
static HLIST_HEAD(binder_dead_nodes);

static struct dentry *binder_debugfs_dir_entry_root;
static struct dentry *binder_debugfs_dir_entry_proc;
static struct binder_node *binder_context_mgr_node;
static kuid_t binder_context_mgr_uid = INVALID_UID;
static int binder_last_id;
static struct workqueue_struct *binder_deferred_workqueue;

#define BINDER_DEBUG_ENTRY(name) \
static int binder_##name##_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, binder_##name##_show, inode->i_private); \
} \
\
static const struct file_operations binder_##name##_fops = { \
	.owner = THIS_MODULE, \
	.open = binder_##name##_open, \
	.read = seq_read, \
	.llseek = seq_lseek, \
	.release = single_release, \
}

static int binder_proc_show(struct seq_file *m, void *unused);
BINDER_DEBUG_ENTRY(proc);

/* This is only defined in include/asm-arm/sizes.h */
#ifndef SZ_1K
#define SZ_1K                               0x400
#endif

#ifndef SZ_4M
#define SZ_4M                               0x400000
#endif

#define FORBIDDEN_MMAP_FLAGS                (VM_WRITE)

#define BINDER_SMALL_BUF_SIZE (PAGE_SIZE * 64)

enum {
	BINDER_DEBUG_USER_ERROR             = 1U << 0,
	BINDER_DEBUG_FAILED_TRANSACTION     = 1U << 1,
	BINDER_DEBUG_DEAD_TRANSACTION       = 1U << 2,
	BINDER_DEBUG_OPEN_CLOSE             = 1U << 3,
	BINDER_DEBUG_DEAD_BINDER            = 1U << 4,
	BINDER_DEBUG_DEATH_NOTIFICATION     = 1U << 5,
	BINDER_DEBUG_READ_WRITE             = 1U << 6,
	BINDER_DEBUG_USER_REFS              = 1U << 7,
	BINDER_DEBUG_THREADS                = 1U << 8,
	BINDER_DEBUG_TRANSACTION            = 1U << 9,
	BINDER_DEBUG_TRANSACTION_COMPLETE   = 1U << 10,
	BINDER_DEBUG_FREE_BUFFER            = 1U << 11,
	BINDER_DEBUG_INTERNAL_REFS          = 1U << 12,
	BINDER_DEBUG_BUFFER_ALLOC           = 1U << 13,
	BINDER_DEBUG_PRIORITY_CAP           = 1U << 14,
	BINDER_DEBUG_BUFFER_ALLOC_ASYNC     = 1U << 15,
};
static uint32_t binder_debug_mask = BINDER_DEBUG_USER_ERROR |
	BINDER_DEBUG_FAILED_TRANSACTION | BINDER_DEBUG_DEAD_TRANSACTION;
module_param_named(debug_mask, binder_debug_mask, uint, S_IWUSR | S_IRUGO);

static bool binder_debug_no_lock;
module_param_named(proc_no_lock, binder_debug_no_lock, bool, S_IWUSR | S_IRUGO);

static DECLARE_WAIT_QUEUE_HEAD(binder_user_error_wait);
static int binder_stop_on_user_error;

static int binder_set_stop_on_user_error(const char *val,
					 struct kernel_param *kp)
{
	int ret;
	ret = param_set_int(val, kp);
	if (binder_stop_on_user_error < 2)
		wake_up(&binder_user_error_wait);
	return ret;
}
module_param_call(stop_on_user_error, binder_set_stop_on_user_error,
	param_get_int, &binder_stop_on_user_error, S_IWUSR | S_IRUGO);

#define binder_debug(mask, x...) \
	do { \
		if (binder_debug_mask & mask) \
			pr_info(x); \
	} while (0)

#define binder_user_error(x...) \
	do { \
		if (binder_debug_mask & BINDER_DEBUG_USER_ERROR) \
			pr_info(x); \
		if (binder_stop_on_user_error) \
			binder_stop_on_user_error = 2; \
	} while (0)

enum binder_stat_types {
	BINDER_STAT_PROC,
	BINDER_STAT_THREAD,
	BINDER_STAT_NODE,
	BINDER_STAT_REF,
	BINDER_STAT_DEATH,
	BINDER_STAT_TRANSACTION,
	BINDER_STAT_TRANSACTION_COMPLETE,
	BINDER_STAT_COUNT
};

struct binder_stats {
	int br[_IOC_NR(BR_FAILED_REPLY) + 1];
	int bc[_IOC_NR(BC_DEAD_BINDER_DONE) + 1];
	int obj_created[BINDER_STAT_COUNT];
	int obj_deleted[BINDER_STAT_COUNT];
};

static struct binder_stats binder_stats;

static inline void binder_stats_deleted(enum binder_stat_types type)
{
	binder_stats.obj_deleted[type]++;
}

static inline void binder_stats_created(enum binder_stat_types type)
{
	binder_stats.obj_created[type]++;
}

struct binder_transaction_log_entry {
	int debug_id;
	int call_type;
	int from_proc;
	int from_thread;
	int target_handle;
	int to_proc;
	int to_thread;
	int to_node;
	int data_size;
	int offsets_size;
};
struct binder_transaction_log {
	int next;
	int full;
	struct binder_transaction_log_entry entry[32];
};
static struct binder_transaction_log binder_transaction_log;
static struct binder_transaction_log binder_transaction_log_failed;

static struct binder_transaction_log_entry *binder_transaction_log_add(
	struct binder_transaction_log *log)
{
	struct binder_transaction_log_entry *e;
	e = &log->entry[log->next];
	memset(e, 0, sizeof(*e));
	log->next++;
	if (log->next == ARRAY_SIZE(log->entry)) {
		log->next = 0;
		log->full = 1;
	}
	return e;
}

/*
 * binder_work的说明:
 * 
 * 每个进程、线程都有一个todo列表(proc->todo, binder_thread->todo)，
 * 用来表示该进程、线程的任务队列，进程、线程每次唤醒时就查看其任务
 * 队列中是否有任务(binder_work)，如果有就处理，如果没有就继续睡眠。
 *
 * 这些todo列表中的work的具体内容是什么呢� 毕竟binder_work对象中
 * 不包括任何有效的数据(除了work type)，其实todo列表中的binder_work
 * 对象都是
 *    binder_transaction对象(binder_transaction->work)、
 *    binder_node对象(binder_node->work)
 *    binder_ref_death对象(binder_ref_death->work)
 * 的一部分，真正处理binder_work的时候(binder_thread_read函数)
 * 都是通过container_of宏来取得binder_work所在的对象，然后具体的
 * 数据(transaction里面就有个货仓)、工作针对的目标(node或者death ref)
 * 就都有了。
 */
struct binder_work {
	// binder_work所处的work队列
	struct list_head entry;
	// work的实际指令
	enum {
		BINDER_WORK_TRANSACTION = 1, // 发起一次transaction时，该work被发送给接收者线程/进程的todo队列
		BINDER_WORK_TRANSACTION_COMPLETE, // driver接收transaction之后立刻给transaction发起者发送该work
		BINDER_WORK_NODE, // service线程/进程的todo队列会收到该work，它表示要处理service的引用计数
		BINDER_WORK_DEAD_BINDER, // node死亡时向client发送此类型work进行通知，最后这三个都是发往client的
		BINDER_WORK_DEAD_BINDER_AND_CLEAR, //client注销死亡通知时，发现还有死亡通知等待处理，此时发送此类型work
		BINDER_WORK_CLEAR_DEATH_NOTIFICATION, // clinet注销死亡通知时，没有待处理的死亡通知，可以正常注销
	} type;
};

struct binder_node {
	int debug_id;
	// 把该work挂载到proc或者thread的todo队列中时，
	// 通过container_of宏就可以取得binder_node对象，见binder_work的说明
	struct binder_work work;
	union {
		struct rb_node rb_node;
		struct hlist_node dead_node;
	};
	// binder_node所在的进程
	struct binder_proc *proc;
	// 记录所有关联到自身的binder_refs对象
	struct hlist_head refs;
	
	int internal_strong_refs;
	int local_weak_refs;
	int local_strong_refs;

	// service对象(如BnCameraService)的weak ref对象内存地址
	void __user *ptr;
	// service对象(如BnCameraService)的内存地址
	void __user *cookie;
	unsigned has_strong_ref:1;
	// 驱动发送消息给service增加强引用，但是service还没有返回Acquire_done消息，
	// 这段时间pending_strong_ref == 1
	unsigned pending_strong_ref:1;
	unsigned has_weak_ref:1;
	// 类似于pending_strong_ref
	unsigned pending_weak_ref:1;
	unsigned has_async_transaction:1;
	unsigned accept_fds:1;
	unsigned min_priority:8;
	struct list_head async_todo;
};

struct binder_ref_death {
	// 见binder_work的说明
	struct binder_work work;
	void __user *cookie;
};

struct binder_ref {
	/* Lookups needed: */
	/*   node + proc => ref (transaction) */
	/*   desc + proc => ref (transaction, inc/dec ref) */
	/*   node => refs + procs (proc exit) */
	int debug_id;
	// binder_ref要挂载到进程的按照desc进行索引refs红黑树(proc->refs_by_desc)
	struct rb_node rb_node_desc;
	// binder_ref还要被挂载到进程的按照node地址进行索引的refs红黑树(proc->refs_by_node)
	struct rb_node rb_node_node;
	// binder_ref还要挂载到node对象的refs队列(binder_node->refs)中，
	// 因为node要知道都有哪些ref关联到它了
	struct hlist_node node_entry;
	// binder_ref还要知道自身位于哪个进程
	struct binder_proc *proc;
	// 以及关联到哪个node了
	struct binder_node *node;
	// 一个binder_ref在一个进程中的唯一代号(也就是应用中的handle)
	uint32_t desc;
	// 强、弱引用计数
	int strong;
	int weak;
	// 死亡通知
	struct binder_ref_death *death;
};

struct binder_buffer {
	// 任何一个buffer都要挂载到进程的buffer队列(proc->buffers)中
	struct list_head entry; /* free and allocated entries by address */
	// 另外还要根据该buffer是否空闲而挂载到进程的
	// 空闲buffer红黑树(proc->free_buffers)
	// 或者
	// 使用中buffer红黑树(proc->allocated_buffer)
	struct rb_node rb_node; /* free entry by size or allocated entry by address */
	
	unsigned free:1;
	unsigned allow_user_free:1;
	unsigned async_transaction:1;
	unsigned debug_id:29;

	// buffer是transaction这只船的货仓物品之一，
	// 所以buffer要知道它归属于哪只船，见binder_transaction结构
	struct binder_transaction *transaction;

	// 通过binder_transaction结构我们已经知道数据被送往哪个港口
	// (transaction_stack, from_parent, to_parent)，然后我们还要
	// 知道数据交由哪个node去处理，毕竟service的代表是node
	struct binder_node *target_node;
	// 下面是具体的数据信息
	size_t data_size;
	size_t offsets_size;
	uint8_t data[0];
};

enum binder_deferred_state {
	BINDER_DEFERRED_PUT_FILES    = 0x01,
	BINDER_DEFERRED_FLUSH        = 0x02,
	BINDER_DEFERRED_RELEASE      = 0x04,
};

struct binder_proc {
	// 在全局变量proc_nodes列表中的标识，用于统计
	struct hlist_node proc_node;
	// 该进程中的所有threads组成的rb-tree，按照pid进行索引
	struct rb_root threads;
	// 该进程中的所有node组成的rb-tree，一个node代表一种service，
	// 如media.player, media.camera等，按照node->ptr，即node
	// 的weak ref进行索引
	struct rb_root nodes;
	// 该进程中的所有ref组成的rb-tree，按照ref-desc进行索引，
	// ref-desc是一个进程中所有ref按照创建的先后顺序分配的一个
	// 整数，desc, handle是同一个东西
	struct rb_root refs_by_desc;
	// 该进程中的所有ref组成的rb-tree，按照node内存地址进行索引
	struct rb_root refs_by_node;
	// 进程pid，它等于进程的tgid(thread group id)
	int pid;
	/*
	 * 每个进程都有一块儿物理内存作为buffer，这块儿物理内存分别
	 * map到内核虚拟内存空间和应用虚拟内存空间(service端)，这样
	 * 就可以减少一次内存复制�
	 *   client -->内存复制--> driver -->由于是物理内存共享，所以实际不用内存复制--> service
	 * 
	 * 下面的vma即为应用虚拟内存空间的一块儿虚拟内存
	 */
	struct vm_area_struct *vma;
	struct mm_struct *vma_vm_mm;
	// tsk, files为常规的进程资源
	struct task_struct *tsk;
	struct files_struct *files;
	// 延迟工作 列表，一些延迟执行的工作放置到该列表中即可
	struct hlist_node deferred_work_node;
	int deferred_work;
	// 内核虚拟内存空间中的一块儿内存，见vma属性的说明
	void *buffer;
	// 由于用作buffer的内核虚拟内存跟应用虚拟内存都是连续的，
	// 所以两者进行转换的时候只需一个地址偏移量即可
	ptrdiff_t user_buffer_offset;

	// 该进程中所有buffer组成的列表，该列表中的元素排列顺序
	// 跟buffer空间中的buffer排列顺序是一致的
	struct list_head buffers;
	// 该进程中所有空闲buffer组成的rb-tree，按照buffer address进行索引
	struct rb_root free_buffers;
	// 该进程中所有使用中的buffer组成的rb-tree，按照buffer address进行索引
	struct rb_root allocated_buffers;
	// 异步传输使用的buffer空间大小async transaction即为ONE_WAY的，一般为
	// death notification
	size_t free_async_space;

	// 物理页面，一个page对应一个物理页面
	struct page **pages;
	// 缓冲区大小
	size_t buffer_size;
	uint32_t buffer_free;
	// 任务队列，这上面挂载的是binder_work对象
	struct list_head todo;
	wait_queue_head_t wait;
	
	struct binder_stats stats;
	struct list_head delivered_death;
	// 最大线程数，一般为15
	int max_threads;
	// 请求的thread数量
	int requested_threads;
	int requested_threads_started;
	// 空闲的thread数量
	int ready_threads;
	long default_priority;
	struct dentry *debugfs_entry;
};

enum {
	BINDER_LOOPER_STATE_REGISTERED  = 0x01,
	BINDER_LOOPER_STATE_ENTERED     = 0x02,
	BINDER_LOOPER_STATE_EXITED      = 0x04,
	BINDER_LOOPER_STATE_INVALID     = 0x08,
	BINDER_LOOPER_STATE_WAITING     = 0x10,
	BINDER_LOOPER_STATE_NEED_RETURN = 0x20
};

struct binder_thread {
	// 该线程所在的进程
	struct binder_proc *proc;
	// 该线程在其进程rb-tree中的代表，它被挂载到proc->threads rb-tree中
	struct rb_node rb_node;
	// 真正的pid，逻辑上的tid
	int pid;
	// looper的一个很重要的作用是判断当前thread是否为pool thread，
	// 任何一个线程都可以通过IPCThreadState向驱动发送消息，但是
	// 只有pool thread才会监听驱动的消息，即pool thread才会处理其
	// todo队列，如果当前线程不是pool thread，内核就将work挂载到
	// proc的todo队列。
	//
	// if(thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED))
	//     current is pool thread, mount work to thread->todo
	// else
	//     current is normal thread, mount work to proc->todo
	int looper;
	struct binder_transaction *transaction_stack;
	// 任务队列，这上面挂载的是binder_work对象
	struct list_head todo;
	uint32_t return_error; /* Write failed, return error code in read buf */
	uint32_t return_error2; /* Write failed, return error code in read */
		/* buffer. Used when sending a reply to a dead process that */
		/* we are also waiting on */
	wait_queue_head_t wait;
	// 统计信息
	struct binder_stats stats;
};

/*
 * 数据从一个进程传递到另一个进程，在driver的角度看就是
 * 从一个进程的proc对象传递到另一个进程的proc对象，然而
 * 一个进程可能有多个线程存在，所以还要区分是从哪个线程
 * 传递到哪个线程，传递的数据用一个binder_transaction
 * 对象来表示，一个binder_transaction最终要挂载到某一个
 * thread，就像每一艘船最终都要停泊在某一个港口一样，
 * binder_transaction就是一只船，而thread->transaction_stack
 * 就是一个港口。
 */
struct binder_transaction {
	int debug_id;
	// 将该work挂载到proc或thread的todo队列中，
	// 就可以使用container_of宏取得binder_transaction对象，
	// 见binder_work的说明
	struct binder_work work;
	// 从哪里来到何处去:
	// 贫僧从东土大唐而来，前往西天取经。。。
	struct binder_thread *from;
	struct binder_transaction *from_parent;
	struct binder_proc *to_proc;
	struct binder_thread *to_thread;
	struct binder_transaction *to_parent;
	unsigned need_reply:1;
	/* unsigned is_dead:1; */	/* not used at the moment */

	// 船上载的具体数据是什么呢，下面就是货仓
	struct binder_buffer *buffer;
	unsigned int	code;
	unsigned int	flags;
	long	priority;
	long	saved_priority;
	kuid_t	sender_euid;
};

static void
binder_defer_work(struct binder_proc *proc, enum binder_deferred_state defer);

static int task_get_unused_fd_flags(struct binder_proc *proc, int flags)
{
	struct files_struct *files = proc->files;
	unsigned long rlim_cur;
	unsigned long irqs;

	if (files == NULL)
		return -ESRCH;

	if (!lock_task_sighand(proc->tsk, &irqs))
		return -EMFILE;

	rlim_cur = task_rlimit(proc->tsk, RLIMIT_NOFILE);
	unlock_task_sighand(proc->tsk, &irqs);

	return __alloc_fd(files, 0, rlim_cur, flags);
}

/*
 * copied from fd_install
 */
static void task_fd_install(
	struct binder_proc *proc, unsigned int fd, struct file *file)
{
	if (proc->files)
		__fd_install(proc->files, fd, file);
}

/*
 * copied from sys_close
 */
static long task_close_fd(struct binder_proc *proc, unsigned int fd)
{
	int retval;

	if (proc->files == NULL)
		return -ESRCH;

	retval = __close_fd(proc->files, fd);
	/* can't restart close syscall because file table entry was cleared */
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		retval = -EINTR;

	return retval;
}

static inline void binder_lock(const char *tag)
{
	trace_binder_lock(tag);
	mutex_lock(&binder_main_lock);
	trace_binder_locked(tag);
}

static inline void binder_unlock(const char *tag)
{
	trace_binder_unlock(tag);
	mutex_unlock(&binder_main_lock);
}

static void binder_set_nice(long nice)
{
	long min_nice;
	if (can_nice(current, nice)) {
		set_user_nice(current, nice);
		return;
	}
	min_nice = 20 - current->signal->rlim[RLIMIT_NICE].rlim_cur;
	binder_debug(BINDER_DEBUG_PRIORITY_CAP,
		     "%d: nice value %ld not allowed use %ld instead\n",
		      current->pid, nice, min_nice);
	set_user_nice(current, min_nice);
	if (min_nice < 20)
		return;
	binder_user_error("%d RLIMIT_NICE not set\n", current->pid);
}

/* 获取binder_buffer的data的size，即data数组的首地址<->下一个buffer的首地址
 * 之间的size。最后一个buffer的大小为data数组首地址到整个proc->buffer结尾
 * 的size。
 */
static size_t binder_buffer_size(struct binder_proc *proc,
				 struct binder_buffer *buffer)
{
	if (list_is_last(&buffer->entry, &proc->buffers))
		return proc->buffer + proc->buffer_size - (void *)buffer->data;
	else
		return (size_t)list_entry(buffer->entry.next,
			struct binder_buffer, entry) - (size_t)buffer->data;
}

/*
 * free buffers红黑树是按照buffer的data size来排列的。
 */
static void binder_insert_free_buffer(struct binder_proc *proc,
				      struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->free_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;
	size_t buffer_size;
	size_t new_buffer_size;

	BUG_ON(!new_buffer->free);

	new_buffer_size = binder_buffer_size(proc, new_buffer);

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: add free buffer, size %zd, at %p\n",
		      proc->pid, new_buffer_size, new_buffer);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);

		buffer_size = binder_buffer_size(proc, buffer);

		if (new_buffer_size < buffer_size)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->free_buffers);
}

/*
 * allocated buffers红黑树是按照buffer的address(kernel address)来排列的。
 */
static void binder_insert_allocated_buffer(struct binder_proc *proc,
					   struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->allocated_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;

	BUG_ON(new_buffer->free);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (new_buffer < buffer)
			p = &parent->rb_left;
		else if (new_buffer > buffer)
			p = &parent->rb_right;
		else
			BUG();
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->allocated_buffers);
}

/*
 * 按照buffer data address指针来查找，该指针是user space的。
 */
static struct binder_buffer *binder_buffer_lookup(struct binder_proc *proc,
						  void __user *user_ptr)
{
	struct rb_node *n = proc->allocated_buffers.rb_node;
	struct binder_buffer *buffer;
	struct binder_buffer *kern_ptr;

	kern_ptr = user_ptr - proc->user_buffer_offset
		- offsetof(struct binder_buffer, data);

	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (kern_ptr < buffer)
			n = n->rb_left;
		else if (kern_ptr > buffer)
			n = n->rb_right;
		else
			return buffer;
	}
	return NULL;
}

/*
 * 为内核虚拟内存和应用虚拟内存分配/释放物理内存: allocate == 1 ? 分配 : 释放;
 * start, end为内核虚拟地址，vma为应用虚拟内存。
 */
static int binder_update_page_range(struct binder_proc *proc, int allocate,
				    void *start, void *end,
				    struct vm_area_struct *vma)
{
	void *page_addr;
	unsigned long user_page_addr;
	struct vm_struct tmp_area;
	struct page **page;
	struct mm_struct *mm;

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: %s pages %p-%p\n", proc->pid,
		     allocate ? "allocate" : "free", start, end);

	if (end <= start)
		return 0;

	trace_binder_update_page_range(proc, allocate, start, end);

	if (vma)
		mm = NULL;
	else
		mm = get_task_mm(proc->tsk);

	if (mm) {
		down_write(&mm->mmap_sem);
		vma = proc->vma;
		if (vma && mm != proc->vma_vm_mm) {
			pr_err("%d: vma mm and task mm mismatch\n",
				proc->pid);
			vma = NULL;
		}
	}

	if (allocate == 0)
		goto free_range;

	if (vma == NULL) {
		pr_err("%d: binder_alloc_buf failed to map pages in userspace, no vma\n",
			proc->pid);
		goto err_no_vma;
	}

	for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
		int ret;
		struct page **page_array_ptr;
		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];

		BUG_ON(*page);
		// 分配物理内存
		*page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
		if (*page == NULL) {
			pr_err("%d: binder_alloc_buf failed for page at %p\n",
				proc->pid, page_addr);
			goto err_alloc_page_failed;
		}
		tmp_area.addr = page_addr;
		tmp_area.size = PAGE_SIZE + PAGE_SIZE /* guard page? */;
		page_array_ptr = page;
		/*
		 * 将分配的物理内存映射到内核虚拟内存，
		 * page_array_ptr指向物理内存对应的struct page结构体，
		 * tmp_area是一个vm_struct结构体，kernel将根据其中保存的内容
		 * 找到对应的OS vm_struct list，并将page与其关联
		 */
		ret = map_vm_area(&tmp_area, PAGE_KERNEL, &page_array_ptr);
		if (ret) {
			pr_err("%d: binder_alloc_buf failed to map page at %p in kernel\n",
			       proc->pid, page_addr);
			goto err_map_kernel_failed;
		}
		/*
		 * user_page_addr是应用虚拟内存地址，
		 * vm_insert_page()将分配的物理内存页与应用虚拟内存关联
		 */
		user_page_addr =
			(uintptr_t)page_addr + proc->user_buffer_offset;
		ret = vm_insert_page(vma, user_page_addr, page[0]);
		if (ret) {
			pr_err("%d: binder_alloc_buf failed to map page at %lx in userspace\n",
			       proc->pid, user_page_addr);
			goto err_vm_insert_page_failed;
		}
		/* vm_insert_page does not seem to increment the refcount */
	}
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return 0;

free_range:
	/* 释放物理内存 */
	for (page_addr = end - PAGE_SIZE; page_addr >= start;
	     page_addr -= PAGE_SIZE) {
		// Step 0. 找到需要释放的物理内存页
		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];
		// Step 1. 如果物理内存被关联到应用虚拟内存，则断开关联
		if (vma)
			zap_page_range(vma, (uintptr_t)page_addr +
				proc->user_buffer_offset, PAGE_SIZE, NULL);
err_vm_insert_page_failed:
		// Step 2. 断开内核虚拟内存与物理内存的关联
		unmap_kernel_range((unsigned long)page_addr, PAGE_SIZE);
err_map_kernel_failed:
		// Step 3. 释放物理内存
		__free_page(*page);
		*page = NULL;
err_alloc_page_failed:
		;
	}
err_no_vma:
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return -ENOMEM;
}

/*
 * 从内核虚拟空间(proc->buffer)中分配binder_buffer对象
 */
static struct binder_buffer *binder_alloc_buf(struct binder_proc *proc,
					      size_t data_size,
					      size_t offsets_size, int is_async)
{
	struct rb_node *n = proc->free_buffers.rb_node;
	struct binder_buffer *buffer;
	size_t buffer_size;
	struct rb_node *best_fit = NULL;
	void *has_page_addr;
	void *end_page_addr;
	size_t size;

	if (proc->vma == NULL) {
		pr_err("%d: binder_alloc_buf, no vma\n",
		       proc->pid);
		return NULL;
	}

	// 按照页面对齐之后的buffer size(数据部分的size，不包括binder_buffer结构体本身的size)
	size = ALIGN(data_size, sizeof(void *)) +
		ALIGN(offsets_size, sizeof(void *));

	if (size < data_size || size < offsets_size) {
		binder_user_error("%d: got transaction with invalid size %zd-%zd\n",
				proc->pid, data_size, offsets_size);
		return NULL;
	}

	if (is_async &&
	    proc->free_async_space < size + sizeof(struct binder_buffer)) {
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: binder_alloc_buf size %zd failed, no async space left\n",
			      proc->pid, size);
		return NULL;
	}

	/*
	 * 搜索合适的空闲buffer， 从proc->free_buffers红黑树的root节点开始查找。
	 * 如果能找到大小相等的最好，找到之后就break，如果没有恰好相等的，就找
	 * 一个==较大==的，依次遍历，最差也是从比需要的size大的buffer中找到一个
	 * 最小的。
	 * 注意: proc->free_buffers最开始只有一个binder_buffer，即将整个内核
	 * 虚拟内存视为一个binder_buffer(free_buffers红黑树的root节点)，此时
	 * 其左、右子树均为NULL。
	 * 遍历有两个可能的结果:
	 *   找到一个buffer，其size恰好等于需要的size，此时buffer==best_fit==n==满足要求的buffer;
	 *   找不到这样的buffer，只能找到相对合适的buffer(best_fit)，此时n==NULL。
	 */
	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);
		buffer_size = binder_buffer_size(proc, buffer);

		/*
		 * 找到最接近size的buffer
		 */
		if (size < buffer_size) {
			best_fit = n;
			n = n->rb_left;
		} else if (size > buffer_size)
			n = n->rb_right;
		else {
			best_fit = n;
			break;
		}
	}
	if (best_fit == NULL) {
		pr_err("%d: binder_alloc_buf size %zd failed, no address space\n",
			proc->pid, size);
		return NULL;
	}
	// n==NULL表明没有找到size恰好相等的buffer，只好使用相对较好的best_fit
	if (n == NULL) {
		buffer = rb_entry(best_fit, struct binder_buffer, rb_node);
		buffer_size = binder_buffer_size(proc, buffer);
	}

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_alloc_buf size %zd got buffer %p size %zd\n",
		      proc->pid, size, buffer, buffer_size);

	has_page_addr =
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK);
	/*
	 * buffer_size比需要的size大一些，大多少呢?
	 * 如果超出的部分还能容下一个==特殊==的buffer，那为何不利用起来，
	 * 再做一个新的buffer出来呢，反正闲着也是闲着！
	 * 这个特殊的buffer就是"data部分仅能容纳一个cmd(4 byte)"的最小buffer。
	 */
	if (n == NULL) {	
		if (size + sizeof(struct binder_buffer) + 4 >= buffer_size)
			// 找到的这个buffer虽然比需要的size大一些，但是它没有空间再容纳一个
			// ==最小==buffer了，所以只能浪费了。
			buffer_size = size; /* no room for other buffers */
		else
			// 哇，超出的部分还可以容纳一个额外的==最小==buffer呢，让我们
			// 废物利用吧!
			buffer_size = size + sizeof(struct binder_buffer);
	}
	end_page_addr =
		(void *)PAGE_ALIGN((uintptr_t)buffer->data + buffer_size);
	if (end_page_addr > has_page_addr)
		end_page_addr = has_page_addr;
	if (binder_update_page_range(proc, 1,
	    (void *)PAGE_ALIGN((uintptr_t)buffer->data), end_page_addr, NULL))
		return NULL;

	// 现在，best_fit这个buffer就归我们所用了，它不再是free的了。
	rb_erase(best_fit, &proc->free_buffers);
	buffer->free = 0;
	// 将其从free rb-tree转移到allocated rb-tree吧
	binder_insert_allocated_buffer(proc, buffer);
	// 哇，让我们在额外的空间里再创建一个==最小==buffer吧，废物利用嘛!
	// 另外，当best_fit是最后一个buffer的时候，这可不仅仅是废物利用，因为
	// 它的数据部分空间可是很大呢，大到包含了整个缓冲区所有剩余内容，
	// 我们必须创建一个新的空闲buffer来接管这么大的空间，否则以后就
	// 没有free buffer可用了(在driver进行第二次分配的时候就会失败的)，
	// 所以这个新buffer可是性命攸关呢!
	if (buffer_size != size) {
		struct binder_buffer *new_buffer = (void *)buffer->data + size;
		list_add(&new_buffer->entry, &buffer->entry);
		new_buffer->free = 1;
		binder_insert_free_buffer(proc, new_buffer);
	}
	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_alloc_buf size %zd got %p\n",
		      proc->pid, size, buffer);
	// 把新的buffer填充数据，就可以返回使用了
	buffer->data_size = data_size;
	buffer->offsets_size = offsets_size;
	buffer->async_transaction = is_async;
	if (is_async) {
		proc->free_async_space -= size + sizeof(struct binder_buffer);
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_alloc_buf size %zd async free %zd\n",
			      proc->pid, size, proc->free_async_space);
	}

	return buffer;
}

// buffer所在page的内存地址
static void *buffer_start_page(struct binder_buffer *buffer)
{
	return (void *)((uintptr_t)buffer & PAGE_MASK);
}
// 
static void *buffer_end_page(struct binder_buffer *buffer)
{
	return (void *)(((uintptr_t)(buffer + 1) - 1) & PAGE_MASK);
}

/*
 * 释放当前buffer与prev/next共同使用的物理内存页
 */
static void binder_delete_free_buffer(struct binder_proc *proc,
				      struct binder_buffer *buffer)
{
	struct binder_buffer *prev, *next = NULL;
	int free_page_end = 1;
	int free_page_start = 1;

	BUG_ON(proc->buffers.next == &buffer->entry);
	prev = list_entry(buffer->entry.prev, struct binder_buffer, entry);
	BUG_ON(!prev->free);
	if (buffer_end_page(prev) == buffer_start_page(buffer)) {
		free_page_start = 0;
		if (buffer_end_page(prev) == buffer_end_page(buffer))
			free_page_end = 0;
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: merge free, buffer %p share page with %p\n",
			      proc->pid, buffer, prev);
	}

	if (!list_is_last(&buffer->entry, &proc->buffers)) {
		next = list_entry(buffer->entry.next,
				  struct binder_buffer, entry);
		if (buffer_start_page(next) == buffer_end_page(buffer)) {
			free_page_end = 0;
			if (buffer_start_page(next) ==
			    buffer_start_page(buffer))
				free_page_start = 0;
			binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
				     "%d: merge free, buffer %p share page with %p\n",
				      proc->pid, buffer, prev);
		}
	}
	list_del(&buffer->entry);
	if (free_page_start || free_page_end) {
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: merge free, buffer %p do not share page%s%s with with %p or %p\n",
			     proc->pid, buffer, free_page_start ? "" : " end",
			     free_page_end ? "" : " start", prev, next);
		binder_update_page_range(proc, 0, free_page_start ?
			buffer_start_page(buffer) : buffer_end_page(buffer),
			(free_page_end ? buffer_end_page(buffer) :
			buffer_start_page(buffer)) + PAGE_SIZE, NULL);
	}
}

/*
 * 释放buffer。
 *
 * 释放时需要做两件事情：
 * a). 内存释放:   释放buffer的数据所占用的物理内存
 * b). buffer合并: 如果prev或nest是空闲buffer就要把这些空闲的buffer进行合并
 */
static void binder_free_buf(struct binder_proc *proc,
			    struct binder_buffer *buffer)
{
	size_t size, buffer_size;

	buffer_size = binder_buffer_size(proc, buffer);

	size = ALIGN(buffer->data_size, sizeof(void *)) +
		ALIGN(buffer->offsets_size, sizeof(void *));

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_free_buf %p size %zd buffer_size %zd\n",
		      proc->pid, buffer, size, buffer_size);

	BUG_ON(buffer->free);
	BUG_ON(size > buffer_size);
	BUG_ON(buffer->transaction != NULL);
	BUG_ON((void *)buffer < proc->buffer);
	BUG_ON((void *)buffer > proc->buffer + proc->buffer_size);

	if (buffer->async_transaction) {
		proc->free_async_space += size + sizeof(struct binder_buffer);

		binder_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_free_buf size %zd async free %zd\n",
			      proc->pid, size, proc->free_async_space);
	}

	// 释放自身数据区的物理内存，注意页对齐，不要把"与其他buffer共享的page"也给释放了
	binder_update_page_range(proc, 0,
		(void *)PAGE_ALIGN((uintptr_t)buffer->data),
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK),
		NULL);
	// 这个buffer自由了, 把它从allocated rb-tree中移除吧
	rb_erase(&buffer->rb_node, &proc->allocated_buffers);
	buffer->free = 1;

	/* !!!!!!
	 * 注意: 下面合并buffer的时候需要调用binder_delete_free_buffer()是否物理内存，
	 * 这是为什么呢，释放buffer的时候不是已经释放物理内存了吗?
	 * 是这样的，释放buffer的物理内存时只会是否"完全"归属于该buffer的物理内存页，
	 * 对于那些与其他(next或prev)共同使用的物理内存页是不会释放的(见上面的物理内存
	 * 释放说明)，现在要合并前后buffer，那么他们与当前buffer共同使用的物理内存页
	 * 也必须释放才行，否则就内存泄露了哦! binder_delete_free_buffer()这个函数就是
	 * 用来释放两个buffer共同使用的物理内存页的。
	 * !!!!!!
	 */
	// 如果当前释放的buffer不是buffer list的最后一个，
	// 就要检查其下一个buffer是否需要合并
	if (!list_is_last(&buffer->entry, &proc->buffers)) {
		struct binder_buffer *next = list_entry(buffer->entry.next,
						struct binder_buffer, entry);
		// 如果next是空闲的，只要把它delete就好了，其所占据的内核虚拟内存
		// 自然就归入到当前buffer中了
		if (next->free) {
			rb_erase(&next->rb_node, &proc->free_buffers);
			binder_delete_free_buffer(proc, next);
		}
	}
	// 如果当前释放的buffer不是buffer list的第一个，
	// 就要检查其上一个buffer是否需要合并
	if (proc->buffers.next != &buffer->entry) {
		struct binder_buffer *prev = list_entry(buffer->entry.prev,
						struct binder_buffer, entry);
		// 如果prev是空闲的，我们需要先把它delete掉，然后将当前buffer的内核虚拟内存
		// 归入prev buffer，然后将prev纳入free rb-tree中
		if (prev->free) {
			binder_delete_free_buffer(proc, buffer);
			rb_erase(&prev->rb_node, &proc->free_buffers);
			buffer = prev;
		}
	}
	// 把释放(物理内存)、合并(如果需要)之后的buffer放置到free rb-tree中
	binder_insert_free_buffer(proc, buffer);
}

static struct binder_node *binder_get_node(struct binder_proc *proc,
					   void __user *ptr)
{
	struct rb_node *n = proc->nodes.rb_node;
	struct binder_node *node;

	while (n) {
		node = rb_entry(n, struct binder_node, rb_node);

		if (ptr < node->ptr)
			n = n->rb_left;
		else if (ptr > node->ptr)
			n = n->rb_right;
		else
			return node;
	}
	return NULL;
}

/*
 * 创建新的binder node，一个node对应到一个service提供者，即一个在Service Manager
 * 中注册的service。
 * cookie就是该service对象在其进程中的address，如BnCameraService
 * 对象在其所在进程中的address.
 * ptr应该是handle值才对啊
 */
static struct binder_node *binder_new_node(struct binder_proc *proc,
					   void __user *ptr,
					   void __user *cookie)
{
	struct rb_node **p = &proc->nodes.rb_node;
	struct rb_node *parent = NULL;
	struct binder_node *node;

	while (*p) {
		parent = *p;
		node = rb_entry(parent, struct binder_node, rb_node);

		if (ptr < node->ptr)
			p = &(*p)->rb_left;
		else if (ptr > node->ptr)
			p = &(*p)->rb_right;
		else
			return NULL;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (node == NULL)
		return NULL;
	binder_stats_created(BINDER_STAT_NODE);
	rb_link_node(&node->rb_node, parent, p);
	rb_insert_color(&node->rb_node, &proc->nodes);
	node->debug_id = ++binder_last_id;
	node->proc = proc;
	node->ptr = ptr;
	node->cookie = cookie;
	node->work.type = BINDER_WORK_NODE;
	INIT_LIST_HEAD(&node->work.entry);
	INIT_LIST_HEAD(&node->async_todo);
	binder_debug(BINDER_DEBUG_INTERNAL_REFS,
		     "%d:%d node %d u%p c%p created\n",
		     proc->pid, current->pid, node->debug_id,
		     node->ptr, node->cookie);
	return node;
}

static int binder_inc_node(struct binder_node *node, int strong, int internal,
			   struct list_head *target_list)
{
	if (strong) {
		if (internal) {
			if (target_list == NULL &&
			    node->internal_strong_refs == 0 &&
			    !(node == binder_context_mgr_node &&
			    node->has_strong_ref)) {
				pr_err("invalid inc strong node for %d\n",
					node->debug_id);
				return -EINVAL;
			}
			node->internal_strong_refs++;
		} else
			node->local_strong_refs++;
		if (!node->has_strong_ref && target_list) {
			list_del_init(&node->work.entry);
			list_add_tail(&node->work.entry, target_list);
		}
	} else {
		if (!internal)
			node->local_weak_refs++;
		if (!node->has_weak_ref && list_empty(&node->work.entry)) {
			if (target_list == NULL) {
				pr_err("invalid inc weak node for %d\n",
					node->debug_id);
				return -EINVAL;
			}
			list_add_tail(&node->work.entry, target_list);
		}
	}
	return 0;
}

static int binder_dec_node(struct binder_node *node, int strong, int internal)
{
	if (strong) {
		if (internal)
			node->internal_strong_refs--;
		else
			node->local_strong_refs--;
		if (node->local_strong_refs || node->internal_strong_refs)
			return 0;
	} else {
		if (!internal)
			node->local_weak_refs--;
		if (node->local_weak_refs || !hlist_empty(&node->refs))
			return 0;
	}
	if (node->proc && (node->has_strong_ref || node->has_weak_ref)) {
		if (list_empty(&node->work.entry)) {
			list_add_tail(&node->work.entry, &node->proc->todo);
			wake_up_interruptible(&node->proc->wait);
		}
	} else {
		if (hlist_empty(&node->refs) && !node->local_strong_refs &&
		    !node->local_weak_refs) {
			list_del_init(&node->work.entry);
			if (node->proc) {
				rb_erase(&node->rb_node, &node->proc->nodes);
				binder_debug(BINDER_DEBUG_INTERNAL_REFS,
					     "refless node %d deleted\n",
					     node->debug_id);
			} else {
				hlist_del(&node->dead_node);
				binder_debug(BINDER_DEBUG_INTERNAL_REFS,
					     "dead node %d deleted\n",
					     node->debug_id);
			}
			kfree(node);
			binder_stats_deleted(BINDER_STAT_NODE);
		}
	}

	return 0;
}


static struct binder_ref *binder_get_ref(struct binder_proc *proc,
					 uint32_t desc)
{
	struct rb_node *n = proc->refs_by_desc.rb_node;
	struct binder_ref *ref;

	while (n) {
		ref = rb_entry(n, struct binder_ref, rb_node_desc);

		if (desc < ref->desc)
			n = n->rb_left;
		else if (desc > ref->desc)
			n = n->rb_right;
		else
			return ref;
	}
	return NULL;
}

static struct binder_ref *binder_get_ref_for_node(struct binder_proc *proc,
						  struct binder_node *node)
{
	struct rb_node *n;
	struct rb_node **p = &proc->refs_by_node.rb_node;
	struct rb_node *parent = NULL;
	struct binder_ref *ref, *new_ref;

	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_node);

		if (node < ref->node)
			p = &(*p)->rb_left;
		else if (node > ref->node)
			p = &(*p)->rb_right;
		else
			return ref;
	}
	new_ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (new_ref == NULL)
		return NULL;
	binder_stats_created(BINDER_STAT_REF);
	new_ref->debug_id = ++binder_last_id;
	new_ref->proc = proc;
	new_ref->node = node;
	rb_link_node(&new_ref->rb_node_node, parent, p);
	rb_insert_color(&new_ref->rb_node_node, &proc->refs_by_node);

	new_ref->desc = (node == binder_context_mgr_node) ? 0 : 1;
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
		ref = rb_entry(n, struct binder_ref, rb_node_desc);
		if (ref->desc > new_ref->desc)
			break;
		new_ref->desc = ref->desc + 1;
	}

	p = &proc->refs_by_desc.rb_node;
	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_desc);

		if (new_ref->desc < ref->desc)
			p = &(*p)->rb_left;
		else if (new_ref->desc > ref->desc)
			p = &(*p)->rb_right;
		else
			BUG();
	}
	rb_link_node(&new_ref->rb_node_desc, parent, p);
	rb_insert_color(&new_ref->rb_node_desc, &proc->refs_by_desc);
	if (node) {
		hlist_add_head(&new_ref->node_entry, &node->refs);

		binder_debug(BINDER_DEBUG_INTERNAL_REFS,
			     "%d new ref %d desc %d for node %d\n",
			      proc->pid, new_ref->debug_id, new_ref->desc,
			      node->debug_id);
	} else {
		binder_debug(BINDER_DEBUG_INTERNAL_REFS,
			     "%d new ref %d desc %d for dead node\n",
			      proc->pid, new_ref->debug_id, new_ref->desc);
	}
	return new_ref;
}

static void binder_delete_ref(struct binder_ref *ref)
{
	binder_debug(BINDER_DEBUG_INTERNAL_REFS,
		     "%d delete ref %d desc %d for node %d\n",
		      ref->proc->pid, ref->debug_id, ref->desc,
		      ref->node->debug_id);

	rb_erase(&ref->rb_node_desc, &ref->proc->refs_by_desc);
	rb_erase(&ref->rb_node_node, &ref->proc->refs_by_node);
	if (ref->strong)
		binder_dec_node(ref->node, 1, 1);
	hlist_del(&ref->node_entry);
	binder_dec_node(ref->node, 0, 1);
	if (ref->death) {
		binder_debug(BINDER_DEBUG_DEAD_BINDER,
			     "%d delete ref %d desc %d has death notification\n",
			      ref->proc->pid, ref->debug_id, ref->desc);
		list_del(&ref->death->work.entry);
		kfree(ref->death);
		binder_stats_deleted(BINDER_STAT_DEATH);
	}
	kfree(ref);
	binder_stats_deleted(BINDER_STAT_REF);
}

static int binder_inc_ref(struct binder_ref *ref, int strong,
			  struct list_head *target_list)
{
	int ret;
	if (strong) {
		if (ref->strong == 0) {
			ret = binder_inc_node(ref->node, 1, 1, target_list);
			if (ret)
				return ret;
		}
		ref->strong++;
	} else {
		if (ref->weak == 0) {
			ret = binder_inc_node(ref->node, 0, 1, target_list);
			if (ret)
				return ret;
		}
		ref->weak++;
	}
	return 0;
}


static int binder_dec_ref(struct binder_ref *ref, int strong)
{
	if (strong) {
		if (ref->strong == 0) {
			binder_user_error("%d invalid dec strong, ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->strong--;
		if (ref->strong == 0) {
			int ret;
			ret = binder_dec_node(ref->node, strong, 1);
			if (ret)
				return ret;
		}
	} else {
		if (ref->weak == 0) {
			binder_user_error("%d invalid dec weak, ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->weak--;
	}
	if (ref->strong == 0 && ref->weak == 0)
		binder_delete_ref(ref);
	return 0;
}

static void binder_pop_transaction(struct binder_thread *target_thread,
				   struct binder_transaction *t)
{
	if (target_thread) {
		BUG_ON(target_thread->transaction_stack != t);
		BUG_ON(target_thread->transaction_stack->from != target_thread);
		target_thread->transaction_stack =
			target_thread->transaction_stack->from_parent;
		t->from = NULL;
	}
	t->need_reply = 0;
	if (t->buffer)
		t->buffer->transaction = NULL;
	kfree(t);
	binder_stats_deleted(BINDER_STAT_TRANSACTION);
}

static void binder_send_failed_reply(struct binder_transaction *t,
				     uint32_t error_code)
{
	struct binder_thread *target_thread;
	BUG_ON(t->flags & TF_ONE_WAY);
	while (1) {
		target_thread = t->from;
		if (target_thread) {
			if (target_thread->return_error != BR_OK &&
			   target_thread->return_error2 == BR_OK) {
				target_thread->return_error2 =
					target_thread->return_error;
				target_thread->return_error = BR_OK;
			}
			if (target_thread->return_error == BR_OK) {
				binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
					     "send failed reply for transaction %d to %d:%d\n",
					      t->debug_id, target_thread->proc->pid,
					      target_thread->pid);

				binder_pop_transaction(target_thread, t);
				target_thread->return_error = error_code;
				wake_up_interruptible(&target_thread->wait);
			} else {
				pr_err("reply failed, target thread, %d:%d, has error code %d already\n",
					target_thread->proc->pid,
					target_thread->pid,
					target_thread->return_error);
			}
			return;
		} else {
			struct binder_transaction *next = t->from_parent;

			binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
				     "send failed reply for transaction %d, target dead\n",
				     t->debug_id);

			binder_pop_transaction(target_thread, t);
			if (next == NULL) {
				binder_debug(BINDER_DEBUG_DEAD_BINDER,
					     "reply failed, no target thread at root\n");
				return;
			}
			t = next;
			binder_debug(BINDER_DEBUG_DEAD_BINDER,
				     "reply failed, no target thread -- retry %d\n",
				      t->debug_id);
		}
	}
}

static void binder_transaction_buffer_release(struct binder_proc *proc,
					      struct binder_buffer *buffer,
					      size_t *failed_at)
{
	size_t *offp, *off_end;
	int debug_id = buffer->debug_id;

	binder_debug(BINDER_DEBUG_TRANSACTION,
		     "%d buffer release %d, size %zd-%zd, failed at %p\n",
		     proc->pid, buffer->debug_id,
		     buffer->data_size, buffer->offsets_size, failed_at);

	if (buffer->target_node)
		binder_dec_node(buffer->target_node, 1, 0);

	offp = (size_t *)(buffer->data + ALIGN(buffer->data_size, sizeof(void *)));
	if (failed_at)
		off_end = failed_at;
	else
		off_end = (void *)offp + buffer->offsets_size;
	for (; offp < off_end; offp++) {
		struct flat_binder_object *fp;
		if (*offp > buffer->data_size - sizeof(*fp) ||
		    buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(void *))) {
			pr_err("transaction release %d bad offset %zd, size %zd\n",
			 debug_id, *offp, buffer->data_size);
			continue;
		}
		fp = (struct flat_binder_object *)(buffer->data + *offp);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct binder_node *node = binder_get_node(proc, fp->binder);
			if (node == NULL) {
				pr_err("transaction release %d bad node %p\n",
					debug_id, fp->binder);
				break;
			}
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        node %d u%p\n",
				     node->debug_id, node->ptr);
			binder_dec_node(node, fp->type == BINDER_TYPE_BINDER, 0);
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
			struct binder_ref *ref = binder_get_ref(proc, fp->handle);
			if (ref == NULL) {
				pr_err("transaction release %d bad handle %ld\n",
				 debug_id, fp->handle);
				break;
			}
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        ref %d desc %d (node %d)\n",
				     ref->debug_id, ref->desc, ref->node->debug_id);
			binder_dec_ref(ref, fp->type == BINDER_TYPE_HANDLE);
		} break;

		case BINDER_TYPE_FD:
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        fd %ld\n", fp->handle);
			if (failed_at)
				task_close_fd(proc, fp->handle);
			break;

		default:
			pr_err("transaction release %d bad object type %lx\n",
				debug_id, fp->type);
			break;
		}
	}
}

static void binder_transaction(struct binder_proc *proc,
			       struct binder_thread *thread,
			       struct binder_transaction_data *tr, int reply)
{
	struct binder_transaction *t;
	struct binder_work *tcomplete;
	size_t *offp, *off_end;
	struct binder_proc *target_proc;
	struct binder_thread *target_thread = NULL;
	struct binder_node *target_node = NULL;
	struct list_head *target_list;
	wait_queue_head_t *target_wait;
	struct binder_transaction *in_reply_to = NULL;
	struct binder_transaction_log_entry *e;
	uint32_t return_error;

	e = binder_transaction_log_add(&binder_transaction_log);
	e->call_type = reply ? 2 : !!(tr->flags & TF_ONE_WAY);
	e->from_proc = proc->pid;
	e->from_thread = thread->pid;
	e->target_handle = tr->target.handle;
	e->data_size = tr->data_size;
	e->offsets_size = tr->offsets_size;

	if (reply) {
		in_reply_to = thread->transaction_stack;
		if (in_reply_to == NULL) {
			binder_user_error("%d:%d got reply transaction with no transaction stack\n",
					  proc->pid, thread->pid);
			return_error = BR_FAILED_REPLY;
			goto err_empty_call_stack;
		}
		binder_set_nice(in_reply_to->saved_priority);
		if (in_reply_to->to_thread != thread) {
			binder_user_error("%d:%d got reply transaction with bad transaction stack, transaction %d has target %d:%d\n",
				proc->pid, thread->pid, in_reply_to->debug_id,
				in_reply_to->to_proc ?
				in_reply_to->to_proc->pid : 0,
				in_reply_to->to_thread ?
				in_reply_to->to_thread->pid : 0);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			goto err_bad_call_stack;
		}
		thread->transaction_stack = in_reply_to->to_parent;
		target_thread = in_reply_to->from;
		if (target_thread == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (target_thread->transaction_stack != in_reply_to) {
			binder_user_error("%d:%d got reply transaction with bad target transaction stack %d, expected %d\n",
				proc->pid, thread->pid,
				target_thread->transaction_stack ?
				target_thread->transaction_stack->debug_id : 0,
				in_reply_to->debug_id);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			target_thread = NULL;
			goto err_dead_binder;
		}
		target_proc = target_thread->proc;
	} else {
		if (tr->target.handle) {
			struct binder_ref *ref;
			ref = binder_get_ref(proc, tr->target.handle);
			if (ref == NULL) {
				binder_user_error("%d:%d got transaction to invalid handle\n",
					proc->pid, thread->pid);
				return_error = BR_FAILED_REPLY;
				goto err_invalid_target_handle;
			}
			target_node = ref->node;
		} else {
			target_node = binder_context_mgr_node;
			if (target_node == NULL) {
				return_error = BR_DEAD_REPLY;
				goto err_no_context_mgr_node;
			}
		}
		e->to_node = target_node->debug_id;
		target_proc = target_node->proc;
		if (target_proc == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (!(tr->flags & TF_ONE_WAY) && thread->transaction_stack) {
			struct binder_transaction *tmp;
			tmp = thread->transaction_stack;
			if (tmp->to_thread != thread) {
				binder_user_error("%d:%d got new transaction with bad transaction stack, transaction %d has target %d:%d\n",
					proc->pid, thread->pid, tmp->debug_id,
					tmp->to_proc ? tmp->to_proc->pid : 0,
					tmp->to_thread ?
					tmp->to_thread->pid : 0);
				return_error = BR_FAILED_REPLY;
				goto err_bad_call_stack;
			}
			while (tmp) {
				if (tmp->from && tmp->from->proc == target_proc)
					target_thread = tmp->from;
				tmp = tmp->from_parent;
			}
		}
	}
	if (target_thread) {
		e->to_thread = target_thread->pid;
		target_list = &target_thread->todo;
		target_wait = &target_thread->wait;
	} else {
		target_list = &target_proc->todo;
		target_wait = &target_proc->wait;
	}
	e->to_proc = target_proc->pid;

	/* TODO: reuse incoming transaction for reply */
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (t == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_t_failed;
	}
	binder_stats_created(BINDER_STAT_TRANSACTION);

	tcomplete = kzalloc(sizeof(*tcomplete), GFP_KERNEL);
	if (tcomplete == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_tcomplete_failed;
	}
	binder_stats_created(BINDER_STAT_TRANSACTION_COMPLETE);

	t->debug_id = ++binder_last_id;
	e->debug_id = t->debug_id;

	if (reply)
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d BC_REPLY %d -> %d:%d, data %p-%p size %zd-%zd\n",
			     proc->pid, thread->pid, t->debug_id,
			     target_proc->pid, target_thread->pid,
			     tr->data.ptr.buffer, tr->data.ptr.offsets,
			     tr->data_size, tr->offsets_size);
	else
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d BC_TRANSACTION %d -> %d - node %d, data %p-%p size %zd-%zd\n",
			     proc->pid, thread->pid, t->debug_id,
			     target_proc->pid, target_node->debug_id,
			     tr->data.ptr.buffer, tr->data.ptr.offsets,
			     tr->data_size, tr->offsets_size);

	if (!reply && !(tr->flags & TF_ONE_WAY))
		t->from = thread;
	else
		t->from = NULL;
	t->sender_euid = proc->tsk->cred->euid;
	t->to_proc = target_proc;
	t->to_thread = target_thread;
	t->code = tr->code;
	t->flags = tr->flags;
	t->priority = task_nice(current);

	trace_binder_transaction(reply, t, target_node);

	t->buffer = binder_alloc_buf(target_proc, tr->data_size,
		tr->offsets_size, !reply && (t->flags & TF_ONE_WAY));
	if (t->buffer == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_binder_alloc_buf_failed;
	}
	t->buffer->allow_user_free = 0;
	t->buffer->debug_id = t->debug_id;
	t->buffer->transaction = t;
	t->buffer->target_node = target_node;
	trace_binder_transaction_alloc_buf(t->buffer);
	if (target_node)
		binder_inc_node(target_node, 1, 0, NULL);

	// 把数据从应用复制到内核，binder号称的"一次数据复制"指的就是这里
	offp = (size_t *)(t->buffer->data + ALIGN(tr->data_size, sizeof(void *)));

	// 首先复制data
	if (copy_from_user(t->buffer->data, tr->data.ptr.buffer, tr->data_size)) {
		binder_user_error("%d:%d got transaction with invalid data ptr\n",
				proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	// 然后复制offset数组
	if (copy_from_user(offp, tr->data.ptr.offsets, tr->offsets_size)) {
		binder_user_error("%d:%d got transaction with invalid offsets ptr\n",
				proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	if (!IS_ALIGNED(tr->offsets_size, sizeof(size_t))) {
		binder_user_error("%d:%d got transaction with invalid offsets size, %zd\n",
				proc->pid, thread->pid, tr->offsets_size);
		return_error = BR_FAILED_REPLY;
		goto err_bad_offset;
	}
	/*
	 * offset数组是一个int数组，第n项记录了第n个flat_binder_object结构体
	 * 在binder_transaction结构体的数据区的偏移量
	 *
	 * offp表示offset数组的开始位置，off_end表示offset数组的结束位置
	 * 下面循环遍历offset数组来处理每一个flat_binder_object对象
	 *
	 */
	off_end = (void *)offp + tr->offsets_size;
	for (; offp < off_end; offp++) {
		struct flat_binder_object *fp;
		/*
		 * 数据有效性检查:
		 * a). 偏移量一定要在data_size的范围以内
		 * b). data_size - 偏移量 > sizeof(flat_binder_object)
		 *     因为buffer_data + 偏移量 这个位置保存的是某一个
		 *     flat_binder_object对象
		 * 下面的if判断即为: if(b || a) ...
		 */
		if (*offp > t->buffer->data_size - sizeof(*fp) ||
		    t->buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(void *))) {
			binder_user_error("%d:%d got transaction with invalid offset, %zd\n",
					proc->pid, thread->pid, *offp);
			return_error = BR_FAILED_REPLY;
			goto err_bad_offset;
		}
		// 现在取到某一个flat_binder_object对象了，下面根据其类型分别处理
		fp = (struct flat_binder_object *)(t->buffer->data + *offp);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct binder_ref *ref;
			struct binder_node *node = binder_get_node(proc, fp->binder);
			if (node == NULL) {
				node = binder_new_node(proc, fp->binder, fp->cookie);
				if (node == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_new_node_failed;
				}
				node->min_priority = fp->flags & FLAT_BINDER_FLAG_PRIORITY_MASK;
				node->accept_fds = !!(fp->flags & FLAT_BINDER_FLAG_ACCEPTS_FDS);
			}
			if (fp->cookie != node->cookie) {
				binder_user_error("%d:%d sending u%p node %d, cookie mismatch %p != %p\n",
					proc->pid, thread->pid,
					fp->binder, node->debug_id,
					fp->cookie, node->cookie);
				goto err_binder_get_ref_for_node_failed;
			}
			ref = binder_get_ref_for_node(target_proc, node);
			if (ref == NULL) {
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_for_node_failed;
			}
			if (fp->type == BINDER_TYPE_BINDER)
				fp->type = BINDER_TYPE_HANDLE;
			else
				fp->type = BINDER_TYPE_WEAK_HANDLE;
			fp->handle = ref->desc;
			binder_inc_ref(ref, fp->type == BINDER_TYPE_HANDLE,
				       &thread->todo);

			trace_binder_transaction_node_to_ref(t, node, ref);
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        node %d u%p -> ref %d desc %d\n",
				     node->debug_id, node->ptr, ref->debug_id,
				     ref->desc);
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
			struct binder_ref *ref = binder_get_ref(proc, fp->handle);
			if (ref == NULL) {
				binder_user_error("%d:%d got transaction with invalid handle, %ld\n",
						proc->pid,
						thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_failed;
			}
			if (ref->node->proc == target_proc) {
				if (fp->type == BINDER_TYPE_HANDLE)
					fp->type = BINDER_TYPE_BINDER;
				else
					fp->type = BINDER_TYPE_WEAK_BINDER;
				fp->binder = ref->node->ptr;
				fp->cookie = ref->node->cookie;
				binder_inc_node(ref->node, fp->type == BINDER_TYPE_BINDER, 0, NULL);
				trace_binder_transaction_ref_to_node(t, ref);
				binder_debug(BINDER_DEBUG_TRANSACTION,
					     "        ref %d desc %d -> node %d u%p\n",
					     ref->debug_id, ref->desc, ref->node->debug_id,
					     ref->node->ptr);
			} else {
				struct binder_ref *new_ref;
				new_ref = binder_get_ref_for_node(target_proc, ref->node);
				if (new_ref == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_get_ref_for_node_failed;
				}
				fp->handle = new_ref->desc;
				binder_inc_ref(new_ref, fp->type == BINDER_TYPE_HANDLE, NULL);
				trace_binder_transaction_ref_to_ref(t, ref,
								    new_ref);
				binder_debug(BINDER_DEBUG_TRANSACTION,
					     "        ref %d desc %d -> ref %d desc %d (node %d)\n",
					     ref->debug_id, ref->desc, new_ref->debug_id,
					     new_ref->desc, ref->node->debug_id);
			}
		} break;

		case BINDER_TYPE_FD: {
			int target_fd;
			struct file *file;

			if (reply) {
				if (!(in_reply_to->flags & TF_ACCEPT_FDS)) {
					binder_user_error("%d:%d got reply with fd, %ld, but target does not allow fds\n",
						proc->pid, thread->pid, fp->handle);
					return_error = BR_FAILED_REPLY;
					goto err_fd_not_allowed;
				}
			} else if (!target_node->accept_fds) {
				binder_user_error("%d:%d got transaction with fd, %ld, but target does not allow fds\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fd_not_allowed;
			}

			file = fget(fp->handle);
			if (file == NULL) {
				binder_user_error("%d:%d got transaction with invalid fd, %ld\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fget_failed;
			}
			target_fd = task_get_unused_fd_flags(target_proc, O_CLOEXEC);
			if (target_fd < 0) {
				fput(file);
				return_error = BR_FAILED_REPLY;
				goto err_get_unused_fd_failed;
			}
			task_fd_install(target_proc, target_fd, file);
			trace_binder_transaction_fd(t, fp->handle, target_fd);
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        fd %ld -> %d\n", fp->handle, target_fd);
			/* TODO: fput? */
			fp->handle = target_fd;
		} break;

		default:
			binder_user_error("%d:%d got transaction with invalid object type, %lx\n",
				proc->pid, thread->pid, fp->type);
			return_error = BR_FAILED_REPLY;
			goto err_bad_object_type;
		}
	}
	if (reply) {
		BUG_ON(t->buffer->async_transaction != 0);
		binder_pop_transaction(target_thread, in_reply_to);
	} else if (!(t->flags & TF_ONE_WAY)) {
		BUG_ON(t->buffer->async_transaction != 0);
		t->need_reply = 1;
		t->from_parent = thread->transaction_stack;
		thread->transaction_stack = t;
	} else {
		BUG_ON(target_node == NULL);
		BUG_ON(t->buffer->async_transaction != 1);
		if (target_node->has_async_transaction) {
			target_list = &target_node->async_todo;
			target_wait = NULL;
		} else
			target_node->has_async_transaction = 1;
	}
	t->work.type = BINDER_WORK_TRANSACTION;
	list_add_tail(&t->work.entry, target_list);
	tcomplete->type = BINDER_WORK_TRANSACTION_COMPLETE;
	list_add_tail(&tcomplete->entry, &thread->todo);
	if (target_wait)
		wake_up_interruptible(target_wait);
	return;

err_get_unused_fd_failed:
err_fget_failed:
err_fd_not_allowed:
err_binder_get_ref_for_node_failed:
err_binder_get_ref_failed:
err_binder_new_node_failed:
err_bad_object_type:
err_bad_offset:
err_copy_data_failed:
	trace_binder_transaction_failed_buffer_release(t->buffer);
	binder_transaction_buffer_release(target_proc, t->buffer, offp);
	t->buffer->transaction = NULL;
	binder_free_buf(target_proc, t->buffer);
err_binder_alloc_buf_failed:
	kfree(tcomplete);
	binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
err_alloc_tcomplete_failed:
	kfree(t);
	binder_stats_deleted(BINDER_STAT_TRANSACTION);
err_alloc_t_failed:
err_bad_call_stack:
err_empty_call_stack:
err_dead_binder:
err_invalid_target_handle:
err_no_context_mgr_node:
	binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
		     "%d:%d transaction failed %d, size %zd-%zd\n",
		     proc->pid, thread->pid, return_error,
		     tr->data_size, tr->offsets_size);

	{
		struct binder_transaction_log_entry *fe;
		fe = binder_transaction_log_add(&binder_transaction_log_failed);
		*fe = *e;
	}

	BUG_ON(thread->return_error != BR_OK);
	if (in_reply_to) {
		thread->return_error = BR_TRANSACTION_COMPLETE;
		binder_send_failed_reply(in_reply_to, return_error);
	} else
		thread->return_error = return_error;
}

int binder_thread_write(struct binder_proc *proc, struct binder_thread *thread,
			void __user *buffer, int size, signed long *consumed)
{
	uint32_t cmd;
	void __user *ptr = buffer + *consumed;
	void __user *end = buffer + size;

	while (ptr < end && thread->return_error == BR_OK) {
		// 取出具体的write-read命令
		if (get_user(cmd, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		trace_binder_command(cmd);
		if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.bc)) {
			binder_stats.bc[_IOC_NR(cmd)]++;
			proc->stats.bc[_IOC_NR(cmd)]++;
			thread->stats.bc[_IOC_NR(cmd)]++;
		}
		switch (cmd) {
		case BC_INCREFS:
		case BC_ACQUIRE:
		case BC_RELEASE:
		case BC_DECREFS: {
			// 这是从client传递进来的命令，要求修改binder_ref对象的引用
			// 首先获取到binder_ref对象，然后分情况处理各个命令
			uint32_t target; // 即handle/desc
			struct binder_ref *ref;
			const char *debug_string;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (target == 0 && binder_context_mgr_node &&
			    (cmd == BC_INCREFS || cmd == BC_ACQUIRE)) {
				ref = binder_get_ref_for_node(proc,
					       binder_context_mgr_node);
				if (ref->desc != target) {
					binder_user_error("%d:%d tried to acquire reference to desc 0, got %d instead\n",
						proc->pid, thread->pid,
						ref->desc);
				}
			} else
				ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("%d:%d refcount change on invalid ref %d\n",
					proc->pid, thread->pid, target);
				break;
			}
			switch (cmd) {
			case BC_INCREFS:
				debug_string = "IncRefs";
				binder_inc_ref(ref, 0, NULL);
				break;
			case BC_ACQUIRE:
				debug_string = "Acquire";
				binder_inc_ref(ref, 1, NULL);
				break;
			case BC_RELEASE:
				debug_string = "Release";
				binder_dec_ref(ref, 1);
				break;
			case BC_DECREFS:
			default:
				debug_string = "DecRefs";
				binder_dec_ref(ref, 0);
				break;
			}
			binder_debug(BINDER_DEBUG_USER_REFS,
				     "%d:%d %s ref %d desc %d s %d w %d for node %d\n",
				     proc->pid, thread->pid, debug_string, ref->debug_id,
				     ref->desc, ref->strong, ref->weak, ref->node->debug_id);
			break;
		}
		case BC_INCREFS_DONE:
		case BC_ACQUIRE_DONE: {
			// 这是service端修改BBinder引用计数完成之后，
			// 告知驱动时发送的消息
			void __user *node_ptr; // weak ref of BBinder
			void *cookie;
			struct binder_node *node;

			if (get_user(node_ptr, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			if (get_user(cookie, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			node = binder_get_node(proc, node_ptr);
			if (node == NULL) {
				binder_user_error("%d:%d %s u%p no match\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" :
					"BC_ACQUIRE_DONE",
					node_ptr);
				break;
			}
			if (cookie != node->cookie) {
				binder_user_error("%d:%d %s u%p node %d cookie mismatch %p != %p\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
					node_ptr, node->debug_id,
					cookie, node->cookie);
				break;
			}
			if (cmd == BC_ACQUIRE_DONE) {
				if (node->pending_strong_ref == 0) {
					binder_user_error("%d:%d BC_ACQUIRE_DONE node %d has no pending acquire request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_strong_ref = 0;
			} else {
				if (node->pending_weak_ref == 0) {
					binder_user_error("%d:%d BC_INCREFS_DONE node %d has no pending increfs request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_weak_ref = 0;
			}
			binder_dec_node(node, cmd == BC_ACQUIRE_DONE, 0);
			binder_debug(BINDER_DEBUG_USER_REFS,
				     "%d:%d %s node %d ls %d lw %d\n",
				     proc->pid, thread->pid,
				     cmd == BC_INCREFS_DONE ? "BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
				     node->debug_id, node->local_strong_refs, node->local_weak_refs);
			break;
		}
		case BC_ATTEMPT_ACQUIRE:
			pr_err("BC_ATTEMPT_ACQUIRE not supported\n");
			return -EINVAL;
		case BC_ACQUIRE_RESULT:
			pr_err("BC_ACQUIRE_RESULT not supported\n");
			return -EINVAL;

		case BC_FREE_BUFFER: {
			void __user *data_ptr;
			struct binder_buffer *buffer;

			if (get_user(data_ptr, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);

			buffer = binder_buffer_lookup(proc, data_ptr);
			if (buffer == NULL) {
				binder_user_error("%d:%d BC_FREE_BUFFER u%p no match\n",
					proc->pid, thread->pid, data_ptr);
				break;
			}
			if (!buffer->allow_user_free) {
				binder_user_error("%d:%d BC_FREE_BUFFER u%p matched unreturned buffer\n",
					proc->pid, thread->pid, data_ptr);
				break;
			}
			binder_debug(BINDER_DEBUG_FREE_BUFFER,
				     "%d:%d BC_FREE_BUFFER u%p found buffer %d for %s transaction\n",
				     proc->pid, thread->pid, data_ptr, buffer->debug_id,
				     buffer->transaction ? "active" : "finished");

			if (buffer->transaction) {
				buffer->transaction->buffer = NULL;
				buffer->transaction = NULL;
			}
			if (buffer->async_transaction && buffer->target_node) {
				BUG_ON(!buffer->target_node->has_async_transaction);
				if (list_empty(&buffer->target_node->async_todo))
					buffer->target_node->has_async_transaction = 0;
				else
					list_move_tail(buffer->target_node->async_todo.next, &thread->todo);
			}
			trace_binder_transaction_buffer_release(buffer);
			binder_transaction_buffer_release(proc, buffer, NULL);
			binder_free_buf(proc, buffer);
			break;
		}

		case BC_TRANSACTION:
		case BC_REPLY: {
			struct binder_transaction_data tr;

			if (copy_from_user(&tr, ptr, sizeof(tr)))
				return -EFAULT;
			ptr += sizeof(tr);
			binder_transaction(proc, thread, &tr, cmd == BC_REPLY);
			break;
		}

		case BC_REGISTER_LOOPER:
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_REGISTER_LOOPER\n",
				     proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_ENTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_REGISTER_LOOPER called after BC_ENTER_LOOPER\n",
					proc->pid, thread->pid);
			} else if (proc->requested_threads == 0) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_REGISTER_LOOPER called without request\n",
					proc->pid, thread->pid);
			} else {
				proc->requested_threads--;
				proc->requested_threads_started++;
			}
			thread->looper |= BINDER_LOOPER_STATE_REGISTERED;
			break;
		case BC_ENTER_LOOPER:
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_ENTER_LOOPER\n",
				     proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_REGISTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_ENTER_LOOPER called after BC_REGISTER_LOOPER\n",
					proc->pid, thread->pid);
			}
			thread->looper |= BINDER_LOOPER_STATE_ENTERED;
			break;
		case BC_EXIT_LOOPER:
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_EXIT_LOOPER\n",
				     proc->pid, thread->pid);
			thread->looper |= BINDER_LOOPER_STATE_EXITED;
			break;

		case BC_REQUEST_DEATH_NOTIFICATION:
		case BC_CLEAR_DEATH_NOTIFICATION: {
			// target指的是handle
			uint32_t target;
			// cookie指的是BpBinder的内存地址
			void __user *cookie;
			struct binder_ref *ref;
			struct binder_ref_death *death;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (get_user(cookie, (void __user * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("%d:%d %s invalid ref %d\n",
					proc->pid, thread->pid,
					cmd == BC_REQUEST_DEATH_NOTIFICATION ?
					"BC_REQUEST_DEATH_NOTIFICATION" :
					"BC_CLEAR_DEATH_NOTIFICATION",
					target);
				break;
			}

			binder_debug(BINDER_DEBUG_DEATH_NOTIFICATION,
				     "%d:%d %s %p ref %d desc %d s %d w %d for node %d\n",
				     proc->pid, thread->pid,
				     cmd == BC_REQUEST_DEATH_NOTIFICATION ?
				     "BC_REQUEST_DEATH_NOTIFICATION" :
				     "BC_CLEAR_DEATH_NOTIFICATION",
				     cookie, ref->debug_id, ref->desc,
				     ref->strong, ref->weak, ref->node->debug_id);

			/*
			 * 注册死亡通知:
			 *  a). 找到binder_ref: handle => binder_ref
			 *  b). 分配binder_ref_death对象
			 *  c). 初始化binder_ref_death对象并关联到binder_ref对象
			 *
			 * 由于binder_ref对象已经关联到binder_node了(创建binder_ref时)，
			 * binder_node所在进程正常/意外死亡时必定会调用到release system call，
			 * 在那里node会检查其所关联的binder_ref(binder_node->refs)是否
			 * 有death对象，如果有就发送死亡通知。
			 */
			if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
				if (ref->death) {
					binder_user_error("%d:%d BC_REQUEST_DEATH_NOTIFICATION death notification already set\n",
						proc->pid, thread->pid);
					break;
				}
				death = kzalloc(sizeof(*death), GFP_KERNEL);
				if (death == NULL) {
					thread->return_error = BR_ERROR;
					binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
						     "%d:%d BC_REQUEST_DEATH_NOTIFICATION failed\n",
						     proc->pid, thread->pid);
					break;
				}
				binder_stats_created(BINDER_STAT_DEATH);
				INIT_LIST_HEAD(&death->work.entry);
				death->cookie = cookie;
				ref->death = death;
				/* 如果关注的node已经死亡，就直接把这个work挂载到client进程的todo列表中，
				 * 下一次client处理todo队列时就会知道node已经死亡了，这次检查是必须进行
				 * 的，否则如果node已经死亡的话，死亡通知就永远发不出来了，因为已经死亡
				 * 的node是不会调用到release system call的，见上面的说明。
				 */
				if (ref->node->proc == NULL) {
					ref->death->work.type = BINDER_WORK_DEAD_BINDER;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&ref->death->work.entry, &thread->todo);
					} else {
						list_add_tail(&ref->death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				}
			} else {
				/*
				 * 注销死亡通知:
				 *  a). 去掉binder_ref里的death对象: ref->death = NULL
				 *  b). 处理已经存在的死亡通知:
				 *      1. 如果当前有死亡通知等待处理，就修改其type为BINDER_WORK_DEAD_BINDER_AND_CLEAR
				 *         即处理死亡通知并注销死亡通知(既然当前已有死亡通知，death->work肯定被挂载到
				 *         client thread的todo队列中了)
				 *      2. 没有死亡通知的话就通知client thread(pool thread)死亡通知已经注销了
				 *         (往client thread的todo队列中发送BINDER_WORK_CLEAR_DEATH_NOTIFICATION消息)
				 */
				if (ref->death == NULL) {
					binder_user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification not active\n",
						proc->pid, thread->pid);
					break;
				}
				death = ref->death;
				if (death->cookie != cookie) {
					binder_user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification cookie mismatch %p != %p\n",
						proc->pid, thread->pid,
						death->cookie, cookie);
					break;
				}
				ref->death = NULL;
				if (list_empty(&death->work.entry)) {
					death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&death->work.entry, &thread->todo);
					} else {
						list_add_tail(&death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				} else {
					BUG_ON(death->work.type != BINDER_WORK_DEAD_BINDER);
					death->work.type = BINDER_WORK_DEAD_BINDER_AND_CLEAR;
				}
			}
		} break;
		case BC_DEAD_BINDER_DONE: {
			// 接收到的死亡通知已经处理完毕
			struct binder_work *w;
			void __user *cookie;
			struct binder_ref_death *death = NULL;
			if (get_user(cookie, (void __user * __user *)ptr))
				return -EFAULT;

			ptr += sizeof(void *);
			list_for_each_entry(w, &proc->delivered_death, entry) {
				struct binder_ref_death *tmp_death = container_of(w, struct binder_ref_death, work);
				if (tmp_death->cookie == cookie) {
					death = tmp_death;
					break;
				}
			}
			binder_debug(BINDER_DEBUG_DEAD_BINDER,
				     "%d:%d BC_DEAD_BINDER_DONE %p found %p\n",
				     proc->pid, thread->pid, cookie, death);
			if (death == NULL) {
				binder_user_error("%d:%d BC_DEAD_BINDER_DONE %p not found\n",
					proc->pid, thread->pid, cookie);
				break;
			}

			list_del_init(&death->work.entry);
			if (death->work.type == BINDER_WORK_DEAD_BINDER_AND_CLEAR) {
				death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
				if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
					list_add_tail(&death->work.entry, &thread->todo);
				} else {
					list_add_tail(&death->work.entry, &proc->todo);
					wake_up_interruptible(&proc->wait);
				}
			}
		} break;

		default:
			pr_err("%d:%d unknown command %d\n",
			       proc->pid, thread->pid, cmd);
			return -EINVAL;
		}
		*consumed = ptr - buffer;
	}
	return 0;
}

void binder_stat_br(struct binder_proc *proc, struct binder_thread *thread,
		    uint32_t cmd)
{
	trace_binder_return(cmd);
	if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.br)) {
		binder_stats.br[_IOC_NR(cmd)]++;
		proc->stats.br[_IOC_NR(cmd)]++;
		thread->stats.br[_IOC_NR(cmd)]++;
	}
}

static int binder_has_proc_work(struct binder_proc *proc,
				struct binder_thread *thread)
{
	return !list_empty(&proc->todo) ||
		(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

static int binder_has_thread_work(struct binder_thread *thread)
{
	return !list_empty(&thread->todo) || thread->return_error != BR_OK ||
		(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

static int binder_thread_read(struct binder_proc *proc,
			      struct binder_thread *thread,
			      void  __user *buffer, int size,
			      signed long *consumed, int non_block)
{
	void __user *ptr = buffer + *consumed;
	void __user *end = buffer + size;

	int ret = 0;
	int wait_for_proc_work;

	if (*consumed == 0) {
		if (put_user(BR_NOOP, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
	}

retry:
	wait_for_proc_work = thread->transaction_stack == NULL &&
				list_empty(&thread->todo);

	if (thread->return_error != BR_OK && ptr < end) {
		if (thread->return_error2 != BR_OK) {
			if (put_user(thread->return_error2, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			binder_stat_br(proc, thread, thread->return_error2);
			if (ptr == end)
				goto done;
			thread->return_error2 = BR_OK;
		}
		if (put_user(thread->return_error, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		binder_stat_br(proc, thread, thread->return_error);
		thread->return_error = BR_OK;
		goto done;
	}


	thread->looper |= BINDER_LOOPER_STATE_WAITING;
	if (wait_for_proc_work)
		proc->ready_threads++;

	binder_unlock(__func__);

	trace_binder_wait_for_work(wait_for_proc_work,
				   !!thread->transaction_stack,
				   !list_empty(&thread->todo));
	if (wait_for_proc_work) {
		if (!(thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
					BINDER_LOOPER_STATE_ENTERED))) {
			binder_user_error("%d:%d ERROR: Thread waiting for process work before calling BC_REGISTER_LOOPER or BC_ENTER_LOOPER (state %x)\n",
				proc->pid, thread->pid, thread->looper);
			wait_event_interruptible(binder_user_error_wait,
						 binder_stop_on_user_error < 2);
		}
		binder_set_nice(proc->default_priority);
		if (non_block) {
			if (!binder_has_proc_work(proc, thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_interruptible_exclusive(proc->wait, binder_has_proc_work(proc, thread));
	} else {
		if (non_block) {
			if (!binder_has_thread_work(thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_interruptible(thread->wait, binder_has_thread_work(thread));
	}

	binder_lock(__func__);

	if (wait_for_proc_work)
		proc->ready_threads--;
	thread->looper &= ~BINDER_LOOPER_STATE_WAITING;

	if (ret)
		return ret;

	while (1) {
		uint32_t cmd;
		struct binder_transaction_data tr;
		struct binder_work *w;
		struct binder_transaction *t = NULL;

		if (!list_empty(&thread->todo))
			w = list_first_entry(&thread->todo, struct binder_work, entry);
		else if (!list_empty(&proc->todo) && wait_for_proc_work)
			w = list_first_entry(&proc->todo, struct binder_work, entry);
		else {
			if (ptr - buffer == 4 && !(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN)) /* no data added */
				goto retry;
			break;
		}

		if (end - ptr < sizeof(tr) + 4)
			break;

		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			t = container_of(w, struct binder_transaction, work);
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {
			cmd = BR_TRANSACTION_COMPLETE;
			if (put_user(cmd, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);

			binder_stat_br(proc, thread, cmd);
			binder_debug(BINDER_DEBUG_TRANSACTION_COMPLETE,
				     "%d:%d BR_TRANSACTION_COMPLETE\n",
				     proc->pid, thread->pid);

			list_del(&w->entry);
			kfree(w);
			binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
		} break;
		case BINDER_WORK_NODE: {
			struct binder_node *node = container_of(w, struct binder_node, work);
			uint32_t cmd = BR_NOOP;
			const char *cmd_name;
			int strong = node->internal_strong_refs || node->local_strong_refs;
			int weak = !hlist_empty(&node->refs) || node->local_weak_refs || strong;
			if (weak && !node->has_weak_ref) {
				cmd = BR_INCREFS;
				cmd_name = "BR_INCREFS";
				node->has_weak_ref = 1;
				node->pending_weak_ref = 1;
				node->local_weak_refs++;
			} else if (strong && !node->has_strong_ref) {
				cmd = BR_ACQUIRE;
				cmd_name = "BR_ACQUIRE";
				node->has_strong_ref = 1;
				node->pending_strong_ref = 1;
				node->local_strong_refs++;
			} else if (!strong && node->has_strong_ref) {
				cmd = BR_RELEASE;
				cmd_name = "BR_RELEASE";
				node->has_strong_ref = 0;
			} else if (!weak && node->has_weak_ref) {
				cmd = BR_DECREFS;
				cmd_name = "BR_DECREFS";
				node->has_weak_ref = 0;
			}
			if (cmd != BR_NOOP) {
				if (put_user(cmd, (uint32_t __user *)ptr))
					return -EFAULT;
				ptr += sizeof(uint32_t);
				if (put_user(node->ptr, (void * __user *)ptr))
					return -EFAULT;
				ptr += sizeof(void *);
				if (put_user(node->cookie, (void * __user *)ptr))
					return -EFAULT;
				ptr += sizeof(void *);

				binder_stat_br(proc, thread, cmd);
				binder_debug(BINDER_DEBUG_USER_REFS,
					     "%d:%d %s %d u%p c%p\n",
					     proc->pid, thread->pid, cmd_name, node->debug_id, node->ptr, node->cookie);
			} else {
				list_del_init(&w->entry);
				if (!weak && !strong) {
					binder_debug(BINDER_DEBUG_INTERNAL_REFS,
						     "%d:%d node %d u%p c%p deleted\n",
						     proc->pid, thread->pid, node->debug_id,
						     node->ptr, node->cookie);
					rb_erase(&node->rb_node, &proc->nodes);
					kfree(node);
					binder_stats_deleted(BINDER_STAT_NODE);
				} else {
					binder_debug(BINDER_DEBUG_INTERNAL_REFS,
						     "%d:%d node %d u%p c%p state unchanged\n",
						     proc->pid, thread->pid, node->debug_id, node->ptr,
						     node->cookie);
				}
			}
		} break;
		case BINDER_WORK_DEAD_BINDER:
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death;
			uint32_t cmd;

			death = container_of(w, struct binder_ref_death, work);
			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION)
				cmd = BR_CLEAR_DEATH_NOTIFICATION_DONE;
			else
				cmd = BR_DEAD_BINDER;
			if (put_user(cmd, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (put_user(death->cookie, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			binder_stat_br(proc, thread, cmd);
			binder_debug(BINDER_DEBUG_DEATH_NOTIFICATION,
				     "%d:%d %s %p\n",
				      proc->pid, thread->pid,
				      cmd == BR_DEAD_BINDER ?
				      "BR_DEAD_BINDER" :
				      "BR_CLEAR_DEATH_NOTIFICATION_DONE",
				      death->cookie);

			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION) {
				list_del(&w->entry);
				kfree(death);
				binder_stats_deleted(BINDER_STAT_DEATH);
			} else
				list_move(&w->entry, &proc->delivered_death);
			if (cmd == BR_DEAD_BINDER)
				goto done; /* DEAD_BINDER notifications can cause transactions */
		} break;
		}

		if (!t)
			continue;

		BUG_ON(t->buffer == NULL);
		if (t->buffer->target_node) {
			struct binder_node *target_node = t->buffer->target_node;
			tr.target.ptr = target_node->ptr;
			tr.cookie =  target_node->cookie;
			t->saved_priority = task_nice(current);
			if (t->priority < target_node->min_priority &&
			    !(t->flags & TF_ONE_WAY))
				binder_set_nice(t->priority);
			else if (!(t->flags & TF_ONE_WAY) ||
				 t->saved_priority > target_node->min_priority)
				binder_set_nice(target_node->min_priority);
			cmd = BR_TRANSACTION;
		} else {
			tr.target.ptr = NULL;
			tr.cookie = NULL;
			cmd = BR_REPLY;
		}
		tr.code = t->code;
		tr.flags = t->flags;
		tr.sender_euid = from_kuid(current_user_ns(), t->sender_euid);

		if (t->from) {
			struct task_struct *sender = t->from->proc->tsk;
			tr.sender_pid = task_tgid_nr_ns(sender,
							task_active_pid_ns(current));
		} else {
			tr.sender_pid = 0;
		}

		tr.data_size = t->buffer->data_size;
		tr.offsets_size = t->buffer->offsets_size;
		tr.data.ptr.buffer = (void *)t->buffer->data +
					proc->user_buffer_offset;
		tr.data.ptr.offsets = tr.data.ptr.buffer +
					ALIGN(t->buffer->data_size,
					    sizeof(void *));

		if (put_user(cmd, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		if (copy_to_user(ptr, &tr, sizeof(tr)))
			return -EFAULT;
		ptr += sizeof(tr);

		trace_binder_transaction_received(t);
		binder_stat_br(proc, thread, cmd);
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d %s %d %d:%d, cmd %d size %zd-%zd ptr %p-%p\n",
			     proc->pid, thread->pid,
			     (cmd == BR_TRANSACTION) ? "BR_TRANSACTION" :
			     "BR_REPLY",
			     t->debug_id, t->from ? t->from->proc->pid : 0,
			     t->from ? t->from->pid : 0, cmd,
			     t->buffer->data_size, t->buffer->offsets_size,
			     tr.data.ptr.buffer, tr.data.ptr.offsets);

		list_del(&t->work.entry);
		t->buffer->allow_user_free = 1;
		if (cmd == BR_TRANSACTION && !(t->flags & TF_ONE_WAY)) {
			t->to_parent = thread->transaction_stack;
			t->to_thread = thread;
			thread->transaction_stack = t;
		} else {
			t->buffer->transaction = NULL;
			kfree(t);
			binder_stats_deleted(BINDER_STAT_TRANSACTION);
		}
		break;
	}

done:

	*consumed = ptr - buffer;
	if (proc->requested_threads + proc->ready_threads == 0 &&
	    proc->requested_threads_started < proc->max_threads &&
	    (thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
	     BINDER_LOOPER_STATE_ENTERED)) /* the user-space code fails to */
	     /*spawn a new thread if we leave this out */) {
		proc->requested_threads++;
		binder_debug(BINDER_DEBUG_THREADS,
			     "%d:%d BR_SPAWN_LOOPER\n",
			     proc->pid, thread->pid);
		if (put_user(BR_SPAWN_LOOPER, (uint32_t __user *)buffer))
			return -EFAULT;
		binder_stat_br(proc, thread, BR_SPAWN_LOOPER);
	}
	return 0;
}

static void binder_release_work(struct list_head *list)
{
	struct binder_work *w;
	while (!list_empty(list)) {
		w = list_first_entry(list, struct binder_work, entry);
		list_del_init(&w->entry);
		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			struct binder_transaction *t;

			t = container_of(w, struct binder_transaction, work);
			if (t->buffer->target_node &&
			    !(t->flags & TF_ONE_WAY)) {
				binder_send_failed_reply(t, BR_DEAD_REPLY);
			} else {
				binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
					"undelivered transaction %d\n",
					t->debug_id);
				t->buffer->transaction = NULL;
				kfree(t);
				binder_stats_deleted(BINDER_STAT_TRANSACTION);
			}
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {
			binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
				"undelivered TRANSACTION_COMPLETE\n");
			kfree(w);
			binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
		} break;
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death;

			death = container_of(w, struct binder_ref_death, work);
			binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
				"undelivered death notification, %p\n",
				death->cookie);
			kfree(death);
			binder_stats_deleted(BINDER_STAT_DEATH);
		} break;
		default:
			pr_err("unexpected work type, %d, not freed\n",
			       w->type);
			break;
		}
	}

}

static struct binder_thread *binder_get_thread(struct binder_proc *proc)
{
	struct binder_thread *thread = NULL;
	struct rb_node *parent = NULL;
	struct rb_node **p = &proc->threads.rb_node;

	while (*p) {
		parent = *p;
		thread = rb_entry(parent, struct binder_thread, rb_node);

		if (current->pid < thread->pid)
			p = &(*p)->rb_left;
		else if (current->pid > thread->pid)
			p = &(*p)->rb_right;
		else
			break;
	}
	if (*p == NULL) {
		thread = kzalloc(sizeof(*thread), GFP_KERNEL);
		if (thread == NULL)
			return NULL;
		binder_stats_created(BINDER_STAT_THREAD);
		thread->proc = proc;
		thread->pid = current->pid;
		init_waitqueue_head(&thread->wait);
		INIT_LIST_HEAD(&thread->todo);
		rb_link_node(&thread->rb_node, parent, p);
		rb_insert_color(&thread->rb_node, &proc->threads);
		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		thread->return_error = BR_OK;
		thread->return_error2 = BR_OK;
	}
	return thread;
}

static int binder_free_thread(struct binder_proc *proc,
			      struct binder_thread *thread)
{
	struct binder_transaction *t;
	struct binder_transaction *send_reply = NULL;
	int active_transactions = 0;

	rb_erase(&thread->rb_node, &proc->threads);
	t = thread->transaction_stack;
	if (t && t->to_thread == thread)
		send_reply = t;
	while (t) {
		active_transactions++;
		binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
			     "release %d:%d transaction %d %s, still active\n",
			      proc->pid, thread->pid,
			     t->debug_id,
			     (t->to_thread == thread) ? "in" : "out");

		if (t->to_thread == thread) {
			t->to_proc = NULL;
			t->to_thread = NULL;
			if (t->buffer) {
				t->buffer->transaction = NULL;
				t->buffer = NULL;
			}
			t = t->to_parent;
		} else if (t->from == thread) {
			t->from = NULL;
			t = t->from_parent;
		} else
			BUG();
	}
	if (send_reply)
		binder_send_failed_reply(send_reply, BR_DEAD_REPLY);
	binder_release_work(&thread->todo);
	kfree(thread);
	binder_stats_deleted(BINDER_STAT_THREAD);
	return active_transactions;
}

static unsigned int binder_poll(struct file *filp,
				struct poll_table_struct *wait)
{
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread = NULL;
	int wait_for_proc_work;

	binder_lock(__func__);

	thread = binder_get_thread(proc);

	wait_for_proc_work = thread->transaction_stack == NULL &&
		list_empty(&thread->todo) && thread->return_error == BR_OK;

	binder_unlock(__func__);

	if (wait_for_proc_work) {
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
		poll_wait(filp, &proc->wait, wait);
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
	} else {
		if (binder_has_thread_work(thread))
			return POLLIN;
		poll_wait(filp, &thread->wait, wait);
		if (binder_has_thread_work(thread))
			return POLLIN;
	}
	return 0;
}

static long binder_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread;
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;

	/*pr_info("binder_ioctl: %d:%d %x %lx\n", proc->pid, current->pid, cmd, arg);*/

	trace_binder_ioctl(cmd, arg);

	ret = wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret)
		goto err_unlocked;

	binder_lock(__func__);
	// 根据pid来查找thread对象，找不到就创建一个新的
	thread = binder_get_thread(proc);
	if (thread == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	switch (cmd) {
	case BINDER_WRITE_READ: {
		struct binder_write_read bwr;
		if (size != sizeof(struct binder_write_read)) {
			ret = -EINVAL;
			goto err;
		}
		if (copy_from_user(&bwr, ubuf, sizeof(bwr))) {
			ret = -EFAULT;
			goto err;
		}
		binder_debug(BINDER_DEBUG_READ_WRITE,
			     "%d:%d write %ld at %08lx, read %ld at %08lx\n",
			     proc->pid, thread->pid, bwr.write_size,
			     bwr.write_buffer, bwr.read_size, bwr.read_buffer);

		/*
		 * 数据是读是写，是由read_size和write_size来决定的。
		 * 读写是站着应用的角度来说的，即
		 * write表示应用向driver写数据
		 * read表示应用从driver读数据
		 */
		if (bwr.write_size > 0) {
			ret = binder_thread_write(proc, thread, (void __user *)bwr.write_buffer, bwr.write_size, &bwr.write_consumed);
			trace_binder_write_done(ret);
			if (ret < 0) {
				bwr.read_consumed = 0;
				if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
					ret = -EFAULT;
				goto err;
			}
		}
		if (bwr.read_size > 0) {
			ret = binder_thread_read(proc, thread, (void __user *)bwr.read_buffer, bwr.read_size, &bwr.read_consumed, filp->f_flags & O_NONBLOCK);
			trace_binder_read_done(ret);
			if (!list_empty(&proc->todo))
				wake_up_interruptible(&proc->wait);
			if (ret < 0) {
				if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
					ret = -EFAULT;
				goto err;
			}
		}
		binder_debug(BINDER_DEBUG_READ_WRITE,
			     "%d:%d wrote %ld of %ld, read return %ld of %ld\n",
			     proc->pid, thread->pid, bwr.write_consumed, bwr.write_size,
			     bwr.read_consumed, bwr.read_size);
		if (copy_to_user(ubuf, &bwr, sizeof(bwr))) {
			ret = -EFAULT;
			goto err;
		}
		break;
	}
	case BINDER_SET_MAX_THREADS:
		if (copy_from_user(&proc->max_threads, ubuf, sizeof(proc->max_threads))) {
			ret = -EINVAL;
			goto err;
		}
		break;
	case BINDER_SET_CONTEXT_MGR:
		if (binder_context_mgr_node != NULL) {
			pr_err("BINDER_SET_CONTEXT_MGR already set\n");
			ret = -EBUSY;
			goto err;
		}
		if (uid_valid(binder_context_mgr_uid)) {
			if (!uid_eq(binder_context_mgr_uid, current->cred->euid)) {
				pr_err("BINDER_SET_CONTEXT_MGR bad uid %d != %d\n",
				       from_kuid(&init_user_ns, current->cred->euid),
				       from_kuid(&init_user_ns, binder_context_mgr_uid));
				ret = -EPERM;
				goto err;
			}
		} else
			binder_context_mgr_uid = current->cred->euid;
		binder_context_mgr_node = binder_new_node(proc, NULL, NULL);
		if (binder_context_mgr_node == NULL) {
			ret = -ENOMEM;
			goto err;
		}
		binder_context_mgr_node->local_weak_refs++;
		binder_context_mgr_node->local_strong_refs++;
		binder_context_mgr_node->has_strong_ref = 1;
		binder_context_mgr_node->has_weak_ref = 1;
		break;
	case BINDER_THREAD_EXIT:
		binder_debug(BINDER_DEBUG_THREADS, "%d:%d exit\n",
			     proc->pid, thread->pid);
		binder_free_thread(proc, thread);
		thread = NULL;
		break;
	case BINDER_VERSION:
		if (size != sizeof(struct binder_version)) {
			ret = -EINVAL;
			goto err;
		}
		if (put_user(BINDER_CURRENT_PROTOCOL_VERSION, &((struct binder_version *)ubuf)->protocol_version)) {
			ret = -EINVAL;
			goto err;
		}
		break;
	default:
		ret = -EINVAL;
		goto err;
	}
	ret = 0;
err:
	if (thread)
		thread->looper &= ~BINDER_LOOPER_STATE_NEED_RETURN;
	binder_unlock(__func__);
	wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret && ret != -ERESTARTSYS)
		pr_info("%d:%d ioctl %x %lx returned %d\n", proc->pid, current->pid, cmd, arg, ret);
err_unlocked:
	trace_binder_ioctl_done(ret);
	return ret;
}

static void binder_vma_open(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;
	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%d open vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));
}

static void binder_vma_close(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;
	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%d close vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));
	proc->vma = NULL;
	proc->vma_vm_mm = NULL;
	binder_defer_work(proc, BINDER_DEFERRED_PUT_FILES);
}

static struct vm_operations_struct binder_vm_ops = {
	.open = binder_vma_open,
	.close = binder_vma_close,
};

static int binder_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	struct vm_struct *area;
	struct binder_proc *proc = filp->private_data;
	const char *failure_string;
	struct binder_buffer *buffer;

	if (proc->tsk != current)
		return -EINVAL;

	if ((vma->vm_end - vma->vm_start) > SZ_4M)
		vma->vm_end = vma->vm_start + SZ_4M;

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "binder_mmap: %d %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));

	if (vma->vm_flags & FORBIDDEN_MMAP_FLAGS) {
		ret = -EPERM;
		failure_string = "bad vm_flags";
		goto err_bad_arg;
	}
	vma->vm_flags = (vma->vm_flags | VM_DONTCOPY) & ~VM_MAYWRITE;

	mutex_lock(&binder_mmap_lock);
	if (proc->buffer) {
		ret = -EBUSY;
		failure_string = "already mapped";
		goto err_already_mapped;
	}

	area = get_vm_area(vma->vm_end - vma->vm_start, VM_IOREMAP);
	if (area == NULL) {
		ret = -ENOMEM;
		failure_string = "get_vm_area";
		goto err_get_vm_area_failed;
	}
	proc->buffer = area->addr;
	proc->user_buffer_offset = vma->vm_start - (uintptr_t)proc->buffer;
	mutex_unlock(&binder_mmap_lock);

#ifdef CONFIG_CPU_CACHE_VIPT
	if (cache_is_vipt_aliasing()) {
		while (CACHE_COLOUR((vma->vm_start ^ (uint32_t)proc->buffer))) {
			pr_info("binder_mmap: %d %lx-%lx maps %p bad alignment\n", proc->pid, vma->vm_start, vma->vm_end, proc->buffer);
			vma->vm_start += PAGE_SIZE;
		}
	}
#endif
	proc->pages = kzalloc(sizeof(proc->pages[0]) * ((vma->vm_end - vma->vm_start) / PAGE_SIZE), GFP_KERNEL);
	if (proc->pages == NULL) {
		ret = -ENOMEM;
		failure_string = "alloc page array";
		goto err_alloc_pages_failed;
	}
	proc->buffer_size = vma->vm_end - vma->vm_start;

	vma->vm_ops = &binder_vm_ops;
	vma->vm_private_data = proc;

	if (binder_update_page_range(proc, 1, proc->buffer, proc->buffer + PAGE_SIZE, vma)) {
		ret = -ENOMEM;
		failure_string = "alloc small buf";
		goto err_alloc_small_buf_failed;
	}
	buffer = proc->buffer;
	INIT_LIST_HEAD(&proc->buffers);
	list_add(&buffer->entry, &proc->buffers);
	buffer->free = 1;
	binder_insert_free_buffer(proc, buffer);
	proc->free_async_space = proc->buffer_size / 2;
	barrier();
	proc->files = get_files_struct(current);
	proc->vma = vma;
	proc->vma_vm_mm = vma->vm_mm;

	/*pr_info("binder_mmap: %d %lx-%lx maps %p\n",
		 proc->pid, vma->vm_start, vma->vm_end, proc->buffer);*/
	return 0;

err_alloc_small_buf_failed:
	kfree(proc->pages);
	proc->pages = NULL;
err_alloc_pages_failed:
	mutex_lock(&binder_mmap_lock);
	vfree(proc->buffer);
	proc->buffer = NULL;
err_get_vm_area_failed:
err_already_mapped:
	mutex_unlock(&binder_mmap_lock);
err_bad_arg:
	pr_err("binder_mmap: %d %lx-%lx %s failed %d\n",
	       proc->pid, vma->vm_start, vma->vm_end, failure_string, ret);
	return ret;
}

static int binder_open(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc;

	binder_debug(BINDER_DEBUG_OPEN_CLOSE, "binder_open: %d:%d\n",
		     current->group_leader->pid, current->pid);

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (proc == NULL)
		return -ENOMEM;
	get_task_struct(current);
	proc->tsk = current;
	INIT_LIST_HEAD(&proc->todo);
	init_waitqueue_head(&proc->wait);
	proc->default_priority = task_nice(current);

	binder_lock(__func__);

	binder_stats_created(BINDER_STAT_PROC);
	hlist_add_head(&proc->proc_node, &binder_procs);
	proc->pid = current->group_leader->pid;
	INIT_LIST_HEAD(&proc->delivered_death);
	filp->private_data = proc;

	binder_unlock(__func__);

	if (binder_debugfs_dir_entry_proc) {
		char strbuf[11];
		snprintf(strbuf, sizeof(strbuf), "%u", proc->pid);
		proc->debugfs_entry = debugfs_create_file(strbuf, S_IRUGO,
			binder_debugfs_dir_entry_proc, proc, &binder_proc_fops);
	}

	return 0;
}

static int binder_flush(struct file *filp, fl_owner_t id)
{
	struct binder_proc *proc = filp->private_data;

	binder_defer_work(proc, BINDER_DEFERRED_FLUSH);

	return 0;
}

static void binder_deferred_flush(struct binder_proc *proc)
{
	struct rb_node *n;
	int wake_count = 0;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n)) {
		struct binder_thread *thread = rb_entry(n, struct binder_thread, rb_node);
		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		if (thread->looper & BINDER_LOOPER_STATE_WAITING) {
			wake_up_interruptible(&thread->wait);
			wake_count++;
		}
	}
	wake_up_interruptible_all(&proc->wait);

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "binder_flush: %d woke %d threads\n", proc->pid,
		     wake_count);
}

static int binder_release(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc = filp->private_data;
	debugfs_remove(proc->debugfs_entry);
	binder_defer_work(proc, BINDER_DEFERRED_RELEASE);

	return 0;
}

static void binder_deferred_release(struct binder_proc *proc)
{
	struct hlist_node *pos;
	struct binder_transaction *t;
	struct rb_node *n;
	int threads, nodes, incoming_refs, outgoing_refs, buffers, active_transactions, page_count;

	BUG_ON(proc->vma);
	BUG_ON(proc->files);

	hlist_del(&proc->proc_node);
	if (binder_context_mgr_node && binder_context_mgr_node->proc == proc) {
		binder_debug(BINDER_DEBUG_DEAD_BINDER,
			     "binder_release: %d context_mgr_node gone\n",
			     proc->pid);
		binder_context_mgr_node = NULL;
	}

	threads = 0;
	active_transactions = 0;
	while ((n = rb_first(&proc->threads))) {
		struct binder_thread *thread = rb_entry(n, struct binder_thread, rb_node);
		threads++;
		active_transactions += binder_free_thread(proc, thread);
	}
	nodes = 0;
	incoming_refs = 0;
	while ((n = rb_first(&proc->nodes))) {
		struct binder_node *node = rb_entry(n, struct binder_node, rb_node);

		nodes++;
		rb_erase(&node->rb_node, &proc->nodes);
		list_del_init(&node->work.entry);
		binder_release_work(&node->async_todo);
		if (hlist_empty(&node->refs)) {
			kfree(node);
			binder_stats_deleted(BINDER_STAT_NODE);
		} else {
			struct binder_ref *ref;
			int death = 0;

			node->proc = NULL;
			node->local_strong_refs = 0;
			node->local_weak_refs = 0;
			hlist_add_head(&node->dead_node, &binder_dead_nodes);

			hlist_for_each_entry(ref, pos, &node->refs, node_entry) {
				incoming_refs++;
				if (ref->death) {
					death++;
					if (list_empty(&ref->death->work.entry)) {
						ref->death->work.type = BINDER_WORK_DEAD_BINDER;
						list_add_tail(&ref->death->work.entry, &ref->proc->todo);
						wake_up_interruptible(&ref->proc->wait);
					} else
						BUG();
				}
			}
			binder_debug(BINDER_DEBUG_DEAD_BINDER,
				     "node %d now dead, refs %d, death %d\n",
				      node->debug_id, incoming_refs, death);
		}
	}
	outgoing_refs = 0;
	while ((n = rb_first(&proc->refs_by_desc))) {
		struct binder_ref *ref = rb_entry(n, struct binder_ref,
						  rb_node_desc);
		outgoing_refs++;
		binder_delete_ref(ref);
	}
	binder_release_work(&proc->todo);
	binder_release_work(&proc->delivered_death);
	buffers = 0;

	while ((n = rb_first(&proc->allocated_buffers))) {
		struct binder_buffer *buffer = rb_entry(n, struct binder_buffer,
							rb_node);
		t = buffer->transaction;
		if (t) {
			t->buffer = NULL;
			buffer->transaction = NULL;
			pr_err("release proc %d, transaction %d, not freed\n",
			       proc->pid, t->debug_id);
			/*BUG();*/
		}
		binder_free_buf(proc, buffer);
		buffers++;
	}

	binder_stats_deleted(BINDER_STAT_PROC);

	page_count = 0;
	if (proc->pages) {
		int i;
		for (i = 0; i < proc->buffer_size / PAGE_SIZE; i++) {
			if (proc->pages[i]) {
				void *page_addr = proc->buffer + i * PAGE_SIZE;
				binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
					     "binder_release: %d: page %d at %p not freed\n",
					     proc->pid, i,
					     page_addr);
				unmap_kernel_range((unsigned long)page_addr,
					PAGE_SIZE);
				__free_page(proc->pages[i]);
				page_count++;
			}
		}
		kfree(proc->pages);
		vfree(proc->buffer);
	}

	put_task_struct(proc->tsk);

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "binder_release: %d threads %d, nodes %d (ref %d), refs %d, active transactions %d, buffers %d, pages %d\n",
		     proc->pid, threads, nodes, incoming_refs, outgoing_refs,
		     active_transactions, buffers, page_count);

	kfree(proc);
}

static void binder_deferred_func(struct work_struct *work)
{
	struct binder_proc *proc;
	struct files_struct *files;

	int defer;
	do {
		binder_lock(__func__);
		mutex_lock(&binder_deferred_lock);
		if (!hlist_empty(&binder_deferred_list)) {
			proc = hlist_entry(binder_deferred_list.first,
					struct binder_proc, deferred_work_node);
			hlist_del_init(&proc->deferred_work_node);
			defer = proc->deferred_work;
			proc->deferred_work = 0;
		} else {
			proc = NULL;
			defer = 0;
		}
		mutex_unlock(&binder_deferred_lock);

		files = NULL;
		if (defer & BINDER_DEFERRED_PUT_FILES) {
			files = proc->files;
			if (files)
				proc->files = NULL;
		}

		if (defer & BINDER_DEFERRED_FLUSH)
			binder_deferred_flush(proc);

		if (defer & BINDER_DEFERRED_RELEASE)
			binder_deferred_release(proc); /* frees proc */

		binder_unlock(__func__);
		if (files)
			put_files_struct(files);
	} while (proc);
}
static DECLARE_WORK(binder_deferred_work, binder_deferred_func);

static void
binder_defer_work(struct binder_proc *proc, enum binder_deferred_state defer)
{
	mutex_lock(&binder_deferred_lock);
	proc->deferred_work |= defer;
	if (hlist_unhashed(&proc->deferred_work_node)) {
		hlist_add_head(&proc->deferred_work_node,
				&binder_deferred_list);
		queue_work(binder_deferred_workqueue, &binder_deferred_work);
	}
	mutex_unlock(&binder_deferred_lock);
}

static void print_binder_transaction(struct seq_file *m, const char *prefix,
				     struct binder_transaction *t)
{
	seq_printf(m,
		   "%s %d: %p from %d:%d to %d:%d code %x flags %x pri %ld r%d",
		   prefix, t->debug_id, t,
		   t->from ? t->from->proc->pid : 0,
		   t->from ? t->from->pid : 0,
		   t->to_proc ? t->to_proc->pid : 0,
		   t->to_thread ? t->to_thread->pid : 0,
		   t->code, t->flags, t->priority, t->need_reply);
	if (t->buffer == NULL) {
		seq_puts(m, " buffer free\n");
		return;
	}
	if (t->buffer->target_node)
		seq_printf(m, " node %d",
			   t->buffer->target_node->debug_id);
	seq_printf(m, " size %zd:%zd data %p\n",
		   t->buffer->data_size, t->buffer->offsets_size,
		   t->buffer->data);
}

static void print_binder_buffer(struct seq_file *m, const char *prefix,
				struct binder_buffer *buffer)
{
	seq_printf(m, "%s %d: %p size %zd:%zd %s\n",
		   prefix, buffer->debug_id, buffer->data,
		   buffer->data_size, buffer->offsets_size,
		   buffer->transaction ? "active" : "delivered");
}

static void print_binder_work(struct seq_file *m, const char *prefix,
			      const char *transaction_prefix,
			      struct binder_work *w)
{
	struct binder_node *node;
	struct binder_transaction *t;

	switch (w->type) {
	case BINDER_WORK_TRANSACTION:
		t = container_of(w, struct binder_transaction, work);
		print_binder_transaction(m, transaction_prefix, t);
		break;
	case BINDER_WORK_TRANSACTION_COMPLETE:
		seq_printf(m, "%stransaction complete\n", prefix);
		break;
	case BINDER_WORK_NODE:
		node = container_of(w, struct binder_node, work);
		seq_printf(m, "%snode work %d: u%p c%p\n",
			   prefix, node->debug_id, node->ptr, node->cookie);
		break;
	case BINDER_WORK_DEAD_BINDER:
		seq_printf(m, "%shas dead binder\n", prefix);
		break;
	case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		seq_printf(m, "%shas cleared dead binder\n", prefix);
		break;
	case BINDER_WORK_CLEAR_DEATH_NOTIFICATION:
		seq_printf(m, "%shas cleared death notification\n", prefix);
		break;
	default:
		seq_printf(m, "%sunknown work: type %d\n", prefix, w->type);
		break;
	}
}

static void print_binder_thread(struct seq_file *m,
				struct binder_thread *thread,
				int print_always)
{
	struct binder_transaction *t;
	struct binder_work *w;
	size_t start_pos = m->count;
	size_t header_pos;

	seq_printf(m, "  thread %d: l %02x\n", thread->pid, thread->looper);
	header_pos = m->count;
	t = thread->transaction_stack;
	while (t) {
		if (t->from == thread) {
			print_binder_transaction(m,
						 "    outgoing transaction", t);
			t = t->from_parent;
		} else if (t->to_thread == thread) {
			print_binder_transaction(m,
						 "    incoming transaction", t);
			t = t->to_parent;
		} else {
			print_binder_transaction(m, "    bad transaction", t);
			t = NULL;
		}
	}
	list_for_each_entry(w, &thread->todo, entry) {
		print_binder_work(m, "    ", "    pending transaction", w);
	}
	if (!print_always && m->count == header_pos)
		m->count = start_pos;
}

static void print_binder_node(struct seq_file *m, struct binder_node *node)
{
	struct binder_ref *ref;
	struct hlist_node *pos;
	struct binder_work *w;
	int count;

	count = 0;
	hlist_for_each_entry(ref, pos, &node->refs, node_entry)
		count++;

	seq_printf(m, "  node %d: u%p c%p hs %d hw %d ls %d lw %d is %d iw %d",
		   node->debug_id, node->ptr, node->cookie,
		   node->has_strong_ref, node->has_weak_ref,
		   node->local_strong_refs, node->local_weak_refs,
		   node->internal_strong_refs, count);
	if (count) {
		seq_puts(m, " proc");
		hlist_for_each_entry(ref, pos, &node->refs, node_entry)
			seq_printf(m, " %d", ref->proc->pid);
	}
	seq_puts(m, "\n");
	list_for_each_entry(w, &node->async_todo, entry)
		print_binder_work(m, "    ",
				  "    pending async transaction", w);
}

static void print_binder_ref(struct seq_file *m, struct binder_ref *ref)
{
	seq_printf(m, "  ref %d: desc %d %snode %d s %d w %d d %p\n",
		   ref->debug_id, ref->desc, ref->node->proc ? "" : "dead ",
		   ref->node->debug_id, ref->strong, ref->weak, ref->death);
}

static void print_binder_proc(struct seq_file *m,
			      struct binder_proc *proc, int print_all)
{
	struct binder_work *w;
	struct rb_node *n;
	size_t start_pos = m->count;
	size_t header_pos;

	seq_printf(m, "proc %d\n", proc->pid);
	header_pos = m->count;

	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n))
		print_binder_thread(m, rb_entry(n, struct binder_thread,
						rb_node), print_all);
	for (n = rb_first(&proc->nodes); n != NULL; n = rb_next(n)) {
		struct binder_node *node = rb_entry(n, struct binder_node,
						    rb_node);
		if (print_all || node->has_async_transaction)
			print_binder_node(m, node);
	}
	if (print_all) {
		for (n = rb_first(&proc->refs_by_desc);
		     n != NULL;
		     n = rb_next(n))
			print_binder_ref(m, rb_entry(n, struct binder_ref,
						     rb_node_desc));
	}
	for (n = rb_first(&proc->allocated_buffers); n != NULL; n = rb_next(n))
		print_binder_buffer(m, "  buffer",
				    rb_entry(n, struct binder_buffer, rb_node));
	list_for_each_entry(w, &proc->todo, entry)
		print_binder_work(m, "  ", "  pending transaction", w);
	list_for_each_entry(w, &proc->delivered_death, entry) {
		seq_puts(m, "  has delivered dead binder\n");
		break;
	}
	if (!print_all && m->count == header_pos)
		m->count = start_pos;
}

static const char *binder_return_strings[] = {
	"BR_ERROR",
	"BR_OK",
	"BR_TRANSACTION",
	"BR_REPLY",
	"BR_ACQUIRE_RESULT",
	"BR_DEAD_REPLY",
	"BR_TRANSACTION_COMPLETE",
	"BR_INCREFS",
	"BR_ACQUIRE",
	"BR_RELEASE",
	"BR_DECREFS",
	"BR_ATTEMPT_ACQUIRE",
	"BR_NOOP",
	"BR_SPAWN_LOOPER",
	"BR_FINISHED",
	"BR_DEAD_BINDER",
	"BR_CLEAR_DEATH_NOTIFICATION_DONE",
	"BR_FAILED_REPLY"
};

static const char *binder_command_strings[] = {
	"BC_TRANSACTION",
	"BC_REPLY",
	"BC_ACQUIRE_RESULT",
	"BC_FREE_BUFFER",
	"BC_INCREFS",
	"BC_ACQUIRE",
	"BC_RELEASE",
	"BC_DECREFS",
	"BC_INCREFS_DONE",
	"BC_ACQUIRE_DONE",
	"BC_ATTEMPT_ACQUIRE",
	"BC_REGISTER_LOOPER",
	"BC_ENTER_LOOPER",
	"BC_EXIT_LOOPER",
	"BC_REQUEST_DEATH_NOTIFICATION",
	"BC_CLEAR_DEATH_NOTIFICATION",
	"BC_DEAD_BINDER_DONE"
};

static const char *binder_objstat_strings[] = {
	"proc",
	"thread",
	"node",
	"ref",
	"death",
	"transaction",
	"transaction_complete"
};

static void print_binder_stats(struct seq_file *m, const char *prefix,
			       struct binder_stats *stats)
{
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(stats->bc) !=
		     ARRAY_SIZE(binder_command_strings));
	for (i = 0; i < ARRAY_SIZE(stats->bc); i++) {
		if (stats->bc[i])
			seq_printf(m, "%s%s: %d\n", prefix,
				   binder_command_strings[i], stats->bc[i]);
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->br) !=
		     ARRAY_SIZE(binder_return_strings));
	for (i = 0; i < ARRAY_SIZE(stats->br); i++) {
		if (stats->br[i])
			seq_printf(m, "%s%s: %d\n", prefix,
				   binder_return_strings[i], stats->br[i]);
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) !=
		     ARRAY_SIZE(binder_objstat_strings));
	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) !=
		     ARRAY_SIZE(stats->obj_deleted));
	for (i = 0; i < ARRAY_SIZE(stats->obj_created); i++) {
		if (stats->obj_created[i] || stats->obj_deleted[i])
			seq_printf(m, "%s%s: active %d total %d\n", prefix,
				binder_objstat_strings[i],
				stats->obj_created[i] - stats->obj_deleted[i],
				stats->obj_created[i]);
	}
}

static void print_binder_proc_stats(struct seq_file *m,
				    struct binder_proc *proc)
{
	struct binder_work *w;
	struct rb_node *n;
	int count, strong, weak;

	seq_printf(m, "proc %d\n", proc->pid);
	count = 0;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  threads: %d\n", count);
	seq_printf(m, "  requested threads: %d+%d/%d\n"
			"  ready threads %d\n"
			"  free async space %zd\n", proc->requested_threads,
			proc->requested_threads_started, proc->max_threads,
			proc->ready_threads, proc->free_async_space);
	count = 0;
	for (n = rb_first(&proc->nodes); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  nodes: %d\n", count);
	count = 0;
	strong = 0;
	weak = 0;
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
		struct binder_ref *ref = rb_entry(n, struct binder_ref,
						  rb_node_desc);
		count++;
		strong += ref->strong;
		weak += ref->weak;
	}
	seq_printf(m, "  refs: %d s %d w %d\n", count, strong, weak);

	count = 0;
	for (n = rb_first(&proc->allocated_buffers); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  buffers: %d\n", count);

	count = 0;
	list_for_each_entry(w, &proc->todo, entry) {
		switch (w->type) {
		case BINDER_WORK_TRANSACTION:
			count++;
			break;
		default:
			break;
		}
	}
	seq_printf(m, "  pending transactions: %d\n", count);

	print_binder_stats(m, "  ", &proc->stats);
}


static int binder_state_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	struct binder_node *node;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder state:\n");

	if (!hlist_empty(&binder_dead_nodes))
		seq_puts(m, "dead nodes:\n");
	hlist_for_each_entry(node, pos, &binder_dead_nodes, dead_node)
		print_binder_node(m, node);

	hlist_for_each_entry(proc, pos, &binder_procs, proc_node)
		print_binder_proc(m, proc, 1);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_stats_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder stats:\n");

	print_binder_stats(m, "", &binder_stats);

	hlist_for_each_entry(proc, pos, &binder_procs, proc_node)
		print_binder_proc_stats(m, proc);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_transactions_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder transactions:\n");
	hlist_for_each_entry(proc, pos, &binder_procs, proc_node)
		print_binder_proc(m, proc, 0);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_proc_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc = m->private;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);
	seq_puts(m, "binder proc state:\n");
	print_binder_proc(m, proc, 1);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static void print_binder_transaction_log_entry(struct seq_file *m,
					struct binder_transaction_log_entry *e)
{
	seq_printf(m,
		   "%d: %s from %d:%d to %d:%d node %d handle %d size %d:%d\n",
		   e->debug_id, (e->call_type == 2) ? "reply" :
		   ((e->call_type == 1) ? "async" : "call "), e->from_proc,
		   e->from_thread, e->to_proc, e->to_thread, e->to_node,
		   e->target_handle, e->data_size, e->offsets_size);
}

static int binder_transaction_log_show(struct seq_file *m, void *unused)
{
	struct binder_transaction_log *log = m->private;
	int i;

	if (log->full) {
		for (i = log->next; i < ARRAY_SIZE(log->entry); i++)
			print_binder_transaction_log_entry(m, &log->entry[i]);
	}
	for (i = 0; i < log->next; i++)
		print_binder_transaction_log_entry(m, &log->entry[i]);
	return 0;
}

static const struct file_operations binder_fops = {
	.owner = THIS_MODULE,
	.poll = binder_poll,
	.unlocked_ioctl = binder_ioctl,
	.mmap = binder_mmap,
	.open = binder_open,
	.flush = binder_flush,
	.release = binder_release,
};

static struct miscdevice binder_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "binder",
	.fops = &binder_fops
};

BINDER_DEBUG_ENTRY(state);
BINDER_DEBUG_ENTRY(stats);
BINDER_DEBUG_ENTRY(transactions);
BINDER_DEBUG_ENTRY(transaction_log);

static int __init binder_init(void)
{
	int ret;

	binder_deferred_workqueue = create_singlethread_workqueue("binder");
	if (!binder_deferred_workqueue)
		return -ENOMEM;

	binder_debugfs_dir_entry_root = debugfs_create_dir("binder", NULL);
	if (binder_debugfs_dir_entry_root)
		binder_debugfs_dir_entry_proc = debugfs_create_dir("proc",
						 binder_debugfs_dir_entry_root);
	ret = misc_register(&binder_miscdev);
	if (binder_debugfs_dir_entry_root) {
		debugfs_create_file("state",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_state_fops);
		debugfs_create_file("stats",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_stats_fops);
		debugfs_create_file("transactions",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_transactions_fops);
		debugfs_create_file("transaction_log",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    &binder_transaction_log,
				    &binder_transaction_log_fops);
		debugfs_create_file("failed_transaction_log",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    &binder_transaction_log_failed,
				    &binder_transaction_log_fops);
	}
	return ret;
}

device_initcall(binder_init);

#define CREATE_TRACE_POINTS
#include "binder_trace.h"

MODULE_LICENSE("GPL v2");
