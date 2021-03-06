#ifndef SEARCH_LAYER_H
#define SEARCH_LAYER_H

#include "Nodes.h"
#include "JobQueue.h"
#include <pthread.h>

typedef struct searchLayer {
	inode_t* sentinel;
	pthread_t helper;
	int numaZone;
	job_queue_t* updates;
	volatile char finished;
	volatile char running;
	int sleep_time;
} searchLayer_t;

//driver functions
void* updateNumaZone(void* args);
int runJob(inode_t* sentinel, q_node_t* job, int zone);

//helper functions
searchLayer_t* constructSearchLayer(inode_t* sentinel, int zone);
searchLayer_t* destructSearchLayer(searchLayer_t* searcher);
void start(searchLayer_t* numask, int sleep_time);
void stop(searchLayer_t* numask);

#endif