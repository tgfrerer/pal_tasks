
# Things which we have learned so far

## Performance testing and measuring

* if you want to poke kernel values you must do this via a single sudo-ed expression:

```sh
sudo /bin/sh -c "echo -1 > /proc/kernel/the_value_i_want_to_poke"
```

* In terms of performance testing, it really pays to take repeated
  measurements, and then average - there is too much noise otherwise,
  and small gains are usually not significant. If you can reduce time
  by about 20%, you have probably found an optimisation, otherwise, it
  may just be noise.

* it's really difficult to create a test workload that is constant for
  whichever worker you throw at it. If you use random number
  generators, who calls the random stream when will have an influence
  on which numbers they get back. And this will change the amount of
  time work takes.

* It's very hard to reach the same performance as a naive thread-job
  implementation for non-continuing jobs.

## Architecture insights

* Having the workers pull work from the scheduler is better than
  having the scheduler push work onto the workers. Since the workers
  are the only ones that read and write to their work queue, you can
  make the work queue for the schedulers super simple.

  Another benefit from this architecture is that a task will get
  resumed by the same thread that put it back onto its queue. That
  way, there is a chance that the cache is still warm.

* Having the workers spin-wait is okay as long as we can keep them
  reasonably busy. Our first implementation used flags and put CPUs to
  sleep - this did not work very well.

* Eagerness of workers has remarkable little influence - I wonder why
  this does not make more of a change.

## Multithreading insights

* You need to be even more careful about multithreading pitfalls - as
  resuming jumps scopes and in that case **we don't have a guaranteed
  nesting of scopes anymore**, we must make sure that we don't depend
  on memory to be still available after `resume` - for example, if we
  resume() the coroutine that is the `waiter` on a `task_list_o`, this
  resume may trigger deletion of the `task_list_o`, because it may
  resume to fall out of scope. The safest thing to do is to not do
  anything after `resume()`.

# TODO:

* we should be able to use the original tasklists as the source to
  pull work into the workers - the scheduler should not have to
  allocate anything. we should be able to implement this as
  a forward-list, where the scheduler only keeps the head of the list,
  and the tail of the list. Adding to the scheduler will add at the
  tail.
