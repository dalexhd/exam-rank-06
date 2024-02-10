#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <netinet/ip.h>
#include <errno.h>

#define MAX_CLIENTS 65536
#define MAX_MSG_SIZE 1000000
#define MAX_BUF_SIZE 1000000

typedef struct {
    int id;
    char *msg_buffer;
} Client;

typedef struct {
    int count;
    int max_fd;
    Client clients[MAX_CLIENTS];
    fd_set rfds, wfds, afds;
    char buf_read[MAX_MSG_SIZE];
    char buf_write[MAX_BUF_SIZE];
} Server;

/**
 * Extracts a single message from a buffer, splitting at the first newline character.
 * The extracted message is removed from the original buffer.
 *
 * @param buf Pointer to a buffer containing messages.
 * @param msg Pointer to store the extracted message.
 * @return 1 if a message is extracted, 0 if no newline character is found, -1 if memory allocation fails.
 */
int extract_message(char **buf, char **msg) {
    char *end = strstr(*buf, "\n");
    if (!end) return 0;

    int len = end - *buf;
    *msg = (char *)calloc(len + 1, sizeof(char));
    if (!*msg) return -1;

    strncpy(*msg, *buf, len);
    (*msg)[len] = '\0';

    int remaining = strlen(*buf) - len - 1;
    char *newbuf = (char *)calloc(remaining + 1, sizeof(char));
    if (!newbuf) {
        free(*msg);
        return -1;
    }

    strcpy(newbuf, end + 1);
    free(*buf);
    *buf = newbuf;

    return 1;
}

/**
 * Appends a string to a dynamically allocated buffer. The buffer is resized as needed.
 *
 * @param buf Pointer to the pointer of the buffer, which may be reallocated.
 * @param add String to be appended.
 * @return Pointer to the modified buffer or NULL if memory allocation fails.
 */
char *str_join(char **buf, const char *add) {
    if (!add) return *buf;

    int len_buf = *buf ? strlen(*buf) : 0;
    int len_add = strlen(add);

    char *newbuf = (char *)realloc(*buf, len_buf + len_add + 1);
    if (!newbuf) return NULL;

    if (*buf) {
        strcat(newbuf, add);
    } else {
        strcpy(newbuf, add);
    }

    *buf = newbuf;
    return newbuf;
}

/**
 * Sends a message to all connected clients except the sender.
 *
 * @param server Pointer to the Server structure.
 * @param sender File descriptor of the client who sent the message.
 * @param msg Message to be broadcasted.
 */
void broadcast(Server *server, int sender, const char *msg) {
    if (!msg) return;
    int msg_len = strlen(msg);
    for (int fd = 0; fd <= server->max_fd; fd++) {
        if (FD_ISSET(fd, &server->wfds) && fd != sender) {
            if (send(fd, msg, msg_len, 0) < 0) {
                write(2, "Broadcast failed\n", 18);
            }
        }
    }
}

/**
 * Handles the addition of a new client to the server.
 *
 * @param server Pointer to the Server structure.
 * @param fd File descriptor of the newly connected client.
 */
void join_client(Server *server, int fd) {
    if (fd >= MAX_CLIENTS) {
        close(fd);
        return;
    }

    server->max_fd = (fd > server->max_fd) ? fd : server->max_fd;
    server->clients[fd].id = server->count++;
    server->clients[fd].msg_buffer = NULL;
    FD_SET(fd, &server->afds);

    sprintf(server->buf_write, "server: client %d just arrived\n", server->clients[fd].id);
    broadcast(server, fd, server->buf_write);
}

/**
 * Handles the removal of a client from the server.
 *
 * @param server Pointer to the Server structure.
 * @param fd File descriptor of the client to be removed.
 */
void rm_client(Server *server, int fd) {
    sprintf(server->buf_write, "server: client %d just left\n", server->clients[fd].id);
    broadcast(server, fd, server->buf_write);

    free(server->clients[fd].msg_buffer);
    server->clients[fd].msg_buffer = NULL;
    FD_CLR(fd, &server->afds);
    close(fd);
}

/**
 * Sends pending messages to a specified client.
 *
 * @param server Pointer to the Server structure.
 * @param fd File descriptor of the client to receive messages.
 */
void send_msg(Server *server, int fd) {
    char *msg;
    while (extract_message(&server->clients[fd].msg_buffer, &msg) > 0) {
        sprintf(server->buf_write, "client %d: %s\n", server->clients[fd].id, msg);
        broadcast(server, fd, server->buf_write);
        free(msg);
    }
}

/**
 * Writes an error message to standard error and exits the program.
 *
 * @param str Error message to be written.
 */
void err(const char *str) {
    write(2, str, strlen(str));
    exit(1);
}

/**
 * Creates a socket for the server.
 *
 * @return File descriptor of the newly created socket.
 */
int create_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        err("Socket creation failed\n");

    return sockfd;
}

/**
 * Initializes the server structure and sets up the initial file descriptors.
 *
 * @param server Pointer to the Server structure to be initialized.
 * @param sockfd File descriptor of the server's main socket.
 */
void init_server(Server *server, int sockfd) {
    server->count = 0;
    server->max_fd = sockfd;
    FD_ZERO(&server->afds);
    FD_SET(sockfd, &server->afds);
}

/**
 * The main function serves as the entry point for the server application. It performs the following tasks:
 *   1. Validates the number of command-line arguments.
 *   2. Initializes the server socket and the server structure.
 *   3. Sets up the server's address and binds the socket to this address.
 *   4. Puts the server socket in a state to listen for incoming client connections.
 *   5. Enters a loop where it:
 *      a. Waits for activity on any of the file descriptors (client sockets and the server socket) using select().
 *      b. Accepts new client connections and handles incoming messages from connected clients.
 *      c. Processes client disconnections and forwards received messages to other clients.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments. Expects the second argument to be the port number.
 * @return Returns 0 upon successful completion, or exits with an error message for any failure scenarios.
 */
int main(int argc, char **argv) {
    // Check if the correct number of command-line arguments is provided
    if (argc != 2)
        err("Usage: server <port>\n");

    // Initialize the server structure and create a socket
    Server server;
    int sockfd = create_socket();
    init_server(&server, sockfd);

    // Set up the server address structure
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    servaddr.sin_port = htons(atoi(argv[1])); // Convert port number from string to integer

    // Bind the server socket to the IP address and port
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        err("Bind failed\n");

    // Listen for incoming connections on the server socket
    if (listen(sockfd, SOMAXCONN) < 0)
        err("Listen failed\n");

    // Main server loop
    while (1) {
        // Prepare the set of file descriptors for the select call
        server.rfds = server.wfds = server.afds;

        // Wait for activity on any of the file descriptors
        if (select(server.max_fd + 1, &server.rfds, &server.wfds, NULL, NULL) < 0)
            err("Select failed\n");

        // Iterate through file descriptors to check for activity
        for (int fd = 0; fd <= server.max_fd; fd++) {
            if (!FD_ISSET(fd, &server.rfds))
                continue;

            // Handle new incoming connection
            if (fd == sockfd) {
                // Accept the new connection
                int newfd = accept(sockfd, NULL, NULL);
                if (newfd >= 0) join_client(&server, newfd);
            } else {
                // Handle data reception from an existing client
                int res = recv(fd, server.buf_read, MAX_MSG_SIZE - 1, 0);
                if (res <= 0) {
                    // Handle client disconnection
                    rm_client(&server, fd);
                } else {
                    // Process received message
                    server.buf_read[res] = '\0';
                    str_join(&server.clients[fd].msg_buffer, server.buf_read);
                    send_msg(&server, fd);
                }
            }
        }
    }
    return 0;
}
