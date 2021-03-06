#ifndef SKIPLISTLAZYLOCK_C
#define SKIPLISTLAZYLOCK_C

#include "SkipListLazyLock.h"

//helper function definitions
inline int search(inode_t *sentinel, int val, inode_t **predecessors, inode_t **successors);
inline int validDeletion(inode_t *candidate, int idx);
inline void unlockLevels(inode_t **nodes, int topLevel);

//Lazily searches the skip list, not regarding locks
//stores the predeccor and successor nodes during a traversal of the skip list.
//if the value is found, then returns the first index in the successor where it was found
//if the value is not found, returns -1
inline int search(inode_t *sentinel, int val, inode_t **predecessors, inode_t **successors) {
  inode_t *previous = sentinel, *current = NULL;
  int idx = -1;
  for (int i = previous -> topLevel - 1; i >= 0; i--) {
    current = previous -> next[i];
    while (current -> val < val) {
      previous = current;
      current = previous -> next[i];
    }
    if (idx == -1 && val == current -> val) {
      idx = i;
    }
    predecessors[i] = previous;
    successors[i] = current;
  }
  return idx;
}

//unlocks all the levels that were locked previously
inline void unlockLevels(inode_t **nodes, int topLevel) {
  inode_t* previous = NULL;
  for (int i = 0; i <= topLevel; i++) {
    if (nodes[i] != previous) {
      pthread_mutex_unlock(&nodes[i] -> lock);
    }
    previous = nodes[i];
  }
}

//Inserts a value into the skip list when the value is not logically in the list
int add(inode_t *sentinel, int val, node_t* dataLayer, int zone) {
  const int topLevel = getRandomLevel(sentinel -> topLevel);
  inode_t *predecessors[sentinel -> topLevel], *successors[sentinel -> topLevel];
  unsigned int backoff = 1;
  struct timespec timeout; 
  char retry = 1;
  
  while(retry) {
    //store the result of a traveral through the skip list while searching for a value
    int idx = search(sentinel, val, predecessors, successors);
    //if already inside the tree
    if (idx != -1) {
      inode_t* candidate = successors[idx];
      //if it isnt markedToDelete for removal, wait until it has been fully linked and return false
      if (candidate -> markedToDelete == 0) {
        while (candidate -> fullylinked == 0) {}
        return 0;
      }
      //if it was deleted, and hasn't been removed yet, we restart
      continue;
    }
    //if it hasn't been found in the tree, attempt to lock all the necessary
    //levels and then insert the new tower of nodes
    int highest_locked = -1;
    inode_t *previous = NULL, *runner = NULL, *prev_pred = NULL;
    char valid = 1;
    for (int i = 0; i < topLevel && valid; i++) {
      previous = predecessors[i];
      runner = successors[i];
      if (previous != prev_pred) {
        pthread_mutex_lock(&previous -> lock);
        highest_locked = i;
        prev_pred = previous;
      }
      //ensures that all values that have been seen remain valid
      valid = previous -> markedToDelete == 0 &&
              runner -> markedToDelete == 0 &&
              (volatile inode_t*)previous -> next[i] == (volatile inode_t*)runner;
    }

    //if we broke out of the loop because one of the nodes wasn't valid, then we unlock
    //all the levels and attempt again
    if (valid == 0) {
      unlockLevels(predecessors, highest_locked);
      if (backoff > 5000) {
        timeout.tv_sec = backoff / 5000;
        timeout.tv_nsec = (backoff % 5000) * 1000000;
        nanosleep(&timeout, NULL);
      }
      backoff *= 2;
      continue;
    }
    //otherwise, all nodes in our critical sections have been locked and we can
    //insert into the list
    inode_t* insertion = constructIndexNode(val, topLevel, dataLayer, zone);
    for (int i = 0; i < topLevel; i++) {
      insertion -> next[i] = successors[i];
      predecessors[i] -> next[i] = insertion;
    }
    insertion -> fullylinked = 1;
    unlockLevels(predecessors, highest_locked);
    return 1;
  }
}

//returns whether or not the candidate node is valid for deletion
inline int validDeletion(inode_t *candidate, int idx) {
  return candidate -> fullylinked && (candidate -> topLevel - 1) == idx && candidate -> markedToDelete;
}

//removes a value in the skip list when present
int removeNode(inode_t *sentinel, int val, int zone) {
  inode_t *predecessors[sentinel -> topLevel], *successors[sentinel -> topLevel];
  inode_t* candidate = NULL;
  int markedToDelete = 0, topLevel = -1;
  unsigned int backoff;
  struct timespec timeout;
  char retry = 1;

  while (retry) {
    //store the result of a traveral through the skip list while searching for a value
    int idx = search(sentinel, val, predecessors, successors);
    //if we're marked to delete or we found a valid value to delete, then attempt to remove
    if (markedToDelete || (idx != -1 && validDeletion(successors[idx], idx))) {
      //if the candidate node hasn't already been marked to delete, grab its lock and mark it for deletion
      if (markedToDelete == 0) {
        candidate = successors[idx];
        pthread_mutex_lock(&candidate -> lock);
        topLevel = candidate -> topLevel;
        if (candidate -> markedToDelete) {
          pthread_mutex_unlock(&candidate -> lock);
          return 0;
        }
        candidate -> markedToDelete = 1;
        markedToDelete = 1;
      }

      //attempt to gain control of all relevant locks before deletion
      int highest_locked = -1;
      int valid = 1;
      inode_t *previous, *runner = NULL, *prev_pred = NULL;
      for (int i = 0; i < topLevel && valid; i++) {
        previous = predecessors[i];
        runner = successors[i];
        if (previous != prev_pred) {
          pthread_mutex_lock(&previous -> lock);
          highest_locked = i;
          prev_pred = previous;
        }
        valid = previous -> markedToDelete == 0 &&
              (volatile inode_t*)previous -> next[i] == (volatile inode_t*)runner;
      }

      //if we broke out of the loop because one of the nodes wasn't valid, then we unlock
      //all the levels and attempt again
      if (valid == 0) {
        unlockLevels(predecessors, highest_locked);
        if (backoff > 5000) {
          timeout.tv_sec = backoff / 5000;
          timeout.tv_nsec = (backoff % 5000) / 1000000;
          nanosleep(&timeout, NULL);
        }
        backoff *= 2;
        continue;
      }
      //otherwise, all nodes in our critical sections have been locked and we can
      //delete the proper nodes in our tower by removing their links
      for (int i = topLevel - 1; i >= 0; i--) {
        predecessors[i] -> next[i] = candidate -> next[i];
      }
      pthread_mutex_lock(&candidate -> dataLayer -> lock); //QUESTION: is this needed?
      candidate -> dataLayer -> references--;
      pthread_mutex_unlock(&candidate -> dataLayer -> lock);
      candidate -> dataLayer = NULL; //QUESTION: will this be a problem and is it needed?
      pthread_mutex_unlock(&candidate -> lock);
      unlockLevels(predecessors, highest_locked);
      return 1;
    }
    //otherwise we didn't find a node with the value and nothing was markedToDelete for deletion,
    //and therefore we return false
    else {
      return 0;
    }
  }
}

#endif