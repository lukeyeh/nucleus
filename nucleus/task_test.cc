#include "nucleus/task.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nucleus {
namespace {

TEST(Task, BlockOn) {
  const auto hello = []() -> Task {
    LOG(INFO) << "hello";
    co_return;
  };

  const auto world = []() -> Task {
    LOG(INFO) << "world";
    co_return;
  };

  const auto foo = [&]() -> Task {
    co_await hello();
    co_await world();
  };

  BlockOn(foo());
}

}  // namespace
}  // namespace nucleus
