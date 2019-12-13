/*
This is the GUI program for safe clients.
*/

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
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
Indicate the order when build the tree model.
*/
enum
{
	USERNAME_COL,//0
	FILENAME_COL,//1
	N_CLOUMNS//2
};

//====****global variable are below****====
//basic variable to show the basic interface
GtkBuilder *builder;
GtkWidget *window;
GtkScrolledWindow *scroll_wid;
GtkButton *check_but, *add_but, *delete_but, *show_but, *quit_but;
GtkFileChooserButton *filechoose_but;
GObject *textlabel;
GError *error = NULL;

//variable to construct the list view
GtkWidget *file_view;
GtkListStore *list_store_user, *list_store_root;
GtkTreeIter iter;
GtkCellRenderer *renderer;
GtkTreeViewColumn *column;
GtkTreeSelection *file_selection;

/*
This is the corresponding mode when user click the different button.
*/
char * oper[4] = {"1", "2", "4", "8"};

/*
This function shows the tip dialog to help the user use this program.
tip_type: 
0 for information； 
1 for warning； 
3 for error.
*/
static void show_tip(gint tip_type, gchararray content, gchararray title)
{
	GtkWidget *dialog;
	dialog = gtk_message_dialog_new(GTK_WINDOW(window),
				GTK_DIALOG_DESTROY_WITH_PARENT,
				tip_type,
				GTK_BUTTONS_OK,
				"%s", content);
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}
/*
The two functions below set the view to show file infomation according to the user's privilege.
If the user is not root,
there will be only one column to show the file name.
If the user is root,
there will be two columns to show the username and file name.
*/
void set_file_view_user(GtkTreeView *view)
{
	gtk_tree_view_set_model (view, GTK_TREE_MODEL(list_store_user));

	/*创建一个文本单元绘制器*/
	renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer), "wrap-mode", PANGO_WRAP_CHAR, NULL);
	/*创建一个视图列表*/
	column = gtk_tree_view_column_new_with_attributes("文件名",renderer,"text",0,NULL);
	/*附加一列列表*/
	gtk_tree_view_append_column(view,column);
}
void set_file_view_root(GtkTreeView *view)
{
	gtk_tree_view_set_model (view, GTK_TREE_MODEL(list_store_root));
	/*创建一个文本单元绘制器*/
	renderer = gtk_cell_renderer_text_new();
	/*创建一个视图列表*/
	column = gtk_tree_view_column_new_with_attributes("用户名",renderer,"text",USERNAME_COL,NULL);
	/*附加一列列表*/
	gtk_tree_view_append_column(view,column);

	/*创建一个文本单元绘制器*/
	renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer), "wrap-mode", PANGO_WRAP_CHAR, NULL);
	/*创建一个视图列表*/
	column = gtk_tree_view_column_new_with_attributes("文件名",renderer,"text",FILENAME_COL,NULL);
	/*附加一列列表*/
	gtk_tree_view_append_column(view,column);
}

/*
This function gets the inode of a file by given the file name.
*/
static unsigned long filename_to_inode(char *filename)
{
	struct stat file_stat;
	stat(filename, &file_stat);
	return file_stat.st_ino;   
}

/*
This function handles the event when show_but/add_but/check_but is clicked.
*/
static void handle(GtkWidget *widget, gpointer op)
{
	int client_sock, rc, sockaddr_len;
	struct sockaddr_un server_sockaddr, client_sockaddr;
	
	union rsp rspbuf;
	struct rsp1 rsp1buf;
	struct passwd * pwd;

	unsigned long ino;
	char *x;
	GFile *selected_file;
	selected_file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(filechoose_but));

	//If you want to add or check an exact file, you need to select a file at first.
	if(atoi(op) != 1 && !selected_file)
	{
		show_tip(1, "You have to select a file or folder to continue", "Wrong operation");
		return ;
	}
	if(atoi(op) == 1)
	{
		ino = 0;
	}
	else
	{   
		x = g_file_get_path(selected_file);
		ino = filename_to_inode(x);
	}
	struct req reqbuf = {atoi(op), ino};

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

	if (atoi(op) == 1)
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

	switch (atoi(op))
	{
		case 1:		//get file list
				if(geteuid()){//not root              
					gtk_list_store_clear(list_store_user);//clear the model at first.
					while(rc > 0){//fill the model with the data get from server.
						gtk_list_store_append(list_store_user,&iter);
						gtk_list_store_set(list_store_user,&iter,
											0, rsp1buf.filename,
											-1);
						rc = recv(client_sock, & rsp1buf, sizeof(struct rsp1), 0);                        
					}
				}
				else//root
				{
					gtk_list_store_clear(list_store_root);
					while (rc > 0)
					{
						pwd = getpwuid(rsp1buf.uid);
						gtk_list_store_append(list_store_root,&iter);
						gtk_list_store_set(list_store_root,&iter,
											USERNAME_COL, pwd->pw_name,
											FILENAME_COL, rsp1buf.filename,
											-1);  
						rc = recv(client_sock, & rsp1buf, sizeof(struct rsp1), 0);
					}                    
				}
				break;
		case 2:
			if (geteuid())	//not root, check a file whether protected or not
			{
				if (rspbuf.stat & 1)
				{
					show_tip(3, "CHECK FAILED!", "Failed operation");
					// printf("%s\n", "CHECK FAILED!");
				}
				else
				{
					if (rspbuf.stat & 4) show_tip(0, "FILE NOT UNDER YOUR PROTECTION.", "Information");
					// printf("%s\n", "FILE NOT UNDER YOUR PROTECTION.");
					else show_tip(0, "FILE UNDER YOUR PROTECTION.", "Information");
					// printf("%s\n", "FILE UNDER YOUR PROTECTION.");
				}
			}
			else	//get file owner by root
			{
				if (rspbuf.uid)
				{
					GtkWidget *dialog;
					pwd = getpwuid(rspbuf.uid);
					dialog = gtk_message_dialog_new(GTK_WINDOW(window),
								GTK_DIALOG_DESTROY_WITH_PARENT,
								0,
								GTK_BUTTONS_OK,
								"owner: %u\tusername: %s\n", rspbuf.uid, pwd->pw_name);
					gtk_window_set_title(GTK_WINDOW(dialog), "Information");
					gtk_dialog_run(GTK_DIALOG(dialog));
					gtk_widget_destroy(dialog);				
					// printf("owner: %u\tusername: %s\n", rspbuf.uid, pwd->pw_name);
				}
				else show_tip(3, "CHECK OWNER FAILED!\nFILE NOT PROTECTED.", "Failed operation");
			}
			break;
		case 4:		//insert
			if (rspbuf.stat & 1)
			{
				if (rspbuf.stat & 2)
				{
					show_tip(3, "INSERT FAILED:\nFILE ALREADY PROTECTED!", "Failed Operation");
				}
				if (rspbuf.stat & 4)
				{
					show_tip(3, "INSERT FAILED:\nFILE NOT OWNED BY YOU!", "Failed Operation");
					// printf("%s\n", "FILE NOT OWNED BY YOU!");
				}
			}
			else
			{
				show_tip(0, "INSERT SUCCEEDED.", "Information");
				gtk_button_clicked(show_but);
				// printf("%s\n", "INSERT SUCCEEDED.");
			}
			break;
	}
	close(client_sock);
	unlink(client_sockaddr.sun_path);
}

/*
This function will remove the file that user selected by click on it.
When you click the delete button, this function will implement.
*/
static void handle_delete(GtkWidget *widget, gpointer data){
	int client_sock, rc, sockaddr_len;
	struct sockaddr_un server_sockaddr, client_sockaddr;
	unsigned long ino;
	union rsp rspbuf;

	GtkTreeIter iter;
	char *FileName;
	GtkTreeModel *model;

	//To delete a file from the safe box, you need to click a file listed at first.
	if (gtk_tree_selection_get_selected(file_selection, &model, &iter))
	{
		if(geteuid()){
			gtk_tree_model_get(model, &iter, 0, &FileName, -1);
			printf("%s\n", FileName);
		}
		else
		{
			gtk_tree_model_get(model, &iter, 1, &FileName, -1);
		}
		ino = filename_to_inode(FileName);

		struct req reqbuf = {8, ino};
		printf("Inode is %lu\n", ino);
		
		
		sockaddr_len = sizeof(struct sockaddr_un);
		memset(&server_sockaddr, 0, sockaddr_len);
		memset(&client_sockaddr, 0, sockaddr_len);

		//create a socket to communicate with the server
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

		rc = recv(client_sock, & rspbuf, sizeof(union rsp), 0);
		if (rc == -1)
		{
			printf("%s\n", "RECV ERROR");
			close(client_sock);
			exit(1);
		}
		printf("%s\n", "Received.");
		if (rspbuf.stat & 1)
		{
			printf("rspbuf.stat: %d\n", rspbuf.stat);
			if (rspbuf.stat & 2)
			{
				show_tip(3, "DELETE FAILED:\nFILE NOT PROTECTED YET!", "Failed Operation");
			}
			if (rspbuf.stat & 4)
			{
				show_tip(3, "DELETE FAILED:\nFILE NOT OWNED BY YOU!", "Failed Operation");
			}
		}
		else
		{
			show_tip(0, "DELETED SUCCESSED", "Information");
			//After successfully deleted a file, fresh the content view to show the change.
			gtk_button_clicked(show_but);
		}
	}
	else
	{
		show_tip(1, "You have to click a file listed below at first!", "Wrong operation");
	}
}

/*
This is the main function that implements the GUI.
*/
int main(int argc, char* argv[])
{
	gtk_init(&argc, &argv);

	/* Construct a GtkBuilder instance and load our UI description */
	builder = gtk_builder_new();
	if (gtk_builder_add_from_file(builder, "user.glade", &error) == 0)//get a builder from the ui file
	{
		g_printerr("Error loading file: %s\n", error->message);
		g_clear_error(&error);
		return 1;
	}

	list_store_user = GTK_LIST_STORE(gtk_builder_get_object(builder, "list_store_user"));
	list_store_root = GTK_LIST_STORE(gtk_builder_get_object(builder, "list_store_root"));

	//Main window
	window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	scroll_wid = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "scroll_wid"));

	file_view = GTK_WIDGET(gtk_builder_get_object(builder, "file_view"));
	if(geteuid()){//If not root
		set_file_view_user(GTK_TREE_VIEW(file_view));
	}
	else{//If root
		set_file_view_root(GTK_TREE_VIEW(file_view));
	}

	file_selection = GTK_TREE_SELECTION(gtk_builder_get_object(builder, "file_selection"));

	//The button to select a file from file manager
	filechoose_but = GTK_FILE_CHOOSER_BUTTON(gtk_builder_get_object(builder, "filechoose_but"));
	//The button to show the files in box
	show_but = GTK_BUTTON(gtk_builder_get_object(builder, "show_but"));
	g_signal_connect(show_but, "clicked", G_CALLBACK(handle), oper[0]);
	//The button to check if a selected file is in the box
	check_but = GTK_BUTTON(gtk_builder_get_object(builder, "check_but"));
	g_signal_connect(check_but, "clicked", G_CALLBACK(handle), oper[1]);
	//The button to add a selected file to the box
	add_but = GTK_BUTTON(gtk_builder_get_object(builder, "add_but"));
	g_signal_connect(add_but, "clicked", G_CALLBACK(handle), oper[2]);
	//The button to remove a selected file from the box
	delete_but = GTK_BUTTON(gtk_builder_get_object(builder, "delete_but"));
	g_signal_connect(delete_but, "clicked", G_CALLBACK(handle_delete), NULL);
	//The button to quit the program
	quit_but = GTK_BUTTON(gtk_builder_get_object(builder, "quit"));
	g_signal_connect_swapped(quit_but, "clicked", G_CALLBACK(gtk_widget_destroy), window);

	gtk_widget_show(window);
	gtk_button_clicked(show_but);
	gtk_main();//Main circulation

	return 0;
}
