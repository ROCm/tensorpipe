/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/epoll.h>

#include <tensorpipe/common/callback.h>
#include <tensorpipe/transport/shm/fd.h>
#include <tensorpipe/transport/shm/reactor.h>

namespace tensorpipe {
namespace transport {
namespace shm {

class Connection;
class Listener;

// Abstract base class called by the epoll(2) event loop.
//
// Dispatch to multiple types is needed because we must deal with a
// few listening sockets and an eventfd(2) per connection.
//
class EventHandler {
 public:
  virtual ~EventHandler() = default;

  virtual void handleEventsFromLoop(int events) = 0;
};

class Loop final {
 public:
  using TDeferredFunction = std::function<void()>;

  Loop();

  // Prefer using deferToLoop over runInLoop when you don't need to wait for the
  // result.
  template <typename F>
  void runInLoop(F&& fn) {
    // When called from the event loop thread itself (e.g., from a callback),
    // deferring would cause a deadlock because the given callable can only be
    // run when the loop is allowed to proceed. On the other hand, it means it
    // is thread-safe to run it immediately. The danger here however is that it
    // can lead to an inconsistent order between operations run from the event
    // loop, from outside of it, and deferred.
    if (reactor_.inReactorThread()) {
      fn();
    } else {
      // Must use a copyable wrapper around std::promise because
      // we use it from a std::function which must be copyable.
      auto promise = std::make_shared<std::promise<void>>();
      auto future = promise->get_future();
      deferToLoop([promise, fn{std::forward<F>(fn)}]() {
        try {
          fn();
          promise->set_value();
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      });
      future.get();
    }
  }

  // Run function on reactor thread.
  // If the function throws, the thread crashes.
  void deferToLoop(TDeferredFunction fn);

  // Provide access to the underlying reactor.
  Reactor& reactor();

  // Register file descriptor with event loop.
  //
  // Trigger the handler if any of the epoll events in the `events`
  // mask occurs. The loop stores a weak_ptr to the handler, so it is
  // the responsibility of the caller to keep the handler alive. If an
  // event is triggered, the loop first acquires a shared_ptr to the
  // handler before calling into its handler function. This ensures
  // that the handler is alive for the duration of this function.
  //
  void registerDescriptor(int fd, int events, std::shared_ptr<EventHandler> h);

  // Unregister file descriptor from event loop.
  //
  // This resets the weak_ptr to the event handler that was registered
  // in `registerDescriptor`. Upon returning, the handler can no
  // longer be called, even if there were pending events for the file
  // descriptor. Only if the loop had acquired a shared_ptr to the
  // handler prior to this function being called, can the handler
  // function still be called.
  //
  void unregisterDescriptor(int fd);

  void close();

  // Tell loop to terminate when no more handlers remain.
  void join();

  ~Loop();

  inline bool inLoopThread() {
    return reactor_.inReactorThread();
  }

  static std::string formatEpollEvents(uint32_t events);

 private:
  static constexpr auto kCapacity_ = 64;

  // The reactor is used to process events for this loop.
  Reactor reactor_;

  // Wake up the event loop.
  void wakeup();

  // Main loop function.
  void loop();

  Fd epollFd_;
  Fd eventFd_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> joined_{false};
  std::thread thread_;

  // Interaction with epoll(7).
  //
  // A dedicated thread runs epoll_wait(2) in a loop and, every time it returns,
  // it defers a function to the reactor which is responsible for processing the
  // epoll events and executing the handlers, and then notify the epoll thread
  // that it is done, for it to start another iteration. This back-and-forth
  // between these threads is done to ensure that all epoll handlers are run
  // from the reactor thread, just like everything else. Doing so makes it
  // easier to reason about how certain events are sequenced. For example, if
  // another processes first makes a write to a connection and then closes the
  // accompanying Unix domain socket, we know for a fact that the reactor will
  // first react to the write, and then react to the epoll event caused by
  // closing the socket. If we didn't force serialization onto the reactor, we
  // would not have this guarantee.
  //
  // It's safe to call epoll_ctl from one thread while another thread is blocked
  // on an epoll_wait call. This means that the kernel internally serializes the
  // operations on a single epoll fd. However, we have no way to control whether
  // a modification of the set of file descriptors monitored by epoll occurred
  // just before or just after the return from the epoll_wait. This means that
  // when we start processing the result of epoll_wait we can't know what set of
  // file descriptors it operated on. This becomes a problem if, for example, in
  // between the moment epoll_wait returns and the moment we process the results
  // a file descriptor is unregistered and closed and another one with the same
  // value is opened and registered: we'd end up calling the handler of the new
  // fd for the events of the old one (which probably include errors).
  //
  // However, epoll offers a way to address this: epoll_wait returns, for each
  // event, the piece of extra data that was provided by the *last* call on
  // epoll_ctl for that fd. This allows us to detect whether epoll_wait had
  // taken into account an update to the set of fds or not. We do so by giving
  // each update a unique identifier, called "record". Each update to a fd will
  // associate a new record to it. The handlers are associated to records (and
  // not to fds), and for each fd we know which handler is the one currently
  // installed. This way when processing an event we can detect whether the
  // record for that event is still valid or whether it is stale, in which case
  // we disregard the event, and wait for it to fire again at the next epoll
  // iteration, with the up-to-date handler.
  std::unordered_map<int, uint64_t> fdToRecord_;
  std::unordered_map<uint64_t, std::weak_ptr<EventHandler>> recordToHandler_;
  uint64_t nextRecord_{1}; // Reserve record 0 for the eventfd
  std::mutex handlersMutex_;

  // Deferred to the reactor to handle the events received by epoll_wait(2).
  void handleEpollEventsFromLoop(std::vector<struct epoll_event> epollEvents);

  friend class Connection;
  friend class Listener;
};

} // namespace shm
} // namespace transport
} // namespace tensorpipe
