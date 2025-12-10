# Web Crawler
Finds the shortest path between 2 Wikipedia links (start/goal) using a Breadth First Search (BFS) approach. Implements multithreading, a thread-safe URL queue using mutexes, and a hash table to track visited URLs. 
## Wikepedia Game
The goal of "The Wikipedia Game" is given 2 Wikipedia articles, navigate from the start article to the target article by only following links within the pages. 
From the Wikipedia article on the subject: 
>"Players start on the same randomly selected article and must navigate to a pre-selected target article solely by clicking links within each page. The objective is to reach the target article in the fewest clicks (articles) or in the least amount of time."
## Multithreading
Traditional single-threaded web crawlers are slow and inefficient for fetching multiple web pages. The objective of this project is to use multithreading in C to more efficiently crawl a set of URLs.
- Uses **pthreads** to fetch multiple web pages concurrently.  
- Each thread processes a URL from the queue independently.  
- Worker threads terminate gracefully when the target is found or max depth/pages are reached.  
## Mutexes
In this project, mutexes are used to safely coordinate multiple threads accessing shared resources:
- URL queue: Only one thread at a time can push or pop URLs, preventing conflicts when multiple threads try to add or remove nodes.  
- Visited set (hash table): Mutex ensures that checking for duplicates and inserting new URLs is thread-safe, so the same page isn’t visited twice.  
- Counters and flags: Pages visited, active threads, and the stop/found flags are all protected by mutexes to prevent inconsistent states.  
By using mutexes, we avoid race conditions and ensure the crawler runs correctly while multiple threads are fetching and processing pages simultaneously.
## Visited Set (hash table)
- Stores URLs in a **hash table** to avoid revisiting the same page.
- Each insert/check operation is thread safe via. mutex locking.
- Improves efficiency by preventing unnecessary page downloads.
## URL Queue
- Thread-safe FIFO queue stores URLs to visit.  
- Supports push/pop operations with condition variables to notify waiting threads.  
- Tracks depth and parent nodes for path reconstruction.
## Libcurl / Parsing
- **Libcurl** is used to download the HTML content of Wikipedia pages concurrently in each worker thread.  
- Each thread initializes its own "CURL" handle to fetch pages without interfering with other threads.  
- **HTML parsing** is done by scanning the page for /wiki/ links:  
  - Extracts both relative (`/wiki/...`) and absolute URLs (`https://en.wikipedia.org/wiki/...`).  
  - Links are canonicalized
  - Only links from the same origin (Wikipedia) are followed.  
- Extracted links are wrapped in UrlNodes and pushed to the **thread-safe queue** _ONLY_ if they haven’t been visited yet.  
- This allows the crawler to efficiently discover new pages and check for the target without revisiting pages unnecessarily.  

## Usage
Use the Makefile to build the executable
```bash
make
```
Run crawler
```bash
./crawler <start_url> <finish_url> <max_depth> [num_threads] [max_pages]
```
Help message
```bash
./crawler -h
```
