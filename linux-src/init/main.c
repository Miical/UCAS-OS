/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork) // ! 为什么inline
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

/*
进程
调度
内存
块设备
字符设备
*/

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
 	ROOT_DEV = ORIG_ROOT_DEV;
 	drive_info = DRIVE_INFO;
	// 内存大小
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;

	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024)
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;

	// 初始化内存虚拟盘
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif


	/* 初始化内存
	 * 里面初始化了 mem_map，记录每个物理页被不同进程引用的次数
	 */
	mem_init(main_memory_start,memory_end);

	/* 初始化中断
	 * 设置中断向量表（IDT）, 把中断号和中断处理函数关联起来
	 * 里面调用了多个 set_trap_gate, 这是一个宏，最终完成的事情就是做一个IDT表项，放到IDT表里
	 */
	trap_init();

	/* 块设备初始化
	 * 里面初始化了 request 数组。将 .dev 置为 -1，将 .next 置为 NULL
	 */
	blk_dev_init();

	chr_dev_init();
	// 字符设备
	tty_init();
	// 时间初始化
	time_init();

	/* 初始化调度
	 * 1. 设置第一个进程在GDT中的结构。一个进程占两个段描述符，分别是LDT和TSS
	 * 里面将 init_task.tss 和 .ldt 设置到了 GDT 表中。设置的时候通过 set_tss_desc、set_ldt_desc 设置
	 *   - 设计了 init_task，union init_task 一共有 4kb，task_struct 占前面 956B，其余是内核栈
	 * 2. 然后初始化64个task数组
	 * 3. 然后让cpu的tr寄存器指向当前任务的tss(tss0)，当ldtr寄存器指向当前任务的ldt(ldt0)
	 * 4. 设置时钟中断门0x20（调度），和系统调用 0x80
	 */
	sched_init();

	/* 普通文件块设备的buffer初始化
	 * 1. 里面根据缓冲区的大小，建立缓冲区的环形链表
	 *   缓冲区从头开始创建buffer_head结构，从尾部开始往前创建数据块。然后将数据块和buffer_head结构关联起来
	 *   然后buffer_head串成一个环链
	 * 2. 接着初始化了hash表，让表中的指针都为NULL
	 */
	buffer_init(buffer_memory_end);

	// 硬盘初始化 设置了hd_interrupt的中断门
	hd_init();
	// 软盘初始化
	floppy_init();

	// 开中断
	sti();


	/* 往下就变成进程0的代码了，特权级变成了3
	 * 做好中断返回的参数，然后iret，跳转到下面进程0的代码
	 */
	move_to_user_mode();

	/* fork 来自宏_syscal0(int, fork)，这个东西直接调用int 80, 传入fork的系统调用号
	 * 然后通过中断门来到 _system_call，由_system_call来处理系统调用（转到_system_call）
	 * _system_call 首先检测合法性，然后压入若干参数、设置内核数据段，接着调用根据系统调用号调用 _fork
	 * _fork中首先调用 find_empty_process 找到一个空的task_struct
	 *     - last_pid 记录一个累计的pid号
	 *     - 找到一个为空的task_struct然后返回下标
	 * 接着调用 copy_process
	 *     - 参数：参数很重要：五个中断压入的值，6个手动压入的寄存器，1个返回值，5个手动压入的值
	 *     - get_free_page: 首先, get_free_page() 获取一页空闲的物理页，从高往低去找物理页。找到以后修改引用计数。
	 *     - task_struct: 然后把让task数组中的项指向这个页代表的task_struct，并将当前进程的task_struct复制给新进程
	 *     - 个性化设置: UNINTERUPTIBLE中断也不可以解除
	 *     - 制作进程断面tss： eip设置为参数中的eip，保证唤醒时同时运行代码，eax设计返回值为0。栈也是用的一个栈
	 *     - 调用 [copy_mem]: 现在当前进程还没有页表和内存，得给他分配
	 * 	       - 获取当前进程的数据段和代码段的基址和限长。
	 *         - 设置新的数据段和代码段基址同时为 64 个块中对应task数组的那个，每个块64M，线性地址空间一共4G
	 *         - 然后把新的代码段和数据段基址放到ldt里
	 *         - 然后调用 [copy_page_tables] 复制页表
	 *             - 拷贝页表和页目录表（对应64M线性地址块的）
	 *             - 满足页表的管理开始地址需要4M对齐，一个页表管理4M，一个进程需要16个页表，内核只有16M
	 *             - 两层循环，外面的循环遍历页目录表表项；通过get_free_page()获得一个空页用于放新页表；
	 *             - 里面的循环遍历页表表项，将页表表项直接拷贝过来，然后两边都设置为只读，并且增加对应物理页的引用计数
	 *         - 设置和文件相关的信息，将tss和ldt描述符放到GDT表中，设置进程状态为TASK_RUNNING
	 * 进程0返回值大于1，不运行 if
	 * 进程1的eip与进程0现在的一样，并且返回值通过eax获取，eax为0，开始运行init。
	 */
	if (!fork()) {		/* we count on this going ok */
		// 进程1开始执行
		init();
		/* 进程1开始init
		 * 调用[sys_setup]，设置设备
		 *     - 里面有一个 [bread(设备号，块号)]
		 *         - 它读设备的某个块到缓冲区，然后返回buffer_head。注意buffer_head的结构，里面存了设备号、块号、b_count(被进程的引用计数)、dirty、lock、bwait(在等待的进程)、环链和哈希表链表指针
		 *         - 调用 [getblk(设备号，块号)] 去获取对应 buffer_head
		 * 	           - 调用 [get_hash_table(设备号，块号)] 去找哈希表上有没有已经做好的这个块的bufffer_head
		 *                 - 调用 [find_buffer(设备号，块号)] 去找到对应哈希表的一行，然后遍历这一行，找到对应的buffer_head
		 *                 - 如果没找到返回 NULL
		 *                 - 如果找到了，...?
		 *             - 如果哈希表上有就直接返回
		 *             - 否则遍历环链表，找到一个空的并且最优的buffer_head（尽量不脏不锁）
		 *             - 如果都不为空，则sleep_on(buffer_wait)，然后repeat
		 *             - 然后调用 [wait_on_buffer(找到的buffer_head)]
		 *                 - 里面 while(bh->block) [sleep_on(bwait)]
		 *                     - ...
		 *             - 这里有可能被别人占上了，再判断一次b_count
		 *             - 处理脏的情况 ...
		 *             - 再检查一遍哈希表上有没有
		 *             - 填充上设备好和块号，从原有哈希表移出，放到新的哈希表上
		 *             - 返回 buffer_head
		 *         - 如果 b_uptodate 为真，说明里面有数据了，直接返回就可以
		 *         - 否则调用 [ll_rw_block(READ, buffer_head)] 读取数据
		 *             - 检查blk_dev上buffer_head.dev对应的设备是否有request_fn，如果没有就返回
		 *             - 然后调用 [make_request(设备，读写，buffer_head)] 来制作请求项
		 *                 -     > 注意请求项的结构 dev, cmd(read/write), sector(起始扇区), nr_sectors(扇区数), buffer(数据缓冲区), waiting(等待的进程), bh(对应的buffer_head), next(下一个请求项)
		 *                 -     > 每个设备有一个请求项的链表
		 *                 - 给缓冲区加锁 lock_buffer
		 *                 - 判断缓冲区如果不需要读写，直接返回
		 *                 - 寻找到一个空的请求项(req->dev < 0)，读从最后开始找，写从2/3处开始找
		 *                 - 如果没有找到就等待有请求项
		 *                 - 制作一个新的请求项
		 *                 - 调用 [add_request] 添加到请求队列
		 *                     - 每个设备都有一个请求队列，如果该设备请求队列里面没东西，直接挂上, 然后调用 request_fn
		 *                         - 对硬盘来说就是调用了 do_hd_request
		 *                         - 如果是写；通过hd_out向硬盘发命令并把write_intr挂上去
		 *                         - 如果是读；通过[hd_out]向硬盘发命令并把read_intr挂上去
		 *                             - 把回调挂到 do_hd这个全局变量上
		 *                             - 通过outb_p给硬盘下命令
		 *                     - 如果有东西，找一个合适的位置放进去 （这里不用调用request_fn，只有队列头去启动）
		 *         - 调用[wait_on_buffer] 等待读取完毕
		 *             -  while(bh->block) [sleep_on(bh->bwait)] 里面需要把当前进程挂到 bh->bwait上，说明当前线程正在等待这个buffer
		 *                 - 把 b_wait原来的东西挂到现在进程的局部变量 tmp 上
		 *                 - 把当前进程挂到 b_wait上
		 *                 - 设置为UNINTERRUPTIBLE，然后调度走当前进程, schedule
		 *                     - 第一次bread时，调度走之后进程0开始一直循环
		 *                 - 回来的时候，把tmp指向的进程激活
		 *     - 设置根文件系统 mount_root
		 */
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	/* 把当前进程置为 UNINTERRUPTIBLE，然后调用 [schedule] 进行调度
	 *     - 处理信号相关
	 *     - 挑一个时间片最长的并且就绪的进程next，如果找不到则next=0
	 *     - 如果找到了，更新时间片 （第一次进程0运行到这,next=1）
	 *     - 调用 [switch_to(next)] 切换到next进程
	 *         - 如果切换到的进程是当前线程，直接往下运行
	 *         - 否则通过 ljmp 切换进程，ljmp动用了任务切换机制，切换了特权级
	 * 然后ljmp到进程1的fork里面
	 * 进程0在这里不停的调度, pause->schedule->switch_to
	 */
	for(;;) pause();
}

/* 硬盘中断回来之后，进入 hd_interrupt
 * 在 hd_interrupt 中，调用之前挂在 do_hd 上的钩子 write_intr 或者 read_intr
 * 以 read_intr 为例，中断时直接调用
 *     - 把硬盘数据读到缓存区里
 *     - 然后移动当前请求项的buffer指针，以及扇区号到下一个。
 * 	   - 如果当前请求项还没读完，再把do_hd挂上read_intr，等待下一次中断
 *     - 如果读完了，调用 [end_request]
 *         - 缓冲块设置为 uptodate，并解锁 buffer
 *         - 然后调用 [wake_up] 唤醒等待缓冲块的进程
 *             - 把 .state 设置为 TASK_RUNNING
 *         - 然后调用 wake_up(wait_for_request) 唤醒等待空闲请求项的进程
 *         - 请求项置为 -1
 *     - 然后继续调用 do_hd_request 进行下一个请求
 */

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
