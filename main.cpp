#include "src/tasks.h"
#include <iostream>
#include <iomanip>

#include <thread>

int raytracer_main( int argc, char** argv ); // from raytracer.cpp

int main( int argc, char** argv ) {

	raytracer_main( argc, argv );

	// argument 0 is the path to the application
	// argument 1 is the number of threads specified, if any
	// argument 2 is which tests to run, a string of 0 or 1
	const int   num_threads = argc >= 2 ? atoi( argv[ 1 ] ) : -1;
	char const* choices     = argc >= 3 ? argv[ 2 ] : "001";

	return 0;

	// Create a scheduler with as many hardware threads as possible
	//  0 ... No worker threads, just one main thread
	//  n ... n number of worker threads
	// -1 ... As many worker threads as cpus, -1
	Scheduler* scheduler = Scheduler::create( num_threads );

	if ( choices && *choices++ == '1' ) {

		std::cout << std::endl;
		std::cout << "- - - - -SCENARIO 1" << std::endl;

		TaskList tasks{};
		auto     task_generator = []( Scheduler* scheduler, int i ) -> Task {
            std::cout << "primary task " << i << " (tid:" << std::hex << std::this_thread::get_id() << ") " << std::endl;
            {
                TaskList inner_list;

                inner_list.add_task( []() -> Task {
                    std::cout << "inside         inner task (tid:" << std::hex << std::this_thread::get_id() << ") " << std::endl;
                    co_return;
                }() );
                inner_list.add_task( []() -> Task {
                    std::cout << "inside another inner task (tid:" << std::hex << std::this_thread::get_id() << ") " << std::endl;
                    co_return;
                }() );
                co_await suspend_task();

                if ( true ) {
                    scheduler->wait_for_task_list( inner_list );
                } else {
                    co_await scheduler->wait_for_task_list_inner( inner_list );
                }

                // put this coroutine back on the scheduler
            }
            std::cout << "resuming primary task " << i << " (tid:" << std::hex << std::this_thread::get_id() << ") " << std::endl;

            // we have resumed this coroutine from the scheduler

            // complete work, signal to the compiler that this is a
            // coroutine for political reasons.
            co_return;
		};

		// add many more tasks
		for ( int i = 0; i != 10; i++ ) {
			tasks.add_task( task_generator( scheduler, i ) );
		}

		// Execute all tasks we find on the task list
		scheduler->wait_for_task_list( tasks );
	}

	// --- vanilla scenario
	if ( choices && *choices++ == '1' ) {

		std::cout << std::endl;
		std::cout << "- - - - -SCENARIO 2" << std::endl;
		TaskList tasks{};
		auto     task_generator = []( int i ) -> Task {
            std::cout << "doing some work: " << i++ << std::endl;

            // put this coroutine back on the scheduler
			co_await suspend_task();

			// we have resumed this coroutine from the scheduler
            std::cout << "resuming work: " << i++ << std::endl;

            // complete work, signal to the compiler that this is a
            // coroutine for political reasons.
            co_return;
		};

		// add many more tasks
		for ( int i = 0; i != 5; i++ ) {
			tasks.add_task( task_generator( i ) );
		}

		// Execute all tasks we find on the task list
		scheduler->wait_for_task_list( tasks );
	}
	//

	// ----------------------------------------------------------------------

	srand( 0xdeadbeef );

	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	if ( choices && *choices++ == '1' ) {

		std::cout << std::endl;
		std::cout << "- - - - - SCENARIO 3" << std::endl;

		/*
		 * This test is for whether we can issue tasks from within our
		 * current task system.
		 */

		TaskList another_task_list{};
		auto     coro_generator = []( uint i, Scheduler* sched ) -> Task {
            // std::cout << "first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
            //           << std::flush;

            std::this_thread::sleep_for( std::chrono::microseconds( rand_r( &i ) % 55000 ) );

			auto inner_coro_generator = []( uint i, int j ) -> Task {
				//    std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
				//              << std::flush;

				std::this_thread::sleep_for( std::chrono::microseconds( rand_r( &i ) % 40000 ) );
				// this yields control back to the await_suspend method, and to our scheduler
				co_await suspend_task();

				std::this_thread::sleep_for( std::chrono::microseconds( rand_r( &i ) % 33000 ) );
				//    std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
				co_return;
			};

			uint32_t num_tasks = rand_r( &i ) % 10;

			// Create a task list for tasks which are spun off from within this task
			TaskList inner_task_list{};

			for ( int j = 0; j != num_tasks; j++ ) {
				inner_task_list.add_task( inner_coro_generator( i, j * 10 ) );
			}

			std::this_thread::sleep_for( std::chrono::microseconds( rand_r( &i ) % 40000 ) );

			// Suspend this task
			co_await suspend_task();

			// ----------| invariant: we are back after resuming.

			std::cout << "executing first level coroutine: " << std::dec << std::setw( 10 ) << i << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
			if ( false ) {
				// Execute, and wait for tasks that we spin out from this task
				co_await sched->wait_for_task_list_inner( inner_task_list );
			} else {
				sched->wait_for_task_list( inner_task_list );
			}

			// Suspend this task again
			co_await suspend_task();

            // ----------| invariant: we are back after resuming.

            // std::cout << "finished first level coroutine: " << std::dec << i << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            co_return;
		};

		for ( int i = 0; i != 30; i++ ) {
			another_task_list.add_task( coro_generator( i * 10, scheduler ) );
		}

		std::cout << "main program starts wait for task list." << std::endl
		          << std::flush;

		scheduler->wait_for_task_list( another_task_list );
	}

	std::cout << "- - - - - Back with main program." << std::endl
	          << std::flush;
	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	delete scheduler;

	return 0;
}
