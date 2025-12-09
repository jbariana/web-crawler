/* crawler.c
 * Minimal single-threaded BFS web crawler for presentation (Option A)
 *
 * Usage:
 *   ./crawler <start_url> <finish_url> <max_depth>
 *
 * Notes:
 *  - Uses libcurl (fetch_url)
 *  - extract_links currently finds "/wiki/..." links (keeps your original)
 *  - Converts relative links (starting with '/') to absolute using start URL origin
 *  - Simple visited set (hash table)
 *  - Simple queue for BFS using UrlNode from path.h
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <curl/curl.h> // libcurl
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "path.h" // UrlNode and Path_print

#define MaxUrlLength 512
#define VISITED_BUCKETS 1000
#define MAX_PAGES_VISIT 1000

 /* ------------------------ Provided HTTP fetch + parsing helpers ------------------------ */

 /* HttpResponse and write_callback - same logic you supplied */
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    HttpResponse* resp = (HttpResponse*)userdata;
    size_t total = size * nmemb;
    char* new_data = realloc(resp->data, resp->size + total + 1); // +1 for null terminator
    if (!new_data) {
        return 0;
    }
    resp->data = new_data;
    memcpy(resp->data + resp->size, ptr, total);
    resp->size += total;
    resp->data[resp->size] = '\0'; // keep data null-terminated
    return total;
}

HttpResponse fetch_url(const char* url)
{
    CURL* curl;
    CURLcode res;
    HttpResponse response;
    response.data = NULL;
    response.size = 0;

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl\n");
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "crawler-demo/1.0");

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl error for %s: %s\n", url, curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    return response;
}

/* extract_links - your original simple wiki-style parser
 * It pushes relative "/wiki/..." (and potentially absolute) into a linked list head.
 * The returned nodes contain node->url set to the found href (as discovered).
 * We will later normalize to full URLs.
 */
void extract_links(const char* html, UrlNode** head) {
    if (!html) return;
    const char* p = html;
    while ((p = strstr(p, "<a href=\"/wiki/")) != NULL) {
        p += strlen("<a href=\""); // move past <a href="
        const char* end = strchr(p, '"');
        if (!end) break;
        size_t len = end - p;
        if (len >= MaxUrlLength) len = MaxUrlLength - 1;

        UrlNode* node = malloc(sizeof(UrlNode));
        if (!node) break;
        strncpy(node->url, p, len);
        node->url[len] = '\0';
        node->parent = NULL;
        node->depth = 0;
        node->next = *head;
        *head = node;
        p = end;
    }
}

/* ------------------------ Queue implementation (UrlQueue) ------------------------ */

typedef struct {
    UrlNode* head;
    UrlNode* tail;
    int size;
} SimpleUrlQueue;

void queue_init(SimpleUrlQueue* q) {
    q->head = q->tail = NULL;
    q->size = 0;
}

int queue_is_empty(SimpleUrlQueue* q) {
    return q->size == 0;
}

void queue_push(SimpleUrlQueue* q, UrlNode* node) {
    node->next = NULL;
    if (q->tail == NULL) {
        q->head = q->tail = node;
    }
    else {
        q->tail->next = node;
        q->tail = node;
    }
    q->size++;
}

UrlNode* queue_pop(SimpleUrlQueue* q) {
    if (q->head == NULL) return NULL;
    UrlNode* node = q->head;
    q->head = node->next;
    if (q->head == NULL) q->tail = NULL;
    node->next = NULL; // detach
    q->size--;
    return node;
}

/* ------------------------ Visited set (simple hash table) ------------------------ */

typedef struct VisitedNode {
    char url[MaxUrlLength];
    struct VisitedNode* next;
} VisitedNode;

typedef struct {
    VisitedNode* buckets[VISITED_BUCKETS];
} VisitedSet;

/* djb2 hash */
static unsigned long djb2(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

void visited_init(VisitedSet* vs) {
    for (int i = 0; i < VISITED_BUCKETS; i++) vs->buckets[i] = NULL;
}

int visited_contains(VisitedSet* vs, const char* url) {
    unsigned long h = djb2(url) % VISITED_BUCKETS;
    VisitedNode* cur = vs->buckets[h];
    while (cur) {
        if (strcmp(cur->url, url) == 0) return 1;
        cur = cur->next;
    }
    return 0;
}

void visited_insert(VisitedSet* vs, const char* url) {
    if (visited_contains(vs, url)) return;
    unsigned long h = djb2(url) % VISITED_BUCKETS;
    VisitedNode* node = malloc(sizeof(VisitedNode));
    if (!node) return;
    strncpy(node->url, url, MaxUrlLength - 1);
    node->url[MaxUrlLength - 1] = '\0';
    node->next = vs->buckets[h];
    vs->buckets[h] = node;
}

/* ------------------------ URL utility: extract origin (scheme + host) ------------------------ */

/* Given "https://en.wikipedia.org/wiki/Apple" -> produce "https://en.wikipedia.org" */
void get_origin(const char* url, char* out_origin, size_t out_size) {
    // Find "://" then the next '/' after host
    const char* p = strstr(url, "://");
    if (!p) {
        // fallback: treat whole url as origin up to first '/'
        const char* s = strchr(url, '/');
        if (!s) {
            strncpy(out_origin, url, out_size - 1);
            out_origin[out_size - 1] = '\0';
            return;
        }
        else {
            size_t len = s - url;
            if (len >= out_size) len = out_size - 1;
            strncpy(out_origin, url, len);
            out_origin[len] = '\0';
            return;
        }
    }
    p += 3; // move past ://
    const char* slash = strchr(p, '/');
    size_t len;
    if (!slash) {
        len = strlen(url);
    }
    else {
        len = slash - url;
    }
    if (len >= out_size) len = out_size - 1;
    strncpy(out_origin, url, len);
    out_origin[len] = '\0';
}

/* Build absolute url: if link starts with '/', prefix origin; otherwise copy link */
void build_full_url(const char* origin, const char* link, char* out, size_t out_size) {
    if (link[0] == '/') {
        // join origin + link
        size_t olen = strlen(origin);
        size_t llen = strlen(link);
        if (olen + llen >= out_size) {
            // truncate
            size_t copy_len = out_size - 1;
            strncpy(out, origin, copy_len);
            out[copy_len] = '\0';
            return;
        }
        strcpy(out, origin);
        strcat(out, link);
    }
    else {
        // copy link directly (may be absolute already)
        strncpy(out, link, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Free a linked list of UrlNode used by extract_links (these were temporary) */
void free_temp_links(UrlNode* head) {
    while (head) {
        UrlNode* tmp = head;
        head = head->next;
        free(tmp);
    }
}

/* ------------------------ Help message ------------------------ */
void help_message() {
    printf("Usage: crawler <start_url> <finish_url> <depth>\n");
    printf("Example: ./crawler https://en.wikipedia.org/wiki/Apple https://en.wikipedia.org/wiki/Orange 3\n");
}

/* ------------------------ BFS crawler (single-threaded) ------------------------ */

int main(int argc, char* argv[])
{
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        help_message();
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "Error: Invalid number of arguments.\n");
        help_message();
        return 1;
    }

    const char* start_url = argv[1];
    const char* finish_url = argv[2];
    int max_depth = atoi(argv[3]);
    if (max_depth < 0) max_depth = 0;

    // origin for building absolute links from relative ones
    char origin[MaxUrlLength];
    get_origin(start_url, origin, sizeof(origin));

    // Initialize queue and visited
    SimpleUrlQueue queue;
    queue_init(&queue);
    VisitedSet visited;
    visited_init(&visited);

    // create start node
    UrlNode* start = malloc(sizeof(UrlNode));
    if (!start) {
        fprintf(stderr, "Memory allocation failed for start node\n");
        return 1;
    }
    strncpy(start->url, start_url, MaxUrlLength - 1);
    start->url[MaxUrlLength - 1] = '\0';
    start->depth = 0;
    start->parent = NULL;
    start->next = NULL;

    queue_push(&queue, start);
    visited_insert(&visited, start->url);

    int pages_visited = 0;
    UrlNode* found_node = NULL;

    while (!queue_is_empty(&queue) && pages_visited < MAX_PAGES_VISIT) {
        UrlNode* cur = queue_pop(&queue);
        if (!cur) break;

        // skip nodes deeper than allowed
        if (cur->depth > max_depth) {
            free(cur);
            continue;
        }

        pages_visited++;

        // Check if this is the finish URL (exact match)
        if (strcmp(cur->url, finish_url) == 0) {
            found_node = cur;
            break;
        }

        // Fetch the page
        HttpResponse resp = fetch_url(cur->url);
        if (resp.data == NULL || resp.size == 0) {
            // nothing fetched; continue
            free(resp.data);
            // do not free cur yet — we can free now since children already enqueued will hold parents
            free(cur);
            continue;
        }

        // Extract links into a temporary list
        UrlNode* links = NULL;
        extract_links(resp.data, &links);

        // For each extracted link, normalize & enqueue if not visited
        for (UrlNode* ln = links; ln != NULL; ln = ln->next) {
            char full[MaxUrlLength];
            build_full_url(origin, ln->url, full, sizeof(full));

            // simple normalization: remove trailing '#' fragments (very small)
            char* hash = strchr(full, '#');
            if (hash) *hash = '\0';

            if (!visited_contains(&visited, full)) {
                // create a new node for BFS
                UrlNode* child = malloc(sizeof(UrlNode));
                if (!child) continue;
                strncpy(child->url, full, MaxUrlLength - 1);
                child->url[MaxUrlLength - 1] = '\0';
                child->depth = cur->depth + 1;
                child->parent = cur; // important: parent link for Path_print
                child->next = NULL;
                queue_push(&queue, child);
                visited_insert(&visited, child->url);
            }
        }

        free_temp_links(links);
        free(resp.data);

        // We free cur only if it's not the parent of any enqueued nodes that we need to keep the parent pointer for.
        // In our simple design, child->parent points to cur; we must keep cur until either:
        //  - it becomes the found node (in which case we stop and print), or
        //  - there are no children that require cur (hard to detect). To keep things simple and safe for Path_print,
        //    we will *not* free cur here so that parent pointers are valid if we find a later node that links back.
        //    BUT to avoid memory leak explosion for the presentation, we free cur when its depth is well below max_depth.
        // For simplicity in this demo, we won't free cur now (we'll let OS reclaim at program exit).
        // If you want aggressive freeing you can implement reference counting.
    }

    if (found_node) {
        printf("Found target URL. Reconstructing path:\n");
        Path_print(found_node);
    }
    else {
        printf("Target URL not found within depth %d (pages visited: %d)\n", max_depth, pages_visited);
    }

    // Note: For simplicity we don't free all outstanding queue nodes and visited table on exit.
    // The OS will reclaim memory on program termination. For production code, free everything.

    return 0;
}