// Copyright (C) 2026 The CharaROM Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "otautil/duress.h"

TEST(DuressWipeTest, AcceptsGeneratedCommand) {
  EXPECT_TRUE(IsDuressWipe({"recovery", "--wipe_data", "--reason=duress"}));
  EXPECT_TRUE(IsDuressWipe({"recovery", "--wipe_data",
                           "--reason=duress,2026-10-24T15:00:00-0400", "--locale=en-US"}));
  EXPECT_TRUE(IsDuressWipe({"recovery", "--shutdown_after", "--wipe_data",
                           "--reason=duress", "--keep_memtag_mode"}));
}

TEST(DuressWipeTest, RejectsOrdinaryRecoveryAndMixedOperations) {
  EXPECT_FALSE(IsDuressWipe({}));
  EXPECT_FALSE(IsDuressWipe({"recovery"}));
  EXPECT_FALSE(IsDuressWipe({"recovery", "--wipe_data"}));
  EXPECT_FALSE(IsDuressWipe({"recovery", "--reason=duress"}));
  EXPECT_FALSE(IsDuressWipe({"recovery", "--wipe_data", "--reason=userrequested"}));
  EXPECT_FALSE(IsDuressWipe({"recovery", "--wipe_data", "--reason=duress-other"}));
  for (const auto& extra : {"--show_text", "--fastboot", "--update_package=/tmp/update.zip",
                            "--wipe_data", "--reason=duress", "--reason=userrequested",
                            "--prompt_and_wipe_data"}) {
    EXPECT_FALSE(IsDuressWipe({"recovery", "--wipe_data", "--reason=duress", extra}));
  }
}
