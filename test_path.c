#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "path.h"

int main (){
    //creating a sample path for testing 
    UrlNode* start = malloc(sizeof(UrlNode));
    UrlNode* mid = malloc(sizeof(UrlNode));
    UrlNode* finish = malloc(sizeof(UrlNode));

    //filling in the URLs 
    strcpy(start->url, "http://testing.com/start");
    strcpy(mid->url, "http://testing.com/mid");
    strcpy(finish->url, "http://testing.com/finish");

    //setting up parent pointers
    start->parent = NULL;
    mid->parent = start;
    finish->parent = mid;

    //printing the path from start to finish
    printf("Testing Path_print function:\n");
    Path_print(finish);

    //freeing allocated memory
    free(start);
    free(mid);
    free(finish);
    
    return 0;
}