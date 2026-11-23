#pragma once

#include <functional>
#include "app_state.h"
#include "event_types.h"

class Fsm {
public:
  using StateHandler = std::function<void()>;

  Fsm();
  ~Fsm() = default;

  AppState state() const { return state_; }
  const char* stateName() const { return AppStateName(state_); }

  // 状态机核心：喂事件 → 执行转换 → 调用 onEnter/onExit
  bool feed(Event::Kind event, const void* payload = nullptr);

  // 注册各状态的处理器
  void onEnter(AppState st, StateHandler handler) { enter_[static_cast<int>(st)] = handler; }
  void onExit (AppState st, StateHandler handler) { exit_ [static_cast<int>(st)] = handler; }

  // 强制跳转（用于异常恢复）
  void forceState(AppState st);

private:
  AppState state_ = AppState::Booting;
  StateHandler enter_[13];
  StateHandler exit_[13];

  void transit(AppState to);
  AppState nextState(Event::Kind event) const;
};

extern Fsm gFsm;  // 全局单例
