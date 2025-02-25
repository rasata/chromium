// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill_assistant/browser/retry_timer.h"

#include <map>
#include <set>

#include "base/bind.h"
#include "base/callback.h"
#include "base/test/mock_callback.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gmock/include/gmock/gmock.h"

using ::testing::_;

namespace autofill_assistant {

namespace {

class RetryTimerTest : public testing::Test {
 protected:
  RetryTimerTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME) {}

  void FastForwardOneSecond() {
    scoped_task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));
  }

  base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>
  AlwaysFailsCallback() {
    return base::BindRepeating(&RetryTimerTest::AlwaysFails,
                               base::Unretained(this));
  }

  void AlwaysFails(base::OnceCallback<void(bool)> callback) {
    try_count_++;
    std::move(callback).Run(false);
  }

  base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>
  SucceedsOnceCallback(int succeds_at) {
    return base::BindRepeating(&RetryTimerTest::SucceedsOnce,
                               base::Unretained(this), succeds_at);
  }

  void SucceedsOnce(int succeeds_at, base::OnceCallback<void(bool)> callback) {
    EXPECT_GE(succeeds_at, try_count_);
    bool success = succeeds_at == try_count_;
    try_count_++;
    std::move(callback).Run(success);
  }

  base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>
  CaptureCallback() {
    return base::BindRepeating(&RetryTimerTest::Capture,
                               base::Unretained(this));
  }

  void Capture(base::OnceCallback<void(bool)> callback) {
    try_count_++;
    captured_callback_ = std::move(callback);
  }

  // scoped_task_environment_ must be first to guarantee other field
  // creation run in that environment.
  base::test::ScopedTaskEnvironment scoped_task_environment_;

  int try_count_ = 0;
  base::OnceCallback<void(bool)> captured_callback_;
  base::MockCallback<base::OnceCallback<void(bool)>> done_callback_;
};

TEST_F(RetryTimerTest, TryOnceAndSucceed) {
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  EXPECT_CALL(done_callback_, Run(true));
  retry_timer.Start(base::TimeDelta::FromSeconds(10), SucceedsOnceCallback(0),
                    done_callback_.Get());
  EXPECT_EQ(1, try_count_);
}

TEST_F(RetryTimerTest, TryOnceAndFail) {
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  EXPECT_CALL(done_callback_, Run(false));
  retry_timer.Start(base::TimeDelta::FromSeconds(0), AlwaysFailsCallback(),
                    done_callback_.Get());
  EXPECT_EQ(1, try_count_);
}

TEST_F(RetryTimerTest, TryMultipleTimesAndSucceed) {
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(10), SucceedsOnceCallback(2),
                    done_callback_.Get());
  EXPECT_EQ(1, try_count_);
  FastForwardOneSecond();
  EXPECT_EQ(2, try_count_);
  EXPECT_CALL(done_callback_, Run(true));
  FastForwardOneSecond();
  EXPECT_EQ(3, try_count_);
}

TEST_F(RetryTimerTest, TryMultipleTimesAndFail) {
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(2), AlwaysFailsCallback(),
                    done_callback_.Get());
  EXPECT_EQ(1, try_count_);
  FastForwardOneSecond();
  EXPECT_EQ(2, try_count_);
  EXPECT_CALL(done_callback_, Run(false));
  FastForwardOneSecond();
  EXPECT_EQ(3, try_count_);
}

TEST_F(RetryTimerTest, Cancel) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(10), AlwaysFailsCallback(),
                    done_callback_.Get());
  EXPECT_EQ(1, try_count_);
  retry_timer.Cancel();
  FastForwardOneSecond();  // nothing should happen
}

TEST_F(RetryTimerTest, CancelWithPendingCallbacks) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);
  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(10), CaptureCallback(),
                    done_callback_.Get());
  ASSERT_TRUE(captured_callback_);
  retry_timer.Cancel();
  std::move(captured_callback_).Run(true);  // Should do nothing
}

TEST_F(RetryTimerTest, GiveUpWhenLeavingScope) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);
  {
    RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
    retry_timer.Start(base::TimeDelta::FromSeconds(10), AlwaysFailsCallback(),
                      done_callback_.Get());
    EXPECT_EQ(1, try_count_);
  }
  FastForwardOneSecond();  // nothing should happen
}

TEST_F(RetryTimerTest, GiveUpWhenLeavingScopeWithPendingCallback) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);
  {
    RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
    retry_timer.Start(base::TimeDelta::FromSeconds(10), CaptureCallback(),
                      done_callback_.Get());
    ASSERT_TRUE(captured_callback_);
  }
  std::move(captured_callback_).Run(true);  // Should do nothing
}

TEST_F(RetryTimerTest, RestartOverridesFirstCall) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);

  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(1), AlwaysFailsCallback(),
                    done_callback_.Get());
  base::MockCallback<base::OnceCallback<void(bool)>> done_callback2;
  retry_timer.Start(base::TimeDelta::FromSeconds(1), AlwaysFailsCallback(),
                    done_callback2.Get());
  EXPECT_EQ(2, try_count_);
  EXPECT_CALL(done_callback2, Run(false));
  FastForwardOneSecond();
  EXPECT_EQ(3, try_count_);
}

TEST_F(RetryTimerTest, RestartOverridesFirstCallWithPendingTask) {
  EXPECT_CALL(done_callback_, Run(_)).Times(0);

  RetryTimer retry_timer(base::TimeDelta::FromSeconds(1));
  retry_timer.Start(base::TimeDelta::FromSeconds(1), CaptureCallback(),
                    done_callback_.Get());
  ASSERT_TRUE(captured_callback_);

  base::MockCallback<base::OnceCallback<void(bool)>> done_callback2;
  retry_timer.Start(base::TimeDelta::FromSeconds(1), AlwaysFailsCallback(),
                    done_callback2.Get());

  std::move(captured_callback_).Run(true);  // Should do nothing

  EXPECT_CALL(done_callback2, Run(false));
  FastForwardOneSecond();
  EXPECT_EQ(3, try_count_);
}

}  // namespace
}  // namespace autofill_assistant
