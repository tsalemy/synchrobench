#ifndef SEARCH_LAYER_C
#define SEARCH_LAYER_C

#define _GNU_SOURCE
#include "SearchLayer.h"
#include "SkipListLazyLock.h"
#include "JobQueue.h"
#include "LinkedList.h"
#include "Hazard.h"
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <numaif.h>
#include <atomic_ops.h>
#include <numa.h>
#include <sched.h>

inline void collect(memory_queue_t* garbage, LinkedList_t* retiredList, int zone);

searchLayer_t* constructSearchLayer(inode_t* sentinel, int zone) {
  searchLayer_t* numask = (searchLayer_t*)malloc(sizeof(searchLayer_t));
  numask -> sentinel = sentinel;
  numask -> numaZone = zone;
  numask -> updates = constructJobQueue();
  numask -> garbage = constructMemoryQueue();
  numask -> running = 0;
  numask -> stopGarbageCollection = 0;
  numask -> sleep_time = 0;
  return numask;
}

searchLayer_t* destructSearchLayer(searchLayer_t* numask) {
  stopIndexLayer(numask);
  destructJobQueue(numask -> updates);
  destructMemoryQueue(numask -> garbage);
  inode_t* runner = numask -> sentinel;
  while (runner != NULL) {
    inode_t* temp = runner;
    runner = (runner -> next == NULL) ? NULL : runner -> next[0];
    destructIndexNode(temp, numask -> numaZone);
  }
  free(numask);
}

int searchLayerSize(searchLayer_t* numask) {
  inode_t* runner = numask -> sentinel;
  int size = -2;
  while (runner != NULL) {
    size++;
    runner = runner -> next[0];
  }
  return size;
}

void startIndexLayer(searchLayer_t* numask, int sleep_time) {
  numask -> sleep_time = sleep_time;
  if (numask -> running == 0) {
    numask -> finished = 0;

    numask -> stopGarbageCollection = 0;
    pthread_create(&numask -> reclaimer, NULL, garbageCollectionIndexLayer, (void*)numask);

    pthread_create(&numask -> helper, NULL, updateNumaZone, (void*)numask);
    numask -> running = 1;
  }
}

void stopIndexLayer(searchLayer_t* numask) {
  if (numask -> running) {
    numask -> finished = 1;
    pthread_join(numask -> helper, NULL);

    numask -> stopGarbageCollection = 1;
    pthread_join(numask -> reclaimer, NULL);

    numask -> running = 0;
  }
}

void* updateNumaZone(void* args) {
  searchLayer_t* numask = (searchLayer_t*)args;
  job_queue_t* updates = numask -> updates;
  memory_queue_t* garbage = numask -> garbage;
  inode_t* sentinel = numask -> sentinel;
  const int numaZone = numask -> numaZone;

  //Pin to Zone & CPU
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(numaZone, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  while (numask -> finished == 0) {
    usleep(10);
    while (numask -> finished == 0 && runJob(sentinel, pop(updates), numaZone, garbage)) {}
  }

  return NULL;
}

int runJob(inode_t* sentinel, q_node_t* job, int zone, memory_queue_t* garbage) {
  if (job == NULL) {
    return 0;
  }
  else if (job -> operation == INSERTION) {
    add(sentinel, job -> val, job -> node, zone);
  }
  else if (job -> operation == REMOVAL) {
    removeNode(sentinel, job -> val, job -> node, zone, garbage);
  }
  free(job);
  return 1;
}

void* garbageCollectionIndexLayer(void* args) {
  searchLayer_t* numask = (searchLayer_t*)args;
  memory_queue_t* garbage = numask -> garbage;
  const int numaZone = numask -> numaZone;

  //Pin to Zone & CPU
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(numaZone, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  //Instantiate thread local retired list
  LinkedList_t* retiredList = constructLinkedList();

  while (numask -> stopGarbageCollection == 0) {
    usleep(10);
    collect(garbage, retiredList, numaZone);
  }
  collect(garbage, retiredList, numaZone);
  destructLinkedList(retiredList, 0);
}

inline void collect(memory_queue_t* garbage, LinkedList_t* retiredList, int zone) {
  m_node_t* subscriber;
  while ((subscriber = mq_pop(garbage)) != NULL) {
    RETIRE_INDEX_NODE(retiredList, subscriber -> node, zone);
    free(subscriber);
  }
}

#endif
