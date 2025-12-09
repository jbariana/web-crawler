#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include "path.h" // provides UrlNode and MaxUrlLength

/* Tunable */
#define VISITED_BUCKETS 8192
#define DEFAULT_THREADS 4
#define DEFAULT_MAX_PAGES 10000

/* --------------------- HTTP helper --------------------- */
typedef struct {
    char *data;
    size_t size;
} HttpResponse;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    HttpResponse *resp = (HttpResponse*)userdata;
    size_t total = size * nmemb;
    char *newbuf = realloc(resp->data, resp->size + total + 1);
    if (!newbuf) return 0; // make curl fail
    resp->data = newbuf;
    memcpy(resp->data + resp->size, ptr, total);
    resp->size += total;
    resp->data[resp->size] = '\0';
    return total;
}

static HttpResponse fetch_url_curl(const char *url, CURL *easy) {
    HttpResponse resp = { .data = NULL, .size = 0 };
    curl_easy_reset(easy);
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "wiki-crawler/1.0");
    CURLcode rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[curl error] %s -> %s\n", url, curl_easy_strerror(rc));
        if (resp.data) { free(resp.data); resp.data = NULL; resp.size = 0; }
    }
    return resp;
}

/* --------------------- Basic HTML link extractor ---------------------
   Very simple: looks for href="/wiki/..." or full absolute URLs containing "/wiki/".
   Returns a temporary linked list of UrlNode (caller must free via free_temp_links).
--------------------------------------------------------------------- */
void extract_links(const char *html, UrlNode **head) {
    if (!html) return;
    const char *p = html;
    while ((p = strstr(p, "href=")) != NULL) {
        p += 5;
        while (*p && isspace((unsigned char)*p)) p++;
        char q = '\0';
        if (*p == '"' || *p == '\'') { q = *p; p++; }

        const char *href = p;
        const char *end = NULL;
        if (q) end = strchr(href, q);
        else {
            end = href;
            while (*end && *end != ' ' && *end != '>') end++;
        }
        if (!end) break;
        size_t len = end - href;
        if (len == 0) { p = end; continue; }

        /* Only accept links that contain /wiki/ near the start or absolute with /wiki/ */
        if ((href[0] == '/' && strncmp(href, "/wiki/", 6) == 0) ||
            (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0)) {
            /* if absolute, ensure /wiki/ appears */
            if (!(href[0] == '/' || strstr(href, "/wiki/"))) { p = end; continue; }

            size_t copy_len = len < MaxUrlLength-1 ? len : MaxUrlLength-1;
            UrlNode *n = malloc(sizeof(UrlNode));
            if (!n) break;
            memset(n, 0, sizeof(UrlNode));
            strncpy(n->url, href, copy_len);
            n->url[copy_len] = '\0';
            n->depth = 0;
            n->parent = NULL;
            n->next = *head;
            *head = n;
        }
        p = end;
    }
}

void free_temp_links(UrlNode *head) {
    while (head) {
        UrlNode *t = head;
        head = head->next;
        free(t);
    }
}

/* --------------------- Visited set (simple hashtable) --------------------- */
typedef struct VisNode {
    char url[MaxUrlLength];
    struct VisNode *next;
} VisNode;

typedef struct {
    VisNode *buckets[VISITED_BUCKETS];
    pthread_mutex_t lock;
} VisitedSet;

static unsigned long djb2(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

void visited_init(VisitedSet *vs) {
    for (int i = 0; i < VISITED_BUCKETS; ++i) vs->buckets[i] = NULL;
    pthread_mutex_init(&vs->lock, NULL);
}

int visited_contains_locked(VisitedSet *vs, const char *url) {
    unsigned long idx = djb2(url) % VISITED_BUCKETS;
    VisNode *cur = vs->buckets[idx];
    while (cur) {
        if (strcmp(cur->url, url) == 0) return 1;
        cur = cur->next;
    }
    return 0;
}

int visited_contains(VisitedSet *vs, const char *url) {
    pthread_mutex_lock(&vs->lock);
    int res = visited_contains_locked(vs, url);
    pthread_mutex_unlock(&vs->lock);
    return res;
}

void visited_insert(VisitedSet *vs, const char *url) {
    if (!url) return;
    pthread_mutex_lock(&vs->lock);
    if (visited_contains_locked(vs, url)) { pthread_mutex_unlock(&vs->lock); return; }
    unsigned long idx = djb2(url) % VISITED_BUCKETS;
    VisNode *node = malloc(sizeof(VisNode));
    if (!node) { pthread_mutex_unlock(&vs->lock); return; }
    strncpy(node->url, url, MaxUrlLength-1);
    node->url[MaxUrlLength-1] = '\0';
    node->next = vs->buckets[idx];
    vs->buckets[idx] = node;
    pthread_mutex_unlock(&vs->lock);
}

/* --------------------- Thread-safe queue --------------------- */
typedef struct {
    UrlNode *head;
    UrlNode *tail;
    int size;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} UrlQueue;

void queue_init(UrlQueue *q) {
    q->head = q->tail = NULL;
    q->size = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void queue_push(UrlQueue *q, UrlNode *n) {
    n->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail == NULL) q->head = q->tail = n;
    else { q->tail->next = n; q->tail = n; }
    q->size++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/* Wait for an item or until should_stop becomes non-zero. Returns a node (caller must free or use). */
UrlNode *queue_pop_wait(UrlQueue *q, int *should_stop) {
    pthread_mutex_lock(&q->lock);
    while (q->head == NULL && !*should_stop) {
        pthread_cond_wait(&q->cond, &q->lock);
    }
    UrlNode *n = NULL;
    if (q->head) {
        n = q->head;
        q->head = n->next;
        if (q->head == NULL) q->tail = NULL;
        n->next = NULL;
        q->size--;
    }
    pthread_mutex_unlock(&q->lock);
    return n;
}

/* --------------------- URL utilities --------------------- */
/* Build absolute URL: if link starts with '/', prefix origin; otherwise copy link */
void build_full_url(const char *origin, const char *link, char *out, size_t out_size) {
    if (!link || !out) return;
    if (link[0] == '/') {
        size_t olen = strlen(origin);
        size_t llen = strlen(link);
        if (olen + llen >= out_size) {
            strncpy(out, origin, out_size-1);
            out[out_size-1] = '\0';
            return;
        }
        strcpy(out, origin);
        strcat(out, link);
    } else {
        strncpy(out, link, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Remove query and fragment in-place */
void strip_query_fragment(char *s) {
    char *p = s;
    while (*p) {
        if (*p == '#' || *p == '?') { *p = '\0'; return; }
        p++;
    }
}

/* Extract wiki title (text after /wiki/). Returns 1 if found. */
int wiki_title_from_url(const char *url, char *out, size_t out_size) {
    const char *p = strstr(url, "/wiki/");
    if (!p) return 0;
    p += 6;
    size_t i = 0;
    while (*p && *p != '#' && *p != '?' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* Canonicalize: strip query/fragment and trailing slash */
void canonicalize_url(char *buf) {
    strip_query_fragment(buf);
    size_t n = strlen(buf);
    if (n > 1 && buf[n-1] == '/') buf[n-1] = '\0';
}

/* --------------------- Controller & state --------------------- */
typedef struct {
    UrlQueue queue;
    VisitedSet visited;
    char origin[MaxUrlLength];      /* e.g. https://en.wikipedia.org */
    char target_title[MaxUrlLength];/* title extracted from finish URL */
    int max_depth;
    int max_pages;
    int pages_visited;
    pthread_mutex_t pages_lock;

    int found;
    UrlNode *found_node;
    pthread_mutex_t found_lock;

    int should_stop;
    pthread_mutex_t stop_lock;

    int active; /* number of threads currently processing a node */
    pthread_mutex_t active_lock;
} Controller;

static Controller ctrl;

void set_stop_flag(int v) {
    pthread_mutex_lock(&ctrl.stop_lock);
    ctrl.should_stop = v;
    pthread_mutex_unlock(&ctrl.stop_lock);
    /* wake all waiting workers */
    pthread_mutex_lock(&ctrl.queue.lock);
    pthread_cond_broadcast(&ctrl.queue.cond);
    pthread_mutex_unlock(&ctrl.queue.lock);
}

int get_stop_flag(void) {
    int v;
    pthread_mutex_lock(&ctrl.stop_lock);
    v = ctrl.should_stop;
    pthread_mutex_unlock(&ctrl.stop_lock);
    return v;
}

void mark_found(UrlNode *node) {
    pthread_mutex_lock(&ctrl.found_lock);
    ctrl.found = 1;
    ctrl.found_node = node;
    pthread_mutex_unlock(&ctrl.found_lock);
    set_stop_flag(1);
}

int increment_pages_visited(void) {
    pthread_mutex_lock(&ctrl.pages_lock);
    ctrl.pages_visited++;
    int v = ctrl.pages_visited;
    pthread_mutex_unlock(&ctrl.pages_lock);
    return v;
}

/* --------------------- Worker thread --------------------- */
void *worker_main(void *arg) {
    (void)arg;
    CURL *easy = curl_easy_init();
    if (!easy) {
        fprintf(stderr, "[thread] curl_easy_init failed\n");
        return NULL;
    }

    while (!get_stop_flag()) {
        UrlNode *cur = queue_pop_wait(&ctrl.queue, &ctrl.should_stop);
        if (!cur) {
            if (get_stop_flag()) break;
            continue;
        }

        /* Book the active slot */
        pthread_mutex_lock(&ctrl.active_lock);
        ctrl.active++;
        pthread_mutex_unlock(&ctrl.active_lock);

        /* skip over-depth nodes */
        if (cur->depth > ctrl.max_depth) {
            pthread_mutex_lock(&ctrl.active_lock);
            ctrl.active--;
            /* if queue empty and no active, stop */
            pthread_mutex_lock(&ctrl.queue.lock);
            int empty = (ctrl.queue.head == NULL);
            pthread_mutex_unlock(&ctrl.queue.lock);
            if (empty && ctrl.active == 0) set_stop_flag(1);
            pthread_mutex_unlock(&ctrl.active_lock);
            continue;
        }

        int pv = increment_pages_visited();
        if (pv > ctrl.max_pages) {
            set_stop_flag(1);
            /* release active */
            pthread_mutex_lock(&ctrl.active_lock);
            ctrl.active--;
            pthread_mutex_unlock(&ctrl.active_lock);
            break;
        }

        /* quick local print log */
        fprintf(stderr, "[visit %d] %s (depth %d)\n", pv, cur->url, cur->depth);

        /* fetch page */
        HttpResponse resp = fetch_url_curl(cur->url, easy);
        if (!resp.data || resp.size == 0) {
            if (resp.data) { free(resp.data); resp.data = NULL; resp.size = 0; }
            /* release active */
            pthread_mutex_lock(&ctrl.active_lock);
            ctrl.active--;
            pthread_mutex_unlock(&ctrl.active_lock);
            /* check termination */
            pthread_mutex_lock(&ctrl.queue.lock);
            int empty = (ctrl.queue.head == NULL);
            pthread_mutex_unlock(&ctrl.queue.lock);
            if (empty && ctrl.active == 0) set_stop_flag(1);
            continue;
        }

        /* get effective URL (after redirects) and compare canonical titles */
        char *effective = NULL;
        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effective);
        if (effective) {
            char eff_copy[MaxUrlLength];
            strncpy(eff_copy, effective, MaxUrlLength-1); eff_copy[MaxUrlLength-1] = '\0';
            canonicalize_url(eff_copy);
            char eff_title[MaxUrlLength];
            if (wiki_title_from_url(eff_copy, eff_title, sizeof(eff_title))) {
                if (strcmp(eff_title, ctrl.target_title) == 0) {
                    /* found target */
                    mark_found(cur);
                    free(resp.data);
                    break;
                }
            }
        }

        /* extract links */
        UrlNode *links = NULL;
        extract_links(resp.data, &links);

        /* For each link: normalize, same-origin check, visited check, enqueue */
        for (UrlNode *ln = links; ln != NULL; ln = ln->next) {
            char full[MaxUrlLength];
            build_full_url(ctrl.origin, ln->url, full, sizeof(full));
            canonicalize_url(full);

            /* only follow same origin (e.g., https://en.wikipedia.org) */
            if (strncmp(full, ctrl.origin, strlen(ctrl.origin)) != 0) continue;

            /* check discovered title for quick match */
            char title[MaxUrlLength];
            if (wiki_title_from_url(full, title, sizeof(title))) {
                if (strcmp(title, ctrl.target_title) == 0) {
                    /* found by link before fetching */
                    UrlNode *found = malloc(sizeof(UrlNode));
                    if (found) {
                        memset(found, 0, sizeof(UrlNode));
                        strncpy(found->url, full, MaxUrlLength-1);
                        found->depth = cur->depth + 1;
                        found->parent = cur;
                    }
                    mark_found(found ? found : cur);
                    break;
                }
            }

            /* enqueue if not visited */
            if (!visited_contains(&ctrl.visited, full)) {
                UrlNode *child = malloc(sizeof(UrlNode));
                if (!child) continue;
                memset(child, 0, sizeof(UrlNode));
                strncpy(child->url, full, MaxUrlLength-1);
                child->depth = cur->depth + 1;
                child->parent = cur;
                child->next = NULL;
                visited_insert(&ctrl.visited, child->url);
                queue_push(&ctrl.queue, child);
            }
        } /* end for links */

        free_temp_links(links);
        free(resp.data);

        /* Release active slot and check termination condition */
        pthread_mutex_lock(&ctrl.active_lock);
        ctrl.active--;
        pthread_mutex_lock(&ctrl.queue.lock);
        int empty = (ctrl.queue.head == NULL);
        pthread_mutex_unlock(&ctrl.queue.lock);
        if (empty && ctrl.active == 0 && !ctrl.found) set_stop_flag(1);
        pthread_mutex_unlock(&ctrl.active_lock);

        if (ctrl.found) break;
    } /* end while */

    curl_easy_cleanup(easy);
    return NULL;
}

/* --------------------- Program entry --------------------- */
void print_usage(const char *prog) {
    fprintf(stderr, "USAGE: %s <start_url> <finish_url> <max_depth> [num_threads] [max_pages]\n", prog);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *start_url = argv[1];
    const char *finish_url = argv[2];
    int max_depth = atoi(argv[3]);
    int num_threads = (argc >= 5) ? atoi(argv[4]) : DEFAULT_THREADS;
    int max_pages = (argc >= 6) ? atoi(argv[5]) : DEFAULT_MAX_PAGES;
    if (num_threads <= 0) num_threads = DEFAULT_THREADS;
    if (max_depth < 0) max_depth = 0;

    /* initialize controller */
    memset(&ctrl, 0, sizeof(ctrl));
    queue_init(&ctrl.queue);
    visited_init(&ctrl.visited);
    pthread_mutex_init(&ctrl.pages_lock, NULL);
    pthread_mutex_init(&ctrl.found_lock, NULL);
    pthread_mutex_init(&ctrl.stop_lock, NULL);
    pthread_mutex_init(&ctrl.active_lock, NULL);

    ctrl.max_depth = max_depth;
    ctrl.max_pages = max_pages;
    ctrl.pages_visited = 0;
    ctrl.found = 0;
    ctrl.found_node = NULL;
    ctrl.should_stop = 0;
    ctrl.active = 0;

    /* build origin from start_url: scheme://host */
    {
        char tmp[MaxUrlLength];
        strncpy(tmp, start_url, MaxUrlLength-1);
        tmp[MaxUrlLength-1] = '\0';
        char *p = strstr(tmp, "://");
        if (p) {
            p += 3;
            char *slash = strchr(p, '/');
            if (slash) *slash = '\0';
        }
        /* tmp now "scheme://host" or original if no scheme */
        strncpy(ctrl.origin, tmp, MaxUrlLength-1);
        ctrl.origin[MaxUrlLength-1] = '\0';
    }

    /* canonicalize finish and extract target title */
    char target_copy[MaxUrlLength];
    strncpy(target_copy, finish_url, MaxUrlLength-1);
    target_copy[MaxUrlLength-1] = '\0';
    canonicalize_url(target_copy);
    if (!wiki_title_from_url(target_copy, ctrl.target_title, sizeof(ctrl.target_title))) {
        fprintf(stderr, "Target must be a /wiki/ page: %s\n", finish_url);
        return 1;
    }

    /* libcurl global init */
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    /* push start node */
    UrlNode *start = malloc(sizeof(UrlNode));
    if (!start) { fprintf(stderr, "malloc failed\n"); return 1; }
    memset(start, 0, sizeof(UrlNode));
    strncpy(start->url, start_url, MaxUrlLength-1);
    start->url[MaxUrlLength-1] = '\0';
    start->depth = 0;
    start->parent = NULL;
    visited_insert(&ctrl.visited, start->url);
    queue_push(&ctrl.queue, start);

    /* create worker threads */
    pthread_t *tids = malloc(sizeof(pthread_t) * num_threads);
    if (!tids) { fprintf(stderr, "malloc failed\n"); return 1; }
    for (int i = 0; i < num_threads; ++i) {
        if (pthread_create(&tids[i], NULL, worker_main, NULL) != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
            set_stop_flag(1);
        }
    }

    /* wait for workers to finish */
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(tids[i], NULL);
    }
    free(tids);

    /* result */
    if (ctrl.found && ctrl.found_node) {
        printf("Found target URL. Reconstructing path:\n");
        Path_print(ctrl.found_node);
    } else {
        printf("Target not found within depth %d (pages visited: %d)\n", ctrl.max_depth, ctrl.pages_visited);
    }

    curl_global_cleanup();
    return 0;
}
