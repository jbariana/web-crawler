/*Just including the headers that were provided by the professor
will write basic CLI and structures, you guys can edit it if you like */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "path.h" //Include the path header file 

#define MaxUrlLength 512 //maximum length of a URL,you guys can adjust as needed

//structure to represent the URLNode in the path
typedef struct UrlNode {
    char url[MaxUrlLength];
    int depth;
    struct UrlNode* parent; //pointer to the parent UrlNode
    struct UrlNode* next; //pointer to the next UrlNode in the queue 

} UrlNode;

//structure for URL queue

typedef struct {
    UrlNode *head;
    urlNode *tail;
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
