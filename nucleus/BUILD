cc_library(
  name = "task",
  hdrs = ["task.h"],
  srcs = ["task.cc"],
  deps = [
    "@com_google_absl//absl/strings:string_view",
    "@com_google_absl//absl/log:check",
    "@com_google_absl//absl/log:log",
  ],
)

cc_test(
    name = "task_test",
    srcs = ["task_test.cc"],
    deps = [
        ":task",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
        "@com_github_google_benchmark//:benchmark_main",
    ],
)
