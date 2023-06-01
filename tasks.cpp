#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"

struct task_list_o {
	std::deque<coroutine> tasks;
	uint32_t              num_tasks = 0; // number of tasks, only gets decremented if taks has been removed

	coroutine pop_task() {
		if ( tasks.empty() ) {
			return {};
		}

		// invariant: tasks is not empty
		coroutine result = tasks.front();

		tasks.pop_front();
		return result;
	}
	void push_task( coroutine const& c ) {
		tasks.push_back( c );
	}
};

class scheduler_impl {
  public:
	std::vector<channel*>     channels; // non-owning - channels are owned by their threads
	std::vector<std::jthread> threads;

	task_list_o* task_list;

	// create a new task list object
	task_list_o** new_task_list() {
		task_list = new task_list_o();
		return &task_list;
	};

	scheduler_impl( int32_t num_worker_threads = 0 );

	~scheduler_impl() {
		// deletes whether last_list is nullptr or not.

		// we must wait until all the threads have been joined.

		// tell all threads to join
		// deleting a jthread object implicitly stops and joins
		threads.clear();

		delete task_list;
	}

	// add new objects to a task list object
	void add_to_task_list( task_list_o** p_t, coroutine c );

	// execute all tasks in the task list, then free the task list object
	void wait_for_task_list( task_list_o** p_t );
};

void scheduled_task::await_suspend( std::coroutine_handle<promise> h ) noexcept {
	// At this point the coroutine pointed to by h has been fully suspended.
	//

	// check if we have a scheduler available via the promise.
	//
	// if not, we have not been placed onto the scheduler,
	// and we should not start execution yet.
	if ( h.promise().scheduler ) {
		auto& scheduler = h.promise().scheduler;

		// put the current coroutine to the back of the scheduler queue
		// as it has been fully suspended at this point.

		scheduler->task_list->push_task( h.promise().get_return_object() );

		if ( false ) {
			// take next task from front of scheduler queue -
			// we can do this so that multiple threads can share the
			// scheduling workload potentially.
			// but we can also disable that, so that there is only one thread
			// that does the scheduling, and that removes elements from the
			// queue.
			coroutine c = scheduler->task_list->pop_task();

			if ( !c.done() ) {
				c();
			} else {
				assert( false && "task must not be done" );
			}

			// TODO: we should remove a task that is being processed from the scheduler.
			//
			// TODO: we can add the current handle back onto the scheduler -
			// as it goes into suspension, the scheduler will have to pick it up again.
			// we add it at the end of the list of tasks so that the scheduler can spend
			// some time with other tasks first.

			// TODO: then the scheduler picks the next element from the list
			// and executes that one.
		}
	}

	// note: if we drop off here, we must do so whilst being in the scheduling thread -
	// as this will return to where the resume() command was issued.
}

void finalize_task::await_suspend( std::coroutine_handle<promise> h ) noexcept {
	// this is the last time that the coroutine will be awakened
	// we do not suspend it anymore after this
	( *h.promise().p_task_list )->num_tasks--; // this needs to be an atomic operation
	std::cout << "Final suspend for coroutine." << std::endl
	          << std::flush;
}

scheduler_impl::scheduler_impl( int32_t num_worker_threads ) {
	// todo: create a threadpool, and initialise threads with spinwaits
	// and somehow add a method to push a coroutine to the thread.

	if ( num_worker_threads < 0 ) {
		// If negative, then this means that we must
		// count backwards from the number of available hardware threads
		num_worker_threads = std::jthread::hardware_concurrency() + num_worker_threads;
	}

	assert( num_worker_threads >= 0 && "inferred number of worker threads must not be negative" );

	// reserve memory so that we can take addresses for channel
	// and don't have to worry about iterator validity
	channels.reserve( num_worker_threads );
	threads.reserve( num_worker_threads );

	// NOTE THAT BY DEFAULT WE DON'T HAVE ANY WORKER THREADS
	//
	for ( int i = 0; i != num_worker_threads; i++ ) {
		channels.emplace_back(
		    new channel() ); // if this fails, then we must manually destroy the channel, otherwise we will leak the channel
		threads.emplace_back(
		    []( std::stop_token stop_token, channel* c ) {
			    using namespace std::literals::chrono_literals;
			    // thread worker implementation:
			    //
			    // TODO: block on channel read
			    //
			    while ( !stop_token.stop_requested() ) {
				    // spinlock
				    std::this_thread::sleep_for( 200ms );
			    }

			    // channel is owned by the thread - when the thread falls out of scope that means that the channel gets deleted
			    delete c;
		    },
		    channels.back() );
	}
}

inline void scheduler_impl::add_to_task_list( task_list_o** p_t, coroutine c ) {
	// TODO: we can block if there is not enough space in tasks to accomodate
	// another job - but that could be problematic, in so far as we must
	// reach a wait_for_tasklist command so that the number of tasks in the list
	// recedes.
	c.promise().scheduler   = this;
	c.promise().p_task_list = p_t;
	( *p_t )->num_tasks++;
	( *p_t )->tasks.push_back( c );
}

void scheduler_impl::wait_for_task_list( task_list_t p_t ) {
	// Test for whether this task list exists
	//
	if ( p_t == nullptr ) {
		return;
	}

	// --------| invariant: task list exists
	//
	// Execute tasks in this task list until there are no more tasks left
	// to execute.

	// we would split the work here onto multiple threads -
	// the threads would then potentially steal work etc
	// but this thread here will be the one that brings everything
	// back together again.
	while ( ( *p_t )->num_tasks ) {
		coroutine c = ( *p_t )->pop_task();
		if ( !c.done() ) {
			c();
		} else {
			assert( false && "task must not be done" );
		}
	}

	// Clean up coroutines that are owned by this task list
	//
	for ( auto& c : ( *p_t )->tasks ) {
		c.destroy();
	}

	// Free task list
	delete *p_t;

	// Once all tasks have been complete, release task list
	*p_t = nullptr;
}

scheduler_o::scheduler_o( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

task_list_o** scheduler_o::new_task_list() {
	return p_impl->new_task_list();
}

void scheduler_o::add_to_task_list( task_list_o** p_t, coroutine c ) {
	p_impl->add_to_task_list( p_t, c );
}

void scheduler_o::wait_for_task_list( task_list_o** p_t ) {
	p_impl->wait_for_task_list( p_t );
}

scheduler_o* scheduler_o::create( int32_t num_worker_threads ) {
	return new scheduler_o( num_worker_threads );
}

scheduler_o::~scheduler_o() {
	delete p_impl;
}
