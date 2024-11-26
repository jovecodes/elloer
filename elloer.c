#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>

typedef enum {
    CLIENT_BANNED = 1 << 0,
} ClientFlags;

typedef struct {
    bool logged_in;
    int socket;
    char *username;
    char *password;
} Client;

typedef struct {
    pthread_mutex_t mutex;
    Client *items;
    int count;
    int capacity;
} Clients;

typedef unsigned long ClientID;

typedef struct {
    char *items;
    size_t count;
} Message;

typedef struct {
    pthread_mutex_t mutex;
    Message *items;
    int count;
    int capacity;
} History;

Clients clients = (Clients){
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .items = NULL,
    .count = 0,
    .capacity = 0
};
History history = (History){
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .items = NULL,
    .count = 0,
    .capacity = 0
};

unsigned long hash_string(const unsigned char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

#define DA_INIT_CAP 256

#define da_append(da, item)                                                              \
    do {                                                                                 \
        if ((da)->count >= (da)->capacity) {                                             \
            (da)->capacity = (da)->capacity == 0 ? DA_INIT_CAP : (da)->capacity*2;       \
            (da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items));     \
            assert((da)->items != NULL && "Out of RAM. Just download more.");            \
        }                                                                                \
        (da)->items[(da)->count++] = (item);                                             \
    } while (0)

#define da_reserve(da, reserve_len)                                                      \
    do {                                                                                 \
        if ((da)->count < reserve_len) {                                                 \
            (da)->capacity = reserve_len + 1;                                            \
            (da)->items = realloc((da)->items, (da)->capacity * sizeof(*(da)->items));   \
            assert((da)->items != NULL && "Out of RAM. Just download more.");            \
            for (; (da)->count < (da)->capacity; ++(da)->count) {                        \
                 (da)->items[(da)->count] = (Client){0};                                 \
            }                                                                            \
            (da)->count = (da)->capacity;                                                \
        }                                                                                \
    } while (0)

#define da_free(da) free((da).items)

void clear_history(History *history) {
    for (size_t i = 0; i < history->count; ++i) {
        free(history->items[i].items);
    }
    history->count = 0;
}

void append_history(Message message) {
    pthread_mutex_lock(&history.mutex);
    da_append(&history, message);
    pthread_mutex_unlock(&history.mutex);
}

Message alloc_message(char *data, size_t count) {
    char *copy = malloc(count);
    for (size_t i = 0; i < count; ++i) {
        copy[i] = data[i];
    }

    return (Message) {copy, count};
}

Message message_printf(const char *fmt, ...) {
    va_list args;
    
    va_start(args, fmt);
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    va_start(args, fmt);
    
    char *res = (char*) malloc(size + 1);  // +1 for the null terminator
    vsprintf(res, fmt, args);
    va_end(args);
    return (Message){res, size};
}

bool client_is_valid(Client *client) {
    return client->username != NULL && client->logged_in;
}

#define MsgFmt "%.*s"
#define MsgArg(msg) (int) msg.count, msg.items

void broadcast_message(ClientID sender, Message message) {
    pthread_mutex_lock(&clients.mutex);
    for (size_t i = 0; i < clients.count; ++i) {
        if (client_is_valid(&clients.items[i]) && sender != i) {
            if (send(clients.items[i].socket, message.items, message.count, 0) < 0) {
                printf("Failed to send message '" MsgFmt "' to client %zu (%s): %s\n", MsgArg(message), sender, clients.items[i].username, strerror(errno));
            }
        }
    }
    pthread_mutex_unlock(&clients.mutex);
}

ClientID try_login(char *username, char *password, int socket) {
    ClientID id = hash_string((unsigned char*) username) % 1024 + 1;

    pthread_mutex_lock(&clients.mutex);
    while (1) {
        if (id > clients.count) {
            da_reserve(&clients, id);
        }
        Client *it = &clients.items[id];
        if (!client_is_valid(it)) {
            it->username = username;
            it->password = password;
            it->socket = socket;
            it->logged_in = true;

            printf("logged in with index %zu\n", id);
            break;
        } else if (strcmp(it->username, username) == 0) {
            printf("id: %zu\n", id);
            if (strcmp(it->password, password) != 0) {
                id = 0;
                break;
            }
            it->socket = socket;
            it->logged_in = true;
            break;
        }
        id++;
    }
    pthread_mutex_unlock(&clients.mutex);
    return id;
}

void remove_client(ClientID id) {
    pthread_mutex_lock(&clients.mutex);
    clients.items[id].logged_in = false;
    pthread_mutex_unlock(&clients.mutex);
}

typedef enum {
    CMD_NONE,
    CMD_QUIT,
} Command;

bool cmd_cmp(const char *cmd, const char *literal) {
    for (int i = 0; i < strlen(literal); ++i) {
        if (cmd[i] != literal[i]) return false;
    }
    return true;
}

bool direct_message(ClientID client, Message msg) {
    pthread_mutex_lock(&clients.mutex);
    int socket = clients.items[client].socket;

    bool result = true;
    if (send(socket, msg.items, msg.count, 0) < 0) {
        printf("Failed to send message to client %zu (%s): %s\n", client, clients.items[client].username, strerror(errno));
        result = false;
    }

    pthread_mutex_unlock(&clients.mutex);
    return result;
}

#define direct_messagef(client, ...) do {       \
     Message msg = message_printf(__VA_ARGS__); \
     direct_message(client, msg);               \
     da_free(msg);                              \
} while (0)

Command process_message(ClientID client, Message message) {
    Command cmd = CMD_NONE;

    if (message.items[0] == '\\') {
        const char *cmd_str = message.items + 1; // skip the leading \

        if (cmd_cmp(cmd_str, "quit")) {
            cmd = CMD_QUIT;
        } else {
            direct_messagef(client, "Unknown command '%.*s'\n", (int) message.count - 3, cmd_str);
        }
        
        da_free(message);
    } else {
        broadcast_message(client, message);
        append_history(message);
    }

    return cmd;
}

ClientID login(int socket) {
    ClientID id;
    char buffer[1024] = {0};
    while (1) {
        char *username = NULL;
        char *password = NULL;
        {
            const char *msg = "Enter username: ";
            if (send(socket, msg, strlen(msg), 0) < 0) {
                perror("Failed to send message to a client");
                return 0;
            }
            ssize_t n = read(socket, buffer, sizeof(buffer) - 1);
            buffer[n - 2] = '\0';

            free(username);
            username = malloc(n - 1);
            strcpy(username, buffer);
        }
        {
            const char *msg = "Enter password: ";
            if (send(socket, msg, strlen(msg), 0) < 0) {
                perror("Failed to send message to a client");
                return 0;
            }
            ssize_t n = read(socket, buffer, sizeof(buffer) - 1);
            buffer[n - 2] = '\0';

            free(password);
            password = malloc(n - 1);
            strcpy(password, buffer);
        }

        id = try_login(username, password, socket);
        if (id != 0) {
            const char *msg = "Success!\n";
            if (send(socket, msg, strlen(msg), 0) < 0) {
                perror("Failed to send message to a client");
                return 0;
            }

            Message message = message_printf("%s has entered the chat.\n", username);
            broadcast_message(id, message);
            da_free(message);

            printf("User %s (id %zu) has successfully logged on.\n", username, id);
            break;
        } else {
            const char *msg = "Incorrect password.\n";
            if (send(socket, msg, strlen(msg), 0) < 0) {
                perror("Failed to send message to a client");
                return 0;
            }
            printf("User entered incorrect password: %s with %s\n", username, password);
        }
    }
    return id;
}

void *handle_connection(void *arg) {
    int socket = *(int *)arg;
    free(arg);

    char buffer[1024] = {0};
    ClientID id = login(socket);

    { // send old history
        pthread_mutex_lock(&history.mutex);
        for (size_t i = 0; i < history.count; ++i) {
            if (send(socket, history.items[i].items, history.items[i].count, 0) < 0) {
                perror("Failed to send message to a client");
            }
        }
        pthread_mutex_unlock(&history.mutex);
    }

    while (1) {
        ssize_t n = read(socket, buffer, sizeof(buffer) - 1);

        if (n == 0) {
            printf("Connection closed by the client.\n");
            break;
        } else if (n < 0) {
            perror("Error reading from socket");
            break;
        }

        Command cmd = process_message(id, alloc_message(buffer, n));
        if (cmd == CMD_QUIT) {
            break;
        }

        buffer[n] = '\0'; // Null-terminate the buffer for safe printing
        printf("%s", buffer);
    }

    Message msg = message_printf("%s has left the chat.", clients.items[id].username);
    broadcast_message(id, msg);
    da_free(msg);

    close(socket);
    return NULL;
}

int open_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

int main(int argc, char **argv) {
    int port = 6969;
    if (argc > 1) port = atoi(argv[1]);

    int server_fd = open_server(port);
    printf("Server is listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_socket < 0) {
            perror("Accept");
            continue;
        }

        printf("Accepted connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pthread_t thread;
        int *socket_ptr = malloc(sizeof(int));
        *socket_ptr = client_socket;
        pthread_create(&thread, NULL, handle_connection, socket_ptr);
        pthread_detach(thread);
    }
    
    pthread_mutex_lock(&clients.mutex);
    for (size_t i = 0; i < clients.count; ++i) {
        free(clients.items[i].username);
    }
    da_free(clients);
    pthread_mutex_unlock(&clients.mutex);

    close(server_fd);
    return 0;
}
