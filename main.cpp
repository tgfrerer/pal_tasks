#include "tasks.h"

//

int main() {
	// we would like a syntax that goes:

	scheduler_o* scheduler = scheduler_o::create( 2 ); // create a scheduler with two hardware worker threads

	task_list_t task_list      = scheduler->new_task_list();
	auto        coro_generator = []( int i ) -> coroutine {
        std::cout << "executing coroutine: i:, " << i++ << std::endl
                  << std::flush;

        // this yields control back to the await_suspend method, and to our scheduler
        co_await scheduled_task();

        std::cout << "executing coroutine: i:, " << i++ << std::endl;
        co_return;
	};

	scheduler->add_to_task_list( task_list, coro_generator( 10 ) );
	scheduler->add_to_task_list( task_list, coro_generator( 20 ) );
	scheduler->add_to_task_list( task_list, coro_generator( 30 ) );

	scheduler->wait_for_task_list( task_list );

	std::cout << "Back with main program." << std::endl
	          << std::flush;

	delete scheduler;
	return 0;
}
