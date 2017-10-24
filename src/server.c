/*
 * Copyright 2017
 * MIT Licensed
 * sockets.c
 * Functions for running the server
 */

// includes
#include "config.h"
#include "server.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "representation.h"
#include <errno.h>
#include <pthread.h>

#include <stdio.h>

/* TODO:
Global fd state:
- separate reading_mux per fd
- watchdog pulse
- pass pointer structure in global fd table instead of fd to threads
*/

/* So what is going on here?
The server uses signal driven IO so that it is not constantly polling dosens of clients.
We use two signal handlers: CONNECT_SIG for when a new client connects and READ_SIG for when new IO is available on a socket

The handler for READ_SIG is quite complex. It can only access shared resources asynchronously by spawning worker threads incase we are interupting code
already has the control mutex locked (in which case a deadlock would occur if we operated synchronously).

When a message is read in it is added to the read_buff queue. Items are requested and returned from this queue at some later time using read_message()
*/

// ofsets past REALTIMESIGMIN
#define CONNECT_SIG 1
#define READ_SIG 2

// global read buffer
static GQueue *read_buff = NULL;
static pthread_mutex_t read_buff_mux = PTHREAD_MUTEX_INITIALIZER;

// serializes access to file descriptors
static pthread_mutex_t reading_mux = PTHREAD_MUTEX_INITIALIZER;

// the listening socket
static int listen_socket = -1;

// sets up a fd for realtime signal IO using signal sig, handled by handler
static bool setup_rt_signal_io(int fd, int sig, void (*handler)(int, siginfo_t *, void *)) {
    // establish signal handler for new connections
    struct sigaction sa;
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO; // SA_RESTART: don't break things so much when we interupt them, SA_SIGINFO: be realtime
    sigemptyset(&sa.sa_mask); // don't mask signals
    if (-1 == sigaction(sig, &sa, NULL)) { // install new settings for this signal
        return false;
    }

    // set the owner of the socket to us so that we get the signals
    if (-1 == fcntl(fd, F_SETOWN, getpid())) {
        return false;
    }

    // specify our handler instead of the default SIGIO handler
    // I am not sure why this is needed in addition to setting sa.sa_sigaction
    if (-1 == fcntl(fd, F_SETSIG, sig)) {
        return false;
    }

    // enable signaling & non-blocking IO
    int flags = fcntl(fd, F_GETFL);
    if (-1 == fcntl(fd, F_SETFL, flags | O_ASYNC | O_NONBLOCK)) {
        return false;
    }

    return true;
}

// attempt to read a full json object from a fd (defined as "{*}", handling nesting)
static ReadStatus read_json_object(int fd, GString **out_string) {
    // assuming that we can read the whole thing at once

    GString *string = g_string_new("{");

    // read first character
    char buf;
    ssize_t num_read = read(fd, &buf, 1);
    if (1 != num_read) { // we could not read a whole character
        int e = errno;
        g_string_free(string, true);

        // if it looks like there just wasn't any data left in the buffer for us to read
        if ((EBADF == e) || (EAGAIN == e) || (EWOULDBLOCK == e) || (0 == num_read)) {
            return END;
        }
        
        // there may have been data but an unknown error occured
        printf("Unknown error errno=%i num_read=%li\n", e, num_read);
        return ERROR;
    }

    // the first character must be {
    if (buf != '{') {
        g_string_free(string, true);

        // skip newline characters so we can telnet in for testing
        if ((buf == '\n') || (buf == 13 /*CR*/)) { 
            puts("newline");
            return read_json_object(fd, out_string);
        }

        printf("invalid c=%i\n", (int) buf);
        return ERROR;
    }

    // read in the rest
    int nest_count = 1; // we already have {
    while (true) {
        // try to read 1 character
        num_read = read(fd, &buf, 1);
        if (1 != num_read) {
            int e = errno;
            g_string_free(string, true);

            // if we reached the end of the buffer
            if ((EAGAIN == e) || (EWOULDBLOCK == e) || (0 == num_read)) {
                puts("partial packet!");
                return ERROR;
            } else {
                // unknown error
                printf("errno=%i. num_read=%li\n", e, num_read);
                return ERROR;
            }
        }

        // we did read in a character successfully...
        
        // nesting count
        if ('{' == buf)
            nest_count += 1;
        else if ('}' == buf)
            nest_count -= 1;

        // add the character to the string
        string = g_string_append_c(string, buf);

        // are we done?
        if (0 == nest_count)
            break;
    }

    *out_string = string;
    return SUCCESS;
}

// fetch a read buffer item from fd
// mutexes are done by the caller
static ReadStatus fetch_item(int fd) {
    // the item we will add to the buffer for this read
    BufferItem *item = malloc(sizeof(BufferItem));
    if (NULL == item) {
        return ERROR;
    }

    // attempt to read in the json object
    GString *obj;
    ReadStatus status = read_json_object(fd, &obj);
    if (SUCCESS != status) {
        free(item);
        return status;
    }

    printf("read object %s\n", obj->str);

    // decode JSON
    if (!decode_message(obj->str, &(item->msg))) {
        puts("decode error\n");
        // report this BufferItem as a software error
        software_error(&(item->msg), "Could not decode message");
    }

    // get the IP address of the remote host
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (0 != getpeername(fd, &addr, &addrlen)) {
        puts("get peer name");
        free_bufferitem(item);
        return ERROR;
    }

    // check it is an IPv4 address
    struct sockaddr_in *ip4_addr = (struct sockaddr_in *) &addr;
    if (AF_INET != ip4_addr->sin_family) {
        free_bufferitem(item);
        return ERROR;
    }

    item->address = ip4_addr->sin_addr;

    // add the item to the queue
    g_queue_push_tail(read_buff, (gpointer) item);

    return SUCCESS;
}

// thread to read in all of the objects waiting in a buffer
/* the actual reading is done in its own thread for the following situation:
 *    1. some normal code has read_buff_mux locked
 *    2. io_handler signal tries to lock read_buff_mux
 *    3. deadlock
 *
 * Using s different thread for the io_handler means that it can block waiting for read_buff_mux without creating a deadlock
 * We handle mutexes out here to force the program to behave more serially:
 *    For example, if the read queue was only locked while it was being used, the system test becomes a race to see if this thread can finish
 *      adding to the queue before the queue item is requested by the test. This way the queue item request can't jump in so easily
 *      (theoretically it could jump in before this thread has had any chance to lock the mutex but in practice this seems not to happen. 
 *      I can't see a better solution than this because we can't lock the mutex in the signal handler because of the above deadlock)
 */
static void *object_reader_thread(void *arg) {
    int fd = *((int *) arg);
    
    // get exclusive access to the read_buff
    if (0 != pthread_mutex_lock(&read_buff_mux)) {
        puts("object reader could not get the read_buff mutex");
        free(arg);
        return NULL;
    }
    
    // read in every available object
    while (true) {
        ReadStatus status = fetch_item(fd);
        if (ERROR == status) {
            puts("Read ERROR from remote host\n"); 
            close(fd);
            break;
        } else if (END == status) { // we have read the whole buffer
            break;
        }
        // else we had a SUCCESS so continue (the buffer may contain another message)
    }
    
    // clean up before returning
    free(arg);
    pthread_mutex_unlock(&read_buff_mux);
    pthread_mutex_unlock(&reading_mux);
    return NULL;
}

// another thread for reporting a connection close
static void *report_close_thread(void *arg) {
    int fd = *((int *) arg);   
    
    // get the IP address of the remote host
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (0 != getpeername(fd, &addr, &addrlen)) {
        puts("get peer name");
        pthread_mutex_unlock(&reading_mux);
        free(arg);
        return NULL;
    }
    
    // we are done with the fd now
    free(arg); // data was copied into fd
    pthread_mutex_unlock(&reading_mux);
    
    // check it is IPv4
    struct sockaddr_in *ip4_addr = (struct sockaddr_in *) &addr;
    if (AF_INET != ip4_addr->sin_family) {
        return NULL;
    }
    
    // allocate the item to go onto the queue
    BufferItem *item = malloc(sizeof(BufferItem));
    if (!item) {
        return NULL;
    }
    
    item->address = ip4_addr->sin_addr;
    
    software_error(&(item->msg), "Connection closed");
    
    // get access to the queue
    if (0 != pthread_mutex_lock(&read_buff_mux)) {
        puts("can't lock queue");
        free_bufferitem(item);
        return NULL;
    }
    
    // add the item to the queue
    g_queue_push_tail(read_buff, (gpointer) item);
    
    pthread_mutex_unlock(&read_buff_mux);
    puts("connection close added to queue");
    
    close(fd);
    return NULL;
}

// realtime signal handler for when IO is available on a connection with a client
static void io_handler(__attribute__ ((unused)) int sig, __attribute__ ((unused)) siginfo_t *si, __attribute__ ((unused)) void *ucontext) {
    printf("IO! fd=%d\n", si->si_fd);
    pthread_t thread;

    // get the exclusive right to do reading as early as we can incase another thread closes the file descriptor
    if (0 != pthread_mutex_lock(&reading_mux)) {
        puts("can't lock reading mux");
        return;
    }

    char buf;
    // check to see if there is any data to read
    // checking si->si_code just seems to report the connection is closed on every signal
    // MSG_PEEK so that the read character is not removed from the read buffer
    ssize_t count = recv(si->si_fd, &buf, 1, MSG_PEEK);
    int e = errno;
    if (1 != count) {
        // BUG: this may report the connection close more than once. si_code, errno and count seem the same each time so I don't know how to tell
        printf("closed connection. fd=%i si_code=%i errno=%i count=%li\n",si->si_fd, si->si_code, e, count);

        // (in another thread) add a message to the queue reporting that the connection closed
        int *thread_arg = malloc(sizeof(int));
        if (!thread_arg) {
            pthread_mutex_unlock(&reading_mux);
            return;
        }

        // start thread
        *thread_arg = si->si_fd;
        pthread_create(&thread, NULL, report_close_thread, (void *) thread_arg); // unlocks the reading mutex & frees thread_arg 
        return;
    }

    // do the actual reading in a different thread (see comment on thread function)
    int *thread_arg = malloc(sizeof(int));
    if (!thread_arg) {
        pthread_mutex_unlock(&reading_mux);
        return;
    }

    // start thread
    *thread_arg = si->si_fd;
    pthread_create(&thread, NULL, object_reader_thread, (void *) thread_arg); // this thread unlocks the reading mux when it is done & frees thread_arg
}

// realtime signal handler for when there is a connection to the listening socket
static void connect_handler(__attribute__ ((unused)) int sig, siginfo_t *si, __attribute__ ((unused)) void *ucontext) {
    // accepts a connection from a remote host and sets it up to do signal driven IO
    printf("connect!\n");

    int fd = accept(si->si_fd, NULL, NULL);
    if (-1 == fd)
        return;

    setup_rt_signal_io(fd, SIGRTMIN + READ_SIG, io_handler);
}

// starts a server listening on addr
// returns success
bool start_server(const struct sockaddr *addr, socklen_t addrlen) {
    if (NULL == addr)
        return false;

    // create IPv4 TCP socket to communicate over
    // non-blocking so we can use signal driven IO
    // cloexec for security (closes fd on an exec() syscall)
    listen_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (-1 == listen_socket)
        return false;

    // bind to the specified address
    if (-1 == bind(listen_socket, addr, addrlen)) {
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    // initialise the read buffer
    read_buff = g_queue_new();
    if (!read_buff) {
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    // set up realtime signal-driven IO on the listening socket
    if (!setup_rt_signal_io(listen_socket, SIGRTMIN + CONNECT_SIG, connect_handler)) {
        close(listen_socket);
        listen_socket = -1;
        g_queue_free(read_buff);
        read_buff = NULL;
        return false;
    }

    // begin listening on the socket
    if (-1 == listen(listen_socket, SOMAXCONN)) {
        close(listen_socket);
        listen_socket = -1;
        g_queue_free(read_buff);
        read_buff = NULL;
        return false;
    }

    return true;
}

// gets a message from the read queue
BufferItem *read_message(void) {
    if (0 != pthread_mutex_lock(&read_buff_mux))
        return NULL;

    BufferItem *ret = (BufferItem *) g_queue_pop_head(read_buff);
    // if this is NULL we should be returning NULL anyway

    pthread_mutex_unlock(&read_buff_mux);

    return ret;
}

// free a BufferItem (wrapper function incase it contains anyting that needs freeing interneally)
void free_bufferitem(BufferItem *item) {
    free(item);
    // todo: free_errormessage
}

void stop_server(void) {
    // close the open socket
    if (-1 != listen_socket) {
        close(listen_socket);
        listen_socket = -1;
    }

    // free up the read buffer
    if (read_buff) {
        pthread_mutex_lock(&read_buff_mux);
        g_queue_free_full(read_buff, (GDestroyNotify) free_bufferitem);
        pthread_mutex_unlock(&read_buff_mux);
    }
}