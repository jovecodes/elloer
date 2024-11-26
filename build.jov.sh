set -xe

cc -o server ./server.c -ggdb -fsanitize=address,undefined
cc -o client ./client.c
./server
# ./client
