#include <net/netlink.h>
#include <net/net_namespace.h>
#include <asm/atomic.h>
#include <linux/semaphore.h>

#define NETLINK_SAFE 30

static struct sock * socket;
static int pid = 0;
static int ino_len = sizeof(unsigned long);
static atomic_t sequence = ATOMIC_INIT(0);

static struct queue
{
	uid_t data[65536];
	struct semaphore sem[65536];
} rspbuf;

DEFINE_RATELIMIT_STATE(rs, 3 * HZ, 1);

/*
** Send inode number to user space daemon process via netlink, and wait for response (uid).
** Note we maintain atomic sequence number to synchronize netlink with response request,
** and use semaphore to synchronize buffer queue read operation with write operation.
** The above plus a large enough buffer queue will avoid race conditions.
*/
static uid_t get_owner(unsigned long inode)
{
	struct sk_buff * skb;
	struct nlmsghdr * nlh;
	unsigned short seq;

	/*
	** If user space daemon process is not ready.
	*/
	if (! pid)
	{
		return 0;
	}
	skb = nlmsg_new(ino_len, GFP_ATOMIC);
	if (! skb)
	{
		return 0;
	}
	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, ino_len, 0);
	seq = atomic_inc_return(& sequence);
	nlh -> nlmsg_seq = seq;
	* (unsigned long *)NLMSG_DATA(nlh) = inode;
	nlmsg_unicast(socket, skb, pid);
	/*
	** Wait for at most 3s. Tested on Linux with 250 HZ timer interrupt frequency.
	*/
	if (down_timeout(& rspbuf.sem[seq], 3 * HZ))
	{
		if (__ratelimit(& rs))
		{
			pid = 0;
			printk(KERN_NOTICE "[safe] Safe terminated!\n");
		}
		return 0;
	}

	return rspbuf.data[seq];
}

/*
** If daemon process is ready, this will receive owner uid;
** Otherwise this will receive a ready signal.
*/
static void nl_receive_callback(struct sk_buff * skb)
{
	struct nlmsghdr * nlh = (struct nlmsghdr *)skb -> data;

	if (* (unsigned long *)NLMSG_DATA(nlh) >> 32 != 0xffffffff)
	{
		rspbuf.data[nlh -> nlmsg_seq] = * (uid_t *)NLMSG_DATA(nlh);
		up(& rspbuf.sem[nlh -> nlmsg_seq]);
	}
	else
	{
		if (NETLINK_CREDS(skb) -> pid == nlh -> nlmsg_pid && ! NETLINK_CREDS(skb) -> uid.val)
		{
			printk(KERN_NOTICE "[safe] Safe initiated!\n");
			pid = nlh -> nlmsg_pid;
		}
	}
}

static int __init netlink_init(void)
{
	struct netlink_kernel_cfg cfg =
	{
		.input = nl_receive_callback,
	};
	int i;

	socket = netlink_kernel_create(& init_net, NETLINK_SAFE, & cfg);
	for (i = 0; i < 65536; ++ i)
	{
		rspbuf.data[i] = 0;
		sema_init(& rspbuf.sem[i], 0);
	}
	ratelimit_set_flags(& rs, RATELIMIT_MSG_ON_RELEASE);

	return 0;
}

static void __exit netlink_exit(void)
{
	if (socket)
	{
		netlink_kernel_release(socket);
	}
}
