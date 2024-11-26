// This half of the program is not finished at all. I want to make a nice gui 
// using the engine, stealing some code from lovial editor.
//
// For right now I have been using telnet as the client.

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define PORT 6969

int main(int argc, char const* argv[])
{
    int status, valread, client_fd;
    struct sockaddr_in serv_addr;
    char buffer[1024] = { 0 };
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("ERROR: Socket creation error.\n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("ERROR: Invalid/unsupported address.\n");
        return -1;
    }

    if ((status = connect(client_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr))) < 0) {
        printf("ERROR: Connection Failed.\n");
        return -1;
    }
    char* hello = "Hello from client";
    send(client_fd, hello, strlen(hello), 0);
    valread = read(client_fd, buffer, 1024 - 1); // subtract 1 for the null
    printf("%s\n", buffer);

    // closing the connected socket
    close(client_fd);
    return 0;
}
