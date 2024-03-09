#ifndef NUCLEUS_TASK_H_
#define NUCLEUS_TASK_H_

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"

namespace nucleus {

// Events happen like this:
//
// *  Someone calls a function that returns a Task let's call this `foo`.
//
// *  Coroutine frame is allocated.
//
// *  Copies parameters into coroutine frame.
//
// *  Constructs Task::promise_type in the coroutine frame.
//
// *  Call promose_type::get_return_object(). This produces our Task object
//    that we'll give to the caller. We will initialize it with a coroutine
//    handle that refers to the current coroutine we're in (i.e. `foo`).
//
// *  Suspend execution of the coroutine `foo`. Note that this is due to the
//    fact that we return `std::suspend_always` in `initial_suspend`.
//
// *  Now, the caller of `foo` has a `Task` object. `foo` is still suspended and
//    has not continued its execution. The caller of `foo` let's say `bar` needs
//    to `co_await` this Task in order for `foo` to resume.
//
//    This is because the `Awaiter` that returned on the overloaded `co_await`
//    of the `Task`, implements `await_suspend`. `await_suspend` is called
//    whenever we co_await on the Awaiter.
//
//    in `await_suspend` we do two things:
//
//      1.  We save our own (`bar`'s) coroutine handle in `foo`'s promise
//          object. This is so that when foo inevitably finishes
//          (asynchronously, it can resume `bar`'s execution (perhaps awaiting
//          more `Task`s)
//
//      2.  Now that callee (`foo`) coroutine has the means to resume the caller
//          (`bar`), we can now resume the callee (`foo`).
//
//
// *  Once the callee (`foo`) has finished execution, by calling `co_return
//    co_await` (just `co_return` for short), we will resume the caller
//    coroutine (`bar`).
//
// *  `bar` may `co_await` on more `Task`s and repeat all of the above, or it
//    could also do the same and hit a co_return. This is because `bar` is a
//    coroutine itself and must resume the coroutine that called it too.
class Task {
 public:
  Task(Task&& t) noexcept : handle_(std::exchange(t.handle_, {})) {}

  class promise_type {
   public:
    Task get_return_object() noexcept {
      return Task{
          std::coroutine_handle<promise_type>::from_promise(*this),
      };
    }

    // Always suspending before the actual coroutine body is important. This is
    // because it gives us the oppurtunity to synchronize the caller and the
    // callee (oursevless). This is convinient as, we can stash the caller's
    // handle before we do any real work, giving ourselves the ability to resume
    // the caller whenever we're done, without any races.
    //
    // Without this initial suspend always suspending, there's the chance that
    // the asynchronous work completes very quickly or even synchronously, and
    // as a consquence delete ourselves, before the caller can even pass its
    // continuation. Concretely, the Awaiter object could be deleted before we
    // can even stash the coroutine's handle, causing a use-after-free.
    std::suspend_always initial_suspend() noexcept { return {}; }

    // Task currently does carry any sort of value.
    void return_void() noexcept {}

    // Don't support exceptions they are not great for performance critical
    // applications.
    void unhandled_exception() noexcept { std::terminate(); }

    // Called on all co_return calls. Think of the `final_suspend` as a return.
    // If you think about a coroutine it is just a more generic function that
    // can temporarily yield at aribtrary points in the function. A regular
    // function just has a single yield point (at the very end).
    //
    // The calling coroutine should have stashed it's own coroutine handle in
    // this promise_type during the initial suspend, so all we need to do here
    // is resume it.
    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      void await_suspend(
          std::coroutine_handle<promise_type> this_coroutine_handle) noexcept {
        this_coroutine_handle.promise().continuation_.Resume();
      }
      void await_resume() noexcept {}
    };
    final_awaiter final_suspend() noexcept { return {}; }

    struct Continuation {
      std::optional<std::coroutine_handle<>> caller_continuation;
      absl::Notification* done;
      void Resume() {
        done->Notify();
        if (caller_continuation.has_value()) {
          caller_continuation->resume();
        }
      }
    };

    // A stash for the caller's coroutine handle. It is stashed during the
    // initial suspension of this coroutine, when we yield control back to the
    // caller where it sets its own coroutine handle during
    // Awaiter::await_suspend, before resuming oureslves here.
    Continuation continuation_;
  };

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // The Awaiter is an objec that we return on the overloaded co_await operator.
  class Awaiter {
   public:
    // Stash the callee's coroutine in the Awaiter. This is so we can stash the
    // caller's coroutine's handle in the callee's. The callee will then be able
    // to resume the caller.
    explicit Awaiter(std::coroutine_handle<Task::promise_type> handle) noexcept
        : this_coroutine_(handle) {}

    // There is a possiblity for opitmization here: If we know that the callee
    // coroutine is done at this point, we can just resume immediately. Perhaps
    // we can surface this as an interface in the future.
    bool await_ready() noexcept { return false; }

    // During the co_await expression the caller's coroutine is passed into the
    // `await_suspend`. We the callee, will stash that coroutine in our
    // promise_object, so that we can resume the caller when we're done.
    void await_suspend(
        std::coroutine_handle<> caller_coroutine_handle) noexcept {
      this_coroutine_.promise().continuation_ = promise_type::Continuation{
          .caller_continuation = caller_coroutine_handle,
          .done = &done_,
      };
      this_coroutine_.resume();
    }

    void await_resume() noexcept {}

    void Run() {
      absl::Notification n;
      this_coroutine_.promise().continuation_ = promise_type::Continuation{
          .caller_continuation = std::nullopt,
          .done = &n,
      };
      this_coroutine_.resume();
      n.WaitForNotification();
    }

   private:
    absl::Notification done_;
    std::coroutine_handle<Task::promise_type> this_coroutine_;
  };
  Awaiter operator co_await() && noexcept {
    return Awaiter{
        handle_,
    };
  }

  // Force users to call BlockOn.
  struct PassKey {
   private:
    explicit PassKey() = default;
    friend void BlockOn(Task task);
  };
  void Run(PassKey) { Awaiter(handle_).Run(); }

 private:
  explicit Task(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  std::coroutine_handle<promise_type> handle_;
};

void BlockOn(Task task);

}  // namespace nucleus

#endif  // NUCLEUS_TASK_H_
