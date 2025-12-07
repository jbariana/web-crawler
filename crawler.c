/*Just including the headers that were provided by the professor
will write basic CLI and structures, you guys can edit it if you like */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <curl/curl.h> // added curl for HTTP requests
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "path.h" //Include the path header file 

#define MaxUrlLength 512 //maximum length of a URL,you guys can adjust as needed

//structure for URL queue
typedef struct {
    UrlNode *head;
    UrlNode *tail;
    int size;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} UrlQueue;

//structure for visited set
typedef struct VisitedNode {
    char url[MaxUrlLength];
    struct VisitedNode* next;
} VisitedNode;

//hash table for visited URLs
typedef struct {
    VisitedNode* buckets[1000];
    pthread_mutex_t lock;
} VisitedSet;

//structure for crawler state
typedef struct {
    UrlQueue* queue;
    VisitedSet* visited;
    char finish_url[MaxUrlLength];
    int max_depth, found;
    UrlNode* finish_node;
    pthread_mutex_t found_lock;
    int pages_visited;
    int max_pages;
} CrawlerState;

//structure for HTTP response
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

//show the help message 
void help_message(){
    printf("Usage: crawler <start_url> <finish_url> <depth>\n");

}

//called by fetch_url to store received data into an HttpResponse buffer
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


// Use to download web page, returns HttpResponse struct based on a given URL
// URL -> Data we can use
HttpResponse fetch_url(const char *url)
{
    
    CURL *curl;         //libcurl handle
    CURLcode res;       //result code

    //create an empty HttpResponse struct  
    HttpResponse response; 
    response.data=NULL;
    response.size=0;

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

//main function
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

    return 0;
}
