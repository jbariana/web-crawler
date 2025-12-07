/*This file implements the Path_print function
  you guys can just include the codes to the main crawler.c after*/

#include <stdio.h>
#include <stdlib.h>
#include "path.h"


void Path_print(UrlNode* node){
    if(!node) 
        return; // if node is NULL, no path exists 

    //count how many nodes are in the path 
    int count = 0;
    UrlNode* temp = node;

    while(temp){
        count++;
        temp = temp->parent;

    }

    //creating an array of char to store the path in order
    char** pathArray = malloc(count * sizeof(char*));
    if(!pathArray){
        fprintf(stderr, "Error: Memory allocation failed.\n");
        return;
    }
    //filling the array with URLs from the path 
    temp = node;
    for (int i = count - 1; i>= 0; i--){
        pathArray[i] = temp->url;
        temp = temp->parent;
    }

    //printing the path from start to finish 
    printf("Path Found:\n");
    for (int i = 0; i < count; i++){
        printf("%s\n", pathArray[i]);
    }

    free(pathArray); //freeing the allocated memory 
}
    