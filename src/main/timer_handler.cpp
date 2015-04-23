#include <time.h>
#include <cstring>
#include <iostream>

#include "timer_handler.h"
#include "log.h"

void* TimerHandler::timer_handler_entry_func(void* arg)
{
  ((TimerHandler*)arg)->run();
  return NULL;
}

TimerHandler::TimerHandler(TimerStore* store,
                           Callback* callback) :
                           _store(store),
                           _callback(callback),
                           _terminate(false),
                           _nearest_new_timer(-1)
{
  pthread_mutex_init(&_mutex, NULL);

#ifdef UNITTEST
  _cond = new MockPThreadCondVar(&_mutex);
#else
  _cond = new CondVar(&_mutex);
#endif

  int rc = pthread_create(&_handler_thread,
                          NULL,
                          &timer_handler_entry_func,
                          (void*)this);
  if (rc < 0)
  {
    printf("Failed to start timer handling thread: %s", strerror(errno));
    exit(2);
  }
}

TimerHandler::~TimerHandler()
{
  if (_handler_thread)
  {
    pthread_mutex_lock(&_mutex);
    _terminate = true;
    _cond->signal();
    pthread_mutex_unlock(&_mutex);
    pthread_join(_handler_thread, NULL);
  }

  delete _cond;
  _cond = NULL;

  pthread_mutex_destroy(&_mutex);

  delete _callback;
}

void TimerHandler::add_timer(Timer* timer)
{
  LOG_DEBUG("Adding timer:  %lu", timer->id);
  pthread_mutex_lock(&_mutex);
  _store->add_timer(timer);
  pthread_mutex_unlock(&_mutex);
}

void TimerHandler::update_replica_tracker_for_timer(TimerID id,
                                                    int replica_index)
{
  pthread_mutex_lock(&_mutex);
  _store->update_replica_tracker_for_timer(id, 
                                           replica_index);
  pthread_mutex_unlock(&_mutex);
}

HTTPCode TimerHandler::get_timers_for_node(std::string request_node,
                                           int max_responses,
                                           std::string cluster_view_id,
                                           std::string& get_response)
{
  pthread_mutex_lock(&_mutex);
  HTTPCode rc = _store->get_timers_for_node(request_node, 
                                            max_responses, 
                                            cluster_view_id, 
                                            get_response);
  pthread_mutex_unlock(&_mutex);

  return rc;
}

// The core function in the timer handler, basic principle is to loop around repeatedly
// retrieving timers from the store, waiting until they need to pop and popping them.
//
// If there are no timers in the store at all, we wait forever for one to be added (or
// until we're terminated).  If we are woken while waiting for one set of timers to
// pop, check the timer store to make sure we're holding the nearest timers.
void TimerHandler::run() {
  std::unordered_set<Timer*> next_timers;
  std::unordered_set<Timer*>::iterator sample_timer;

  pthread_mutex_lock(&_mutex);

  _store->get_next_timers(next_timers);

  while (!_terminate)
  {
    if (!next_timers.empty())
    {
      LOG_DEBUG("Have a timer to pop");
      pthread_mutex_unlock(&_mutex);
      pop(next_timers);
      pthread_mutex_lock(&_mutex);
    }
    else
    {
      struct timespec next_pop;
      clock_gettime(CLOCK_MONOTONIC, &next_pop);

      if (next_pop.tv_nsec < 990 * 1000 * 1000)
      {
        next_pop.tv_nsec += 10 * 1000 * 1000;
      }
      else
      {
        next_pop.tv_nsec -= 990 * 1000 * 1000;
        next_pop.tv_sec += 1;
      }

      int rc = _cond->timedwait(&next_pop);

      if (rc < 0 && rc != ETIMEDOUT)
      {
        printf("Failed to wait for condition variable: %s", strerror(errno));
        exit(2);
      }
    }

    _store->get_next_timers(next_timers);
  }

  for (std::unordered_set<Timer*>::iterator it = next_timers.begin(); 
                                            it != next_timers.end(); 
                                            ++it)
  {
    delete *it;
  }

  next_timers.clear();

  pthread_mutex_unlock(&_mutex);
}

/*****************************************************************************/
/* PRIVATE FUNCTIONS                                                         */
/*****************************************************************************/

// Pop a set of timers, this function takes ownership of the timers and
// thus empties the passed in set.
void TimerHandler::pop(std::unordered_set<Timer*>& timers)
{
  for (std::unordered_set<Timer*>::iterator it = timers.begin(); 
                                            it != timers.end(); 
                                            ++it)
  {
    pop(*it);
  }
  timers.clear();
}

// Pop a specific timer, if required pass the timer on to the replication layer to
// reset the timer for another pop, otherwise destroy the timer record.
void TimerHandler::pop(Timer* timer)
{
  // Tombstones are reaped when they pop.
  if (timer->is_tombstone())
  {
    LOG_DEBUG("Discarding expired tombstone");
    delete timer;
    return;
  }

  // Increment the timer's sequence before sending the callback.
  timer->sequence_number++;

  // Update the timer in case it has out of date configuration
  timer->update_cluster_information();

  // The callback takes ownership of the timer at this point.
  _callback->perform(timer); timer = NULL;
}
