#include "nucleus/task.h"

#include <coroutine>
#include <exception>
#include <utility>

namespace nucleus {

void BlockOn(Task task) {
  task.Run(Task::PassKey{});
}

}  // namespace nucleus
