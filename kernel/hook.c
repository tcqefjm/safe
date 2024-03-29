#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/file.h>
#include <linux/dirent.h>
#include "netlink.c"
#include "crypto.c"

MODULE_LICENSE("GPL");

typedef void (* sys_call_ptr_t)(void);
typedef asmlinkage ssize_t (* old_syscall_t)(struct pt_regs * regs);

old_syscall_t old_read = NULL;
old_syscall_t old_write = NULL;
old_syscall_t old_execve = NULL;
old_syscall_t old_rename = NULL;
old_syscall_t old_unlink = NULL;
old_syscall_t old_unlinkat = NULL;
old_syscall_t old_getdents64 = NULL;
old_syscall_t old_openat = NULL;

sys_call_ptr_t * sys_call_table = NULL;
pte_t * pte = NULL;
unsigned int level = 0;

/*
** The following two functions get inode number from fd or from filename.
** Note we intensionally exclude character device file and block device file from
** further privilege check, so the safe won't degrade system performance.
*/
static unsigned long get_ino_from_fd(unsigned int fd)
{
	struct fd f = fdget(fd);
	umode_t mode = 0;
	unsigned long ino = 0;

	if (! IS_ERR(f.file))
	{
		mode = f.file -> f_inode -> i_mode;
		if (! S_ISCHR(mode) && ! S_ISBLK(mode))
		{
			ino = f.file -> f_inode -> i_ino;
		}
		fdput(f);
	}

	return ino;
}

static unsigned long get_ino_from_name(int dfd, const char * filename)
{
	struct kstat stat;
	umode_t mode = 0;
	unsigned long ino = 0;
	int error = vfs_statx(dfd, filename, AT_NO_AUTOMOUNT, & stat, STATX_BASIC_STATS);

	if (! error)
	{
		mode = stat.mode;
		if (! S_ISCHR(mode) && ! S_ISBLK(mode))
		{
			ino = stat.ino;
		}
	}

	return ino;
}

/*
** Check privilege for hooked read, write, execve, getdents64 syscall.
** Privilege 2 indicates file is not in safe, or the request is from root,
** in which case original syscall will be executed;
** Privilege 1 indicates file is in safe, and the request is from owner,
** in which case original syscall along with patch (excrypt, decrypt...) will be executed;
** Privilege 0 indicates file is in safe, and the request is not from owner,
** in which case syscall will be refused to execute.
** Note the first 10 reserved inodes are excluded from privilege check.
*/
static unsigned char check_privilege(unsigned long ino, uid_t uid)
{
	uid_t owner = 0;
	unsigned char privilege = 2;

	if (ino > 10 && uid)
	{
		owner = get_owner(ino);
	}
	if (owner)
	{
		privilege = (owner == uid) ? 1 : 0;
	}

	return privilege;
}

/*
** Check protection for hooked unlink, unlinkat syscall.
** Privilege 1 indicates file is not in safe, in which case original syscall will be executed;
** Privilege 0 indicates file is in safe, in which case syscall will be refused to execute.
** Note the first 10 reserved inodes are excluded from protection check.
*/
static unsigned char check_protection(unsigned long ino)
{
	uid_t owner = 0;

	if (ino > 10)
	{
		owner = get_owner(ino);
	}

	return (owner == 0);
}

static loff_t get_pos_from_fd(unsigned int fd, unsigned char op)
{
	loff_t pos = 0;
	struct fd f = fdget(fd);

	if (f.file)
	{
		if (op && (f.file -> f_flags & O_APPEND))
		{
			struct kstat stat;
			vfs_fstat(fd, & stat);
			pos = stat.size;
		}
		else
		{
			pos = f.file -> f_pos;
		}
		fdput(f);
	}

	return pos;
}

/*
** The following functions are hooked syscalls, which check file privilege or
** protection for specific user, and execute corresponding operation.
**
** Note Linux follows System V AMD64 ABI calling convention, so:
** rdi				|rsi				|rdx				|r10
** first parameter	|second parameter	|third parameter	|fourth parameter
*/
/*
** ssize_t read(unsigned int fd, char * buf, size_t count);
*/
asmlinkage ssize_t hooked_read(struct pt_regs * regs)
{
	unsigned long ino;
	uid_t uid;
	ssize_t ret = -1;
	loff_t pos = 0;

	ino = get_ino_from_fd(regs -> di);
	uid = current_euid().val;
	switch (check_privilege(ino, uid))
	{
		case 2:
			ret = old_read(regs);
			break;
		case 1:
			pos = get_pos_from_fd(regs -> di, 0);
			ret = old_read(regs);
			transform((char *)regs -> si, ino, pos, ret);
			break;
		case 0:
			;
	}

	return ret;
}

/*
** ssize_t write(unsigned int fd, const char * buf, size_t count);
*/
asmlinkage ssize_t hooked_write(struct pt_regs * regs)
{
	unsigned long ino;
	uid_t uid;
	ssize_t ret = -1;
	loff_t pos = 0;

	ino = get_ino_from_fd(regs -> di);
	uid = current_euid().val;
	switch (check_privilege(ino, uid))
	{
		case 2:
			ret = old_write(regs);
			break;
		case 1:
			pos = get_pos_from_fd(regs -> di, 1);
			transform((char *)regs -> si, ino, pos, regs -> dx);
			ret = old_write(regs);
			break;
		case 0:
			;
	}

	return ret;
}

/*
** ssize_t execve(const char * filename, const char * const argv[], const char * const envp[]);
*/
asmlinkage ssize_t hooked_execve(struct pt_regs * regs)
{
	unsigned long ino;
	uid_t uid;
	ssize_t ret = -1;

	ino = get_ino_from_name(AT_FDCWD, (char *)regs -> di);
	uid = current_euid().val;
	switch (check_privilege(ino, uid))
	{
		case 2:
		case 1:
			ret = old_execve(regs);
			break;
		case 0:
			;
	}

	return ret;
}

/*
** ssize_t rename(const char * oldname, const char * newname);
*/
asmlinkage ssize_t hooked_rename(struct pt_regs * regs)
{
	unsigned long oldino, newino;
	uid_t uid;
	ssize_t ret = -1;

	oldino = get_ino_from_name(AT_FDCWD, (char *)regs -> di);
	newino = get_ino_from_name(AT_FDCWD, (char *)regs -> si);
	uid = current_euid().val;
	if (check_privilege(oldino, uid) && check_protection(newino))
	{
		ret = old_rename(regs);
	}

	return ret;
}

/*
** ssize_t unlink(const char * pathname);
*/
asmlinkage ssize_t hooked_unlink(struct pt_regs * regs)
{
	unsigned long ino;
	ssize_t ret = -1;

	ino = get_ino_from_name(AT_FDCWD, (char *)regs -> di);
	if (check_protection(ino))
	{
		ret = old_unlink(regs);
	}

	return ret;
}

/*
** ssize_t unlinkat(int dfd, const char * pathname, int flag);
*/
asmlinkage ssize_t hooked_unlinkat(struct pt_regs * regs)
{
	unsigned long ino;
	ssize_t ret = -1;

	ino = get_ino_from_name(regs -> di, (char *)regs -> si);
	if (check_protection(ino))
	{
		ret = old_unlinkat(regs);
	}

	return ret;
}

/*
** ssize_t getdents64(unsigned int fd, struct linux_dirent64 * dirent, unsigned int count);
*/
asmlinkage ssize_t hooked_getdents64(struct pt_regs * regs)
{
	uid_t uid;
	ssize_t ret = -1;
	unsigned long bpos;
	struct linux_dirent64 * d;

	uid = current_euid().val;
	ret = old_getdents64(regs);
	/*
	** Iterate over linux_dirent64 structs to hide unprivileged files.
	*/
	/*
	** There is a weird bug in this implentation, which I'll review later.
	*/
	/* for (bpos = 0; bpos < ret; )
	{
		if (! check_privilege(d -> d_ino, uid))
		{
			ret -= d -> d_reclen;
			memcpy(d, (void *)d + d -> d_reclen, ret - bpos);
		}
		else
		{
			bpos += d -> d_reclen;
		}
	} */
	for (bpos = 0; bpos < ret; bpos += d -> d_reclen)
	{
		d = (struct linux_dirent64 *)(regs -> si + bpos);
		if (! check_privilege(d -> d_ino, uid))
		{
			d -> d_ino = 0;
			memset(d -> d_name, 0, d -> d_reclen - 20);
		}
	}

	return ret;
}

/*
** ssize_t openat(int dfd, const char * filename, int flags, int mode);
*/
asmlinkage ssize_t hooked_openat(struct pt_regs * regs)
{
	unsigned long ino;
	uid_t uid;
	ssize_t ret = -1;

	ino = get_ino_from_name(regs -> di, (char *)regs -> si);
	uid = current_euid().val;
	if (check_privilege(ino, uid))
	{
		ret = old_openat(regs);
	}

	return ret;
}

/*
** Get sys_call_table address.
*/
static sys_call_ptr_t * get_sys_call_table(void)
{
	sys_call_ptr_t * _sys_call_table = NULL;

	_sys_call_table = (sys_call_ptr_t *)kallsyms_lookup_name("sys_call_table");

	return _sys_call_table;
}

/*
** Initialize kernel netlink module and hook syscalls.
*/
static int __init hook_init(void)
{
	netlink_init();

	sys_call_table = get_sys_call_table();
	old_read = (old_syscall_t)sys_call_table[__NR_read];
	old_write = (old_syscall_t)sys_call_table[__NR_write];
	old_execve = (old_syscall_t)sys_call_table[__NR_execve];
	old_rename = (old_syscall_t)sys_call_table[__NR_rename];
	old_unlink = (old_syscall_t)sys_call_table[__NR_unlink];
	old_unlinkat = (old_syscall_t)sys_call_table[__NR_unlinkat];
	old_getdents64 = (old_syscall_t)sys_call_table[__NR_getdents64];
	old_openat = (old_syscall_t)sys_call_table[__NR_openat];
	pte = lookup_address((unsigned long)sys_call_table, & level);
	set_pte_atomic(pte, pte_mkwrite(* pte));
	sys_call_table[__NR_read] = (sys_call_ptr_t)hooked_read;
	sys_call_table[__NR_write] = (sys_call_ptr_t)hooked_write;
	sys_call_table[__NR_execve] = (sys_call_ptr_t)hooked_execve;
	sys_call_table[__NR_rename] = (sys_call_ptr_t)hooked_rename;
	sys_call_table[__NR_unlink] = (sys_call_ptr_t)hooked_unlink;
	sys_call_table[__NR_unlinkat] = (sys_call_ptr_t)hooked_unlinkat;
	sys_call_table[__NR_getdents64] = (sys_call_ptr_t)hooked_getdents64;
	sys_call_table[__NR_openat] = (sys_call_ptr_t)hooked_openat;
	set_pte_atomic(pte, pte_clear_flags(* pte, _PAGE_RW));

	return 0;
}

/*
** Unhook syscalls and release kernel netlink module.
*/
static void __exit hook_exit(void)
{
	set_pte_atomic(pte, pte_mkwrite(* pte));
	sys_call_table[__NR_read] = (sys_call_ptr_t)old_read;
	sys_call_table[__NR_write] = (sys_call_ptr_t)old_write;
	sys_call_table[__NR_execve] = (sys_call_ptr_t)old_execve;
	sys_call_table[__NR_rename] = (sys_call_ptr_t)old_rename;
	sys_call_table[__NR_unlink] = (sys_call_ptr_t)old_unlink;
	sys_call_table[__NR_unlinkat] = (sys_call_ptr_t)old_unlinkat;
	sys_call_table[__NR_getdents64] = (sys_call_ptr_t)old_getdents64;
	sys_call_table[__NR_openat] = (sys_call_ptr_t)old_openat;
	set_pte_atomic(pte, pte_clear_flags(* pte, _PAGE_RW));

	netlink_exit();
}

module_init(hook_init);
module_exit(hook_exit);
