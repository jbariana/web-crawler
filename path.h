/*This is a declaration header file to declare the URLNode struct and
function used to print a reconstructed path from start url to the finish url*/ 

#ifndef PATH_H
#define PATH_H

#define MaxUrlLength 512 //maximum length of a URL,you guys can adjust as needed

/*Structure to represent the URLNode in the path*/
typedef struct UrlNode {
    char url[MaxUrlLength];
    int depth;
    struct UrlNode* parent; //pointer to the parent UrlNode
    struct UrlNode* next; //pointer to the next UrlNode in the queue 

} UrlNode;

/*prints the path by following parent pointers*/
void Path_print(UrlNode* node);

#endif 
