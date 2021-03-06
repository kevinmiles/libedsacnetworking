/*
 * Copyright 2017
 * GPL3 Licensed
 * sockets.c
 * Functions for running the server
 */

/* So what is going on here?
The server uses signal driven IO so that it is not constantly polling dosens of clients.
We use two signal handlers: CONNECT_SIG for when a new client connects and READ_SIG for when new IO is available on a socket

When a message is read in it is added to the read_buff queue. Items are requested and returned from this queue at some later time using read_message()

Also, clients are expected to periodically send KEEP_ALIVE messages so that we know that they are running. The time of the most recent one of these is stored in the connection table.
Periodically these times are checked against the current time to see if everything is it should be. 
*/

// includes
#include "config.h"
#include "edsac_server.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "edsac_representation.h"
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include "edsac_timer.h"
#include <errno.h>

// stores information about an active connection
typedef struct {
    int fd;
    pthread_mutex_t mutex;
    struct sockaddr_in addr;
    time_t last_keep_alive;
    /* this is a bit of a hack to get around an issue:
    pthreads requires a mutex to be unlocked for it to be destroyed.
    If anything is waiting on it when this unlock occurs (right before destruction) then it gets the lock before the mutex is destroyed
    this flag causes it to return immediately instead of trusting the mutex lock */
    bool destroyed;
} ConnectionData;

// result from reading from a socket (not used externally)
typedef enum {
    SUCCESS,
    ERROR,
    END
} ReadStatus;

static void free_connectiondata(ConnectionData *condata);

// ofsets past REALTIMESIGMIN
#define CONNECT_SIG 1
#define READ_SIG 2

// global read buffer
static GQueue *read_buff = NULL;
static pthread_mutex_t read_buff_mux = PTHREAD_MUTEX_INITIALIZER;

// global store of connections
static pthread_mutex_t connections_mux = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *connections_table = NULL;

// the listening socket
static int listen_socket = -1;

// the timer id
timer_t timer_id;

// helper for get_connected_list
static void list_ip_addrs(__attribute__((unused)) gpointer key, gpointer value, gpointer user_data) {
    assert(NULL != value);
    assert(NULL != user_data);

    ConnectionData *con_data = value;
    GSList **list = user_data;

    struct sockaddr_in *list_data = malloc(sizeof(struct sockaddr_in));
    assert(NULL != list_data);
    memcpy(list_data, &con_data->addr, sizeof(*list_data));

    *list = g_slist_prepend(*list, list_data); 
}

// returns a list containing all of the IP addresses in the connections table
GSList *get_connected_list(void) {
    GSList *ret = NULL;

    assert(0 == pthread_mutex_lock(&connections_mux));
    g_hash_table_foreach(connections_table, (GHFunc) list_ip_addrs, &ret);
    assert(0 == pthread_mutex_unlock(&connections_mux));

    return ret;
}

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
        printf("Unknown error errno=%s num_read=%li\n", strerror(e), num_read);
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
                printf("errno=%s. num_read=%li\n", strerror(e), num_read);
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
static ReadStatus fetch_item(ConnectionData *condata) {
    // the item we will add to the buffer for this read
    BufferItem *item = malloc(sizeof(BufferItem));
    if (NULL == item) {
        return ERROR;
    }

    // attempt to read in the json object
    GString *obj;
    ReadStatus status = read_json_object(condata->fd, &obj); // frees obj on error
    if (SUCCESS != status) {
        free(item);
        return status;
    }

    // decode JSON
    if (!decode_message(obj->str, &(item->msg))) {
        printf("decode error on: %s\n", obj->str);
        // report this BufferItem as a software error
        software_error(&(item->msg), "Could not decode message");
    }

    g_string_free(obj, true);

    if (KEEP_ALIVE == item->msg.type) {
        free_bufferitem(item);
        // update last_keep_alive
        condata->last_keep_alive = time(NULL);
    } else { // "real" messages
        item->address = condata->addr.sin_addr;
        item->recv_time = time(NULL);

        // add the item to the queue
        g_queue_push_tail(read_buff, (gpointer) item);
    }

    return SUCCESS;
}

static void destroy_connection(ConnectionData *condata) {
    // destroy the connection

    // get access to connections_table
    if (0 != pthread_mutex_lock(&connections_mux)) {
        puts("Couldn't remove item from hash table");
        return;
    }
    
    // calls free_connectiondata for us
    if (TRUE != g_hash_table_remove(connections_table, &(condata->fd))) {
        puts("Couldn't remove item from hash table");
        pthread_mutex_unlock(&connections_mux);
    }

    pthread_mutex_unlock(&connections_mux);
}

// read in an object waiting in a buffer
static void *object_reader(ConnectionData *condata) {
    // get exclusive access to read_buff
    if (0 != pthread_mutex_lock(&read_buff_mux)) {
        perror("object reader could not get the read_buff mutex");
        return NULL;
    }
    
    // read in every available object
    ReadStatus status = fetch_item(condata);
    if (ERROR == status) {
        puts("Read ERROR from remote host\n"); 
        destroy_connection(condata);
        pthread_mutex_unlock(&read_buff_mux);
        return NULL;
    } 
    
    // clean up before returning
    pthread_mutex_unlock(&read_buff_mux);
    pthread_mutex_unlock(&(condata->mutex));
    return NULL;
}

// for reporting a connection close
static void *report_close(ConnectionData *condata) {
    // allocate the item to go onto the queue
    BufferItem *item = malloc(sizeof(BufferItem));
    if (!item) {
        return NULL;
    }
    
    item->address = condata->addr.sin_addr;
    item->recv_time = time(NULL);
    
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

    destroy_connection(condata);
    return NULL;
}

// realtime signal handler for when IO is available on a connection with a client
static void io_handler(__attribute__ ((unused)) int sig, __attribute__ ((unused)) siginfo_t *si, __attribute__ ((unused)) void *ucontext) {
    /* locking mutexes in something which can interupt code which already has the mutex locked may seem to be begging for deadlock
     *   but in practice I was unable to reproduce this deadlock so we will do it here instead of a different thread (the old solution) for performance reasons
     */
    // look up the file descriptor in the connections table
    if (0 != pthread_mutex_lock(&connections_mux)) {
//        perror("Couldn't lock connections table");
        return;
    }

    ConnectionData *condata = g_hash_table_lookup(connections_table, &(si->si_fd));
    pthread_mutex_unlock(&connections_mux);
    if (NULL == condata) {
//        printf("%i not in table\n", si->si_fd);
        return;
    }
    assert(si->si_fd == condata->fd);

    // get the exclusive right to do reading as early as we can incase another thread closes the file descriptor
    if (0 != pthread_mutex_lock(&(condata->mutex))) {
//        perror("condatamux");
        return;
    }
    if (condata->destroyed) { // did we get between the unlock and destroy in free_condata?
//        puts("condatamux dead");
        return;
    }

    char buf;
    // check to see if there is any data to read
    // checking si->si_code just seems to report the connection is closed on every signal
    // MSG_PEEK so that the read character is not removed from the read buffer
    ssize_t count = recv(si->si_fd, &buf, 1, MSG_PEEK);
//    int e = errno;
    if (1 != count) {
//        printf("closed connection. fd=%i si_code=%i errno=%s count=%li\n",si->si_fd, si->si_code, strerror(e), count);

        report_close(condata);
        return;
    }

    // do the actual reading 
    object_reader((void *) condata);
}

// realtime signal handler for when there is a connection to the listening socket
static void connect_handler(__attribute__ ((unused)) int sig, siginfo_t *si, __attribute__ ((unused)) void *ucontext) {
    // accepts a connection from a remote host and sets it up to do signal driven IO
    int fd = accept(si->si_fd, NULL, NULL);
    if (-1 == fd)
        return;

    // add fd to the connections table:

    // allocate memory for the ConnectionData
    ConnectionData *condata = malloc(sizeof(ConnectionData));
    if (NULL == condata) {
        close(fd);
        return;
    }

    // get the remote address
    socklen_t addrlen = sizeof(condata->addr);
    if (0 != getpeername(fd, &(condata->addr), &addrlen)) {
        close(fd);
        free(condata);
        return;
    }
    // I found experimentally that getpeername only works on the second call. Tested on Ubuntu, Debian and CentOS
    if (0 != getpeername(fd, &(condata->addr), &addrlen)) {
        close(fd);
        free(condata);
        return;
    }
    char addr[160] = {'\n'}; // buffer to hold string-ified ip4 address
    inet_ntop(AF_INET, &(condata->addr.sin_addr.s_addr), addr, sizeof(addr));
    printf("Connect from %s\n", addr);

    // set up condata->mutex
    if (-1 == pthread_mutex_init(&(condata->mutex), NULL)) {
        close(fd);
        free(condata);
        return;
    }

    // set the last message time to now
    if (-1 == time(&(condata->last_keep_alive))) {
        close(fd);
        free_connectiondata(condata);
        return;
    }

    condata->fd = fd;
    condata->destroyed = false;

    // get access to connections table
    if (0 != pthread_mutex_lock(&connections_mux)) {
        close(fd);
        free_connectiondata(condata);
        return;
    }

    // g_hash_table needs the key to stick around
    int *key = malloc(sizeof(int));
    if (NULL == key) {
        close(fd);
        free_connectiondata(condata);
        return;
    }
    *key = fd;

    // put it into the connections table
    if (FALSE == g_hash_table_insert(connections_table, key, condata)) {
        puts("ERROR: duplicate entry in connections table!");
        exit(EXIT_FAILURE);
        return;
    }

    pthread_mutex_unlock(&connections_mux);

    setup_rt_signal_io(fd, SIGRTMIN + READ_SIG, io_handler);
}

// function to check if a keep alive message has been received for a given connection
static void check_keep_alive(__attribute__((unused)) gpointer key, gpointer value, __attribute__((unused)) gpointer user_data) {
    if (NULL == value)
        return;

    ConnectionData *condata = (ConnectionData *) value;

    // get current time
    time_t now = time(NULL);
    if (-1 == now)
        return;

    // calculate time difference
    time_t diff = now - condata->last_keep_alive;

    if (diff > (KEEP_ALIVE_PROD)) {
        char addr[16] = {'\n'}; // buffer to hold string-ified ip4 address
        inet_ntop(AF_INET, &(condata->addr.sin_addr.s_addr), addr, sizeof(addr));
        printf("No KEEP_ALIVE from %s (fd=%i) for %li seconds!\n", addr, condata->fd, diff);

        // report error 
        BufferItem *err = malloc(sizeof(BufferItem));
        if (NULL == err) {
            perror("Couldn't allocate message buffer");
            return;
        }
        software_error(&(err->msg), "Connection timeout");
        memcpy(&(err->address), &(condata->addr.sin_addr), sizeof(err->address));
        err->recv_time = time(NULL);

        if (0 != pthread_mutex_trylock(&read_buff_mux)) {
            perror("Couldn't lock read_buff_mux");
            free_bufferitem(err);
            return;
        }

        g_queue_push_tail(read_buff, err);
        pthread_mutex_unlock(&read_buff_mux);
    }
}

// called periodically to check if we have received a KEEP_ALIVE message recently
static void iter_keep_alives(__attribute__((unused)) void *compulsory) {
    // get lock on connections_table
    // only doing trylock because it doesn't matter if we skip this every so often
    if (0 != pthread_mutex_trylock(&connections_mux)) {
        return;
    }

    // check each connection
    g_hash_table_foreach(connections_table, check_keep_alive, NULL);

    pthread_mutex_unlock(&connections_mux);
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
    if (-1 == listen_socket) {
        perror("start_server: create socket");
        return false;
    }

    // bind to the specified address
    if (-1 == bind(listen_socket, addr, addrlen)) {
        close(listen_socket);
        perror("start_server: binding");
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

    // initialise the connections table
    connections_table = g_hash_table_new_full(g_int_hash, g_int_equal, (GDestroyNotify) free, (GDestroyNotify) free_connectiondata); 
    if (!connections_table) {
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
        g_hash_table_destroy(connections_table);
        connections_table = NULL;
        return false;
    }

    // set up keep_alive checker
    if (false == create_timer((timer_handler_t) iter_keep_alives, &timer_id, (KEEP_ALIVE_INTERVAL) * (KEEP_ALIVE_CHECK_PERIOD))) {
        close(listen_socket);
        listen_socket = -1;
        g_queue_free(read_buff);
        read_buff = NULL;
        g_hash_table_destroy(connections_table);
        connections_table = NULL;
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
    if (0 != pthread_mutex_lock(&read_buff_mux)) {
        perror("Can't lock read_buff_mux");
        return NULL;
    }

    BufferItem *ret = (BufferItem *) g_queue_pop_head(read_buff);
    // if this is NULL we should be returning NULL anyway

    pthread_mutex_unlock(&read_buff_mux);

    return ret;
}

// free a BufferItem (wrapper function incase it contains anyting that needs freeing interneally)
void free_bufferitem(BufferItem *item) {
    free_message(&(item->msg));
    free(item);
}

// free a ConnectionData
static void free_connectiondata(ConnectionData *condata) {
    condata->destroyed = true;
    pthread_mutex_unlock(&(condata->mutex));
    // if something jumps in here then it should check the destroyed flag
    errno = pthread_mutex_destroy(&(condata->mutex));
    if (0 != errno) {
        //perror("destroy condata mux");
    }
    close(condata->fd);
    free(condata);
}

static void do_nothing(__attribute__((unused)) int compulsory) {
    // literally do nothing
}

#define DISABLE_SIGNAL(_signal) \
    memset(&sa, 0, sizeof(sa)); \
    sa.sa_handler = do_nothing; \
    if (-1 == sigaction(_signal, &sa, NULL)) { \
        perror("Couldn't disable signal"); \
    } 


void stop_server(void) {
    // disable signal handlers
    struct sigaction sa;
    DISABLE_SIGNAL(SIGRTMIN + READ_SIG)
    DISABLE_SIGNAL(SIGRTMIN + CONNECT_SIG)

    // disable KEEP_ALIVE check
    stop_timer(timer_id);

    // close the open socket
    if (-1 != listen_socket) {
        close(listen_socket);
        listen_socket = -1;
    }

    // free up the connection table and close all the active connections
    if (connections_table) {
        pthread_mutex_lock(&connections_mux);
        g_hash_table_destroy(connections_table);
        connections_table = NULL;
        pthread_mutex_unlock(&connections_mux);
    }

    // free up the read buffer
    if (read_buff) {
        pthread_mutex_lock(&read_buff_mux);
        g_queue_free_full(read_buff, (GDestroyNotify) free_bufferitem);
        read_buff = NULL;
        pthread_mutex_unlock(&read_buff_mux);
    }

}