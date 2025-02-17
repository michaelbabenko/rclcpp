// Copyright 2015 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rclcpp/timer.hpp"

#include <chrono>
#include <string>
#include <memory>
#include <thread>

#include "rmw/impl/cpp/demangle.hpp"

#include "rclcpp/contexts/default_context.hpp"
#include "rclcpp/detail/cpp_callback_trampoline.hpp"
#include "rclcpp/exceptions.hpp"
#include "rclcpp/logging.hpp"
#include "rcutils/logging_macros.h"

using rclcpp::TimerBase;

TimerBase::TimerBase(
  rclcpp::Clock::SharedPtr clock,
  std::chrono::nanoseconds period,
  rclcpp::Context::SharedPtr context)
: clock_(clock), timer_handle_(nullptr)
{
  if (nullptr == context) {
    context = rclcpp::contexts::get_global_default_context();
  }

  auto rcl_context = context->get_rcl_context();

  timer_handle_ = std::shared_ptr<rcl_timer_t>(
    new rcl_timer_t, [ = ](rcl_timer_t * timer) mutable
    {
      {
        std::lock_guard<std::mutex> clock_guard(clock->get_clock_mutex());
        if (rcl_timer_fini(timer) != RCL_RET_OK) {
          RCUTILS_LOG_ERROR_NAMED(
            "rclcpp",
            "Failed to clean up rcl timer handle: %s", rcl_get_error_string().str);
          rcl_reset_error();
        }
      }
      delete timer;
      // Captured shared pointers by copy, reset to make sure timer is finalized before clock
      clock.reset();
      rcl_context.reset();
    });

  *timer_handle_.get() = rcl_get_zero_initialized_timer();

  rcl_clock_t * clock_handle = clock_->get_clock_handle();
  {
    std::lock_guard<std::mutex> clock_guard(clock_->get_clock_mutex());
    rcl_ret_t ret = rcl_timer_init(
      timer_handle_.get(), clock_handle, rcl_context.get(), period.count(), nullptr,
      rcl_get_default_allocator());
    if (ret != RCL_RET_OK) {
      rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't initialize rcl timer handle");
    }
  }
}

TimerBase::~TimerBase()
{
  clear_on_reset_callback();
}

void
TimerBase::cancel()
{
  rcl_ret_t ret = rcl_timer_cancel(timer_handle_.get());
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't cancel timer");
  }
}

bool
TimerBase::is_canceled()
{
  bool is_canceled = false;
  rcl_ret_t ret = rcl_timer_is_canceled(timer_handle_.get(), &is_canceled);
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't get timer cancelled state");
  }
  return is_canceled;
}

void
TimerBase::reset()
{
  rcl_ret_t ret = RCL_RET_OK;
  {
    std::lock_guard<std::recursive_mutex> lock(callback_mutex_);
    ret = rcl_timer_reset(timer_handle_.get());
  }
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't reset timer");
  }
}

bool
TimerBase::is_ready()
{
  bool ready = false;
  rcl_ret_t ret = rcl_timer_is_ready(timer_handle_.get(), &ready);
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Failed to check timer");
  }
  return ready;
}

std::chrono::nanoseconds
TimerBase::time_until_trigger()
{
  int64_t time_until_next_call = 0;
  rcl_ret_t ret = rcl_timer_get_time_until_next_call(
    timer_handle_.get(), &time_until_next_call);
  if (ret == RCL_RET_TIMER_CANCELED) {
    return std::chrono::nanoseconds::max();
  } else if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Timer could not get time until next call");
  }
  return std::chrono::nanoseconds(time_until_next_call);
}

std::shared_ptr<const rcl_timer_t>
TimerBase::get_timer_handle()
{
  return timer_handle_;
}

bool
TimerBase::exchange_in_use_by_wait_set_state(bool in_use_state)
{
  return in_use_by_wait_set_.exchange(in_use_state);
}

void
TimerBase::set_on_reset_callback(std::function<void(size_t)> callback)
{
  if (!callback) {
    throw std::invalid_argument(
            "The callback passed to set_on_reset_callback "
            "is not callable.");
  }

  auto new_callback =
    [callback, this](size_t reset_calls) {
      try {
        callback(reset_calls);
      } catch (const std::exception & exception) {
        RCLCPP_ERROR_STREAM(
          rclcpp::get_logger("rclcpp"),
          "rclcpp::TimerBase@" << this <<
            " caught " << rmw::impl::cpp::demangle(exception) <<
            " exception in user-provided callback for the 'on reset' callback: " <<
            exception.what());
      } catch (...) {
        RCLCPP_ERROR_STREAM(
          rclcpp::get_logger("rclcpp"),
          "rclcpp::TimerBase@" << this <<
            " caught unhandled exception in user-provided callback " <<
            "for the 'on reset' callback");
      }
    };

  std::lock_guard<std::recursive_mutex> lock(callback_mutex_);

  // Set it temporarily to the new callback, while we replace the old one.
  // This two-step setting, prevents a gap where the old std::function has
  // been replaced but rcl hasn't been told about the new one yet.
  set_on_reset_callback(
    rclcpp::detail::cpp_callback_trampoline<
      decltype(new_callback), const void *, size_t>,
    static_cast<const void *>(&new_callback));

  // Store the std::function to keep it in scope, also overwrites the existing one.
  on_reset_callback_ = new_callback;

  // Set it again, now using the permanent storage.
  set_on_reset_callback(
    rclcpp::detail::cpp_callback_trampoline<
      decltype(on_reset_callback_), const void *, size_t>,
    static_cast<const void *>(&on_reset_callback_));
}

void
TimerBase::clear_on_reset_callback()
{
  std::lock_guard<std::recursive_mutex> lock(callback_mutex_);

  if (on_reset_callback_) {
    set_on_reset_callback(nullptr, nullptr);
    on_reset_callback_ = nullptr;
  }
}

void
TimerBase::set_on_reset_callback(rcl_event_callback_t callback, const void * user_data)
{
  rcl_ret_t ret = rcl_timer_set_on_reset_callback(timer_handle_.get(), callback, user_data);

  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Failed to set timer on reset callback");
  }
}
