#include <stdio.h>
#include "csapp.h"
#include <bits/pthreadtypes.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_NUM 10
#define Assert(expression) if (!(expression)) { printf("Assertion failure in %s, line %d\n", __FUNCTION__, __LINE__); return; }
#define SBUF_SIZE 25
#define THREAD_NUM 16
/* You won't lose style points for including this long line in your code */
static const char* user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct {
    char host[MAXLINE], port[MAXLINE], uri[MAXLINE];
    uint32_t hashval;
}URL;
const uint32_t base = 31;
typedef struct {
    char obj[MAX_OBJECT_SIZE], host[MAXLINE], port[MAXLINE], uri[MAXLINE];
    uint32_t hashval, LRU, read_cnt;
    int empty;
    size_t size;
    sem_t w, mutex;
}Cache;
Cache cache[CACHE_NUM];
sem_t t_mutex;
uint64_t timestamp;

typedef struct {
    int* buf;          /* Buffer array */
    int n;             /* Maximum number of slots */
    int front;         /* buf[(front+1)%n] is first item */
    int rear;          /* buf[rear%n] is last item */
    sem_t mutex;       /* Protects accesses to buf */
    sem_t slots;       /* Counts available slots */
    sem_t items;       /* Counts available items */
} sbuf_t;
sbuf_t sbuf;

void sigint_handler(int sig);
void cache_init();
void read_lock(int i);
void read_unlock(int i);
void write_lock(int i);
void write_unlock(int i);
uint64_t get_timestamp();
int cache_find(URL* url);
void cache_insert(URL* url, char* cache_s, size_t size);
void get_hash(URL* url);
void parse_url(URL* url, char* str);
void build_header(char* http_hdr, URL* url, rio_t* rio_p);
void Solve(int connfd);
void* mythread(void* arg);
void sbuf_init(sbuf_t* sp, int n);
void sbuf_deinit(sbuf_t* sp);
void sbuf_insert(sbuf_t* sp, int item);
int sbuf_remove(sbuf_t* sp);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    sbuf_init(&sbuf, SBUF_SIZE);
    cache_init();
    int listenfd = Open_listenfd(argv[1]);
    printf("Proxy server is running on port %s ...\n", argv[1]);
    pthread_t tid;
    for (int i = 0; i < THREAD_NUM; ++i) {
        Pthread_create(&tid, NULL, mythread, NULL);
    }
    struct sockaddr_storage clientaddr;
    socklen_t clientlen = sizeof(struct sockaddr_storage);
    char host[MAXLINE], port[MAXLINE];
    while (1) {
        int connfd = Accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
        Getnameinfo((struct sockaddr*)&clientaddr, clientlen, host, MAXLINE, port, MAXLINE, 0);
        printf("Accept connection from %s:%s\n", host, port);
    }
    return 0;
}

void sigint_handler(int sig) {
    sbuf_deinit(&sbuf);
}

void cache_init() {
    Sem_init(&t_mutex, 0, 1);
    for (int i = 0; i < CACHE_NUM; ++i) {
        cache[i].empty = 1;
        Sem_init(&cache[i].w, 0, 1);
        Sem_init(&cache[i].mutex, 0, 1);
    }
}

void read_lock(int i) {
    P(&cache[i].mutex);
    if (++cache[i].read_cnt == 1)
        P(&cache[i].w);
    V(&cache[i].mutex);
}

void read_unlock(int i) {
    P(&cache[i].mutex);
    if (--cache[i].read_cnt == 0)
        V(&cache[i].w);
    V(&cache[i].mutex);
}

void write_lock(int i) {
    P(&cache[i].w);
}

void write_unlock(int i) {
    V(&cache[i].w);
}

uint64_t get_timestamp() {
    P(&t_mutex);
    int ret = ++timestamp;
    if (ret == 0) {
        for (int i = 0; i < CACHE_NUM; ++i) {
            read_lock(i);
            cache[i].LRU = 0;
            read_unlock(i);
        }
        ret = ++timestamp;
    }
    V(&t_mutex);
    return ret;
}

int cache_find(URL* url) {
    int ret = -1;
    for (int i = 0; i < CACHE_NUM; ++i) {
        read_lock(i);
        if (cache[i].empty || cache[i].hashval != url->hashval) {
            read_unlock(i);
            continue;
        }
        if (!strcmp(url->host, cache[i].host) && !strcmp(url->port, cache[i].port) && !strcmp(url->uri, cache[i].uri)) {
            cache[i].LRU = get_timestamp();
            ret = i;
            break;
        }
        read_unlock(i);
    }
    return ret;
}

void cache_insert(URL* url, char* cache_s, size_t size) {
    uint64_t minv = UINT64_MAX; int idx = -1;
    for (int i = 0; i < CACHE_NUM; ++i) {
        read_lock(i);
        if (cache[i].empty) {
            idx = i;
            read_unlock(i);
            break;
        }
        if (cache[i].LRU < minv) {
            minv = cache[i].LRU;
            idx = i;
        }
        read_unlock(i);
    }
    Assert(idx != -1);
    write_lock(idx);
    cache[idx].empty = 0;
    cache[idx].hashval = url->hashval;
    strcpy(cache[idx].host, url->host);
    strcpy(cache[idx].port, url->port);
    strcpy(cache[idx].uri, url->uri);
    memcpy(cache[idx].obj, cache_s, size);
    cache[idx].size = size;
    cache[idx].LRU = get_timestamp();
    write_unlock(idx);
}

void get_hash(URL* url) {
    uint32_t hashval = 0;
    int n = strlen(url->host);
    for (int i = 0; i < n; ++i)
        hashval = hashval * base + (uint32_t)url->host[i];
    n = strlen(url->port);
    for (int i = 0; i < n; ++i)
        hashval = hashval * base + (uint32_t)url->port[i];
    n = strlen(url->uri);
    for (int i = 0; i < n; ++i)
        hashval = hashval * base + (uint32_t)url->uri[i];
    url->hashval = hashval;
}

void parse_url(URL* url, char* str) {
    printf("url: %s\n", str);
    int n = strlen(str), l = 1;
    Assert(n > 1);
    while ((str[l - 1] != '/' || str[l] != '/') && l + 1 < n) ++l;
    if (!(str[l - 1] == '/' && str[l] == '/')) l = -1;
    int r = l;
    while (r + 1 < n && str[r + 1] != '/' && str[r + 1] != '?' && str[r + 1] != ':') ++r;
    memcpy(url->host, str + l + 1, r - l);
    url->host[r - l] = '\0';
    l = r;
    if (r + 1 < n && str[r + 1] == ':') {
        l = r = r + 1;
        while (r + 1 < n && str[r + 1] != '/' && str[r + 1] != '?') ++r;
        memcpy(url->port, str + l + 1, r - l);
        url->port[r - l] = '\0';
        l = r;
    }
    else {
        strcpy(url->port, "80");
    }
    if (r + 1 < n) strcpy(url->uri, str + r + 1);
    else strcpy(url->uri, "/");
}

void build_header(char* http_hdr, URL* url, rio_t* rio_p) {
    static char* conn_hdr = "Connection: close\r\n";
    static char* proxy_hdr = "Proxy-Connection: close\r\n";
    char host_hdr[MAXLINE], method_hdr[MAXLINE], buf[MAXLINE], other_hdr[MAXLINE];
    sprintf(host_hdr, "Host: %s\r\n", url->host);
    sprintf(method_hdr, "GET %s HTTP/1.0\r\n", url->uri);
    int cnt = 0;
    while (Rio_readlineb(rio_p, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break;
        if (!strncasecmp(buf, "Host:", 5)) {
            strcpy(host_hdr, buf);
        }
        else
            if (strncasecmp(buf, "Connection:", 11) && strncasecmp(buf, "Proxy-Connection:", 17) && strncasecmp(buf, "User-Agent:", 11)) {
                cnt += sprintf(other_hdr + cnt, "%s", buf);
            }
    }
    sprintf(http_hdr, "%s%s%s%s%s%s\r\n", method_hdr, host_hdr, conn_hdr, proxy_hdr, user_agent_hdr, other_hdr);
}

void Solve(int connfd) {
    char buf[MAXLINE], method[MAXLINE], url_s[MAXLINE], version[MAXLINE], http_hdr[MAXLINE], cache_s[MAX_OBJECT_SIZE];
    rio_t rio, host_rio;

    Rio_readinitb(&rio, connfd);
    int ret = Rio_readlineb(&rio, buf, MAXLINE);
    Assert(ret > 0);
    printf("method_hdr: %s", buf);
    sscanf(buf, "%s%s%s", method, url_s, version);

    URL url;
    parse_url(&url, url_s);
    get_hash(&url);

    int idx = cache_find(&url);
    if (idx != -1) {
        Rio_writen(connfd, cache[idx].obj, cache[idx].size);
        read_unlock(idx);
        printf("Cache hit and proxy transfer %zu bytes\n", cache[idx].size);
        return;
    }

    build_header(http_hdr, &url, &rio);

    int hostfd = Open_clientfd(url.host, url.port);
    if (hostfd < 0) {
        printf("Fail to connect %s:%s\n", url.host, url.port);
        return;
    }

    Rio_readinitb(&host_rio, hostfd);
    Rio_writen(hostfd, http_hdr, strlen(http_hdr));

    int n = 0; size_t tot = 0;
    while ((n = Rio_readlineb(&host_rio, buf, MAXLINE)) > 0) {
        Rio_writen(connfd, buf, n);
        printf("Proxy transfer %d bytes\n", n);
        if (tot + n <= MAX_OBJECT_SIZE)
            memcpy(cache_s + tot, buf, n);
        tot += n;
    }
    Close(hostfd);

    if (tot > 0 && tot <= MAX_OBJECT_SIZE)
        cache_insert(&url, cache_s, tot);
}

void* mythread(void* arg) {
    Pthread_detach(Pthread_self());

    while (1) {
        int connfd = sbuf_remove(&sbuf);
        Solve(connfd);
        Close(connfd);
    }

    pthread_exit(NULL);
}

void sbuf_init(sbuf_t* sp, int n)
{
    sp->buf = Calloc(n, sizeof(int));
    sp->n = n;                       /* Buffer holds max of n items */
    sp->front = sp->rear = 0;        /* Empty buffer iff front == rear */
    Sem_init(&sp->mutex, 0, 1);      /* Binary semaphore for locking */
    Sem_init(&sp->slots, 0, n);      /* Initially, buf has n empty slots */
    Sem_init(&sp->items, 0, 0);      /* Initially, buf has zero data items */
}
/* $end sbuf_init */

/* Clean up buffer sp */
/* $begin sbuf_deinit */
void sbuf_deinit(sbuf_t* sp)
{
    Free(sp->buf);
}
/* $end sbuf_deinit */

/* Insert item onto the rear of shared buffer sp */
/* $begin sbuf_insert */
void sbuf_insert(sbuf_t* sp, int item)
{
    P(&sp->slots);                          /* Wait for available slot */
    P(&sp->mutex);                          /* Lock the buffer */
    sp->buf[(++sp->rear) % (sp->n)] = item;   /* Insert the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->items);                          /* Announce available item */
}
/* $end sbuf_insert */

/* Remove and return the first item from buffer sp */
/* $begin sbuf_remove */
int sbuf_remove(sbuf_t* sp)
{
    int item;
    P(&sp->items);                          /* Wait for available item */
    P(&sp->mutex);                          /* Lock the buffer */
    item = sp->buf[(++sp->front) % (sp->n)];  /* Remove the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->slots);                          /* Announce available slot */
    return item;
}
