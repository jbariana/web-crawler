/* Multithreaded Wikipedia Crawler */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <unistd.h>

#include "path.h"   // UrlNode + Path_print()

#define MaxUrlLength 512          // from path.h 
#define VISITED_BUCKETS 1000      // number of hash buckets
#define MAX_THREADS 10            // number of worker threads

/* -------------------- URL QUEUE -------------------- */
/* This queue is used for BFS over Wikipedia URLs. */

typedef struct {
    UrlNode *head;
    UrlNode *tail;
    int size;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} UrlQueue;

/* -------------------- VISITED SET -------------------- */
/* Hash table to avoid revisiting the same URL. */

typedef struct VisitedNode {
    char url[MaxUrlLength];
    struct VisitedNode* next;
} VisitedNode;

typedef struct {
    VisitedNode* buckets[VISITED_BUCKETS];
    pthread_mutex_t lock;   // single global lock (Option A)
} VisitedSet;

/* -------------------- CRAWLER STATE -------------------- */
/* Shared state passed to all worker threads. */

typedef struct {
    UrlQueue* queue;
    VisitedSet* visited;

    char finish_url[MaxUrlLength];   // target article
    int max_depth;
    int found;                       // 1 if finish_url found

    UrlNode* finish_node;            // reconstructed chain end-node

    pthread_mutex_t found_lock;      // protects found, pages_visited, finish_node
    int pages_visited;
    int max_pages;

    int done;                        // signal threads to stop
} CrawlerState;

/* -------------------- HTTP RESPONSE -------------------- */


typedef struct {
    char* data;
    size_t size;
} HttpResponse;

/* show the help message  */
void help_message(){
    printf("Usage: crawler <start_url> <finish_url> <depth>\n");
}

/* called by fetch_url to store received data into an HttpResponse buffer */
size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    HttpResponse *resp = (HttpResponse *)userdata;  //cast userdata to HttpResponse*
    size_t total = size * nmemb;                    //total bytes received in this call

    //Expand the buffer to hold the new data
    char *new_data = realloc(resp->data, resp->size + total);
    if (!new_data) {
        return 0;  // returning 0 tells libcurl to abort the transfer
    }
    resp->data = new_data;

    // copy the new data into the buffer
    memcpy(resp->data + resp->size, ptr, total);
    resp->size += total;  // update total size
    
    // tell libcurl how many bytes were handled
    return total;
}

/* 
 * Use to download web page, returns HttpResponse struct based on a given URL
 * URL -> Data we can use
 */
HttpResponse fetch_url(const char *url)
{
    CURL *curl;         //libcurl handle
    CURLcode res;       //result code

    //create an empty HttpResponse struct  
    HttpResponse response; 
    response.data = NULL;
    response.size = 0;

    //initialize curl
    curl = curl_easy_init();
    //check if there was failure
    if(!curl){
        fprintf(stderr, "Failed to initialize curl\n");
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);                       //url to fetch
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);             //redirects automatically
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);  //set callback function
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);   //pass response struct
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);                   //timeout of 10 seconds
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ",,,,,");             //user agent string

    //perform the http request
    res = curl_easy_perform(curl);

    //check if request succeeded
    if(res != CURLE_OK){
        fprintf(stderr, "curl error for %s: %s\n", url, curl_easy_strerror(res));
    }

    // clean curl and free resources
    curl_easy_cleanup(curl);

    //return response struct (data and size)
    return response;
}


void simple_extract_links(const char *html, UrlNode **head) {
    const char *p = html;

    //loop through the HTML, finding each "/wiki/..." link
    while ((p = strstr(p, "<a href=\"/wiki/")) != NULL) {
        p += strlen("<a href=\"");                     //move pointer past the "<a href="

        const char *end = strchr(p, '"');           //find the closing quote of the URL
        if (!end) break;                                 //stop if malformed

        size_t len = end - p;                            //calculate length of URL
        if (len >= MaxUrlLength) len = MaxUrlLength - 1; //enforce max length

        //allocate a new URL node and copy the URL into it
        UrlNode *node = malloc(sizeof(UrlNode));
        strncpy(node->url, p, len);
        node->url[len] = '\0';

        //insert the new node at the head of the linked list
        node->next = *head;
        *head = node;

        // continue searching after this link
        p = end;
    }
}

/* ====================================================== */
/* =============== QUEUE IMPLEMENTATION ================= */
/* ====================================================== */

void init_queue(UrlQueue* q) {
    q->head = q->tail = NULL;
    q->size = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

/* Enqueue a new UrlNode into the shared queue (BFS frontier). */
void enqueue(UrlQueue* q, const char* url, int depth, UrlNode* parent) {
    UrlNode* node = malloc(sizeof(UrlNode));
    if (!node) return;

    strncpy(node->url, url, MaxUrlLength);
    node->url[MaxUrlLength - 1] = '\0'; // ensure null-terminated
    node->depth = depth;
    node->parent = parent;
    node->next = NULL;

    pthread_mutex_lock(&q->lock);

    if (q->tail == NULL) {
        q->head = q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    q->size++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/* Dequeue a UrlNode from the shared queue; blocks if empty until work or done. */
UrlNode* dequeue(UrlQueue* q, CrawlerState* state) {
    pthread_mutex_lock(&q->lock);

    while (q->head == NULL && !state->done) {
        pthread_cond_wait(&q->cond, &q->lock);
    }

    if (q->head == NULL) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    UrlNode* node = q->head;
    q->head = node->next;
    if (q->head == NULL)
        q->tail = NULL;

    q->size--;
    pthread_mutex_unlock(&q->lock);
    return node;
}

/* ====================================================== */
/* =============== VISITED SET IMPLEMENTATION =========== */
/* ====================================================== */

unsigned int hash_string(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash % VISITED_BUCKETS;
}

void init_visited_set(VisitedSet* set) {
    for (int i = 0; i < VISITED_BUCKETS; i++) {
        set->buckets[i] = NULL;
    }
    pthread_mutex_init(&set->lock, NULL);
}

/* Add URL to visited set; return 1 if newly added, 0 if already present. */
int add_visited(VisitedSet* set, const char* url) {
    pthread_mutex_lock(&set->lock);

    unsigned int idx = hash_string(url);
    VisitedNode* node = set->buckets[idx];

    while (node) {
        if (strcmp(node->url, url) == 0) {
            pthread_mutex_unlock(&set->lock);
            return 0;   // already visited
        }
        node = node->next;
    }

    VisitedNode* newnode = malloc(sizeof(VisitedNode));
    if (!newnode) {
        pthread_mutex_unlock(&set->lock);
        return 0;
    }

    strncpy(newnode->url, url, MaxUrlLength);
    newnode->url[MaxUrlLength - 1] = '\0';
    newnode->next = set->buckets[idx];
    set->buckets[idx] = newnode;

    pthread_mutex_unlock(&set->lock);
    return 1;
}

/* ====================================================== */
/* =============== CUSTOM LINK PARSER =================== */
/* ====================================================== */
/* This replaces regex: parses HTML manually for /wiki/ links. */

void extract_links_crawler(const char *html, int depth, UrlNode* parent, CrawlerState* state) {
    if (!html) return;

    const char *p = html;

    while ((p = strstr(p, "<a href=\"/wiki/")) != NULL) {

        // Move pointer right after "<a href=\""
        p += strlen("<a href=\"");

        const char *end = strchr(p, '"');
        if (!end) break;

        int len = (int)(end - p);
        if (len <= 6 || len >= MaxUrlLength) {
            p = end;
            continue;
        }

        char path[MaxUrlLength];
        strncpy(path, p, len);
        path[len] = '\0';

        // Skip unwanted namespaces
        if (strncmp(path, "/wiki/File:", 11) == 0 ||
            strncmp(path, "/wiki/Wikipedia:", 16) == 0 ||
            strncmp(path, "/wiki/Help:", 11) == 0 ||
            strncmp(path, "/wiki/Special:", 14) == 0 ||
            strncmp(path, "/wiki/Template:", 15) == 0 ||
            strncmp(path, "/wiki/Talk:", 11) == 0 ||
            strncmp(path, "/wiki/Category:", 15) == 0) {
            p = end;
            continue;
        }

        // Build full URL
        char full[MaxUrlLength];
        snprintf(full, MaxUrlLength, "https://en.wikipedia.org%s", path);
        full[MaxUrlLength - 1] = '\0';

        // Check if target already found
        pthread_mutex_lock(&state->found_lock);
        int already_found = state->found;
        pthread_mutex_unlock(&state->found_lock);

        /* If this link is the finish_url, record the path. */
        if (!already_found && strcmp(full, state->finish_url) == 0) {
            pthread_mutex_lock(&state->found_lock);

            if (!state->found) {
                state->found = 1;

                // Build deep copy chain of parent nodes
                UrlNode* chain = NULL;
                for (UrlNode* p2 = parent; p2; p2 = p2->parent) {
                    UrlNode* copy = malloc(sizeof(UrlNode));
                    if (!copy) break;
                    strncpy(copy->url, p2->url, MaxUrlLength);
                    copy->url[MaxUrlLength - 1] = '\0';
                    copy->depth = p2->depth;
                    copy->parent = chain;
                    copy->next = NULL;
                    chain = copy;
                }

                // Final node
                UrlNode* fin = malloc(sizeof(UrlNode));
                if (fin) {
                    strncpy(fin->url, full, MaxUrlLength);
                    fin->url[MaxUrlLength - 1] = '\0';
                    fin->depth = depth + 1;
                    fin->parent = chain;
                    fin->next = NULL;
                    state->finish_node = fin;
                }

                printf("\n[FOUND] Target article reached!\n");
            }

            pthread_mutex_unlock(&state->found_lock);
        }

        // BFS expansion if not found and depth limit not reached
        if (!already_found && depth < state->max_depth) {
            enqueue(state->queue, full, depth + 1, parent);
        }

        p = end;
    }
}

/* ====================================================== */
/* =============== WORKER THREAD FUNCTION =============== */
/* ====================================================== */

void* crawler_thread(void* arg) {
    CrawlerState* state = (CrawlerState*)arg;

    while (!state->done) {
        UrlNode* current = dequeue(state->queue, state);
        if (!current) break; // queue empty and done set

        // skip if already visited
        if (!add_visited(state->visited, current->url)) {
            free(current);
            continue;
        }

        // update stats
        pthread_mutex_lock(&state->found_lock);
        state->pages_visited++;
        int pages = state->pages_visited;
        int already_found = state->found;
        pthread_mutex_unlock(&state->found_lock);

        if (pages % 50 == 0) {
            printf("[INFO] Visited %d pages...\n", pages);
        }

        // fetch HTML for current URL via classmate's fetch_url()
        HttpResponse resp = fetch_url(current->url);

        if (resp.data != NULL && resp.size > 0) {
            // Make a null-terminated copy for string functions
            char* html = malloc(resp.size + 1);
            if (html) {
                memcpy(html, resp.data, resp.size);
                html[resp.size] = '\0';

                // parse HTML and enqueue new links
                extract_links_crawler(html, current->depth, current, state);

                free(html);
            }
            free(resp.data);
        }

        free(current);

        // Stop condition: found or max_pages reached
        if (already_found || pages >= state->max_pages) {
            state->done = 1;
            pthread_cond_broadcast(&state->queue->cond);
            break;
        }

        // be polite, short sleep
        usleep(10000);
    }

    return NULL;
}

/* ====================================================== */
/* =============== HELPER: URL VALIDATION =============== */
/* ====================================================== */

int is_wikipedia_url(const char* url) {
    return strncmp(url, "https://en.wikipedia.org/wiki/", 30) == 0;
}

/* ====================================================== */
/* ======================== MAIN ======================== */
/* ====================================================== */

int main(int argc, char *argv[])
{
    //when -h is provided, show help message
    if (argc == 2 && strcmp(argv[1], "-h") == 0){
        help_message();
        return 0;
    }

    //check for correct number of arguments
    if (argc != 4){
        fprintf(stderr, "Error: Invalid number of arguments.\n");
        help_message();
        return 1;
    }

    const char* start_url  = argv[1];
    const char* finish_url = argv[2];
    int max_depth          = atoi(argv[3]);

    if (!is_wikipedia_url(start_url) || !is_wikipedia_url(finish_url)) {
        fprintf(stderr, "Both start_url and finish_url must be Wikipedia article URLs.\n");
        return 1;
    }

    if (max_depth <= 0) {
        fprintf(stderr, "Depth must be a positive integer.\n");
        return 1;
    }

    // initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // initialize queue and visited set
    UrlQueue queue;
    VisitedSet visited;
    init_queue(&queue);
    init_visited_set(&visited);

    // initialize shared state
    CrawlerState state;
    state.queue = &queue;
    state.visited = &visited;
    strncpy(state.finish_url, finish_url, MaxUrlLength);
    state.finish_url[MaxUrlLength - 1] = '\0';
    state.max_depth = max_depth;
    state.found = 0;
    state.finish_node = NULL;
    state.pages_visited = 0;
    state.max_pages = 500; // safety cap
    state.done = 0;
    pthread_mutex_init(&state.found_lock, NULL);

    // enqueue the starting URL at depth 0, no parent
    enqueue(&queue, start_url, 0, NULL);

    // create worker threads
    pthread_t threads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_create(&threads[i], NULL, crawler_thread, &state);
    }

    // wait for all threads to finish
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n[STATS] Total pages visited: %d\n", state.pages_visited);

    // if a path was found, print it using Path_print from path.c
    if (state.finish_node != NULL) {
        Path_print(state.finish_node);
    } else {
        printf("\nNo path found from:\n%s\nTO\n%s\nwithin depth %d.\n",
               start_url, finish_url, max_depth);
    }

    curl_global_cleanup();
    return 0;
}
