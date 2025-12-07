#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "path.h"

/*
 * Prints the reconstructed path from the starting URL
 * to the given node by following parent pointers.
 *
 * Example output for a 3-level chain:
 *   http://testing.com/start
 *   http://testing.com/mid
 *   http://testing.com/finish
 */
void Path_print(UrlNode* node) {
    if (node == NULL) {
        return;
    }

    // Step 1: determine path length by walking backwards
    int count = 0;
    UrlNode* temp = node;
    while (temp != NULL) {
        count++;
        temp = temp->parent;
    }

    // Step 2: allocate an array to act as a stack
    UrlNode** stack = malloc(sizeof(UrlNode*) * count);
    if (stack == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    // Step 3: push nodes onto stack in reverse order
    temp = node;
    for (int i = count - 1; i >= 0; i--) {
        stack[i] = temp;
        temp = temp->parent;
    }

    // Step 4: print from start to finish
    for (int i = 0; i < count; i++) {
        printf("%s\n", stack[i]->url);
    }

    // cleanup
    free(stack);
}