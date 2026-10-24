// Copyright (C) 2026 The CharaROM Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>

// Only the unattended command produced by RecoverySystem may suppress the UI.
// Keep in sync with com.android.server.power.DuressReboot.
inline bool IsDuressWipe(const std::vector<std::string>& args) {
  int wipes = 0;
  int reasons = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--wipe_data") {
      ++wipes;
    } else if (arg == "--reason=duress" || arg.rfind("--reason=duress,", 0) == 0) {
      ++reasons;
    } else if (arg.rfind("--locale=", 0) != 0 && arg != "--shutdown_after" &&
               arg != "--keep_memtag_mode") {
      return false;
    }
  }
  return wipes == 1 && reasons == 1;
}
