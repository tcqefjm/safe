/*
** This is the CUI program for safe clients.
*/

#include <sys/un.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <pwd.h>

#define SERVER_PATH "/tmp/safe.socket"
#define CLIENT_PATH "/tmp/safe.%u.socket"

/*
** request to server
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
};

/*
** response from server for op != 1
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
	unsigned char stat;
	uid_t uid;
};

/*
** response to client for op == 1
** This responses with owner uid and file pathname
*/
struct rsp1
{
	uid_t uid;
	char filename[4096];
};

/*
** This is the main processing function. It connects to server, sends 
** request, and gets result. The are four different requests: get 
** file list; check whether a file is protected or get its owner; insert a
** file into protection area; and delete a file from protection. According to
** user's identity, root user and not-root user can get different results.
*/
void handle(unsigned char op, unsigned long ino)	//main process
{
	int client_sock, rc, sockaddr_len;
	struct sockaddr_un server_sockaddr, client_sockaddr;
	struct req reqbuf = {op, ino};
	union rsp rspbuf;
	struct rsp1 rsp1buf;
	struct passwd * pwd;

	sockaddr_len = sizeof(struct sockaddr_un);
	memset(& server_sockaddr, 0, sockaddr_len);
	memset(& client_sockaddr, 0, sockaddr_len);

	client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client_sock == -1)
	{
		printf("%s\n", "SOCKET ERROR");
		exit(1);
	}

	client_sockaddr.sun_family = AF_UNIX;
	snprintf(client_sockaddr.sun_path, 107, CLIENT_PATH, geteuid());
	unlink(client_sockaddr.sun_path);
	rc = bind(client_sock, (struct sockaddr *)& client_sockaddr, sockaddr_len);
	if (rc == -1)
	{
		printf("%s\n", "BIND ERROR");
		close(client_sock);
		exit(1);
	}

	server_sockaddr.sun_family = AF_UNIX;
	strcpy(server_sockaddr.sun_path, SERVER_PATH);
	rc = connect(client_sock, (struct sockaddr *)& server_sockaddr, sockaddr_len);
	if (rc == -1)
	{
		printf("%s\n", "CONNECT ERROR");
		close(client_sock);
		exit(1);
	}

	rc = send(client_sock, & reqbuf, sizeof(struct req), 0);
	if (rc == -1)
	{
		printf("%s\n", "SEND ERROR");
		close(client_sock);
		exit(1);
	}

	if (op == 1)
	{
		rc = recv(client_sock, & rsp1buf, sizeof(struct rsp1), 0);
	}
	else
	{
		rc = recv(client_sock, & rspbuf, sizeof(union rsp), 0);
	}
	if (rc == -1)
	{
		printf("%s\n", "RECV ERROR");
		close(client_sock);
		exit(1);
	}

	switch (op)
	{
		case 1:		//get file list
			printf("%s\n", "FILE LIST:");
			if (geteuid())	//not root, get specific user's file
			{
				printf("%s\n", "filename");
				while (rc > 0)
				{
					printf("%s\n",rsp1buf.filename);
					rc = recv(client_sock, & rsp1buf, sizeof(struct rsp1), 0);
				}
			}
			else	//root, get all users' file
			{
				printf("%s\t%s\n", "owner", "filename");
				while (rc > 0)
				{
					pwd = getpwuid(rsp1buf.uid);
					printf("%s\t%s\n", pwd -> pw_name, rsp1buf.filename);
					rc = recv(client_sock, & rsp1buf, sizeof(struct rsp1), 0);
				}
			}
			break;
		case 2:
			if (geteuid())	//not root, check a file whether protected or not
			{
				if (rspbuf.stat & 1)
				{
					printf("%s\n", "CHECK FAILED!");
				}
				else
				{
					if (rspbuf.stat & 4) printf("%s\n", "FILE NOT UNDER YOUR PROTECTION.");
					else printf("%s\n", "FILE UNDER YOUR PROTECTION.");
				}
			}
			else	//get file owner by root
			{
				if (rspbuf.uid)
				{
					pwd = getpwuid(rspbuf.uid);
					printf("owner: %u\tusername: %s\n", rspbuf.uid, pwd->pw_name);
				}
				else printf("%s\n", "CHECK OWNER FAILED:\tFILE NOT PROTECTED!");
			}
			break;
		case 4:		//insert
			if (rspbuf.stat & 1)
			{
				printf("%s\t", "INSERT FAILED:");
				if (rspbuf.stat & 2)
				{
					printf("%s\n", "FILE ALREADY PROTECTED!");
				}
				if (rspbuf.stat & 4)
				{
					printf("%s\n", "FILE NOT OWNED BY YOU!");
				}
			}
			else
			{
				printf("%s\n", "INSERT SUCCEEDED.");
			}
			break;
		case 8:		//delete
			if (rspbuf.stat & 1)
			{
				printf("%s\t", "DELETE FAILED:");
				if (rspbuf.stat & 2)
				{
					printf("%s\n", "FILE NOT PROTECTED YET!");
				}
				if (rspbuf.stat & 4)
				{
					printf("%s\n", "FILE NOT OWNED BY YOU!");
				}
			}
			else
			{
				printf("%s\n", "DELETE SUCCEEDED.");
			}
			break;
	}

	close(client_sock);
	unlink(client_sockaddr.sun_path);
}

void usage(void)
{
	printf("%s\n", "Usage: safe [OPTION]... [FILE]...\n\n"
	"  -l (list)	list all files under protection\n"
	"  -c (check)	check whether file is under protection;\n"
	"  		for root check owner of given file\n"
	"  -i (insert)	insert given file into protection area\n"
	"  -d (delete)	delete given file from protection area\n");
}

/*
** This is the main function that checks input parameters.
*/
int main(int argc, char ** argv)
{
	int ch;
	unsigned char option = 0;
	unsigned long inode = 0;
	struct stat file_stat;

	while ((ch = getopt(argc, argv, "lcid")) != -1)
	{
		switch (ch)
		{
			case 'l':
				option = 1;
				break;
			case 'c':
				option = 2;
				break;
			case 'i':
				option = 4;
				break;
			case 'd':
				option = 8;
				break;
			default:
				usage();
				return -1;
		}
	}
	if (optind != 2 || (argc < 3 && option > 1))
	{
		usage();
		return -1;
	}
	if (option == 1)
	{
		handle(option, 0);
		return 0;
	}
	while (optind < argc)
	{
		if (! stat(argv[optind++], &file_stat))
		{
			inode = file_stat.st_ino;
			handle(option, inode);
		}
		else
		{
			printf("%s: No such file or directory\n", argv[optind - 1]);
		}
	}
	return 0;
}
