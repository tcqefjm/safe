/*
** This should be run as a daemon process to handle client side and kernel space communication.
** And the process should be started with root privileges.
*/
#define _GNU_SOURCE

#include <sqlite3.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "ncheck.c"

#define DB_PATH "/var/tmp/safe.db"
#define CREATE "CREATE TABLE IF NOT EXISTS safe"\
			"("									\
				"inode INTEGER PRIMARY KEY,"	\
				"owner INTEGER"					\
			")"
#define SELECT1 "SELECT inode FROM safe WHERE owner = %u"
#define SELECT1_ROOT "SELECT inode, owner FROM safe"
#define SELECT2 "SELECT owner FROM safe WHERE inode = %lu LIMIT 1"
#define SELECT_CHECK "SELECT 1 FROM safe WHERE inode = %lu LIMIT 1"
#define INSERT "INSERT INTO safe VALUES (%lu, %u)"
#define DELETE "DELETE FROM safe WHERE inode = %lu"

#define SOCK_PATH "/tmp/safe.socket"
#define NETLINK_SAFE 30

char sql[64] = { 0 };
sqlite3 * db;
int req_len, rsp_len, rsp1_len, rc, server_sock, client_sock;

/*
** request from client
** op	|ino|operation
** 1	|0	|list all files owned by specific user; for root this means all files
** 2	|	|check if file is protected by specific user; for root this gets file owner
** 4	|	|insert file into protection area
** 8	|	|delete file from protection area
*/
struct req
{
	unsigned char op;
	unsigned long ino;
} reqbuf;

/*
** response to client for op != 1
** There are 3 flag bits in stat
** 4			|2				|1
** owner error	|existence error|operation error
** For example,
** op = 4, the existence bit means file to insert is already in database;
** op = 8, the existence bit means file to delete is not in database, etc.
** uid is only activated when root requesting op = 2
*/
union rsp
{
	unsigned int stat;
	uid_t uid;
} rspbuf;

/*
** response to client for op == 1
** This responses with owner uid and file pathname
*/
struct rsp1
{
	uid_t uid;
	char filename[4096];
} rsp1buf;

static int callback_get_filelist(void * NotUsed, int argc, char ** argv, char ** azColName)
{
	unsigned long inode = (unsigned long)atol(argv[0]);

	get_filename_from_ino(inode, rsp1buf.filename);
	send(client_sock, & rsp1buf, rsp1_len, 0);

	return 0;
}

static int callback_get_filelist_root(void * NotUsed, int argc, char ** argv, char ** azColName)
{
	unsigned long inode = (unsigned long)atol(argv[0]);

	rsp1buf.uid = (uid_t)atoi(argv[1]);
	get_filename_from_ino(inode, rsp1buf.filename);
	send(client_sock, & rsp1buf, rsp1_len, 0);

	return 0;
}

static int callback_get_fileowner_or_check(void * result, int argc, char ** argv, char ** azColName)
{
	* (uid_t *)result = atoi(* argv);

	return 0;
}

void select_get_filelist(uid_t owner)
{
	if (owner)	// request not from root
	{
		rsp1buf.uid = owner;
		snprintf(sql, 63, SELECT1, owner);
		rc = sqlite3_exec(db, sql, callback_get_filelist, 0, NULL);
	}
	else	// request from root
	{
		strcpy(sql, SELECT1_ROOT);
		rc = sqlite3_exec(db, sql, callback_get_filelist_root, 0, NULL);
	}
}

void select_get_fileowner_or_check(unsigned long inode, uid_t owner)
{
	uid_t result = 0;

	snprintf(sql, 63, SELECT2, inode);
	rc = sqlite3_exec(db, sql, callback_get_fileowner_or_check, & result, NULL);
	if (owner)	// for normal user check protection
	{
		rspbuf.stat = (owner == result) ? 0 : 4;
	}
	else	// for root get owner
	{
		rspbuf.uid = (unsigned long)result;
	}
	if (rc != SQLITE_OK)
	{
		rspbuf.stat = 1;
	}
	send(client_sock, & rspbuf, rsp_len, 0);
}

void insert(unsigned long inode, uid_t owner)
{
	uid_t result = 0;

	snprintf(sql, 63, SELECT_CHECK, inode);
	rc = sqlite3_exec(db, sql, callback_get_fileowner_or_check, & result, NULL);
	if (result)	// check whether already in database
	{
		rspbuf.stat = 3;
	}
	else
	{
		if (! owner)	// for root always succeed
		{
			rspbuf.stat = 0;
		}
		else
		{
			if ( owner == get_owner_from_ino(inode) )	// check whether request from file owner
			{
				if (fork())
				{
					wait(& rspbuf.stat);
				}
				else	//drop privilege to file owner for encryption during insert
				{
					char filename[4096];
					struct stat statbuf;
					int status = 1;
					get_filename_from_ino(inode, filename);
					stat(filename, & statbuf);
					if (S_ISREG(statbuf.st_mode))
					{
						FILE * f = fopen(filename, "rb+");
						if (f)
						{
							fseek(f, 0, SEEK_END);
							long fsize = ftell(f);
							fseek(f, 0, SEEK_SET);
							char * buffer = malloc(fsize + 1);
							if (buffer)
							{
								seteuid(owner);
								setbuf(f, NULL);
								fread(buffer, 1, fsize, f);
								buffer[fsize] = 0;
								snprintf(sql, 63, INSERT, inode, owner);
								rc = sqlite3_exec(db, sql, NULL, 0, NULL);
								status = (rc == SQLITE_OK) ? 0 : 1;
								fseek(f, 0, SEEK_SET);
								fwrite(buffer, 1, fsize, f);
								free(buffer);
							}
							fclose(f);
						}
					}
					else
					{
						snprintf(sql, 63, INSERT, inode, owner);
						rc = sqlite3_exec(db, sql, NULL, 0, NULL);
						status = (rc == SQLITE_OK) ? 0 : 1;
					}
					exit(status);
				}
			}
			else
			{
				rspbuf.stat = 5;
			}
		}
	}
	send(client_sock, & rspbuf, rsp_len, 0);
}

void delete(unsigned long inode, uid_t owner)
{
	uid_t result = 0;

	snprintf(sql, 63, SELECT2, inode);
	rc = sqlite3_exec(db, sql, callback_get_fileowner_or_check, & result, NULL);
	if (! result)	// check whether not in database
	{
		rspbuf.stat = 3;
	}
	else
	{
		if (! owner || owner == result)	// request from root or owner
		{
			if (fork())
			{
				wait(& rspbuf.stat);
			}
			else	//drop privilege to file owner for decryption during delete
			{
				char filename[4096];
				struct stat statbuf;
				int status = 1;
				get_filename_from_ino(inode, filename);
				stat(filename, & statbuf);
				if (S_ISREG(statbuf.st_mode))
				{
					FILE * f = fopen(filename, "rb+");
					if (f)
					{
						fseek(f, 0, SEEK_END);
						long fsize = ftell(f);
						fseek(f, 0, SEEK_SET);
						char * buffer = malloc(fsize + 1);
						if (buffer)
						{
							seteuid(result);
							setbuf(f, NULL);
							fread(buffer, 1, fsize, f);
							buffer[fsize] = 0;
							snprintf(sql, 63, DELETE, inode);
							rc = sqlite3_exec(db, sql, NULL, 0, NULL);
							status = (rc == SQLITE_OK) ? 0 : 1;
							fseek(f, 0, SEEK_SET);
							fwrite(buffer, 1, fsize, f);
							free(buffer);
						}
						fclose(f);
					}
				}
				else
				{
					snprintf(sql, 63, DELETE, inode);
					rc = sqlite3_exec(db, sql, NULL, 0, NULL);
					status = (rc == SQLITE_OK) ? 0 : 1;
				}
				exit(status);
			}
		}
		else
		{
			rspbuf.stat = 5;
		}
	}
	send(client_sock, & rspbuf, rsp_len, 0);
}

/*
** This is the main processing function.
** It will create 2 processes, one handles requests from client side,
** the other handles communication from kernel space for control purposes.
*/
int main(int argc, char ** argv)
{
	int sockaddr_len, ucred_len;
	struct sockaddr_un server_sockaddr, client_sockaddr;
	struct ucred cr;

	req_len = sizeof(struct req);
	rsp_len = sizeof(union rsp);
	rsp1_len = sizeof(struct rsp1);
	sockaddr_len = sizeof(struct sockaddr_un);
	ucred_len = sizeof(struct ucred);
	memset(& server_sockaddr, 0, sockaddr_len);
	memset(& client_sockaddr, 0, sockaddr_len);

	rc = sqlite3_open(DB_PATH, & db);
	if (rc)
	{
		printf("%s\n", "SQLITE OPEN ERROR");
		sqlite3_close(db);
		exit(1);
	}
	chmod(DB_PATH, 0600);
	rc = sqlite3_exec(db, CREATE, NULL, 0, NULL);
	if (rc != SQLITE_OK)
	{
		printf("%s\n", "CREATE TABLE ERROR");
		sqlite3_close(db);
		exit(1);
	}

	/*
	** Parent process handles kernel communication.
	*/
	if (fork())
	{
		struct sockaddr_nl src_sockaddr, dest_sockaddr;
		struct nlmsghdr * nlh = NULL;
		struct msghdr msg;
		struct iovec iov;

		nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(sizeof(unsigned long)));
		memset(& src_sockaddr, 0, sizeof(struct sockaddr_nl));
		memset(& dest_sockaddr, 0, sizeof(struct sockaddr_nl));
		memset(nlh, 0, NLMSG_SPACE(sizeof(unsigned long)));
		memset(& msg, 0, sizeof(struct msghdr));

		server_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_SAFE);
		src_sockaddr.nl_family = AF_NETLINK;
		src_sockaddr.nl_pid = getpid();
		src_sockaddr.nl_groups = 0;
		bind(server_sock, (struct sockaddr *)& src_sockaddr, sizeof(struct sockaddr_nl));
		dest_sockaddr.nl_family = AF_NETLINK;
		dest_sockaddr.nl_pid = 0;
		dest_sockaddr.nl_groups = 0;
		nlh -> nlmsg_len = NLMSG_SPACE(sizeof(unsigned long));
		nlh -> nlmsg_pid = getpid();
		nlh -> nlmsg_flags = 0;
		iov.iov_base = (void *)nlh;
		iov.iov_len = NLMSG_SPACE(sizeof(unsigned long));
		msg.msg_name = (void *)& dest_sockaddr;
		msg.msg_namelen = sizeof(struct sockaddr_nl);
		msg.msg_iov = & iov;
		msg.msg_iovlen = 1;

		/*
		** First send a ready signal to kernel space.
		*/
		* (unsigned long *)NLMSG_DATA(nlh) = (unsigned long)0xffffffff << 32;
		sendmsg(server_sock, & msg, 0);
		while (1)
		{
			recvmsg(server_sock, & msg, 0);
			snprintf(sql, 63, SELECT2, * (unsigned long *)NLMSG_DATA(nlh));
			* (unsigned long *)NLMSG_DATA(nlh) = 0;
			sqlite3_exec(db, sql, callback_get_fileowner_or_check, (uid_t *)NLMSG_DATA(nlh), NULL);
			sendmsg(server_sock, & msg, 0);
		}

		close(server_sock);
		free(nlh);
		sqlite3_close(db);
	}
	/*
	** Child process handles client communication.
	*/
	else
	{
		server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (server_sock == -1)
		{
			printf("%s\n", "SOCKET ERROR");
			exit(1);
		}
		server_sockaddr.sun_family = AF_UNIX;
		strcpy(server_sockaddr.sun_path, SOCK_PATH);
		unlink(SOCK_PATH);
		rc = bind(server_sock, (struct sockaddr *)& server_sockaddr, sockaddr_len);
		if (rc == -1)
		{
			printf("%s\n", "BIND ERROR");
			close(server_sock);
			exit(1);
		}
		chmod(SOCK_PATH, 0666);
		rc = listen(server_sock, 16);
		if (rc == -1)
		{
			printf("%s\n", "LISTEN ERROR");
			close(server_sock);
			exit(1);
		}

		while (1)
		{
			client_sock = accept(server_sock, (struct sockaddr *)& client_sockaddr, & sockaddr_len);
			if (client_sock == -1)
			{
				close(client_sock);
				continue;
			}
			rc = recv(client_sock, & reqbuf, req_len, 0);
			if (rc == -1)
			{
				close(client_sock);
				continue;
			}
			/*
			** Get socket peer identification.
			*/
			if (getsockopt(client_sock, SOL_SOCKET, SO_PEERCRED, & cr, & ucred_len) == -1)
			{
				close(client_sock);
				continue;
			}
			switch (reqbuf.op)
			{
				case 1:	//send filelist
					select_get_filelist(cr.uid);
					break;
				case 2:	//send ownership
					select_get_fileowner_or_check(reqbuf.ino, cr.uid);
					break;
				case 4:	//send insert status
					insert(reqbuf.ino, cr.uid);
					break;
				case 8:	//send delete status
					delete(reqbuf.ino, cr.uid);
					break;
			}
			close(client_sock);
		}
		close(server_sock);
		close(client_sock);
		sqlite3_close(db);
	}

	return 0;
}
